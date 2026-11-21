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
#include <chrono>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace iris::media::v2 {
namespace {

constexpr std::uint8_t kDefaultOpusPayloadType = 111;
constexpr std::uint8_t kDefaultH264PayloadType = 96;
constexpr int kDefaultMtu = 1200;

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

bool equalsIgnoreCase(const std::string& value,
                      const std::string& expected) {
    if (value.size() != expected.size()) {
        return false;
    }
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (std::tolower(static_cast<unsigned char>(value[index])) !=
            std::tolower(static_cast<unsigned char>(expected[index]))) {
            return false;
        }
    }
    return true;
}

MediaKind parseKind(const std::string& value) {
    if (equalsIgnoreCase(value, "video")) {
        return MediaKind::Video;
    }
    if (equalsIgnoreCase(value, "audio")) {
        return MediaKind::Audio;
    }
    return MediaKind::Unknown;
}

CodecType parseCodec(const std::string& value) {
    if (equalsIgnoreCase(value, "opus")) {
        return CodecType::Opus;
    }
    if (equalsIgnoreCase(value, "h264")) {
        return CodecType::H264;
    }
    return CodecType::Unknown;
}

std::uint8_t defaultPayloadType(CodecType codec) noexcept {
    return codec == CodecType::H264
        ? kDefaultH264PayloadType
        : kDefaultOpusPayloadType;
}

