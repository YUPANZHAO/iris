#include "transport/rtp_engine.h"

#include "transport/udp_transport.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace iris::media::transport {
namespace {

constexpr std::size_t kRTPHeaderSize = 12;
constexpr std::size_t kMaximumDatagramSize = 65507;
constexpr int kDefaultMtu = 1200;

std::atomic<std::uint32_t> gNextAutomaticSsrc{0x49520000};
std::atomic<std::uint32_t> gNextTimestampOrigin{0x12000000};

struct ParsedRTP {
    std::uint8_t payloadType = 0;
    std::uint16_t sequenceNumber = 0;
    std::uint32_t timestamp = 0;
    std::uint32_t ssrc = 0;
    bool marker = false;
    const std::uint8_t* payload = nullptr;
    std::size_t payloadSize = 0;
};

struct StreamBinding {
    StreamId streamId = 0;
    std::uint8_t payloadType = 0;
    int clockRate = 0;
    bool hasPayloadType = false;
    bool hasClockRate = false;

    bool complete() const noexcept {
        return hasPayloadType && hasClockRate;
    }
};

struct RTPStreamState {
    std::uint8_t payloadType = 0;
    int clockRate = 0;
    std::uint16_t sequenceNumber = 0;
    std::uint32_t ssrc = 0;
    std::int64_t mediaTimestampOriginMicros = 0;
    std::uint32_t rtpTimestampOrigin = 0;
    bool hasTimestampOrigin = false;
};

std::uint16_t readU16(const std::uint8_t* data) noexcept {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(data[0]) << 8) |
        static_cast<std::uint16_t>(data[1]));
}

std::uint32_t readU32(const std::uint8_t* data) noexcept {
    return (static_cast<std::uint32_t>(data[0]) << 24) |
           (static_cast<std::uint32_t>(data[1]) << 16) |
           (static_cast<std::uint32_t>(data[2]) << 8) |
           static_cast<std::uint32_t>(data[3]);
}

void writeU16(std::uint8_t* data, std::uint16_t value) noexcept {
    data[0] = static_cast<std::uint8_t>(value >> 8);
    data[1] = static_cast<std::uint8_t>(value);
}

void writeU32(std::uint8_t* data, std::uint32_t value) noexcept {
    data[0] = static_cast<std::uint8_t>(value >> 24);
    data[1] = static_cast<std::uint8_t>(value >> 16);
    data[2] = static_cast<std::uint8_t>(value >> 8);
    data[3] = static_cast<std::uint8_t>(value);
}

