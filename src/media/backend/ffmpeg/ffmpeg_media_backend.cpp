#include "media/backend/ffmpeg/ffmpeg_media_backend.h"

extern "C" {
#include <libavdevice/avdevice.h>
}

#include <utility>

namespace iris::media {

FfmpegMediaBackend::FfmpegMediaBackend(MediaRuntimeConfig runtime)
    : runtime_(std::move(runtime)) {
    static const bool initialized = [] {
        avdevice_register_all();
        return true;
    }();
    (void)initialized;
}

FfmpegMediaBackend::~FfmpegMediaBackend() {
    stop();
}

MediaResult FfmpegMediaBackend::start(
    const MediaSessionConfig& config,
    IMediaPacketSink& packetSink) {
    stop();

    if (config.audioEnabled) {
        audio_ = std::make_unique<FfmpegAudioPipeline>();
        const auto result = audio_->start(runtime_, config, packetSink);
        if (!result.ok) {
            stop();
            return result;
        }
    }
    if (config.videoEnabled) {
        video_ = std::make_unique<FfmpegVideoPipeline>();
        const auto result = video_->start(runtime_, config, packetSink);
        if (!result.ok) {
            stop();
            return result;
        }
    }
    running_ = audio_ || video_;
    return running_
        ? MediaResult::success()
        : MediaResult::failure("no media stream was started");
}

void FfmpegMediaBackend::stop() noexcept {
    running_ = false;
    if (video_) {
        video_->stop();
        video_.reset();
    }
    if (audio_) {
        audio_->stop();
        audio_.reset();
    }
}

MediaResult FfmpegMediaBackend::onReceivedPacket(
    MediaKind kind,
    bool isRtcp,
    ByteView packet) {
    if (!running_) {
        return MediaResult::failure("media backend is not running");
    }
    if (isRtcp) {
        return MediaResult::success();
    }
    if (kind == MediaKind::Audio && audio_) {
        return audio_->onRtpPacket(packet);
    }
    if (kind == MediaKind::Video && video_) {
        return video_->onRtpPacket(packet);
    }
    return MediaResult::failure("no pipeline for RTP payload type");
}

MediaResult FfmpegMediaBackend::setAudioMuted(bool muted) {
    if (audio_) {
        audio_->setMuted(muted);
    }
    return MediaResult::success();
}

MediaResult FfmpegMediaBackend::setVideoMuted(bool muted) {
    if (video_) {
        video_->setMuted(muted);
    }
    return MediaResult::success();
}

MediaResult FfmpegMediaBackend::setTargetBitrate(
    MediaKind,
    std::uint32_t) {
    return MediaResult::failure(
        "dynamic bitrate control is reserved for the WebRTC backend");
}

MediaResult FfmpegMediaBackend::requestVideoKeyFrame() {
    return video_
        ? video_->requestKeyFrame()
        : MediaResult::failure("video pipeline is not running");
}

MediaStats FfmpegMediaBackend::stats() const {
    MediaStats result;
    if (audio_) {
        audio_->addStats(result);
    }
    if (video_) {
        video_->addStats(result);
    }
    return result;
}

} // namespace iris::media

