#include "media/transport/pj_ioqueue_udp_transport.h"

#include "media/transport/pj_ioqueue_service.h"

#include <pjlib.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace iris::media {
namespace {

constexpr std::size_t kReceiveSlotCount = 8;
constexpr std::size_t kSendSlotCount = 128;
constexpr std::size_t kReceiveBufferSize = 65536;
constexpr unsigned kSocketBufferSize = 1024 * 1024;

std::string pj_error(pj_status_t status) {
    char message[PJ_ERR_MSG_SIZE]{};
    pj_strerror(status, message, sizeof(message));
    return message;
}

bool is_rtcp_payload(std::uint8_t second_byte) {
    // Unlike RTP, the RTCP packet type occupies the complete second byte.
    return second_byte >= 192 && second_byte <= 223;
}

} // namespace

struct PjIoqueueUdpTransport::Impl {
    struct ReceiveSlot {
        pj_ioqueue_op_key_t op_key{};
        std::vector<std::uint8_t> packet =
            std::vector<std::uint8_t>(kReceiveBufferSize);
        pj_sockaddr source{};
        int source_length = 0;
    };

    struct SendSlot {
        pj_ioqueue_op_key_t op_key{};
        std::vector<std::uint8_t> packet;
        bool busy = false;
    };

    Impl(MediaKind value, PjIoqueueService& service)
        : media_kind(value), service_(service) {
    }