bool parseRTP(const std::uint8_t* data,
              std::size_t size,
              ParsedRTP* parsed) noexcept {
    if (!data || !parsed || size < kRTPHeaderSize ||
        (data[0] >> 6) != 2) {
        return false;
    }

    std::size_t offset = kRTPHeaderSize +
        static_cast<std::size_t>(data[0] & 0x0f) * 4;
    if (offset > size) {
        return false;
    }
    if ((data[0] & 0x10) != 0) {
        if (size - offset < 4) {
            return false;
        }
        const auto extensionWords = readU16(data + offset + 2);
        offset += 4 + static_cast<std::size_t>(extensionWords) * 4;
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
    if (payloadEnd <= offset) {
        return false;
    }

    parsed->payloadType = data[1] & 0x7f;
    parsed->marker = (data[1] & 0x80) != 0;
    parsed->sequenceNumber = readU16(data + 2);
    parsed->timestamp = readU32(data + 4);
    parsed->ssrc = readU32(data + 8);
    parsed->payload = data + offset;
    parsed->payloadSize = payloadEnd - offset;
    return true;
}

bool isRtcpPacket(const std::uint8_t* data,
                  std::size_t size) noexcept {
    return data && size >= 2 &&
           (data[0] >> 6) == 2 &&
           data[1] >= 192 && data[1] <= 223;
}

bool parseRtcpPacket(const std::uint8_t* data,
                     std::size_t size,
                     std::size_t* offset,
                     RtcpParams* params) {
    if (!data || !offset || !params || *offset > size ||
        size - *offset < 4 || (data[*offset] >> 6) != 2) {
        return false;
    }

    const auto start = *offset;
    const auto packetType = data[start + 1];
    if (packetType < 192 || packetType > 223) {
        return false;
    }
    const auto packetSize =
        (static_cast<std::size_t>(readU16(data + start + 2)) + 1) * 4;
    if (packetSize < 4 || packetSize > size - start) {
        return false;
    }

    std::size_t padding = 0;
    if ((data[start] & 0x20) != 0) {
        padding = data[start + packetSize - 1];
        if (padding == 0 || padding > packetSize - 4) {
            return false;
        }
    }
    const auto payloadSize = packetSize - 4 - padding;
    const auto* payload = data + start + 4;

    params->packetType = packetType;
    params->count = data[start] & 0x1f;
    params->ssrc =
        payloadSize >= 4 ? readU32(payload) : 0;
    params->payload =
        std::make_shared<ByteBuffer>(payload, payloadSize);
    *offset += packetSize;
    return true;
}

std::uint32_t timestampDelta(std::int64_t micros,
                             int clockRate) noexcept {
    const bool negative = micros < 0;
    const auto magnitude = negative
        ? static_cast<std::uint64_t>(-(micros + 1)) + 1
        : static_cast<std::uint64_t>(micros);
    const auto ticks =
        (magnitude / 1000000U) *
            static_cast<std::uint64_t>(clockRate) +
        (magnitude % 1000000U) *
            static_cast<std::uint64_t>(clockRate) / 1000000U;
    return negative
        ? static_cast<std::uint32_t>(0 - ticks)
        : static_cast<std::uint32_t>(ticks);
}

std::uint32_t rtpTimestampFor(RTPStreamState& stream,
                              std::int64_t timestampMicros) noexcept {
    if (!stream.hasTimestampOrigin) {
        stream.mediaTimestampOriginMicros = timestampMicros;
        stream.rtpTimestampOrigin =
            gNextTimestampOrigin.fetch_add(0x20000000);
        stream.hasTimestampOrigin = true;
    }
    return stream.rtpTimestampOrigin +
           timestampDelta(
               timestampMicros -
                   stream.mediaTimestampOriginMicros,
               stream.clockRate);
}

std::int64_t mediaTimestampMicrosFor(
    RTPStreamState& stream,
    std::uint32_t timestamp) noexcept {
    if (!stream.hasTimestampOrigin) {
        stream.mediaTimestampOriginMicros = 0;
        stream.rtpTimestampOrigin = timestamp;
        stream.hasTimestampOrigin = true;
    }
    const auto delta =
        static_cast<std::int32_t>(
            timestamp - stream.rtpTimestampOrigin);
    return static_cast<std::int64_t>(delta) * 1000000LL /
           stream.clockRate;
}

std::uint32_t allocateSsrc() noexcept {
    auto value = gNextAutomaticSsrc.fetch_add(1);
    if (value == 0) {
        value = gNextAutomaticSsrc.fetch_add(1);
    }
    return value;
}

std::optional<std::uint64_t> parseUnsigned(const std::string& text) {
    try {
        std::size_t consumed = 0;
        const auto value = std::stoull(text, &consumed);
        if (consumed != text.size()) {
            return std::nullopt;
        }
        return value;
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<int> parseInteger(const std::string& text) {
    try {
        std::size_t consumed = 0;
        const auto value = std::stoi(text, &consumed);
        if (consumed != text.size()) {
            return std::nullopt;
        }
        return value;
    } catch (...) {
        return std::nullopt;
    }
}

Status applyDirectBinding(const AttributeMap& options,
                          std::map<StreamId, StreamBinding>& bindings) {
    const auto streamIdValue = options.find("streamId");
    const auto payloadValue = options.find("payloadType");
    const auto clockValue = options.find("clockRate");
    if (streamIdValue == options.end() &&
        payloadValue == options.end() &&
        clockValue == options.end()) {
        return {};
    }
    if (streamIdValue == options.end() ||
        payloadValue == options.end() ||
        clockValue == options.end()) {
        return Status::failure(
            "streamId, payloadType, and clockRate are required together");
    }

    const auto streamId = parseUnsigned(streamIdValue->second);
    const auto payloadType = parseInteger(payloadValue->second);
    const auto clockRate = parseInteger(clockValue->second);
    if (!streamId || !payloadType ||
        *payloadType < 0 || *payloadType > 127 ||
        !clockRate || *clockRate <= 0) {
        return Status::failure("invalid RTP stream configuration");
    }

    auto& binding = bindings[*streamId];
    binding.streamId = *streamId;
    binding.payloadType =
        static_cast<std::uint8_t>(*payloadType);
    binding.clockRate = *clockRate;
    binding.hasPayloadType = true;
    binding.hasClockRate = true;
    return {};
}

Status applyPrefixedBindings(
    const AttributeMap& options,
    std::map<StreamId, StreamBinding>& bindings) {
    const std::string prefix = "stream.";
    for (const auto& entry : options) {
        if (entry.first.rfind(prefix, 0) != 0) {
            continue;
        }
        const auto separator = entry.first.find('.', prefix.size());
        if (separator == std::string::npos) {
            continue;
        }
        const auto streamId = parseUnsigned(
            entry.first.substr(
                prefix.size(), separator - prefix.size()));
        if (!streamId) {
            return Status::failure("invalid RTP stream ID");
        }

        const auto field = entry.first.substr(separator + 1);
        auto& binding = bindings[*streamId];
        binding.streamId = *streamId;
        if (field == "payloadType") {
            const auto value = parseInteger(entry.second);
            if (!value || *value < 0 || *value > 127) {
                return Status::failure(
                    "invalid RTP payload type");
            }
            binding.payloadType =
                static_cast<std::uint8_t>(*value);
            binding.hasPayloadType = true;
        } else if (field == "clockRate") {
            const auto value = parseInteger(entry.second);
            if (!value || *value <= 0) {
                return Status::failure("invalid RTP clock rate");
            }
            binding.clockRate = *value;
            binding.hasClockRate = true;
        }
    }
    return {};
}

Status parseMtu(const AttributeMap& options, int* mtu) {
    const auto value = options.find("mtu");
    if (value == options.end()) {
        return {};
    }
    const auto parsed = parseInteger(value->second);
    if (!parsed ||
        *parsed <= static_cast<int>(kRTPHeaderSize) ||
        *parsed > static_cast<int>(kMaximumDatagramSize)) {
        return Status::failure("invalid RTP MTU");
    }
    *mtu = *parsed;
    return {};
}

} // namespace

struct RTPEngine::Impl {
    Impl() {
        transport = std::make_unique<UdpTransport>();
    }

    Status applyOptions(const AttributeMap& options) {
        std::lock_guard<std::mutex> lock(mutex);
        auto nextBindings = bindings;
        auto result = applyDirectBinding(options, nextBindings);
        if (!result.ok) {
            return result;
        }
        result = applyPrefixedBindings(options, nextBindings);
        if (!result.ok) {
            return result;
        }

        int nextMtu = mtu;
        result = parseMtu(options, &nextMtu);
        if (!result.ok) {
            return result;
        }

        std::map<std::uint8_t, StreamId> payloadTypes;
        for (const auto& entry : nextBindings) {
            if (!entry.second.complete()) {
                continue;
            }
            const auto bound =
                payloadTypes.find(entry.second.payloadType);
            if (bound != payloadTypes.end() &&
                bound->second != entry.first) {
                return Status::failure(
                    "RTP payload type is bound to multiple streams");
            }
            payloadTypes[entry.second.payloadType] = entry.first;
        }

        bindings = std::move(nextBindings);
        streamsByPayloadType = std::move(payloadTypes);
        mtu = nextMtu;

        for (const auto& entry : bindings) {
            if (!entry.second.complete()) {
                continue;
            }
            const auto update = [&](RTPStreamState& state) {
                if (state.payloadType != entry.second.payloadType ||
                    state.clockRate != entry.second.clockRate) {
                    state.hasTimestampOrigin = false;
                }
                state.payloadType = entry.second.payloadType;
                state.clockRate = entry.second.clockRate;
            };
            const auto tx = txStreams.find(entry.first);
            if (tx != txStreams.end()) {
                update(tx->second);
            }
            const auto rx = rxStreams.find(entry.first);
            if (rx != rxStreams.end()) {
                update(rx->second);
            }
        }
        return {};
    }

    Status send(const RtpParams& params,
                const EncodedPacket& packet) {
        if (!packet.data || !*packet.data ||
            !packet.data->data() || packet.data->size() == 0) {
            return Status::failure("encoded packet payload is empty");
        }

        std::unique_lock<std::mutex> lock(mutex);
        const auto binding = bindings.find(packet.streamId);
        if (binding == bindings.end() || !binding->second.complete()) {
            return Status::failure(
                "RTP stream is not configured");
        }

        auto& state = txStreams[packet.streamId];
        if (state.payloadType != binding->second.payloadType ||
            state.clockRate != binding->second.clockRate) {
            state.hasTimestampOrigin = false;
        }
        state.payloadType = binding->second.payloadType;
        state.clockRate = binding->second.clockRate;
        if (state.ssrc == 0) {
            state.ssrc = allocateSsrc();
        }

        const auto timestamp =
            rtpTimestampFor(state, packet.ptsMicros);
        const auto maxPayload =
            static_cast<std::size_t>(mtu) - kRTPHeaderSize;
        const auto payloadSize = packet.data->size();
        const auto fragmentCount =
            (payloadSize + maxPayload - 1) / maxPayload;

        std::vector<MediaBuffer> datagrams;
        datagrams.reserve(fragmentCount);
        const auto* payload =
            static_cast<const std::uint8_t*>(packet.data->data());
        std::size_t offset = 0;
        for (std::size_t index = 0; index < fragmentCount; ++index) {
            const auto size =
                std::min(maxPayload, payloadSize - offset);
            auto datagram = std::make_shared<ByteBuffer>(
                kRTPHeaderSize + size);
            if (!datagram || !*datagram) {
                return Status::failure(
                    "failed to allocate RTP datagram");
            }
            auto* bytes = static_cast<std::uint8_t*>(datagram->data());
            const bool last = index + 1 == fragmentCount;
            bytes[0] = 0x80;
            bytes[1] = static_cast<std::uint8_t>(
                (last && params.marker ? 0x80 : 0x00) |
                state.payloadType);
            writeU16(bytes + 2, state.sequenceNumber);
            writeU32(bytes + 4, timestamp);
            writeU32(bytes + 8, state.ssrc);
            std::memcpy(
                bytes + kRTPHeaderSize, payload + offset, size);
            ++state.sequenceNumber;
            offset += size;
            datagrams.push_back(std::move(datagram));
        }

        auto* networkTransport = transport.get();
        lock.unlock();
        for (auto& datagram : datagrams) {
            const auto sent =
                networkTransport->send(std::move(datagram));
            if (!sent.ok) {
                return sent;
            }
        }
        return {};
    }

    void setSink(std::weak_ptr<RTPTransportSink> value) noexcept {
        std::lock_guard<std::mutex> lock(mutex);
        sink = std::move(value);
    }

    Status receiveRtcp(MediaBuffer datagram) {
        const auto* bytes =
            static_cast<const std::uint8_t*>(datagram->data());
        std::vector<RtcpParams> packets;
        std::size_t offset = 0;
        while (offset < datagram->size()) {
            RtcpParams params;
            if (!parseRtcpPacket(
                    bytes, datagram->size(), &offset, &params)) {
                return Status::failure("invalid RTCP datagram");
            }
            packets.push_back(std::move(params));
        }

        std::weak_ptr<RTPTransportSink> target;
        {
            std::lock_guard<std::mutex> lock(mutex);
            target = sink;
        }
        if (auto value = target.lock()) {
            for (const auto& params : packets) {
                value->onRtcpRecv(params);
            }
        }
        return {};
    }

    Status receive(MediaBuffer datagram) {
        if (!datagram || !*datagram || !datagram->data() ||
            datagram->size() == 0) {
            return Status::failure("RTP datagram is empty");
        }

        ParsedRTP parsed;
        const auto* bytes =
            static_cast<const std::uint8_t*>(datagram->data());
        if (isRtcpPacket(bytes, datagram->size())) {
            return receiveRtcp(std::move(datagram));
        }
        if (!parseRTP(bytes, datagram->size(), &parsed)) {
            return Status::failure("invalid RTP datagram");
        }

        EncodedPacket packet;
        RtpParams params;
        std::weak_ptr<RTPTransportSink> target;
        {
            std::lock_guard<std::mutex> lock(mutex);
            const auto boundPayload =
                streamsByPayloadType.find(parsed.payloadType);
            if (boundPayload == streamsByPayloadType.end()) {
                return Status::failure("unbound RTP payload type");
            }
            const auto binding = bindings.find(boundPayload->second);
            if (binding == bindings.end() ||
                !binding->second.complete()) {
                return Status::failure("unbound RTP stream");
            }

            auto& state = rxStreams[binding->first];
            if (state.payloadType != binding->second.payloadType ||
                state.clockRate != binding->second.clockRate) {
                state.hasTimestampOrigin = false;
            }
            state.payloadType = binding->second.payloadType;
            state.clockRate = binding->second.clockRate;
            if (state.ssrc != 0 && state.ssrc != parsed.ssrc) {
                state.hasTimestampOrigin = false;
            }
            state.ssrc = parsed.ssrc;
            state.sequenceNumber =
                static_cast<std::uint16_t>(
                    parsed.sequenceNumber + 1);

            packet.streamId = binding->first;
            packet.data = std::make_shared<ByteBuffer>(
                parsed.payload, parsed.payloadSize);
            packet.ptsMicros =
                mediaTimestampMicrosFor(state, parsed.timestamp);
            params.marker = parsed.marker;
            target = sink;
        }

        if (!packet.data || !*packet.data) {
            return Status::failure(
                "failed to allocate encoded packet payload");
        }
        if (auto value = target.lock()) {
            value->onRtpRecv(params, packet);
        }
        return {};
    }

    class TransportSink final : public ITransportSink {
    public:
        explicit TransportSink(std::weak_ptr<Impl> value)
            : impl(std::move(value)) {
        }

        void onTransportData(MediaBuffer data) override {
            if (auto target = impl.lock()) {
                (void)target->receive(std::move(data));
            }
        }

    private:
        std::weak_ptr<Impl> impl;
    };

    std::unique_ptr<Transport> transport;
    std::shared_ptr<TransportSink> transportSink;

    mutable std::mutex mutex;
    std::weak_ptr<RTPTransportSink> sink;

    std::map<StreamId, StreamBinding> bindings;
    std::map<std::uint8_t, StreamId> streamsByPayloadType;
    std::map<StreamId, RTPStreamState> txStreams;
    std::map<StreamId, RTPStreamState> rxStreams;
    int mtu = kDefaultMtu;
};

RTPEngine::RTPEngine()
    : impl_(std::make_shared<Impl>()) {
    impl_->transportSink =
        std::make_shared<Impl::TransportSink>(impl_);
    impl_->transport->setSink(impl_->transportSink);
}

RTPEngine::~RTPEngine() = default;

RTPEngine::RTPEngine(RTPEngine&&) noexcept = default;

RTPEngine& RTPEngine::operator=(RTPEngine&&) noexcept = default;

Status RTPEngine::send(const RtpParams& params,
                       const EncodedPacket& packet) {
    return impl_->send(params, packet);
}

void RTPEngine::setSink(
    std::weak_ptr<RTPTransportSink> sink) noexcept {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->sink = std::move(sink);
}

Status RTPEngine::configure(const AttributeMap& options) {
    const auto configured = impl_->applyOptions(options);
    if (!configured.ok) {
        return configured;
    }
    return impl_->transport->configure(options);
}

Status RTPEngine::attach(const AttributeMap& options) {
    const auto configured = impl_->applyOptions(options);
    if (!configured.ok) {
        return configured;
    }
    return impl_->transport->attach(options);
}

void RTPEngine::detach() noexcept {
    impl_->transport->detach();
}

} // namespace iris::media::transport
