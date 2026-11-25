#pragma once

#include "codec/codec_types.h"

#include <memory>

namespace iris::media::codec {

class FFmpegOpusDecoder final : public AudioDecoder {
public:
    FFmpegOpusDecoder();
    ~FFmpegOpusDecoder() override;

    FFmpegOpusDecoder(const FFmpegOpusDecoder&) = delete;
    FFmpegOpusDecoder& operator=(const FFmpegOpusDecoder&) = delete;
    FFmpegOpusDecoder(FFmpegOpusDecoder&&) noexcept;
    FFmpegOpusDecoder& operator=(FFmpegOpusDecoder&&) noexcept;

    Status configure(const AudioDecoderConfig& config) override;
    Status decode(const EncodedPacket& packet) override;
    Status flush() override;
    void setRawFrameSink(
        std::weak_ptr<IAudioFrameSink> sink) noexcept override;
    void reset() noexcept override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace iris::media::codec