    MediaResult open(const TransportConfig& config) {
        close();
        if (!config.rtcpMux) {
            return MediaResult::failure(
                "PJ ioqueue transport currently requires RTCP-mux");
        }

        auto& service = service_;
        if (!service.ok()) {
            return MediaResult::failure(service.error());
        }

        pool_ = pj_pool_create(
            service.pool_factory(), "iris-media-udp%p",
            4096, 4096, nullptr);
        if (!pool_) {
            return MediaResult::failure(
                "failed to create PJ media transport pool");
        }

        const std::string bind_address =
            config.bindAddress.empty() || config.bindAddress == "auto"
                ? std::string("0.0.0.0")
                : config.bindAddress;
        pj_str_t address = pj_str(const_cast<char*>(
            bind_address.c_str()));
        pj_sockaddr local{};
        auto status = pj_sockaddr_init(
            pj_AF_INET(), &local, &address,
            static_cast<pj_uint16_t>(config.rtpPortMin));
        if (status != PJ_SUCCESS) {
            cleanup_pool();
            return MediaResult::failure(
                "failed to initialize local media address: " +
                pj_error(status));
        }

        status = pj_sock_socket(
            pj_AF_INET(), pj_SOCK_DGRAM(), 0, &socket_);
        if (status != PJ_SUCCESS) {
            cleanup_pool();
            return MediaResult::failure(
                "failed to create PJ UDP socket: " +
                pj_error(status));
        }

        unsigned receive_buffer = kSocketBufferSize;
        pj_sock_setsockopt_sobuf(
            socket_, pj_SO_RCVBUF(), PJ_TRUE, &receive_buffer);
        unsigned send_buffer = kSocketBufferSize;
        pj_sock_setsockopt_sobuf(
            socket_, pj_SO_SNDBUF(), PJ_TRUE, &send_buffer);

        status = pj_sock_bind(
            socket_, &local, pj_sockaddr_get_len(&local));
        if (status != PJ_SUCCESS) {
            cleanup_socket();
            cleanup_pool();
            return MediaResult::failure(
                "failed to bind PJ UDP socket: " +
                pj_error(status));
        }

        pj_sockaddr bound{};
        int bound_length = sizeof(bound);
        status = pj_sock_getsockname(
            socket_, &bound, &bound_length);
        if (status != PJ_SUCCESS) {
            cleanup_socket();
            cleanup_pool();
            return MediaResult::failure(
                "failed to query PJ UDP socket: " +
                pj_error(status));
        }

        status = pj_grp_lock_create(pool_, nullptr, &group_lock_);
        if (status != PJ_SUCCESS) {
            cleanup_socket();
            cleanup_pool();
            return MediaResult::failure(
                "failed to create PJ transport group lock: " +
                pj_error(status));
        }
        status = pj_grp_lock_add_ref(group_lock_);
        if (status != PJ_SUCCESS) {
            pj_grp_lock_destroy(group_lock_);
            group_lock_ = nullptr;
            cleanup_socket();
            cleanup_pool();
            return MediaResult::failure(
                "failed to retain PJ transport group lock: " +
                pj_error(status));
        }

        pj_ioqueue_callback callbacks{};
        callbacks.on_read_complete = &Impl::on_read_complete;
        callbacks.on_write_complete = &Impl::on_write_complete;
        status = pj_ioqueue_register_sock2(
            pool_, service.ioqueue(), socket_, group_lock_,
            this, &callbacks, &key_);
        if (status != PJ_SUCCESS) {
            pj_grp_lock_dec_ref(group_lock_);
            group_lock_ = nullptr;
            cleanup_socket();
            cleanup_pool();
            return MediaResult::failure(
                "failed to register PJ UDP socket: " +
                pj_error(status));
        }
        status = pj_ioqueue_set_concurrency(key_, PJ_FALSE);
        if (status != PJ_SUCCESS) {
            close();
            return MediaResult::failure(
                "failed to serialize PJ UDP callbacks: " +
                pj_error(status));
        }

        receive_slots_.reserve(kReceiveSlotCount);
        for (std::size_t index = 0;
             index < kReceiveSlotCount;
             ++index) {
            auto slot = std::make_unique<ReceiveSlot>();
            pj_ioqueue_op_key_init(
                &slot->op_key, sizeof(slot->op_key));
            receive_slots_.push_back(std::move(slot));
        }
        send_slots_.reserve(kSendSlotCount);
        for (std::size_t index = 0;
             index < kSendSlotCount;
             ++index) {
            auto slot = std::make_unique<SendSlot>();
            pj_ioqueue_op_key_init(
                &slot->op_key, sizeof(slot->op_key));
            send_slots_.push_back(std::move(slot));
        }

        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            closing_ = false;
            description_.address =
                config.advertiseAddress.empty() ||
                    config.advertiseAddress == "auto"
                ? discover_local_address()
                : config.advertiseAddress;
            description_.rtpPort = pj_sockaddr_get_port(&bound);
            description_.rtcpPort = description_.rtpPort;
            description_.rtcpMux = true;
            description_.secure = false;
        }

