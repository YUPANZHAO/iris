#include "media/transport/rtp_transport.h"

#include "media/engine/media_buffer.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
using NativeSocket = SOCKET;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using NativeSocket = int;
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace iris::media::v2 {
namespace {

constexpr std::uint8_t kDefaultPayloadType = 96;
constexpr int kDefaultClockRate = 90000;
constexpr std::size_t kRtpHeaderSize = 12;
constexpr std::size_t kMaximumDatagramSize = 65507;

struct EndpointParts {
    std::string host;
    std::uint16_t port = 0;
};

std::optional<EndpointParts> parseEndpoint(const std::string& value) {
    if (value.empty()) {
        return std::nullopt;
    }
    const auto separator = value.rfind(':');
    if (separator == std::string::npos) {
        return std::nullopt;
    }
    EndpointParts result;
    result.host = value.substr(0, separator);
    if (result.host.size() >= 2 &&
        result.host.front() == '[' && result.host.back() == ']') {
        result.host = result.host.substr(1, result.host.size() - 2);
    }
    try {
        const auto port = std::stoul(value.substr(separator + 1));
        if (port == 0 || port > 65535) {
            return std::nullopt;
        }
        result.port = static_cast<std::uint16_t>(port);
        return result;
    } catch (...) {
        return std::nullopt;
    }
}

bool parseRemoteEndpoint(const std::string& value,
                         sockaddr_in* result) {
    if (!result) {
        return false;
    }
    const auto endpoint = parseEndpoint(value);
    if (!endpoint) {
        return false;
    }
    sockaddr_in parsed{};
    parsed.sin_family = AF_INET;
    parsed.sin_port = htons(endpoint->port);
    if (inet_pton(
            AF_INET, endpoint->host.c_str(), &parsed.sin_addr) != 1) {
        return false;
    }
    *result = parsed;
    return true;
}

std::string option(const AttributeMap& values,
                   const std::string& key,
                   const std::string& fallback = {}) {
    const auto iterator = values.find(key);
    return iterator == values.end() ? fallback : iterator->second;
}

int intOption(const AttributeMap& values,
              const std::string& key,
              int fallback) {
    const auto iterator = values.find(key);
    if (iterator == values.end()) {
        return fallback;
    }
    try {
        return std::stoi(iterator->second);
    } catch (...) {
        return fallback;
    }
}

std::int64_t rtpTimeToMicros(std::uint32_t timestamp,
                             int clockRate) noexcept {
    if (clockRate <= 0) {
        return 0;
    }
    return static_cast<std::int64_t>(timestamp) *
        1000000LL / clockRate;
}

std::uint32_t microsToRtpTime(std::int64_t micros,
                              int clockRate) noexcept {
    if (clockRate <= 0 || micros < 0) {
        return 0;
    }
    return static_cast<std::uint32_t>(
        micros * clockRate / 1000000LL);
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

bool validSocket(NativeSocket socket) noexcept {
#ifdef _WIN32
    return socket != INVALID_SOCKET;
#else
    return socket >= 0;
#endif
}

NativeSocket invalidSocket() noexcept {
#ifdef _WIN32
    return INVALID_SOCKET;
#else
    return -1;
#endif
}

std::string socketError() {
#ifdef _WIN32
    return "winsock error " + std::to_string(WSAGetLastError());
#else
    return std::strerror(errno);
#endif
}

struct ParsedRtp {
    std::uint8_t payloadType = 0;
    std::uint16_t sequence = 0;
    std::uint32_t timestamp = 0;
    std::uint32_t ssrc = 0;
    bool marker = false;
    const std::uint8_t* payload = nullptr;
    std::size_t payloadSize = 0;
};

struct ParsedRtcp {
    std::uint32_t packetCount = 0;
};

bool parseRtp(const std::uint8_t* data,
              std::size_t size,
              ParsedRtp* result) {
    if (!data || size < kRtpHeaderSize || (data[0] >> 6) != 2) {
        return false;
    }
    const std::size_t csrcCount = data[0] & 0x0f;
    std::size_t offset = kRtpHeaderSize + csrcCount * 4;
    if (offset > size) {
        return false;
    }
    if ((data[0] & 0x10) != 0) {
        if (offset + 4 > size) {
            return false;
        }
        const auto extensionWords =
            (static_cast<std::size_t>(data[offset + 2]) << 8) |
            data[offset + 3];
        offset += 4 + extensionWords * 4;
        if (offset > size) {
            return false;
        }
    }
    std::size_t payloadEnd = size;
    if ((data[0] & 0x20) != 0) {
        const auto padding = data[size - 1];
        if (padding == 0 || padding > size - offset) {
            return false;
        }
        payloadEnd -= padding;
    }
    result->payloadType = data[1] & 0x7f;
    result->marker = (data[1] & 0x80) != 0;
    result->sequence =
        static_cast<std::uint16_t>((data[2] << 8) | data[3]);
    result->timestamp =
        (static_cast<std::uint32_t>(data[4]) << 24) |
        (static_cast<std::uint32_t>(data[5]) << 16) |
        (static_cast<std::uint32_t>(data[6]) << 8) |
        data[7];
    result->ssrc =
        (static_cast<std::uint32_t>(data[8]) << 24) |
        (static_cast<std::uint32_t>(data[9]) << 16) |
        (static_cast<std::uint32_t>(data[10]) << 8) |
        data[11];
    result->payload = data + offset;
    result->payloadSize = payloadEnd - offset;
    return true;
}

bool parseRtcp(const std::uint8_t* data,
               std::size_t size,
               ParsedRtcp* result) {
    if (!data || size < 4 || (data[0] >> 6) != 2) {
        return false;
    }
    std::size_t offset = 0;
    ParsedRtcp parsed;
    while (offset < size) {
        if (size - offset < 4 || (data[offset] >> 6) != 2) {
            return false;
        }
        const auto packetType = data[offset + 1];
        if (packetType < 192 || packetType > 223) {
            return false;
        }
        const auto lengthWords =
            (static_cast<std::size_t>(data[offset + 2]) << 8) |
            data[offset + 3];
        const auto packetSize = (lengthWords + 1) * 4;
        if (packetSize < 4 || packetSize > size - offset) {
            return false;
        }
        offset += packetSize;
        parsed.packetCount += 1;
    }
    if (offset != size || parsed.packetCount == 0) {
        return false;
    }
    if (result) {
        *result = parsed;
    }
    return true;
}

struct RtpStreamRuntime {
    StreamId id = 0;
    std::uint8_t payloadType = 0;
    int clockRate = 0;
    std::uint16_t sequenceNumber = 0;
    std::uint32_t ssrc = 0;
};

} // namespace

struct RTPTransportState {
    mutable std::mutex mutex;
    std::mutex streamMutex;
    std::mutex socketMutex;
    std::thread receiveThread;
    std::atomic<bool> running{false};
    NativeSocket socket = invalidSocket();
    sockaddr_in remote{};
    bool remoteConfigured = false;
    AttributeMap configOptions;
    TransportInfo currentInfo;
    TransportStats currentStats;
    std::weak_ptr<IEncodedPacketSink> packetSink;
    std::map<StreamId, RtpStreamRuntime> streams;
    std::map<std::uint8_t, StreamId> streamsByPayloadType;

    ~RTPTransportState() {
        close();
    }

    RtpStreamRuntime& ensureStream(StreamId id) {
        auto iterator = streams.find(id);
        if (iterator != streams.end()) {
            return iterator->second;
        }
        RtpStreamRuntime state;
        state.id = id == 0 ? 1 : id;
        state.payloadType = kDefaultPayloadType;
        state.clockRate = kDefaultClockRate;
        static std::atomic<std::uint32_t> nextSsrc{0x51000000};
        state.ssrc = nextSsrc.fetch_add(1, std::memory_order_relaxed);

        const auto prefix = "stream." + std::to_string(state.id) + ".";
        state.payloadType = static_cast<std::uint8_t>(std::clamp(
            intOption(configOptions, prefix + "payloadType",
                      state.payloadType),
            0, 127));
        state.clockRate = std::max(
            1, intOption(configOptions, prefix + "clockRate",
                         state.clockRate));
        streamsByPayloadType[state.payloadType] = state.id;
        return streams.emplace(state.id, std::move(state)).first->second;
    }

    Status emitRtp(RtpStreamRuntime& stream,
                   std::uint32_t timestamp,
                   bool marker,
                   const std::uint8_t* payload,
                   std::size_t payloadSize) {
        if (!payload || payloadSize == 0 ||
            payloadSize > kMaximumDatagramSize - kRtpHeaderSize) {
            return Status{false, 1, "invalid RTP payload"};
        }
        std::vector<std::uint8_t> packet(kRtpHeaderSize + payloadSize);
        packet[0] = 0x80;
        packet[1] = static_cast<std::uint8_t>(
            (marker ? 0x80 : 0) | stream.payloadType);
        packet[2] = static_cast<std::uint8_t>(
            stream.sequenceNumber >> 8);
        packet[3] = static_cast<std::uint8_t>(
            stream.sequenceNumber);
        packet[4] = static_cast<std::uint8_t>(timestamp >> 24);
        packet[5] = static_cast<std::uint8_t>(timestamp >> 16);
        packet[6] = static_cast<std::uint8_t>(timestamp >> 8);
        packet[7] = static_cast<std::uint8_t>(timestamp);
        packet[8] = static_cast<std::uint8_t>(stream.ssrc >> 24);
        packet[9] = static_cast<std::uint8_t>(stream.ssrc >> 16);
        packet[10] = static_cast<std::uint8_t>(stream.ssrc >> 8);
        packet[11] = static_cast<std::uint8_t>(stream.ssrc);
        std::memcpy(
            packet.data() + kRtpHeaderSize, payload, payloadSize);
        stream.sequenceNumber =
            static_cast<std::uint16_t>(stream.sequenceNumber + 1);
        auto buffer = copyToMediaBuffer(packet.data(), packet.size());
        if (!buffer.buffer) {
            return Status{false, 1, "failed to allocate RTP datagram"};
        }
        auto result = rawSend(std::move(buffer));
        if (result.ok) {
            std::lock_guard<std::mutex> lock(mutex);
            currentStats.rtpPacketsSent += 1;
        }
        return result;
    }

    void applyStreamOptionsLocked() {
        streamsByPayloadType.clear();
        std::unordered_set<StreamId> configuredStreams;
        const std::string prefix = "stream.";
        for (const auto& entry : configOptions) {
            if (entry.first.rfind(prefix, 0) != 0) {
                continue;
            }
            const auto separator = entry.first.find(
                '.', prefix.size());
            if (separator == std::string::npos) {
                continue;
            }
            try {
                configuredStreams.insert(
                    static_cast<StreamId>(std::stoull(
                        entry.first.substr(
                            prefix.size(),
                            separator - prefix.size()))));
            } catch (...) {
            }
        }
        for (auto iterator = streams.begin();
             iterator != streams.end();) {
            if (configuredStreams.find(iterator->first) ==
                configuredStreams.end()) {
                iterator = streams.erase(iterator);
            } else {
                ++iterator;
            }
        }
        for (const auto id : configuredStreams) {
            auto& stream = ensureStream(id);
            const auto streamPrefix =
                "stream." + std::to_string(stream.id) + ".";
            stream.payloadType = static_cast<std::uint8_t>(std::clamp(
                intOption(configOptions,
                          streamPrefix + "payloadType",
                          stream.payloadType),
                0, 127));
            stream.clockRate = std::max(
                1, intOption(configOptions,
                             streamPrefix + "clockRate",
                             stream.clockRate));
            streamsByPayloadType[stream.payloadType] = stream.id;
        }
    }

    Status configure(const AttributeMap& options) {
        close();
#ifdef _WIN32
        static const bool winsockReady = [] {
            WSADATA data{};
            return WSAStartup(MAKEWORD(2, 2), &data) == 0;
        }();
        if (!winsockReady) {
            return Status{false, 1, "failed to initialize Winsock"};
        }
#endif
        std::scoped_lock lock(streamMutex, mutex);
        if (options.find("listen") != options.end() &&
            options.at("listen") == "false") {
            return Status{false, 1, "RTP transport requires a listen socket"};
        }
        configOptions = options;
        streams.clear();
        streamsByPayloadType.clear();
        remote = {};
        remoteConfigured = false;
        currentInfo = {};
        currentStats = {};

        const auto bindAddress =
            option(options, "bindAddress", "0.0.0.0");
        const auto port = intOption(options, "port", 0);
        socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (!validSocket(socket)) {
            return Status{false, 1, "failed to create RTP socket: " +
                socketError()};
        }
        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_port = htons(static_cast<std::uint16_t>(port));
        if (bindAddress.empty() || bindAddress == "auto") {
            local.sin_addr.s_addr = INADDR_ANY;
        } else if (inet_pton(AF_INET, bindAddress.c_str(), &local.sin_addr) != 1) {
            closeSocket(socket);
            socket = invalidSocket();
            return Status{false, 1, "invalid RTP bind address"};
        }
        if (::bind(
                socket, reinterpret_cast<sockaddr*>(&local),
                sizeof(local)) != 0) {
            const auto message = socketError();
            closeSocket(socket);
            socket = invalidSocket();
            return Status{false, 1, "failed to bind RTP socket: " + message};
        }

#ifdef _WIN32
        DWORD timeout = 100;
        setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<const char*>(&timeout),
                   sizeof(timeout));
#else
        timeval timeout{};
        timeout.tv_usec = 100000;
        setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO,
                   &timeout, sizeof(timeout));
#endif

        sockaddr_in bound{};
#ifdef _WIN32
        int boundLength = sizeof(bound);
#else
        socklen_t boundLength = sizeof(bound);
#endif
        getsockname(socket, reinterpret_cast<sockaddr*>(&bound),
                    &boundLength);
        std::array<char, INET_ADDRSTRLEN> address{};
        inet_ntop(AF_INET, &bound.sin_addr, address.data(), address.size());
        std::string advertised = address.data();
        if (advertised == "0.0.0.0") {
            advertised = "127.0.0.1";
        }
        currentInfo.state = TransportState::Configured;
        currentInfo.endpoint =
            advertised + ":" +
            std::to_string(ntohs(bound.sin_port));
        return {};
    }

    Status open(const AttributeMap& options) {
        std::scoped_lock lock(streamMutex, mutex);
        if (!validSocket(socket) ||
            (currentInfo.state != TransportState::Configured &&
             currentInfo.state != TransportState::Open)) {
            return Status{false, 1, "RTP transport is not configured"};
        }
        auto nextOptions = configOptions;
        for (const auto& entry : options) {
            nextOptions[entry.first] = entry.second;
        }
        sockaddr_in nextRemote{};
        const auto remoteValue =
            option(nextOptions, "remoteEndpoint");
        const bool nextRemoteConfigured = !remoteValue.empty();
        if (nextRemoteConfigured &&
            !parseRemoteEndpoint(remoteValue, &nextRemote)) {
            return Status{false, 1, "invalid RTP remote endpoint"};
        }
        configOptions = std::move(nextOptions);
        remote = nextRemote;
        remoteConfigured = nextRemoteConfigured;
        applyStreamOptionsLocked();
        if (currentInfo.state == TransportState::Open) {
            return {};
        }
        currentInfo.state = TransportState::Open;
        running.store(true, std::memory_order_release);
        receiveThread = std::thread([this] { receiveLoop(); });
        return {};
    }

    void close() noexcept {
        {
            std::lock_guard<std::mutex> lock(mutex);
            running.store(false, std::memory_order_release);
            currentInfo.state = TransportState::Closed;
            packetSink.reset();
        }
        {
            std::lock_guard<std::mutex> lock(socketMutex);
            if (validSocket(socket)) {
                closeSocket(socket);
                socket = invalidSocket();
            }
        }
        if (receiveThread.joinable() &&
            receiveThread.get_id() != std::this_thread::get_id()) {
            receiveThread.join();
        } else if (receiveThread.joinable()) {
            receiveThread.detach();
        }
    }

    Status rawSend(MediaBufferView data) {
        const auto* bytes = bufferViewData(data);
        const auto size = bufferViewSize(data);
        if (!bytes || size == 0) {
            return Status{false, 1, "empty RTP datagram"};
        }
        sockaddr_in destination{};
        NativeSocket currentSocket = invalidSocket();
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!remoteConfigured ||
                currentInfo.state != TransportState::Open) {
                currentStats.sendErrors += 1;
                return Status{false, 1, "RTP remote endpoint is not configured"};
            }
            destination = remote;
            currentSocket = socket;
        }
        int sent = -1;
        {
            std::lock_guard<std::mutex> lock(socketMutex);
            if (!validSocket(currentSocket)) {
                std::lock_guard<std::mutex> stateLock(mutex);
                currentStats.sendErrors += 1;
                return Status{false, 1, "RTP socket is closed"};
            }
            sent = ::sendto(
                currentSocket, reinterpret_cast<const char*>(bytes),
                static_cast<int>(size), 0,
                reinterpret_cast<const sockaddr*>(&destination),
                sizeof(destination));
        }
        std::lock_guard<std::mutex> lock(mutex);
        if (sent < 0 || static_cast<std::size_t>(sent) != size) {
            currentStats.sendErrors += 1;
            return Status{false, 1, "RTP send failed: " + socketError()};
        }
        currentStats.messagesSent += 1;
        currentStats.bytesSent += size;
        return {};
    }

    void receiveLoop() {
        std::vector<std::uint8_t> packet(65536);
        while (running.load(std::memory_order_acquire)) {
            sockaddr_in source{};
#ifdef _WIN32
            int sourceLength = sizeof(source);
#else
            socklen_t sourceLength = sizeof(source);
#endif
            const auto received = ::recvfrom(
                socket, reinterpret_cast<char*>(packet.data()),
                static_cast<int>(packet.size()), 0,
                reinterpret_cast<sockaddr*>(&source), &sourceLength);
            if (received <= 0) {
                if (!running.load(std::memory_order_acquire)) {
                    break;
                }
                continue;
            }
            const auto size = static_cast<std::size_t>(received);
            TransportDatagramReady(packet.data(), size);
        }
    }

    void TransportDatagramReady(const std::uint8_t* data,
                                std::size_t size) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            currentStats.messagesReceived += 1;
            currentStats.bytesReceived += size;
        }
        if (size >= 2 && data[1] >= 192 && data[1] <= 223) {
            ParsedRtcp rtcp;
            if (parseRtcp(data, size, &rtcp)) {
                std::lock_guard<std::mutex> lock(mutex);
                currentStats.rtcpPacketsReceived += rtcp.packetCount;
                return;
            }
        }
        ParsedRtp rtp;
        if (!parseRtp(data, size, &rtp) || !rtp.payload ||
            rtp.payloadSize == 0) {
            std::lock_guard<std::mutex> lock(mutex);
            currentStats.receiveErrors += 1;
            return;
        }
        EncodedPacket packet;
        {
            std::lock_guard<std::mutex> lock(streamMutex);
            auto binding = streamsByPayloadType.find(rtp.payloadType);
            RtpStreamRuntime* state = nullptr;
            if (binding != streamsByPayloadType.end()) {
                auto iterator = streams.find(binding->second);
                if (iterator != streams.end()) {
                    state = &iterator->second;
                }
            }
            if (!state) {
                StreamId id = 1;
                while (streams.find(id) != streams.end()) {
                    ++id;
                }
                state = &ensureStream(id);
                state->payloadType = rtp.payloadType;
                streamsByPayloadType[rtp.payloadType] = state->id;
            }
            packet.streamId = state->id;
            packet.data = copyToMediaBuffer(
                rtp.payload, rtp.payloadSize);
            packet.ptsMicros = rtpTimeToMicros(
                rtp.timestamp, state->clockRate);
        }
        if (!packet.data.buffer) {
            std::lock_guard<std::mutex> lock(mutex);
            currentStats.droppedMessages += 1;
            return;
        }
        {
            std::lock_guard<std::mutex> lock(mutex);
            currentStats.rtpPacketsReceived += 1;
        }
        auto sink = [&] {
            std::lock_guard<std::mutex> lock(mutex);
            return packetSink.lock();
        }();
        if (sink) {
            sink->onEncodedPacket(packet);
        }
    }
};

