#pragma once

// Shared codec contracts, frame/packet types, and abstract codec interfaces.

#include "media_types.h"

#include <cstddef>

namespace iris::media::codec {

using ::iris::media::AttributeMap;
using ::iris::media::MediaBuffer;
using ::iris::media::MediaKind;
using ::iris::media::Status;
using ::iris::media::StreamId;

enum class CodecType {
    Unknown,
    H264,
    Opus,
};

struct EncodedPacket {
    StreamId streamId = 0;
    MediaKind kind = MediaKind::Unknown;
    CodecType codecType = CodecType::Unknown;
    MediaBuffer data;
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

struct AudioRawFrame {
    StreamId streamId = 0;
    AudioFormat format;
    MediaBuffer samples;
    std::int64_t ptsMicros = 0;
    bool endOfStream = false;
};

struct VideoRawFrame {
    StreamId streamId = 0;
    VideoFormat format;
    MediaBuffer pixels;
    std::int64_t ptsMicros = 0;
    bool key = false;
    bool endOfStream = false;
};

class IAudioFrameSink {
public:
    virtual ~IAudioFrameSink() = default;

    virtual void onAudioFrame(const AudioRawFrame& frame) = 0;
};

class IVideoFrameSink {
public:
    virtual ~IVideoFrameSink() = default;

    virtual void onVideoFrame(const VideoRawFrame& frame) = 0;
};

struct AudioEncoderConfig {
    CodecType codecType = CodecType::Unknown;
    AudioFormat inputFormat;
    int bitrateKbps = 0;
    AttributeMap parameters;
};

struct VideoEncoderConfig {
    CodecType codecType = CodecType::Unknown;
    VideoFormat inputFormat;
    int bitrateKbps = 0;
    AttributeMap parameters;
};

struct AudioDecoderConfig {
    CodecType codecType = CodecType::Unknown;
    AudioFormat outputFormat;
    AttributeMap parameters;
};

struct VideoDecoderConfig {
    CodecType codecType = CodecType::Unknown;
    VideoPixelFormat outputFormat = VideoPixelFormat::I420;
    AttributeMap parameters;
};

class AudioEncoder {
public:
    virtual ~AudioEncoder() = default;

    virtual Status configure(const AudioEncoderConfig& config) = 0;
    virtual Status encode(const AudioRawFrame& frame) = 0;
    virtual Status flush() = 0;
    virtual void setEncodedPacketSink(
        std::weak_ptr<IEncodedPacketSink> sink) noexcept = 0;
    virtual void reset() noexcept = 0;
};

class VideoEncoder {
public:
    virtual ~VideoEncoder() = default;

    virtual Status configure(const VideoEncoderConfig& config) = 0;
    virtual Status encode(const VideoRawFrame& frame) = 0;
    virtual Status flush() = 0;
    virtual void setEncodedPacketSink(
        std::weak_ptr<IEncodedPacketSink> sink) noexcept = 0;
    virtual void reset() noexcept = 0;
};

class AudioDecoder {
public:
    virtual ~AudioDecoder() = default;

    virtual Status configure(const AudioDecoderConfig& config) = 0;
    virtual Status decode(const EncodedPacket& packet) = 0;
    virtual Status flush() = 0;
    virtual void setRawFrameSink(
        std::weak_ptr<IAudioFrameSink> sink) noexcept = 0;
    virtual void reset() noexcept = 0;
};

class VideoDecoder {
public:
    virtual ~VideoDecoder() = default;

    virtual Status configure(const VideoDecoderConfig& config) = 0;
    virtual Status decode(const EncodedPacket& packet) = 0;
    virtual Status flush() = 0;
    virtual void setRawFrameSink(
        std::weak_ptr<IVideoFrameSink> sink) noexcept = 0;
    virtual void reset() noexcept = 0;
};

} // namespace iris::media::codec
