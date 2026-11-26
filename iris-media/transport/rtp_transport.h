#pragma once

// RTP payload transport boundary shared by concrete implementations.

#include "codec/codec_types.h"
#include "transport/transport.h"

#include <cstdint>
#include <memory>

namespace iris::media::transport {

using codec::EncodedPacket;

struct RtpParams {
    bool marker = false;
};

struct RtcpParams {
    std::uint8_t packetType = 0;
    std::uint8_t count = 0;
    std::uint32_t ssrc = 0;
    MediaBuffer payload;
};

class RTPTransportSink {
public:
    virtual ~RTPTransportSink() = default;

    virtual void onRtpRecv(const RtpParams& params,
                           const EncodedPacket& packet) = 0;
    virtual void onRtcpRecv(const RtcpParams& params) = 0;
};

class RTPTransport : public Transport {
public:
    ~RTPTransport() override = default;

    virtual Status send(const RtpParams& params,
                        const EncodedPacket& packet) = 0;

    virtual void setSink(
        std::weak_ptr<RTPTransportSink> sink) noexcept = 0;

protected:
    Status send(MediaBuffer) final {
        return Status::failure(
            "RTPTransport sends RtpParams and EncodedPacket values");
    }

    void setSink(std::weak_ptr<ITransportSink>) noexcept final {
    }
};

} // namespace iris::media::transport
