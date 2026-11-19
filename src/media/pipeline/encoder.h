#pragma once

#include "media/pipeline/media_types.h"

namespace iris::media::pipeline {

struct AudioEncoderConfig {
    int sampleRate = 48000;
    int channels = 2;
    int bitrate = 64000;
    int frameSamples = 960;
};

struct VideoEncoderConfig {
    int width = 640;
    int height = 360;
    int framesPerSecond = 15;
    int bitrate = 800000;
    int gopSize = 15;
};

class IAudioEncoder {
public:
    virtual ~IAudioEncoder() = default;

    virtual MediaResult configure(
        const AudioEncoderConfig& config,
        EncodedPacketCallback callback) = 0;
    virtual MediaResult encode(const AudioFrameView& frame) = 0;
    virtual MediaResult flush() = 0;
};

class IVideoEncoder {
public:
    virtual ~IVideoEncoder() = default;

    virtual MediaResult configure(
        const VideoEncoderConfig& config,
        EncodedPacketCallback callback) = 0;
    virtual MediaResult encode(const VideoFrameView& frame) = 0;
    virtual MediaResult flush() = 0;
};

class IH264VideoEncoder : public IVideoEncoder {
public:
    // Annex-B SPS/PPS parameter-set NAL units required by H.264 RTP.
    virtual ByteView parameterSets() const noexcept = 0;
};

} // namespace iris::media::pipeline
