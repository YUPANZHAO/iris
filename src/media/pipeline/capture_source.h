#pragma once

#include "media/pipeline/media_types.h"

namespace iris::media::pipeline {

// Capture sources form composable chains. A processing source, such as a
// watermark source, implements the same interface and consumes another source
// internally before publishing its own frames to the configured sink.
class ICaptureSource {
public:
    virtual ~ICaptureSource() = default;

    virtual MediaResult start() = 0;
    virtual void stop() noexcept = 0;
    virtual bool running() const noexcept = 0;
};

class IAudioCaptureSource : public ICaptureSource {
public:
    using FrameSink =
        std::function<void(const AudioFrameView& frame)>;

    virtual void setFrameSink(FrameSink sink) = 0;
    virtual void setMuted(bool muted) = 0;
};

class IVideoCaptureSource : public ICaptureSource {
public:
    using FrameSink =
        std::function<void(const VideoFrameView& frame)>;

    virtual void setFrameSink(FrameSink sink) = 0;
    virtual void setMuted(bool muted) = 0;
};

} // namespace iris::media::pipeline
