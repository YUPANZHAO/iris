#pragma once

#include "../media_types.h"
#include "../codec/codec_types.h"

#include <memory>
#include <string>
#include <vector>

namespace iris::media::v2 {

enum class CaptureState {
    Closed,
    Opening,
    Capturing,
    Paused,
    Failed,
};

struct CaptureSourceInfo {
    std::string id;
    std::string name;
    MediaKind kind = MediaKind::Unknown;
    std::string backend;
    bool isDefault = false;
    AttributeMap attributes;
};

// Generic capture config. Device and platform details live in options rather
// than in audio/video-specific fields.
struct CaptureConfig {
    MediaKind kind = MediaKind::Unknown;
    std::string source = "auto";
    AttributeMap options;
};

struct AudioCaptureConfig {
    AudioFormat format;
};

struct VideoCaptureConfig {
    VideoFormat format;
};

class ICaptureObserver {
public:
    virtual ~ICaptureObserver() = default;

    virtual void onCaptureStateChanged(CaptureState state) = 0;
    virtual void onCaptureError(const Status& status) = 0;
};

// Capture only produces native media samples through typed derived interfaces.
// It never decodes encoded media.
class ICaptureSource {
public:
    virtual ~ICaptureSource() = default;

    virtual Status open(const CaptureConfig& config) = 0;
    virtual Status start() = 0;
    virtual Status pause() = 0;
    virtual Status resume() = 0;
    virtual void stop() noexcept = 0;
    virtual void close() noexcept = 0;

    virtual void setObserver(std::weak_ptr<ICaptureObserver> observer) = 0;

    virtual CaptureSourceInfo info() const = 0;
    virtual CaptureState state() const noexcept = 0;
};

// Audio output is derived from the generic capture lifecycle.
class IAudioCaptureSource : public ICaptureSource {
public:
    ~IAudioCaptureSource() override = default;

    virtual Status configure(const AudioCaptureConfig& config) = 0;
    virtual void setAudioSink(
        std::weak_ptr<IAudioFrameSink> sink) = 0;
};

class IVideoCaptureSource : public ICaptureSource {
public:
    ~IVideoCaptureSource() override = default;

    virtual Status configure(const VideoCaptureConfig& config) = 0;
    virtual void setVideoSink(
        std::weak_ptr<IVideoFrameSink> sink) = 0;
};

class ICaptureProvider {
public:
    virtual ~ICaptureProvider() = default;

    virtual std::vector<CaptureSourceInfo> list(MediaKind kind) const = 0;
    virtual std::unique_ptr<ICaptureSource> create(
        const CaptureConfig& config,
        Status* status) = 0;
};

} // namespace iris::media::v2
