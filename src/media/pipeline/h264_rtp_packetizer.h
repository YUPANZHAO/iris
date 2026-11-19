#pragma once

#include "media/pipeline/rtp_packetizer.h"
#include "media/transport/rtp.h"

#include <cstdint>
#include <vector>

namespace iris::media::pipeline {

class H264RtpPacketizer final : public IVideoRtpPacketizer {
public:
    void setParameterSets(ByteView parameterSets) override;

    MediaResult packetize(
        const EncodedPacketView& accessUnit,
        std::uint32_t rtpTimestamp,
        const RtpParameters& parameters,
        const RtpDatagramCallback& sink) override;

private:
    MediaResult sendPayload(const std::uint8_t* data,
                            std::size_t size,
                            std::uint8_t payloadType,
                            std::uint32_t rtpTimestamp,
                            bool marker,
                            const RtpDatagramCallback& sink);

    RtpSender rtp_;
    std::vector<std::uint8_t> parameterSets_;
};

} // namespace iris::media::pipeline
