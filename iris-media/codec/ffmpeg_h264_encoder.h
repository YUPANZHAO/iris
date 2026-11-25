#pragma once

#include "codec/codec_types.h"

#include <memory>

namespace iris::media::codec {

class FFmpegH264Encoder final : public VideoEncoder {
public:
    FFmpegH264Encoder();
    ~FFmpegH264Encoder() override;

    FFmpegH264Encoder(const FFmpegH264Encoder&) = delete;
    FFmpegH264Encoder& operator=(const FFmpegH264Encoder&) = delete;
    FFmpegH264Encoder(FFmpegH264Encoder&&) noexcept;
    FFmpegH264Encoder& operator=(FFmpegH264Encoder&&) noexcept;

    Status configure(const VideoEncoderConfig& config) override;
    Status encode(const VideoRawFrame& frame) override;
    Status flush() override;
    void setEncodedPacketSink(
        std::weak_ptr<IEncodedPacketSink> sink) noexcept override;
    void reset() noexcept override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace iris::media::codec
