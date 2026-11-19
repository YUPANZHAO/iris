#pragma once

#include "media/pipeline/encoder.h"

#include <memory>

namespace iris::media {

class FfmpegAudioEncoder final : public pipeline::IAudioEncoder {
public:
    FfmpegAudioEncoder();
    ~FfmpegAudioEncoder() override;

    FfmpegAudioEncoder(const FfmpegAudioEncoder&) = delete;
    FfmpegAudioEncoder& operator=(const FfmpegAudioEncoder&) = delete;

    MediaResult configure(
        const pipeline::AudioEncoderConfig& config,
        pipeline::EncodedPacketCallback callback) override;
    MediaResult encode(const pipeline::AudioFrameView& frame) override;
    MediaResult flush() override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace iris::media
