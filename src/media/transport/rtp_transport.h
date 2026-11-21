#pragma once

#include "transport.h"

namespace iris::media::v2 {

// Concrete RTP transport. RTP framing and packetization remain private
// implementation details; the public packet boundary is inherited EncodedPacket
// send/sink and contains no RTP-specific message type.
class RTPTransport final : public IEncodedPacketTransport {
public:
    RTPTransport();
    ~RTPTransport() override;

    Status open(const TransportConfig& config) override;
    void close() noexcept override;

    Status send(const EncodedPacket& packet) override;
    void setSink(std::weak_ptr<IEncodedPacketSink> sink) override;

    Status configureStream(StreamId streamId,
                           MediaKind kind,
                           CodecType codec,
                           std::uint8_t payloadType,
                           int clockRate);
    Status setRemoteEndpoint(const std::string& endpoint);

    TransportInfo info() const override;
    TransportStats stats() const override;

protected:
    Status send(MediaBufferView data) override;
    void setSink(std::weak_ptr<ITransportSink> sink) override;
};

} // namespace iris::media::v2
