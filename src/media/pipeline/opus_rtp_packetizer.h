#pragma once

#include "media/pipeline/rtp_packetizer.h"
#include "media/transport/rtp.h"

namespace iris::media::pipeline {

class OpusRtpPacketizer final : public IAudioRtpPacketizer {
public:
    MediaResult packetize(
        const EncodedPacketView& packet,
        std::uint32_t rtpTimestamp,
        const RtpParameters& parameters,
        const RtpDatagramCallback& sink) override;

private:
    RtpSender rtp_;
};

} // namespace iris::media::pipeline
