#pragma once

// Capture-source contracts for audio and video devices.

#include "codec/codec_types.h"
#include "media_types.h"

#include <memory>
#include <string>

namespace iris::media::capture {

using Status = ::iris::media::Status;
using AttributeMap = ::iris::media::AttributeMap;

struct CaptureSourceConfig {
    // FFmpeg input format/device backend, such as "dshow", "v4l2", "alsa", or
    // "lavfi". An empty value lets FFmpeg choose a backend from the URL.
    std::string backend;

    // Device URL or platform-specific device name.
    std::string name;

    // Options passed to the selected FFmpeg input device.
    AttributeMap options;
};

// Base interface for a capture source. Implementations own all device and
// decoding resources. start() is synchronous through device setup; decoded
// frames are delivered asynchronously until stop() is called or the input ends.
class CaptureSource {
public:
    virtual ~CaptureSource() = default;

    virtual Status start() = 0;
    virtual void stop() noexcept = 0;
    virtual bool running() const noexcept = 0;
    virtual std::string lastError() const = 0;
};

using AudioFrame = codec::AudioRawFrame;
using AudioFrameSink = codec::IAudioFrameSink;
using AudioFormat = codec::AudioFormat;
using AudioSampleFormat = codec::AudioSampleFormat;

struct AudioCaptureConfig : CaptureSourceConfig {
    // A zero sample rate or channel count selects the device value. The
    // implementation converts frames to the requested sample format.
    AudioFormat outputFormat;
};

class AudioCaptureSource : public CaptureSource {
public:
    ~AudioCaptureSource() override = default;

    virtual Status configure(const AudioCaptureConfig& config) = 0;
    virtual void setFrameSink(
        std::weak_ptr<AudioFrameSink> sink) noexcept = 0;

    // Returns the concrete format after start(), once the device has been
    // opened. Before that, it reflects a configured output format if known.
    virtual AudioFormat outputFormat() const noexcept = 0;
};

using VideoFrame = codec::VideoRawFrame;
using VideoFrameSink = codec::IVideoFrameSink;
using VideoFormat = codec::VideoFormat;
using VideoPixelFormat = codec::VideoPixelFormat;
using Rational = codec::Rational;

struct VideoCaptureConfig : CaptureSourceConfig {
    // A zero width or height keeps the device dimensions; a zero frame rate
    // keeps the stream frame rate. Pixels are converted to pixelFormat.
    VideoFormat outputFormat;
};

class VideoCaptureSource : public CaptureSource {
public:
    ~VideoCaptureSource() override = default;

    virtual Status configure(const VideoCaptureConfig& config) = 0;
    virtual void setFrameSink(
        std::weak_ptr<VideoFrameSink> sink) noexcept = 0;

    // Returns the concrete format after start(), once the device has been
    // opened. Before that, it reflects a configured output format if known.
    virtual VideoFormat outputFormat() const noexcept = 0;
};

} // namespace iris::media::capture

namespace iris::media {

// Convenience alias for callers that prefer to mirror the directory name.
namespace capture_source = capture;

} // namespace iris::media
