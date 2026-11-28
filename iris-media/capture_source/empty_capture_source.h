#pragma once

#include "capture_source/capture_source.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace iris::media::capture {

// Produces silence so a send-only audio stream remains active without a device.
class EmptyAudioCaptureSource final : public AudioCaptureSource {
public:
    explicit EmptyAudioCaptureSource(std::string text = {});
    ~EmptyAudioCaptureSource() override;

    EmptyAudioCaptureSource(const EmptyAudioCaptureSource&) = delete;
    EmptyAudioCaptureSource& operator=(const EmptyAudioCaptureSource&) = delete;

    Status configure(const AudioCaptureConfig& config) override;
    Status start() override;
    void stop() noexcept override;
    bool running() const noexcept override;
    std::string lastError() const override;
    void setFrameSink(std::weak_ptr<AudioFrameSink> sink) noexcept override;
    AudioFormat outputFormat() const noexcept override;

    void setText(std::string text);
    std::string text() const;

private:
    void run() noexcept;

    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::weak_ptr<AudioFrameSink> sink_;
    std::thread worker_;
    std::atomic<bool> running_{false};
    AudioFormat format_;
    std::string text_;
    std::string error_;
    bool configured_ = false;
};

// Produces a dark-gray canvas with centered white text.
class EmptyVideoCaptureSource final : public VideoCaptureSource {
public:
    explicit EmptyVideoCaptureSource(std::string text = {});
    ~EmptyVideoCaptureSource() override;

    EmptyVideoCaptureSource(const EmptyVideoCaptureSource&) = delete;
    EmptyVideoCaptureSource& operator=(const EmptyVideoCaptureSource&) = delete;

    Status configure(const VideoCaptureConfig& config) override;
    Status start() override;
    void stop() noexcept override;
    bool running() const noexcept override;
    std::string lastError() const override;
    void setFrameSink(std::weak_ptr<VideoFrameSink> sink) noexcept override;
    VideoFormat outputFormat() const noexcept override;

    void setText(std::string text);
    std::string text() const;

private:
    Status renderCanvasImpl();
    void run() noexcept;

    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::weak_ptr<VideoFrameSink> sink_;
    std::thread worker_;
    std::atomic<bool> running_{false};
    VideoFormat format_;
    std::vector<std::uint8_t> canvas_;
    std::string text_;
    std::string error_;
    bool configured_ = false;
};

} // namespace iris::media::capture
