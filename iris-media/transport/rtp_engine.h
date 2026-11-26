#pragma once

// A small RTP implementation backed by an internally owned Transport.

#include "transport/rtp_transport.h"

#include <memory>

namespace iris::media::transport {

class RTPEngine final : public RTPTransport {
public:
    RTPEngine();
    ~RTPEngine() override;

    RTPEngine(const RTPEngine&) = delete;
    RTPEngine& operator=(const RTPEngine&) = delete;
    RTPEngine(RTPEngine&&) noexcept;
    RTPEngine& operator=(RTPEngine&&) noexcept;

    Status send(const RtpParams& params,
                const EncodedPacket& packet) override;
    void setSink(
        std::weak_ptr<RTPTransportSink> sink) noexcept override;

    Status configure(const AttributeMap& options) override;
    Status attach(const AttributeMap& options) override;
    void detach() noexcept override;

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace iris::media::transport
