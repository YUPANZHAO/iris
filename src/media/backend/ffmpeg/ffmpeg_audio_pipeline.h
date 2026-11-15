#pragma once

#include "iris/media.h"
#include "media/engine/media_interfaces.h"

#include <cstdint>
#include <memory>

namespace iris::media {

class FfmpegAudioPipeline {
public:
    FfmpegAudioPipeline();
    ~FfmpegAudioPipeline();

    FfmpegAudioPipeline(const FfmpegAudioPipeline&) = delete;
    FfmpegAudioPipeline& operator=(const FfmpegAudioPipeline&) = delete;

    MediaResult start(const MediaRuntimeConfig& runtime,
                      const MediaSessionConfig& session,
                      IMediaPacketSink& sink);
    void stop() noexcept;
    MediaResult onRtpPacket(ByteView packet);
    void setMuted(bool muted);
    void addStats(MediaStats& stats) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace iris::media

