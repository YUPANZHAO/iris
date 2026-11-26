#include "pipeline/ffmpeg_audio_stream.h"

#include <utility>

namespace iris::media::pipeline {

ffmpeg_audio_stream::~ffmpeg_audio_stream() {
    close();
}

Status ffmpeg_audio_stream::setCapture(
    std::shared_ptr<capture::AudioCaptureSource> capture) {
    if (!capture) {
        return Status::failure("audio capture source is null");
    }
    capture_ = std::move(capture);
    return {};
}

Status ffmpeg_audio_stream::setEncoder(
    std::shared_ptr<codec::AudioEncoder> encoder) {
    if (!encoder) {
        return Status::failure("audio encoder is null");
    }
    encoder_ = std::move(encoder);
    return {};
}

Status ffmpeg_audio_stream::open() {
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

void ffmpeg_audio_stream::close() noexcept {
    if (capture_) {
        capture_->stop();
        capture_->setFrameSink({});
    }
    if (encoder_) {
        (void)encoder_->flush();
        encoder_->setEncodedPacketSink({});
    }
}

void ffmpeg_audio_stream::onAudioFrame(
    const codec::AudioRawFrame& frame) {
    if (encoder_) {
        (void)encoder_->encode(frame);
    }
}

} // namespace iris::media::pipeline
