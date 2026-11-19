#pragma once

#include "iris/media.h"
#include "media/pipeline/capture_source.h"

#include <memory>

namespace iris::media {

class IVideoFrameSink;

class FFmpegVideoCaptureSource final
    : public pipeline::IVideoCaptureSource {
public:
    FFmpegVideoCaptureSource();
    ~FFmpegVideoCaptureSource() override;

    FFmpegVideoCaptureSource(
        const FFmpegVideoCaptureSource&) = delete;
    FFmpegVideoCaptureSource& operator=(
        const FFmpegVideoCaptureSource&) = delete;

    MediaResult configure(const MediaRuntimeConfig& runtime,
                          const MediaSessionConfig& session,
                          IVideoFrameSink* previewSink);

    MediaResult start() override;
    void stop() noexcept override;
    bool running() const noexcept override;
    void setFrameSink(FrameSink sink) override;
    void setMuted(bool muted) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace iris::media
