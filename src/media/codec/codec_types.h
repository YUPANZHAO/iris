#pragma once

#include "../media_types.h"

#include <cstdint>
#include <memory>

namespace iris::media::v2 {

enum class CodecType {
    Unknown,
    Opus,
    H264,
};

struct EncodedPacket {
    StreamId streamId = 0;
    MediaKind kind = MediaKind::Unknown;
    CodecType codecType = CodecType::Unknown;
    MediaBufferView data;
    std::int64_t ptsMicros = 0;
    bool key = false;
    bool endOfStream = false;
};

class IEncodedPacketSink {
public:
    virtual ~IEncodedPacketSink() = default;

    virtual void onEncodedPacket(const EncodedPacket& packet) = 0;
};

enum class AudioSampleFormat {
    Signed16,
    Float32,
};

struct AudioFormat {
    int sampleRate = 0;
    int channels = 0;
    AudioSampleFormat sampleFormat = AudioSampleFormat::Signed16;
};

struct AudioFrame {
    AudioFormat format;
    MediaBufferView samples;
    std::int64_t ptsMicros = 0;
    bool discontinuity = false;
};

class IAudioFrameSink {
public:
    virtual ~IAudioFrameSink() = default;

    virtual void onAudioFrame(const AudioFrame& frame) = 0;
};

struct AudioEncoderConfig {
    CodecType codecType = CodecType::Unknown;
    AudioFormat inputFormat;
    int bitrateKbps = 0;
    AttributeMap parameters;
};

struct AudioDecoderConfig {
    CodecType codecType = CodecType::Unknown;
    AudioFormat outputFormat;
    AttributeMap parameters;
};

// Payloads are delivered exclusively through sinks. encode()/decode() report
// operation status only.
class IAudioEncoder {
public:
    virtual ~IAudioEncoder() = default;

    virtual void setSink(
        std::weak_ptr<IEncodedPacketSink> sink) noexcept = 0;
    virtual Status configure(const AudioEncoderConfig& config) = 0;
    virtual Status encode(const AudioFrame& frame) = 0;
    virtual Status flush() = 0;
    virtual void reset() noexcept = 0;
};

class IAudioDecoder {
public:
    virtual ~IAudioDecoder() = default;

    virtual void setSink(std::weak_ptr<IAudioFrameSink> sink) noexcept = 0;
    virtual Status configure(const AudioDecoderConfig& config) = 0;
    virtual Status decode(const EncodedPacket& packet) = 0;
    virtual void reset() noexcept = 0;
};

enum class VideoPixelFormat {
    I420,
    NV12,
    BGRA,
};

struct Rational {
    int numerator = 0;
    int denominator = 1;
};

struct VideoFormat {
    int width = 0;
    int height = 0;
    Rational frameRate;
    VideoPixelFormat pixelFormat = VideoPixelFormat::I420;
};

struct VideoFrame {
    VideoFormat format;
    MediaBufferView pixels;
    std::int64_t ptsMicros = 0;
    bool key = false;
};

class IVideoFrameSink {
public:
    virtual ~IVideoFrameSink() = default;

    virtual void onVideoFrame(const VideoFrame& frame) = 0;
};

struct VideoEncoderConfig {
    CodecType codecType = CodecType::Unknown;
    VideoFormat inputFormat;
    int bitrateKbps = 0;
    AttributeMap parameters;
};

struct VideoDecoderConfig {
    CodecType codecType = CodecType::Unknown;
    VideoPixelFormat outputFormat = VideoPixelFormat::I420;
    AttributeMap parameters;
};

class IVideoEncoder {
public:
    virtual ~IVideoEncoder() = default;

    virtual void setSink(
        std::weak_ptr<IEncodedPacketSink> sink) noexcept = 0;
    virtual Status configure(const VideoEncoderConfig& config) = 0;
    virtual Status encode(const VideoFrame& frame) = 0;
    virtual Status flush() = 0;
    virtual Status setTargetBitrate(int bitrateKbps) = 0;
    virtual void reset() noexcept = 0;
};

class IVideoDecoder {
public:
    virtual ~IVideoDecoder() = default;

    virtual void setSink(std::weak_ptr<IVideoFrameSink> sink) noexcept = 0;
    virtual Status configure(const VideoDecoderConfig& config) = 0;
    virtual Status decode(const EncodedPacket& packet) = 0;
    virtual void reset() noexcept = 0;
};

class ICodecFactory {
public:
    virtual ~ICodecFactory() = default;

    virtual std::unique_ptr<IAudioEncoder> createAudioEncoder(
        CodecType type) = 0;
    virtual std::unique_ptr<IAudioDecoder> createAudioDecoder(
        CodecType type) = 0;
    virtual std::unique_ptr<IVideoEncoder> createVideoEncoder(
        CodecType type) = 0;
    virtual std::unique_ptr<IVideoDecoder> createVideoDecoder(
        CodecType type) = 0;
};

} // namespace iris::media::v2