        for (const auto& slot : receive_slots_) {
            const auto result = arm_receive(*slot);
            if (!result.ok) {
                close();
                return result;
            }
        }
        return MediaResult::success();
    }

    void close() noexcept {
        pj_ioqueue_key_t* key = nullptr;
        pj_grp_lock_t* group_lock = nullptr;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            closing_ = true;
            handler_ = {};
            key = key_;
            group_lock = group_lock_;
        }

        {
            std::lock_guard<std::mutex> io_lock(io_mutex_);
            if (key) {
                pj_ioqueue_clear_key(key);
                pj_ioqueue_unregister(key);
            }
        }
        cleanup_socket();

        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            key_ = nullptr;
            remote_configured_ = false;
            receive_slots_.clear();
            send_slots_.clear();
        }
        if (group_lock) {
            pj_grp_lock_dec_ref(group_lock);
        }
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            group_lock_ = nullptr;
        }
        cleanup_pool();
    }

    TransportDescription description() const {
        std::lock_guard<std::mutex> lock(state_mutex_);
        return description_;
    }

    void set_handler(PacketHandler handler) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        handler_ = std::move(handler);
    }

    MediaResult set_remote(const std::string& address,
                           std::uint16_t port) {
        if (address.empty() || port == 0) {
            return MediaResult::failure(
                "invalid remote media address");
        }
        pj_str_t value = pj_str(const_cast<char*>(address.c_str()));
        pj_sockaddr remote{};
        const auto status = pj_sockaddr_init(
            pj_AF_INET(), &remote, &value, port);
        if (status != PJ_SUCCESS) {
            return MediaResult::failure(
                "invalid remote media address: " +
                pj_error(status));
        }
        std::lock_guard<std::mutex> lock(state_mutex_);
        remote_ = remote;
        remote_length_ = pj_sockaddr_get_len(&remote);
        remote_configured_ = true;
        return MediaResult::success();
    }

    MediaResult send(MediaKind kind,
                     bool is_rtcp,
                     ByteView packet) {
        if (kind != media_kind) {
            return MediaResult::failure(
                "media kind does not match transport");
        }
        if (!packet.data || packet.size == 0 || packet.size > 65535) {
            return MediaResult::failure(
                is_rtcp ? "invalid RTCP datagram" :
                          "invalid RTP datagram");
        }

        std::lock_guard<std::mutex> io_lock(io_mutex_);
        SendSlot* selected = nullptr;
        pj_ioqueue_key_t* key = nullptr;
        pj_sockaddr remote{};
        int remote_length = 0;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            if (closing_ || !key_ || !remote_configured_) {
                return MediaResult::failure(
                    "PJ media transport is not ready");
            }
            key = key_;
            remote = remote_;
            remote_length = remote_length_;
            for (const auto& slot : send_slots_) {
                if (!slot->busy) {
                    selected = slot.get();
                    break;
                }
            }
            if (!selected) {
                return MediaResult::failure(
                    "PJ media transport send queue is full");
            }
            selected->busy = true;
            selected->packet.assign(
                packet.data, packet.data + packet.size);
        }

        pj_ssize_t size =
            static_cast<pj_ssize_t>(selected->packet.size());
        const auto status = pj_ioqueue_sendto(
            key, &selected->op_key, selected->packet.data(),
            &size, 0, &remote, remote_length);
        if (status != PJ_EPENDING) {
            std::lock_guard<std::mutex> lock(state_mutex_);
            selected->busy = false;
        }
        if (status == PJ_SUCCESS || status == PJ_EPENDING) {
            return MediaResult::success();
        }
        return MediaResult::failure(
            "failed to send PJ media datagram: " +
            pj_error(status));
    }

    MediaResult arm_receive(ReceiveSlot& slot) {
        pj_ioqueue_key_t* key = nullptr;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            if (closing_ || !key_) {
                return MediaResult::success();
            }
            key = key_;
        }

        pj_ioqueue_op_key_init(
            &slot.op_key, sizeof(slot.op_key));
        pj_ssize_t size =
            static_cast<pj_ssize_t>(slot.packet.size());
        slot.source_length = sizeof(slot.source);
        const auto status = pj_ioqueue_recvfrom(
            key, &slot.op_key, slot.packet.data(), &size,
            PJ_IOQUEUE_ALWAYS_ASYNC,
            &slot.source, &slot.source_length);
        if (status == PJ_EPENDING) {
            return MediaResult::success();
        }
        return MediaResult::failure(
            "failed to arm PJ UDP receive: " +
            pj_error(status));
    }

    static void on_read_complete(
        pj_ioqueue_key_t* key,
        pj_ioqueue_op_key_t* op_key,
        pj_ssize_t bytes_read) {
        auto* transport = static_cast<Impl*>(
            pj_ioqueue_get_user_data(key));
        if (!transport) {
            return;
        }

        ReceiveSlot* slot = nullptr;
        for (const auto& candidate : transport->receive_slots_) {
            if (&candidate->op_key == op_key) {
                slot = candidate.get();
                break;
            }
        }
        if (!slot) {
            return;
        }

        PacketHandler handler;
        bool should_rearm = false;
        {
            std::lock_guard<std::mutex> lock(
                transport->state_mutex_);
            handler = transport->handler_;
            should_rearm = !transport->closing_ && bytes_read >= 0;
        }

        if (handler && bytes_read > 0) {
            const auto second = static_cast<std::uint8_t>(
                bytes_read > 1 ? slot->packet[1] : 0);
            handler(
                transport->media_kind,
                is_rtcp_payload(second),
                ByteView{
                    slot->packet.data(),
                    static_cast<std::size_t>(bytes_read)});
        }

        if (should_rearm) {
            transport->arm_receive(*slot);
        }
    }

    static void on_write_complete(
        pj_ioqueue_key_t* key,
        pj_ioqueue_op_key_t* op_key,
        pj_ssize_t) {
        auto* transport = static_cast<Impl*>(
            pj_ioqueue_get_user_data(key));
        if (!transport) {
            return;
        }
        std::lock_guard<std::mutex> lock(transport->state_mutex_);
        for (const auto& slot : transport->send_slots_) {
            if (&slot->op_key == op_key) {
                slot->busy = false;
                break;
            }
        }
    }

    static std::string discover_local_address() {
        pj_sockaddr address{};
        if (pj_gethostip(pj_AF_INET(), &address) != PJ_SUCCESS) {
            return "127.0.0.1";
        }
        std::array<char, PJ_INET6_ADDRSTRLEN> text{};
        pj_sockaddr_print(
            &address, text.data(), static_cast<int>(text.size()), 0);
        return text.data();
    }

    void cleanup_socket() noexcept {
        if (socket_ != PJ_INVALID_SOCKET) {
            pj_sock_close(socket_);
            socket_ = PJ_INVALID_SOCKET;
        }
    }

    void cleanup_pool() noexcept {
        if (pool_) {
            pj_pool_release(pool_);
            pool_ = nullptr;
        }
    }

    const MediaKind media_kind;
    PjIoqueueService& service_;
    mutable std::mutex state_mutex_;
    std::mutex io_mutex_;
    PacketHandler handler_;
    TransportDescription description_;
    pj_pool_t* pool_ = nullptr;
    pj_sock_t socket_ = PJ_INVALID_SOCKET;
    pj_grp_lock_t* group_lock_ = nullptr;
    pj_ioqueue_key_t* key_ = nullptr;
    pj_sockaddr remote_{};
    int remote_length_ = 0;
    bool remote_configured_ = false;
    bool closing_ = true;
    std::vector<std::unique_ptr<ReceiveSlot>> receive_slots_;
    std::vector<std::unique_ptr<SendSlot>> send_slots_;
};

PjIoqueueUdpTransport::PjIoqueueUdpTransport(
    MediaKind media_kind,
    PjIoqueueService& service)
    : impl_(std::make_unique<Impl>(media_kind, service)) {
}

PjIoqueueUdpTransport::~PjIoqueueUdpTransport() {
    close();
}

MediaResult PjIoqueueUdpTransport::open(
    const TransportConfig& config) {
    return impl_->open(config);
}

void PjIoqueueUdpTransport::close() noexcept {
    impl_->close();
}

TransportDescription PjIoqueueUdpTransport::description() const {
    return impl_->description();
}

void PjIoqueueUdpTransport::setPacketHandler(
    PacketHandler handler) {
    impl_->set_handler(std::move(handler));
}

MediaResult PjIoqueueUdpTransport::sendRtp(
    MediaKind kind,
    ByteView packet) {
    return impl_->send(kind, false, packet);
}

MediaResult PjIoqueueUdpTransport::sendRtcp(
    MediaKind kind,
    ByteView packet) {
    return impl_->send(kind, true, packet);
}

MediaResult PjIoqueueUdpTransport::setRemote(
    const std::string& address,
    std::uint16_t port) {
    return impl_->set_remote(address, port);
}

} // namespace iris::media
