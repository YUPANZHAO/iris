#pragma once

#include "iris/media.h"
#include "media/engine/media_interfaces.h"

#include <memory>

namespace iris::media {

class FfmpegVideoPipeline {
public:
    FfmpegVideoPipeline();
    ~FfmpegVideoPipeline();

    FfmpegVideoPipeline(const FfmpegVideoPipeline&) = delete;
    FfmpegVideoPipeline& operator=(const FfmpegVideoPipeline&) = delete;

    MediaResult start(const MediaRuntimeConfig& runtime,
                      const MediaSessionConfig& session,
                      IMediaPacketSink& sink,
                      IVideoFrameSink* frameSink = nullptr);
    void stop() noexcept;
    MediaResult onRtpPacket(ByteView packet);
    void setMuted(bool muted);
    MediaResult requestKeyFrame();
    void addStats(MediaStats& stats) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace iris::media

