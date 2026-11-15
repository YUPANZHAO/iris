#pragma once

#include "iris/config.h"
#include "media/backend/ffmpeg/ffmpeg_audio_pipeline.h"
#include "media/backend/ffmpeg/ffmpeg_video_pipeline.h"
#include "media/engine/media_interfaces.h"

#include <memory>

namespace iris::media {

class FfmpegMediaBackend final : public IMediaBackend {
public:
    explicit FfmpegMediaBackend(MediaRuntimeConfig runtime);
    ~FfmpegMediaBackend() override;

    MediaResult start(const MediaSessionConfig& config,
                      IMediaPacketSink& packetSink) override;
    void stop() noexcept override;
    MediaResult onReceivedPacket(MediaKind kind,
                                 bool isRtcp,
                                 ByteView packet) override;

    MediaResult setAudioMuted(bool muted) override;
    MediaResult setVideoMuted(bool muted) override;
    MediaResult setTargetBitrate(MediaKind kind,
                                 std::uint32_t bitrateKbps) override;
    MediaResult requestVideoKeyFrame() override;
    MediaStats stats() const override;

private:
    MediaRuntimeConfig runtime_;
    std::unique_ptr<FfmpegAudioPipeline> audio_;
    std::unique_ptr<FfmpegVideoPipeline> video_;
    bool running_ = false;
};

} // namespace iris::media

