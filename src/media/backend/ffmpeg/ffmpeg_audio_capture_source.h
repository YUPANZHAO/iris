#pragma once

#include "iris/media.h"
#include "media/pipeline/capture_source.h"

#include <memory>

namespace iris::media {

class FFmpegAudioCaptureSource final
    : public pipeline::IAudioCaptureSource {
public:
    FFmpegAudioCaptureSource();
    ~FFmpegAudioCaptureSource() override;

    FFmpegAudioCaptureSource(
        const FFmpegAudioCaptureSource&) = delete;
    FFmpegAudioCaptureSource& operator=(
        const FFmpegAudioCaptureSource&) = delete;

    MediaResult configure(const MediaRuntimeConfig& runtime,
                          const MediaSessionConfig& session);

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
