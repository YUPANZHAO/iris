#pragma once

#include "capture_source/capture_source.h"

#include <memory>

namespace iris::media::capture {

class FFmpegVideoCaptureSource final : public VideoCaptureSource {
public:
    FFmpegVideoCaptureSource();
    ~FFmpegVideoCaptureSource() override;

    FFmpegVideoCaptureSource(const FFmpegVideoCaptureSource&) = delete;
    FFmpegVideoCaptureSource& operator=(
        const FFmpegVideoCaptureSource&) = delete;
    FFmpegVideoCaptureSource(FFmpegVideoCaptureSource&&) noexcept;
    FFmpegVideoCaptureSource& operator=(
        FFmpegVideoCaptureSource&&) noexcept;

    Status configure(const VideoCaptureConfig& config) override;
    Status start() override;
    void stop() noexcept override;
    bool running() const noexcept override;
    std::string lastError() const override;
    void setFrameSink(std::weak_ptr<VideoFrameSink> sink) noexcept override;
    VideoFormat outputFormat() const noexcept override;

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace iris::media::capture