std::mutex gStateMutex;
std::unordered_map<const RTPTransport*, std::shared_ptr<RTPTransportState>>
    gStates;

std::shared_ptr<RTPTransportState> stateFor(
    const RTPTransport* transport) {
    std::lock_guard<std::mutex> lock(gStateMutex);
    const auto iterator = gStates.find(transport);
    return iterator == gStates.end() ? nullptr : iterator->second;
}

RTPTransport::RTPTransport() {
    std::lock_guard<std::mutex> lock(gStateMutex);
    gStates.emplace(this, std::make_shared<RTPTransportState>());
}

RTPTransport::~RTPTransport() {
    std::shared_ptr<RTPTransportState> state;
    {
        std::lock_guard<std::mutex> lock(gStateMutex);
        const auto iterator = gStates.find(this);
        if (iterator != gStates.end()) {
            state = iterator->second;
            gStates.erase(iterator);
        }
    }
    if (state) {
        state->close();
    }
}

Status RTPTransport::configure(const AttributeMap& options) {
    auto state = stateFor(this);
    return state ? state->configure(options)
                 : Status{false, 1, "RTP transport state is unavailable"};
}

Status RTPTransport::open(const AttributeMap& options) {
    auto state = stateFor(this);
    return state ? state->open(options)
                 : Status{false, 1, "RTP transport state is unavailable"};
}

