#pragma once

#include "transport.h"

namespace iris::media::v2 {

// Concrete RTP transport. Each EncodedPacket is treated as one opaque RTP
// payload; codec-specific payload formatting belongs above this layer.
class RTPTransport final : public IEncodedPacketTransport {
public:
    RTPTransport();
    ~RTPTransport() override;

    Status configure(const AttributeMap& options) override;
    Status open(const AttributeMap& options) override;
    void close() noexcept override;

    Status send(const EncodedPacket& packet) override;
    void setSink(std::weak_ptr<IEncodedPacketSink> sink) override;

    TransportInfo info() const override;
    TransportStats stats() const override;

protected:
    Status send(MediaBufferView data) override;
    void setSink(std::weak_ptr<ITransportSink> sink) override;
};

} // namespace iris::media::v2
