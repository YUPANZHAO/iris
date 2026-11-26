#include "transport/udp_transport.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
using NativeSocket = SOCKET;
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#endif

#include <atomic>
#include <cstddef>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace iris::media::transport {
namespace {

constexpr std::size_t kMaximumDatagramSize = 65507;

NativeSocket invalidSocket() noexcept {
#ifdef _WIN32
    return INVALID_SOCKET;
#else
    return -1;
#endif
}

bool validSocket(NativeSocket socket) noexcept {
#ifdef _WIN32
    return socket != INVALID_SOCKET;
#else
    return socket >= 0;
#endif
}

void closeSocket(NativeSocket socket) noexcept {
#ifdef _WIN32
    if (socket != INVALID_SOCKET) {
        closesocket(socket);
    }
#else
    if (socket >= 0) {
        ::close(socket);
    }
#endif
}

std::string socketError(int error) {
#ifdef _WIN32
    return "winsock error " + std::to_string(error);
#else
    return std::strerror(error);
#endif
}

int lastSocketError() noexcept {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

bool resolveIPv4(const std::string& address,
                 std::uint16_t port,
                 sockaddr_in* result) {
    if (!result) {
        return false;
    }

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;

    const auto& host =
        address.empty() || address == "*" ? "0.0.0.0" : address;
    addrinfo* resolved = nullptr;
    const auto code = getaddrinfo(
        host.c_str(), nullptr, &hints, &resolved);
    if (code != 0 || !resolved) {
        return false;
    }

    std::memcpy(result, resolved->ai_addr, sizeof(sockaddr_in));
    result->sin_port = htons(port);
    freeaddrinfo(resolved);
    return true;
}

std::string option(const AttributeMap& values,
                   const std::string& key,
                   const std::string& fallback = {}) {
    const auto iterator = values.find(key);
    return iterator == values.end() ? fallback : iterator->second;
}

std::optional<int> portOption(const AttributeMap& values,
                              const std::string& key) {
    const auto iterator = values.find(key);
    if (iterator == values.end()) {
        return std::nullopt;
    }
    try {
        std::size_t consumed = 0;
        const auto value =
            std::stoi(iterator->second, &consumed);
        if (consumed != iterator->second.size()) {
            return -1;
        }
        if (value < 0 || value > 65535) {
            return -1;
        }
        return value;
    } catch (...) {
        return -1;
    }
}

struct Endpoint {
    std::string address;
    std::uint16_t port = 0;
};

std::optional<Endpoint> parseEndpoint(const std::string& value) {
    const auto separator = value.rfind(':');
    if (separator == std::string::npos) {
        return std::nullopt;
    }
    try {
        const auto portText = value.substr(separator + 1);
        std::size_t consumed = 0;
        const auto port = std::stoi(portText, &consumed);
        if (consumed != portText.size()) {
            return std::nullopt;
        }
        if (port <= 0 || port > 65535) {
            return std::nullopt;
        }
        auto address = value.substr(0, separator);
        if (address.size() >= 2 &&
            address.front() == '[' && address.back() == ']') {
            address = address.substr(1, address.size() - 2);
        }
        if (address.empty()) {
            return std::nullopt;
        }
        return Endpoint{
            std::move(address), static_cast<std::uint16_t>(port)};
    } catch (...) {
        return std::nullopt;
    }
}

} // namespace

struct UdpTransport::Impl
    : public std::enable_shared_from_this<UdpTransport::Impl> {
    ~Impl() {
        closeNetwork();
    }

    Status configure(const AttributeMap& options) {
        setState(TransportState::Configuring, {});
        emitState(TransportState::Configuring, {});

        const auto address = option(
            options,
            "localAddress",
            option(options, "bindAddress", "0.0.0.0"));
        auto port = portOption(options, "localPort");
        if (!port) {
            port = portOption(options, "port");
        }
        if (port && *port < 0) {
            return fail("invalid UDP local port");
        }

#ifdef _WIN32
        static const int winsockResult = [] {
            WSADATA data{};
            return WSAStartup(MAKEWORD(2, 2), &data);
        }();
        if (winsockResult != 0) {
            return fail("failed to initialize Winsock");
        }
#endif

        sockaddr_in local{};
        if (!resolveIPv4(
                address,
                static_cast<std::uint16_t>(port.value_or(0)),
                &local)) {
            return fail("invalid UDP local address: " + address);
        }

        closeNetwork();
        {
            std::lock_guard<std::mutex> lock(mutex);
            remote = {};
            hasRemote = false;
        }
        const auto nextSocket =
            ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (!validSocket(nextSocket)) {
            return fail(
                "failed to create UDP socket: " +
                socketError(lastSocketError()));
        }
        if (::bind(
                nextSocket,
                reinterpret_cast<const sockaddr*>(&local),
                sizeof(local)) != 0) {
            const auto message = socketError(lastSocketError());
            closeSocket(nextSocket);
            return fail("failed to bind UDP socket: " + message);
        }

#ifdef _WIN32
        DWORD timeout = 100;
        setsockopt(
            nextSocket,
            SOL_SOCKET,
            SO_RCVTIMEO,
            reinterpret_cast<const char*>(&timeout),
            sizeof(timeout));
#else
        timeval timeout{};
        timeout.tv_usec = 100000;
        setsockopt(
            nextSocket,
            SOL_SOCKET,
            SO_RCVTIMEO,
            &timeout,
            sizeof(timeout));
#endif

        sockaddr_in bound{};
#ifdef _WIN32
        int boundSize = sizeof(bound);
#else
        socklen_t boundSize = sizeof(bound);
#endif
        if (getsockname(
                nextSocket,
                reinterpret_cast<sockaddr*>(&bound),
                &boundSize) != 0) {
            const auto message = socketError(lastSocketError());
            closeSocket(nextSocket);
            return fail("failed to inspect UDP socket: " + message);
        }

        {
            std::lock_guard<std::mutex> lock(mutex);
            socket = nextSocket;
            remote = {};
            hasRemote = false;
            status = {};
            state = TransportState::Configured;
        }
        emitState(TransportState::Configured, {});
        return {};
    }

    Status attach(const AttributeMap& options) {
        setState(TransportState::Attaching, {});
        emitState(TransportState::Attaching, {});

        sockaddr_in nextRemote{};
        bool nextHasRemote = false;
        const auto endpointValue = option(options, "remoteEndpoint");
        if (!endpointValue.empty()) {
            const auto endpoint = parseEndpoint(endpointValue);
            if (!endpoint ||
                !resolveIPv4(
                    endpoint->address, endpoint->port, &nextRemote)) {
                return fail("invalid UDP remote endpoint");
            }
            nextHasRemote = true;
        } else {
            const auto address = option(options, "remoteAddress");
            auto port = portOption(options, "remotePort");
            if (port && *port < 0) {
                return fail("invalid UDP remote port");
            }
            const bool hasPort = port.has_value();
            if (address.empty() == hasPort) {
                return fail(
                    "UDP remote address and port must be set together");
            }
            if (!address.empty()) {
                if (*port == 0) {
                    return fail("invalid UDP remote port");
                }
                if (!resolveIPv4(
                        address,
                        static_cast<std::uint16_t>(*port),
                        &nextRemote)) {
                    return fail(
                        "invalid UDP remote address: " + address);
                }
                nextHasRemote = true;
            }
        }

        bool configured = false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            configured = validSocket(socket);
            if (configured) {
                remote = nextRemote;
                hasRemote = nextHasRemote;
                status = {};
                state = TransportState::Connected;
            }
        }
        if (!configured) {
            return fail("UDP transport is not configured");
        }
        startReceive();
        const auto startStatus = statusValue();
        if (!startStatus.ok) {
            return startStatus;
        }
        emitState(TransportState::Connected, {});
        return {};
    }

