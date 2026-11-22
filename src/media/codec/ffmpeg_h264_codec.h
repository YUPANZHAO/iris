#pragma once

#include "media/codec/codec_types.h"

#include <memory>

namespace iris::media::v2 {

class FFmpegH264VideoEncoder final : public IVideoEncoder {
public:
    FFmpegH264VideoEncoder();
    ~FFmpegH264VideoEncoder() override;

    void setSink(std::weak_ptr<IEncodedPacketSink> sink) noexcept override;
    Status configure(const VideoEncoderConfig& config) override;
    Status encode(const VideoFrame& frame) override;
    Status flush() override;
    Status setTargetBitrate(int bitrateKbps) override;
    void reset() noexcept override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class FFmpegH264VideoDecoder final : public IVideoDecoder {
public:
    FFmpegH264VideoDecoder();
    ~FFmpegH264VideoDecoder() override;

    void setSink(std::weak_ptr<IVideoFrameSink> sink) noexcept override;
    Status configure(const VideoDecoderConfig& config) override;
    Status decode(const EncodedPacket& packet) override;
    void reset() noexcept override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace iris::media::v2
