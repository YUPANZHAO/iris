#pragma once

#include "media/pipeline/media_types.h"

namespace iris::media::pipeline {

class IAudioRtpPacketizer {
public:
    virtual ~IAudioRtpPacketizer() = default;

    virtual MediaResult packetize(
        const EncodedPacketView& packet,
        std::uint32_t rtpTimestamp,
        const RtpParameters& parameters,
        const RtpDatagramCallback& sink) = 0;
};

class IVideoRtpPacketizer {
public:
    virtual ~IVideoRtpPacketizer() = default;

    // Parameter sets are encoder configuration data, not media samples.
    virtual void setParameterSets(ByteView parameterSets) = 0;
    virtual MediaResult packetize(
        const EncodedPacketView& accessUnit,
        std::uint32_t rtpTimestamp,
        const RtpParameters& parameters,
        const RtpDatagramCallback& sink) = 0;
};

} // namespace iris::media::pipeline