    void detach() noexcept {
        setState(TransportState::Detaching, {});
        emitState(TransportState::Detaching, {});

        closeNetwork();
        {
            std::lock_guard<std::mutex> lock(mutex);
            remote = {};
            hasRemote = false;
            status = {};
            state = TransportState::Disconnected;
        }
        emitState(TransportState::Disconnected, {});
    }

    Status send(const MediaBuffer& data) {
        if (!data || !*data || !data->data() || data->size() == 0) {
            return Status::failure("UDP message is empty");
        }
        if (data->size() > kMaximumDatagramSize) {
            return Status::failure("UDP message exceeds datagram limit");
        }

        sockaddr_in destination{};
        NativeSocket current = invalidSocket();
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!status.ok) {
                return status;
            }
            if (state != TransportState::Connected) {
                return Status::failure("UDP transport is not connected");
            }
            if (!hasRemote) {
                return Status::failure(
                    "UDP remote endpoint is not configured");
            }
            destination = remote;
            current = socket;
        }
        if (!validSocket(current)) {
            return Status::failure("UDP socket is closed");
        }

        const auto sent = ::sendto(
            current,
            static_cast<const char*>(data->data()),
            static_cast<int>(data->size()),
            0,
            reinterpret_cast<const sockaddr*>(&destination),
            sizeof(destination));
        if (sent < 0 ||
            static_cast<std::size_t>(sent) != data->size()) {
            const auto result = Status::failure(
                "UDP send failed: " +
                socketError(lastSocketError()));
            setState(TransportState::Failed, result);
            emitState(TransportState::Failed, result);
            return result;
        }
        return {};
    }

    void setSink(std::weak_ptr<ITransportSink> value) noexcept {
        std::lock_guard<std::mutex> lock(mutex);
        sink = std::move(value);
    }

    Status statusValue() const {
        std::lock_guard<std::mutex> lock(mutex);
        return status;
    }

    void startReceive() {
        if (worker.joinable()) {
            return;
        }
        running.store(true, std::memory_order_release);
        try {
            worker = std::thread([self = shared_from_this()] {
                self->receiveLoop();
            });
        } catch (...) {
            running.store(false, std::memory_order_release);
            fail("failed to start UDP receive thread");
        }
    }

    void closeNetwork() noexcept {
        NativeSocket current = invalidSocket();
        {
            std::lock_guard<std::mutex> lock(mutex);
            running.store(false, std::memory_order_release);
            current = socket;
            socket = invalidSocket();
        }
        closeSocket(current);

        if (!worker.joinable()) {
            return;
        }
        if (worker.get_id() == std::this_thread::get_id()) {
            worker.detach();
        } else {
            worker.join();
        }
    }

    void receiveLoop() noexcept {
        std::vector<std::uint8_t> bytes(65536);
        while (running.load(std::memory_order_acquire)) {
            NativeSocket current = invalidSocket();
            {
                std::lock_guard<std::mutex> lock(mutex);
                current = socket;
            }
            if (!validSocket(current)) {
                break;
            }

            sockaddr_in source{};
#ifdef _WIN32
            int sourceSize = sizeof(source);
#else
            socklen_t sourceSize = sizeof(source);
#endif
            const auto received = ::recvfrom(
                current,
                reinterpret_cast<char*>(bytes.data()),
                static_cast<int>(bytes.size()),
                0,
                reinterpret_cast<sockaddr*>(&source),
                &sourceSize);
            if (received <= 0) {
                continue;
            }

            auto data = std::make_shared<ByteBuffer>(
                bytes.data(), static_cast<std::size_t>(received));
            if (!data || !*data) {
                continue;
            }

            std::shared_ptr<ITransportSink> target;
            {
                std::lock_guard<std::mutex> lock(mutex);
                target = sink.lock();
            }
            if (!target) {
                continue;
            }
            try {
                target->onTransportData(std::move(data));
            } catch (...) {
                // A sink failure must not terminate the receive thread.
            }
        }
    }

    Status fail(std::string message) {
        const auto result = Status::failure(std::move(message));
        setState(TransportState::Failed, result);
        emitState(TransportState::Failed, result);
        return result;
    }

    void setState(TransportState value, const Status& valueStatus) {
        std::lock_guard<std::mutex> lock(mutex);
        state = value;
        status = valueStatus;
    }

    void emitState(TransportState value, const Status& valueStatus) noexcept {
        std::shared_ptr<ITransportSink> target;
        {
            std::lock_guard<std::mutex> lock(mutex);
            target = sink.lock();
        }
        if (!target) {
            return;
        }
        try {
            target->onTransportState(TransportEvent{value, valueStatus});
        } catch (...) {
            // Events must not alter transport lifecycle progress.
        }
    }

    mutable std::mutex mutex;
    TransportState state = TransportState::Idle;
    Status status;
    NativeSocket socket = invalidSocket();
    sockaddr_in remote{};
    bool hasRemote = false;
    std::atomic<bool> running{false};
    std::thread worker;
    std::weak_ptr<ITransportSink> sink;
};

UdpTransport::UdpTransport()
    : impl_(std::make_shared<Impl>()) {
}

UdpTransport::~UdpTransport() {
    impl_->closeNetwork();
}

Status UdpTransport::send(MediaBuffer data) {
    return impl_->send(data);
}

void UdpTransport::setSink(
    std::weak_ptr<ITransportSink> sink) noexcept {
    impl_->setSink(std::move(sink));
}

Status UdpTransport::configure(const AttributeMap& options) {
    return impl_->configure(options);
}

Status UdpTransport::attach(const AttributeMap& options) {
    return impl_->attach(options);
}

void UdpTransport::detach() noexcept {
    impl_->detach();
}

} // namespace iris::media::transport
