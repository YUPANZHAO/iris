#include "pipeline/capture_stream.h"

namespace iris::media::pipeline {

CaptureAudioStream::~CaptureAudioStream() {
    close();
}

Status CaptureAudioStream::setCapture(
    std::shared_ptr<capture::AudioCaptureSource> capture) {
    if (!capture) {
        return Status::failure("audio capture source is null");
    }
    capture_ = std::move(capture);
    return {};
}

Status CaptureAudioStream::setEncoder(
    std::shared_ptr<codec::AudioEncoder> encoder) {
    if (!encoder) {
        return Status::failure("audio encoder is null");
    }
    encoder_ = std::move(encoder);
    return {};
}

Status CaptureAudioStream::open() {
    const auto self = weak_from_this();
    if (self.expired()) {
        return Status::failure(
            "audio stream must be owned by a shared pointer to open");
    }
    if (!capture_) {
        return Status::failure("audio stream has no capture source");
    }
    if (!encoder_) {
        return Status::failure("audio stream has no encoder");
    }

    capture_->setFrameSink(self);
    encoder_->setEncodedPacketSink(self);
    auto result = capture_->start();
    if (!result.ok) {
        capture_->setFrameSink({});
        encoder_->setEncodedPacketSink({});
    }
    return result;
}

void CaptureAudioStream::close() noexcept {
    if (capture_) {
        capture_->stop();
        capture_->setFrameSink({});
    }
    if (encoder_) {
        (void)encoder_->flush();
        encoder_->setEncodedPacketSink({});
    }
}

void CaptureAudioStream::onAudioFrame(
    const codec::AudioRawFrame& frame) {
    if (encoder_ && !muted_.load(std::memory_order_relaxed)) {
        (void)encoder_->encode(frame);
    }
}

CaptureVideoStream::~CaptureVideoStream() {
    close();
}

Status CaptureVideoStream::setCapture(
    std::shared_ptr<capture::VideoCaptureSource> capture) {
    if (!capture) {
        return Status::failure("video capture source is null");
    }
    capture_ = std::move(capture);
    return {};
}

Status CaptureVideoStream::setEncoder(
    std::shared_ptr<codec::VideoEncoder> encoder) {
    if (!encoder) {
        return Status::failure("video encoder is null");
    }
    encoder_ = std::move(encoder);
    return {};
}

Status CaptureVideoStream::open() {
    const auto self = weak_from_this();
    if (self.expired()) {
        return Status::failure(
            "video stream must be owned by a shared pointer to open");
    }
    if (!capture_) {
        return Status::failure("video stream has no capture source");
    }
    if (!encoder_) {
        return Status::failure("video stream has no encoder");
    }

    capture_->setFrameSink(self);
    encoder_->setEncodedPacketSink(self);
    auto result = capture_->start();
    if (!result.ok) {
        capture_->setFrameSink({});
        encoder_->setEncodedPacketSink({});
    }
    return result;
}

void CaptureVideoStream::close() noexcept {
    if (capture_) {
        capture_->stop();
        capture_->setFrameSink({});
    }
    if (encoder_) {
        (void)encoder_->flush();
        encoder_->setEncodedPacketSink({});
    }
}

void CaptureVideoStream::onVideoFrame(
    const codec::VideoRawFrame& frame) {
    std::shared_ptr<codec::IVideoFrameSink> preview;
    {
        std::lock_guard<std::mutex> lock(previewMutex_);
        preview = previewSink_.lock();
    }
    if (preview) {
        preview->onVideoFrame(frame);
    }
    if (encoder_ && !muted_.load(std::memory_order_relaxed)) {
        (void)encoder_->encode(frame);
    }
}

void CaptureVideoStream::setPreviewSink(
    std::weak_ptr<codec::IVideoFrameSink> sink) noexcept {
    std::lock_guard<std::mutex> lock(previewMutex_);
    previewSink_ = std::move(sink);
}

} // namespace iris::media::pipeline
