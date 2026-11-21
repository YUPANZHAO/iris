#pragma once

#include "media/codec/codec_types.h"

#include <memory>

namespace iris::media::v2 {

class OpusAudioEncoder final : public IAudioEncoder {
public:
    OpusAudioEncoder();
    ~OpusAudioEncoder() override;

    void setSink(std::weak_ptr<IEncodedPacketSink> sink) noexcept override;
    Status configure(const AudioEncoderConfig& config) override;
    Status encode(const AudioFrame& frame) override;
    Status flush() override;
    void reset() noexcept override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class OpusAudioDecoder final : public IAudioDecoder {
public:
    OpusAudioDecoder();
    ~OpusAudioDecoder() override;

    void setSink(std::weak_ptr<IAudioFrameSink> sink) noexcept override;
    Status configure(const AudioDecoderConfig& config) override;
    Status decode(const EncodedPacket& packet) override;
    void reset() noexcept override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class H264VideoEncoder final : public IVideoEncoder {
public:
    H264VideoEncoder();
    ~H264VideoEncoder() override;

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

class H264VideoDecoder final : public IVideoDecoder {
public:
    H264VideoDecoder();
    ~H264VideoDecoder() override;

    void setSink(std::weak_ptr<IVideoFrameSink> sink) noexcept override;
    Status configure(const VideoDecoderConfig& config) override;
    Status decode(const EncodedPacket& packet) override;
    void reset() noexcept override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

std::unique_ptr<IAudioEncoder> createAudioEncoder(CodecType type);
std::unique_ptr<IAudioDecoder> createAudioDecoder(CodecType type);
std::unique_ptr<IVideoEncoder> createVideoEncoder(CodecType type);
std::unique_ptr<IVideoDecoder> createVideoDecoder(CodecType type);

} // namespace iris::media::v2
