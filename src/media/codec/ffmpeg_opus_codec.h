#pragma once

#include "media/codec/codec_types.h"

#include <memory>

namespace iris::media::v2 {

class FFmpegOpusAudioEncoder final : public IAudioEncoder {
public:
    FFmpegOpusAudioEncoder();
    ~FFmpegOpusAudioEncoder() override;

    void setSink(std::weak_ptr<IEncodedPacketSink> sink) noexcept override;
    Status configure(const AudioEncoderConfig& config) override;
    Status encode(const AudioFrame& frame) override;
    Status flush() override;
    void reset() noexcept override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class FFmpegOpusAudioDecoder final : public IAudioDecoder {
public:
    FFmpegOpusAudioDecoder();
    ~FFmpegOpusAudioDecoder() override;

    void setSink(std::weak_ptr<IAudioFrameSink> sink) noexcept override;
    Status configure(const AudioDecoderConfig& config) override;
    Status decode(const EncodedPacket& packet) override;
    void reset() noexcept override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace iris::media::v2