void RTPTransport::close() noexcept {
    if (auto state = stateFor(this)) {
        state->close();
    }
}

Status RTPTransport::send(const EncodedPacket& packet) {
    auto state = stateFor(this);
    if (!state) {
        return Status{false, 1, "RTP transport state is unavailable"};
    }
    const auto* data = bufferViewData(packet.data);
    const auto size = bufferViewSize(packet.data);
    if (!data || size == 0) {
        return Status{false, 1, "empty encoded packet"};
    }
    std::lock_guard<std::mutex> lock(state->streamMutex);
    auto& stream = state->ensureStream(packet.streamId);
    const auto timestamp = microsToRtpTime(
        packet.ptsMicros, stream.clockRate);
    return state->emitRtp(
        stream, timestamp, true, data, size);
}

void RTPTransport::setSink(
    std::weak_ptr<IEncodedPacketSink> sink) {
    if (auto state = stateFor(this)) {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->packetSink = std::move(sink);
    }
}

Status RTPTransport::send(MediaBufferView data) {
    auto state = stateFor(this);
    return state ? state->rawSend(std::move(data))
                 : Status{false, 1, "RTP transport state is unavailable"};
}

void RTPTransport::setSink(std::weak_ptr<ITransportSink> sink) {
    (void)sink;
}

TransportInfo RTPTransport::info() const {
    if (auto state = stateFor(this)) {
        std::lock_guard<std::mutex> lock(state->mutex);
        return state->currentInfo;
    }
    return {};
}

TransportStats RTPTransport::stats() const {
    if (auto state = stateFor(this)) {
        std::lock_guard<std::mutex> lock(state->mutex);
        return state->currentStats;
    }
    return {};
}

} // namespace iris::media::v2
