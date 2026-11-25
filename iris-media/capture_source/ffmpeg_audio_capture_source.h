#pragma once

#include "capture_source/capture_source.h"

#include <memory>

namespace iris::media::capture {

class FFmpegAudioCaptureSource final : public AudioCaptureSource {
public:
    FFmpegAudioCaptureSource();
    ~FFmpegAudioCaptureSource() override;

    FFmpegAudioCaptureSource(const FFmpegAudioCaptureSource&) = delete;
    FFmpegAudioCaptureSource& operator=(
        const FFmpegAudioCaptureSource&) = delete;
    FFmpegAudioCaptureSource(FFmpegAudioCaptureSource&&) noexcept;
    FFmpegAudioCaptureSource& operator=(
        FFmpegAudioCaptureSource&&) noexcept;

    Status configure(const AudioCaptureConfig& config) override;
    Status start() override;
    void stop() noexcept override;
    bool running() const noexcept override;
    std::string lastError() const override;
    void setFrameSink(std::weak_ptr<AudioFrameSink> sink) noexcept override;
    AudioFormat outputFormat() const noexcept override;

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace iris::media::capture
