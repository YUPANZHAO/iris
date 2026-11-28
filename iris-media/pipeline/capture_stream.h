#pragma once

#include "pipeline/stream.h"

#include "capture_source/capture_source.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <type_traits>
#include <utility>

namespace iris::media::pipeline {

// Audio capture-to-encoder pipeline. The capture source and encoder are
// configured by the caller; open() connects and starts them as one stream.
class CaptureAudioStream final
    : public Stream,
      public capture::AudioFrameSink,
      public std::enable_shared_from_this<CaptureAudioStream> {
public:
    CaptureAudioStream() = default;
    ~CaptureAudioStream() override;

    CaptureAudioStream(const CaptureAudioStream&) = delete;
    CaptureAudioStream& operator=(const CaptureAudioStream&) = delete;

    Status open() override;
    void close() noexcept override;

    Status setCapture(
        std::shared_ptr<capture::AudioCaptureSource> capture);
    template <
        typename Capture,
        std::enable_if_t<
            std::is_base_of_v<capture::AudioCaptureSource, Capture>,
            int> = 0>
    Status setCapture(std::unique_ptr<Capture> capture) {
        return setCapture(
            std::shared_ptr<capture::AudioCaptureSource>(std::move(capture)));
    }

    Status setEncoder(std::shared_ptr<codec::AudioEncoder> encoder);
    template <
        typename Encoder,
        std::enable_if_t<
            std::is_base_of_v<codec::AudioEncoder, Encoder>,
            int> = 0>
    Status setEncoder(std::unique_ptr<Encoder> encoder) {
        return setEncoder(
            std::shared_ptr<codec::AudioEncoder>(std::move(encoder)));
    }

    void setMuted(bool muted) noexcept {
        muted_.store(muted, std::memory_order_relaxed);
    }

    void onAudioFrame(const codec::AudioRawFrame& frame) override;

private:
    std::shared_ptr<capture::AudioCaptureSource> capture_;
    std::shared_ptr<codec::AudioEncoder> encoder_;
    std::atomic<bool> muted_{false};
};

// Video capture-to-encoder pipeline. The capture source and encoder are
// configured by the caller; open() connects and starts them as one stream.
class CaptureVideoStream final
    : public Stream,
      public capture::VideoFrameSink,
      public std::enable_shared_from_this<CaptureVideoStream> {
public:
    CaptureVideoStream() = default;
    ~CaptureVideoStream() override;

    CaptureVideoStream(const CaptureVideoStream&) = delete;
    CaptureVideoStream& operator=(const CaptureVideoStream&) = delete;

    Status open() override;
    void close() noexcept override;

    Status setCapture(
        std::shared_ptr<capture::VideoCaptureSource> capture);
    template <
        typename Capture,
        std::enable_if_t<
            std::is_base_of_v<capture::VideoCaptureSource, Capture>,
            int> = 0>
    Status setCapture(std::unique_ptr<Capture> capture) {
        return setCapture(
            std::shared_ptr<capture::VideoCaptureSource>(std::move(capture)));
    }

    Status setEncoder(std::shared_ptr<codec::VideoEncoder> encoder);
    template <
        typename Encoder,
        std::enable_if_t<
            std::is_base_of_v<codec::VideoEncoder, Encoder>,
            int> = 0>
    Status setEncoder(std::unique_ptr<Encoder> encoder) {
        return setEncoder(
            std::shared_ptr<codec::VideoEncoder>(std::move(encoder)));
    }

    void setMuted(bool muted) noexcept {
        muted_.store(muted, std::memory_order_relaxed);
    }

    void setPreviewSink(
        std::weak_ptr<codec::IVideoFrameSink> sink) noexcept;

    void onVideoFrame(const codec::VideoRawFrame& frame) override;

private:
    mutable std::mutex previewMutex_;
    std::weak_ptr<codec::IVideoFrameSink> previewSink_;
    std::shared_ptr<capture::VideoCaptureSource> capture_;
    std::shared_ptr<codec::VideoEncoder> encoder_;
    std::atomic<bool> muted_{false};
};

} // namespace iris::media::pipeline
