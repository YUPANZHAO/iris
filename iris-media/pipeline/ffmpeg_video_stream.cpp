#include "pipeline/ffmpeg_video_stream.h"

#include <utility>

namespace iris::media::pipeline {

ffmpeg_video_stream::~ffmpeg_video_stream() {
    close();
}

Status ffmpeg_video_stream::setCapture(
    std::shared_ptr<capture::VideoCaptureSource> capture) {
    if (!capture) {
        return Status::failure("video capture source is null");
    }
    capture_ = std::move(capture);
    return {};
}

Status ffmpeg_video_stream::setEncoder(
    std::shared_ptr<codec::VideoEncoder> encoder) {
    if (!encoder) {
        return Status::failure("video encoder is null");
    }
    encoder_ = std::move(encoder);
    return {};
}

Status ffmpeg_video_stream::open() {
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

void ffmpeg_video_stream::close() noexcept {
    if (capture_) {
        capture_->stop();
        capture_->setFrameSink({});
    }
    if (encoder_) {
        (void)encoder_->flush();
        encoder_->setEncodedPacketSink({});
    }
}

void ffmpeg_video_stream::onVideoFrame(
    const codec::VideoRawFrame& frame) {
    if (encoder_) {
        (void)encoder_->encode(frame);
    }
}

} // namespace iris::media::pipeline
