#pragma once

#include "media/pipeline/encoder.h"

#include <memory>

namespace iris::media {

class FfmpegVideoEncoder final : public pipeline::IH264VideoEncoder {
public:
    FfmpegVideoEncoder();
    ~FfmpegVideoEncoder() override;

    FfmpegVideoEncoder(const FfmpegVideoEncoder&) = delete;
    FfmpegVideoEncoder& operator=(const FfmpegVideoEncoder&) = delete;

    MediaResult configure(
        const pipeline::VideoEncoderConfig& config,
        pipeline::EncodedPacketCallback callback) override;
    MediaResult encode(const pipeline::VideoFrameView& frame) override;
    MediaResult flush() override;
    ByteView parameterSets() const noexcept override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace iris::media
