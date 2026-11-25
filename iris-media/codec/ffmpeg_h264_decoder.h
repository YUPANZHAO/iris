#pragma once

#include "codec/codec_types.h"

#include <memory>

namespace iris::media::codec {

class FFmpegH264Decoder final : public VideoDecoder {
public:
    FFmpegH264Decoder();
    ~FFmpegH264Decoder() override;

    FFmpegH264Decoder(const FFmpegH264Decoder&) = delete;
    FFmpegH264Decoder& operator=(const FFmpegH264Decoder&) = delete;
    FFmpegH264Decoder(FFmpegH264Decoder&&) noexcept;
    FFmpegH264Decoder& operator=(FFmpegH264Decoder&&) noexcept;

    Status configure(const VideoDecoderConfig& config) override;
    Status decode(const EncodedPacket& packet) override;
    Status flush() override;
    void setRawFrameSink(
        std::weak_ptr<IVideoFrameSink> sink) noexcept override;
    void reset() noexcept override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace iris::media::codec