int defaultClockRate(CodecType codec) noexcept {
    return codec == CodecType::H264 ? 90000 : 48000;
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

bool parseRtp(const std::uint8_t* data,
              std::size_t size,
              ParsedRtp* result) {
    if (!data || size < 12 || (data[0] >> 6) != 2) {
        return false;
    }
    const std::size_t csrcCount = data[0] & 0x0f;
    std::size_t offset = 12 + csrcCount * 4;
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

struct H264NalView {
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
};

void appendH264StartCode(std::vector<std::uint8_t>& output) {
    output.insert(output.end(), {0, 0, 0, 1});
}

std::vector<H264NalView> splitH264Nals(const std::uint8_t* data,
                                       std::size_t size) {
    std::vector<H264NalView> result;
    std::vector<std::size_t> starts;
    if (!data || size == 0) {
        return result;
    }
    for (std::size_t index = 0; index + 3 <= size; ++index) {
        if (index + 4 <= size &&
            data[index] == 0 && data[index + 1] == 0 &&
            data[index + 2] == 0 && data[index + 3] == 1) {
            starts.push_back(index + 4);
            index += 3;
        } else if (data[index] == 0 && data[index + 1] == 0 &&
                   data[index + 2] == 1) {
            starts.push_back(index + 3);
            index += 2;
        }
    }
    if (starts.empty()) {
        result.push_back({data, size});
        return result;
    }
    for (std::size_t index = 0; index < starts.size(); ++index) {
        const auto begin = starts[index];
        auto end = index + 1 < starts.size()
            ? starts[index + 1] - 3
            : size;
        while (end > begin && data[end - 1] == 0) {
            --end;
        }
        if (end > begin) {
            result.push_back({data + begin, end - begin});
        }
    }
    return result;
}

bool containsH264Nal(const std::vector<H264NalView>& nals,
                     std::uint8_t type) {
    return std::any_of(
        nals.begin(), nals.end(), [type](const H264NalView& nal) {
            return nal.size > 0 && (nal.data[0] & 0x1f) == type;
        });
}

struct H264ReceiveState {
    std::vector<std::uint8_t> accessUnit;
    std::optional<std::uint32_t> timestamp;
    std::optional<std::uint16_t> expectedSequence;
    bool fragmentOpen = false;
    bool damaged = false;

    void reset() {
        accessUnit.clear();
        timestamp.reset();
        expectedSequence.reset();
        fragmentOpen = false;
        damaged = false;
    }

    bool appendAggregation(const std::uint8_t* payload,
                           std::size_t size) {
        std::size_t offset = 1;
        while (offset + 2 <= size) {
            const auto length =
                (static_cast<std::size_t>(payload[offset]) << 8) |
                payload[offset + 1];
            offset += 2;
            if (length == 0 || length > size - offset) {
                return false;
            }
            const auto type = payload[offset] & 0x1f;
            if (type == 0 || type > 23) {
                return false;
            }
            appendH264StartCode(accessUnit);
            accessUnit.insert(
                accessUnit.end(), payload + offset,
                payload + offset + length);
            offset += length;
        }
        return offset > 1 && offset == size;
    }

    bool appendFragment(const std::uint8_t* payload,
                        std::size_t size) {
        if (size < 2) {
            return false;
        }
        const auto header = payload[1];
        const auto type = header & 0x1f;
        const bool start = (header & 0x80) != 0;
        const bool end = (header & 0x40) != 0;
        if (type == 0 || type > 23) {
            return false;
        }
        if (start) {
            if (end || fragmentOpen) {
                return false;
            }
            appendH264StartCode(accessUnit);
            accessUnit.push_back(
                static_cast<std::uint8_t>((payload[0] & 0xe0) | type));
        } else if (!fragmentOpen) {
            return false;
        }
        accessUnit.insert(accessUnit.end(), payload + 2, payload + size);
        fragmentOpen = !end;
        return true;
    }
};

struct RtpStreamRuntime {
    StreamId id = 0;
    MediaKind kind = MediaKind::Unknown;
    CodecType codec = CodecType::Unknown;
    std::uint8_t payloadType = 0;
    int clockRate = 0;
    std::uint16_t sequenceNumber = 0;
    std::uint32_t ssrc = 0;
    std::vector<std::uint8_t> parameterSets;
    H264ReceiveState h264;
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
    TransportConfig config;
    TransportInfo currentInfo;
    TransportStats currentStats;
    std::weak_ptr<ITransportSink> rawSink;
    std::weak_ptr<IEncodedPacketSink> packetSink;
    std::map<StreamId, RtpStreamRuntime> streams;
    std::map<std::uint8_t, StreamId> streamsByPayloadType;
    int mtu = kDefaultMtu;

    ~RTPTransportState() {
        close();
    }

    RtpStreamRuntime& ensureStream(StreamId id,
                                   MediaKind kind,
                                   CodecType codec) {
        auto iterator = streams.find(id);
        if (iterator != streams.end()) {
            if (codec != CodecType::Unknown) {
                iterator->second.codec = codec;
            }
            if (kind != MediaKind::Unknown) {
                iterator->second.kind = kind;
            }
            return iterator->second;
        }
        const auto effectiveCodec =
            codec == CodecType::Unknown ? CodecType::Opus : codec;
        RtpStreamRuntime state;
        state.id = id == 0 ? 1 : id;
        state.codec = effectiveCodec;
        state.kind = kind == MediaKind::Unknown
            ? (effectiveCodec == CodecType::H264
                   ? MediaKind::Video
                   : MediaKind::Audio)
            : kind;
        state.payloadType = defaultPayloadType(effectiveCodec);
        state.clockRate = defaultClockRate(effectiveCodec);
        static std::atomic<std::uint32_t> nextSsrc{0x51000000};
        state.ssrc = nextSsrc.fetch_add(1, std::memory_order_relaxed);

        const auto prefix = "stream." + std::to_string(state.id) + ".";
        state.payloadType = static_cast<std::uint8_t>(std::clamp(
            intOption(config.options, prefix + "payloadType",
                      state.payloadType),
            0, 127));
        state.clockRate = std::max(
            1, intOption(config.options, prefix + "clockRate",
                         state.clockRate));
        const auto configuredKind =
            parseKind(option(config.options, prefix + "kind"));
        if (configuredKind != MediaKind::Unknown) {
            state.kind = configuredKind;
        }
        const auto configuredCodec =
            parseCodec(option(config.options, prefix + "codec"));
        if (configuredCodec != CodecType::Unknown) {
            state.codec = configuredCodec;
        }
        streamsByPayloadType[state.payloadType] = state.id;
        return streams.emplace(state.id, std::move(state)).first->second;
    }

    Status emitRtp(RtpStreamRuntime& stream,
                   std::uint32_t timestamp,
                   bool marker,
                   const std::uint8_t* payload,
                   std::size_t payloadSize) {
        if (!payload || payloadSize == 0 ||
            payloadSize > 65535 - 12) {
            return Status{false, 1, "invalid RTP payload"};
        }
        std::vector<std::uint8_t> packet(12 + payloadSize);
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
            packet.data() + 12, payload, payloadSize);
        stream.sequenceNumber =
            static_cast<std::uint16_t>(stream.sequenceNumber + 1);
        auto buffer = copyToMediaBuffer(packet.data(), packet.size());
        if (!buffer.buffer) {
            return Status{false, 1, "failed to allocate RTP datagram"};
        }
        return rawSend(std::move(buffer));
    }

    Status packetizeH264(RtpStreamRuntime& stream,
                         std::uint32_t timestamp,
                         const std::uint8_t* data,
                         std::size_t size,
                         bool key) {
        auto nals = splitH264Nals(data, size);
        if (nals.empty()) {
            return Status{false, 1, "H.264 access unit has no NAL units"};
        }

        std::vector<std::uint8_t> currentParameters;
        for (const auto& nal : nals) {
            const auto type = nal.size > 0 ? nal.data[0] & 0x1f : 0;
            if (type == 7 || type == 8) {
                appendH264StartCode(currentParameters);
                currentParameters.insert(
                    currentParameters.end(), nal.data, nal.data + nal.size);
            }
        }
        if (!currentParameters.empty()) {
            stream.parameterSets = currentParameters;
        }

        std::vector<H264NalView> toSend;
        if (key && !containsH264Nal(nals, 7) &&
            !stream.parameterSets.empty()) {
            toSend = splitH264Nals(
                stream.parameterSets.data(), stream.parameterSets.size());
        }
        toSend.insert(toSend.end(), nals.begin(), nals.end());
        for (std::size_t nalIndex = 0;
             nalIndex < toSend.size();
             ++nalIndex) {
            const auto nal = toSend[nalIndex];
            const bool finalNal = nalIndex + 1 == toSend.size();
            const auto maximum = std::max<std::size_t>(3, mtu);
            if (nal.size <= maximum) {
                auto result = emitRtp(
                    stream, timestamp, finalNal,
                    nal.data, nal.size);
                if (!result.ok) {
                    return result;
                }
                continue;
            }
            const auto indicator = static_cast<std::uint8_t>(
                (nal.data[0] & 0xe0) | 28);
            std::size_t offset = 1;
            bool first = true;
            while (offset < nal.size) {
                const auto fragmentSize =
                    std::min(maximum - 2, nal.size - offset);
                std::vector<std::uint8_t> payload(fragmentSize + 2);
                payload[0] = indicator;
                payload[1] = static_cast<std::uint8_t>(
                    (first ? 0x80 : 0) |
                    (offset + fragmentSize >= nal.size ? 0x40 : 0) |
                    (nal.data[0] & 0x1f));
                std::memcpy(
                    payload.data() + 2,
                    nal.data + offset,
                    fragmentSize);
                const auto last =
                    offset + fragmentSize >= nal.size;
                auto result = emitRtp(
                    stream, timestamp,
                    finalNal && last,
                    payload.data(), payload.size());
                if (!result.ok) {
                    return result;
                }
                first = false;
                offset += fragmentSize;
            }
        }
        return {};
    }

    Status configureStream(StreamId streamId,
                           MediaKind kind,
                           CodecType codec,
                           std::uint8_t payloadType,
                           int clockRate) {
        std::lock_guard<std::mutex> lock(streamMutex);
        auto& stream = ensureStream(streamId, kind, codec);
        for (auto iterator = streamsByPayloadType.begin();
             iterator != streamsByPayloadType.end();) {
            if (iterator->second == streamId) {
                iterator = streamsByPayloadType.erase(iterator);
            } else {
                ++iterator;
            }
        }
        stream.kind = kind;
        stream.codec = codec;
        stream.payloadType = payloadType;
        stream.clockRate = std::max(1, clockRate);
        streamsByPayloadType[payloadType] = streamId;
        return {};
    }

    Status setRemoteEndpoint(const std::string& endpointValue) {
        const auto endpoint = parseEndpoint(endpointValue);
        if (!endpoint) {
            return Status{false, 1, "invalid RTP remote endpoint"};
        }
        sockaddr_in value{};
        value.sin_family = AF_INET;
        value.sin_port = htons(endpoint->port);
        if (inet_pton(
                AF_INET, endpoint->host.c_str(), &value.sin_addr) != 1) {
            return Status{false, 1, "invalid RTP remote address"};
        }
        std::lock_guard<std::mutex> lock(mutex);
        remote = value;
        remoteConfigured = true;
        return {};
    }

    Status open(const TransportConfig& value) {
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
        std::lock_guard<std::mutex> lock(mutex);
        if (value.options.find("listen") != value.options.end() &&
            value.options.at("listen") == "false") {
            return Status{false, 1, "RTP transport requires a listen socket"};
        }
        config = value;
        streams.clear();
        streamsByPayloadType.clear();
        mtu = std::max(256, intOption(value.options, "mtu", kDefaultMtu));
        std::unordered_set<StreamId> configuredStreams;
        const std::string prefix = "stream.";
        for (const auto& entry : config.options) {
            if (entry.first.rfind(prefix, 0) != 0) {
                continue;
            }
            const auto separator = entry.first.find(
                '.', prefix.size());
            if (separator == std::string::npos) {
                continue;
            }
            try {
                const auto id = static_cast<StreamId>(std::stoull(
                    entry.first.substr(
                        prefix.size(),
                        separator - prefix.size())));
                configuredStreams.insert(id);
            } catch (...) {
            }
        }
        for (const auto id : configuredStreams) {
            (void)ensureStream(id, MediaKind::Unknown, CodecType::Unknown);
        }

        auto remoteValue = option(value.options, "remoteEndpoint", value.endpoint);
        if (!remoteValue.empty()) {
            const auto endpoint = parseEndpoint(remoteValue);
            if (!endpoint) {
                return Status{false, 1, "invalid RTP remote endpoint"};
            }
            remote = {};
            remote.sin_family = AF_INET;
            remote.sin_port = htons(endpoint->port);
            if (inet_pton(AF_INET, endpoint->host.c_str(), &remote.sin_addr) != 1) {
                return Status{false, 1, "invalid RTP remote address"};
            }
            remoteConfigured = true;
        }

        const auto bindAddress =
            option(value.options, "bindAddress", "0.0.0.0");
        const auto port = intOption(value.options, "port", 0);
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
        currentInfo.state = TransportState::Open;
        currentInfo.endpoint =
            advertised + ":" +
            std::to_string(ntohs(bound.sin_port));
        running.store(true, std::memory_order_release);
        receiveThread = std::thread([this] { receiveLoop(); });
        return {};
    }

    void close() noexcept {
        {
            std::lock_guard<std::mutex> lock(mutex);
            running.store(false, std::memory_order_release);
            currentInfo.state = TransportState::Closed;
            rawSink.reset();
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
        auto raw = copyToMediaBuffer(data, size);
        if (raw.buffer) {
            auto sink = [&] {
                std::lock_guard<std::mutex> lock(mutex);
                return rawSink.lock();
            }();
            if (sink) {
                sink->onTransportData(std::move(raw));
            }
        }
        if (size >= 2 && data[1] >= 192 && data[1] <= 223) {
            return;
        }
        ParsedRtp rtp;
        if (!parseRtp(data, size, &rtp) || !rtp.payload ||
            rtp.payloadSize == 0) {
            std::lock_guard<std::mutex> lock(mutex);
            currentStats.receiveErrors += 1;
            return;
        }
        EncodedPacket packet;
        std::optional<StreamId> completedStream;
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
                const auto codec = rtp.payloadType == kDefaultH264PayloadType
                    ? CodecType::H264
                    : CodecType::Opus;
                state = &ensureStream(1, MediaKind::Unknown, codec);
                state->payloadType = rtp.payloadType;
                streamsByPayloadType[rtp.payloadType] = state->id;
            }
            if (!state || state->codec != CodecType::H264) {
                packet.streamId = state ? state->id : 1;
                packet.kind = state ? state->kind : MediaKind::Audio;
                packet.codecType =
                    state ? state->codec : CodecType::Opus;
                packet.data = copyToMediaBuffer(rtp.payload, rtp.payloadSize);
                packet.ptsMicros = rtpTimeToMicros(
                    rtp.timestamp, state ? state->clockRate : 48000);
                packet.key = true;
                completedStream = packet.streamId;
            } else {
                auto& h264 = state->h264;
                if (h264.timestamp && *h264.timestamp != rtp.timestamp) {
                    h264.reset();
                }
                if (h264.expectedSequence &&
                    *h264.expectedSequence != rtp.sequence) {
                    h264.damaged = true;
                }
                h264.expectedSequence =
                    static_cast<std::uint16_t>(rtp.sequence + 1);
                h264.timestamp = rtp.timestamp;
                if (!h264.damaged) {
                    const auto nalType = rtp.payload[0] & 0x1f;
                    if (nalType == 28) {
                        h264.damaged = !h264.appendFragment(
                            rtp.payload, rtp.payloadSize);
                    } else if (h264.fragmentOpen) {
                        h264.damaged = true;
                    } else if (nalType == 24) {
                        h264.damaged = !h264.appendAggregation(
                            rtp.payload, rtp.payloadSize);
                    } else if (nalType >= 1 && nalType <= 23) {
                        appendH264StartCode(h264.accessUnit);
                        h264.accessUnit.insert(
                            h264.accessUnit.end(), rtp.payload,
                            rtp.payload + rtp.payloadSize);
                    } else {
                        h264.damaged = true;
                    }
                }
                if (rtp.marker) {
                    if (!h264.damaged && !h264.fragmentOpen &&
                        !h264.accessUnit.empty()) {
                        packet.streamId = state->id;
                        packet.kind = MediaKind::Video;
                        packet.codecType = CodecType::H264;
                        packet.data = copyToMediaBuffer(
                            h264.accessUnit.data(), h264.accessUnit.size());
                        packet.ptsMicros = rtpTimeToMicros(
                            rtp.timestamp, state->clockRate);
                        const auto nals = splitH264Nals(
                            h264.accessUnit.data(), h264.accessUnit.size());
                        packet.key = containsH264Nal(nals, 5);
                        completedStream = packet.streamId;
                    }
                    h264.accessUnit.clear();
                    h264.fragmentOpen = false;
                    h264.damaged = false;
                    h264.expectedSequence.reset();
                }
            }
        }
        if (!completedStream || !packet.data.buffer) {
            return;
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

Status RTPTransport::open(const TransportConfig& config) {
    auto state = stateFor(this);
    return state ? state->open(config)
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
    auto& stream = state->ensureStream(
        packet.streamId, packet.kind, packet.codecType);
    const auto timestamp = microsToRtpTime(
        packet.ptsMicros, stream.clockRate);
    if (stream.codec == CodecType::H264) {
        return state->packetizeH264(
            stream, timestamp, data, size, packet.key);
    }
    if (stream.codec == CodecType::Opus) {
        auto result = state->emitRtp(
            stream, timestamp, true, data, size);
        return result;
    }
    return Status{false, 1, "unsupported encoded codec"};
}

void RTPTransport::setSink(
    std::weak_ptr<IEncodedPacketSink> sink) {
    if (auto state = stateFor(this)) {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->packetSink = std::move(sink);
    }
}

Status RTPTransport::configureStream(StreamId streamId,
                                     MediaKind kind,
                                     CodecType codec,
                                     std::uint8_t payloadType,
                                     int clockRate) {
    auto state = stateFor(this);
    return state
        ? state->configureStream(
              streamId, kind, codec, payloadType, clockRate)
        : Status{false, 1, "RTP transport state is unavailable"};
}

Status RTPTransport::setRemoteEndpoint(
    const std::string& endpoint) {
    auto state = stateFor(this);
    return state ? state->setRemoteEndpoint(endpoint)
                 : Status{false, 1, "RTP transport state is unavailable"};
}

Status RTPTransport::send(MediaBufferView data) {
    auto state = stateFor(this);
    return state ? state->rawSend(std::move(data))
                 : Status{false, 1, "RTP transport state is unavailable"};
}

void RTPTransport::setSink(std::weak_ptr<ITransportSink> sink) {
    if (auto state = stateFor(this)) {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->rawSink = std::move(sink);
    }
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
