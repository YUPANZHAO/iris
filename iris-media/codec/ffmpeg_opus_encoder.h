#pragma once

#include "codec/codec_types.h"

#include <memory>

namespace iris::media::codec {

class FFmpegOpusEncoder final : public AudioEncoder {
public:
    FFmpegOpusEncoder();
    ~FFmpegOpusEncoder() override;

    FFmpegOpusEncoder(const FFmpegOpusEncoder&) = delete;
    FFmpegOpusEncoder& operator=(const FFmpegOpusEncoder&) = delete;
    FFmpegOpusEncoder(FFmpegOpusEncoder&&) noexcept;
    FFmpegOpusEncoder& operator=(FFmpegOpusEncoder&&) noexcept;

    Status configure(const AudioEncoderConfig& config) override;
    Status encode(const AudioRawFrame& frame) override;
    Status flush() override;
    void setEncodedPacketSink(
        std::weak_ptr<IEncodedPacketSink> sink) noexcept override;
    void reset() noexcept override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace iris::media::codec
