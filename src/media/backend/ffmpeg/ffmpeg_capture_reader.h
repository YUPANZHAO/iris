#pragma once

#include "media/backend/ffmpeg/capture_sources.h"
#include "media/engine/media_interfaces.h"

#include <functional>
#include <memory>

struct AVFrame;

namespace iris::media {

struct CaptureReaderConfig {
    MediaKind kind = MediaKind::Audio;
    int videoWidth = 0;
    int videoHeight = 0;
    int videoFps = 0;
};

class FfmpegCaptureReader {
public:
    using FrameCallback =
        std::function<void(const AVFrame& frame)>;

    FfmpegCaptureReader();
    ~FfmpegCaptureReader();

    FfmpegCaptureReader(const FfmpegCaptureReader&) = delete;
    FfmpegCaptureReader& operator=(const FfmpegCaptureReader&) = delete;

    MediaResult open(const CaptureSourceSpec& source,
                     const CaptureReaderConfig& config);
    bool read(const FrameCallback& callback);
    void abortInput() noexcept;
    void close() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace iris::media
