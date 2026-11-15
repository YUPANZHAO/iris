#pragma once

#include "iris/config.h"
#include "iris/media.h"
#include "media/engine/media_interfaces.h"
#include "media/sdp/codec_registry.h"
#include "media/sdp/sdp_controller.h"

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace iris::media {

class PjIoqueueService;
class VideoFrameSinkHub;

class MediaEngine {
public:
    MediaEngine(MediaRuntimeConfig runtime,
                PjIoqueueService& io_service);
    ~MediaEngine();

    MediaEngine(const MediaEngine&) = delete;
    MediaEngine& operator=(const MediaEngine&) = delete;

    SdpResult prepareOutgoing(const CallId& callId,
                              const std::string& localUri);
    SdpResult prepareAnswer(const CallId& callId,
                            const std::string& localUri,
                            const std::string& remoteOffer);
    SdpResult createReinviteOffer(
        const CallId& callId,
        MediaDirection audioDirection,
        MediaDirection videoDirection);
    SdpResult createReinviteAnswer(
        const CallId& callId,
        const std::string& remoteOffer);
    MediaResult applyIncomingOffer(const CallId& callId,
                                   const std::string& remoteOffer);
    MediaResult applyRemoteAnswer(const CallId& callId,
                                  const std::string& remoteAnswer);
    MediaResult start(const CallId& callId);
    MediaResult suspend(const CallId& callId);
    MediaResult reconcileActivity(const CallId& callId);
    void stop(const CallId& callId) noexcept;
    void stopAll() noexcept;

    MediaResult setAudioMuted(const CallId& callId, bool muted);
    MediaResult setVideoMuted(const CallId& callId, bool muted);
    void setInputSource(MediaKind kind, const std::string& source);
    std::string inputSource(MediaKind kind) const;
    void setVideoFrameCallback(VideoFrameCallback callback);
    MediaStats stats(const CallId& callId) const;
    bool audioEnabled() const noexcept { return runtime_.audioEnabled; }
    bool videoEnabled() const noexcept { return runtime_.videoEnabled; }

private:
    struct Session;
    Session& ensureSession(const CallId& callId);
    Session* findSession(const CallId& callId) const;

    MediaRuntimeConfig runtime_;
    PjIoqueueService& io_service_;
    CodecRegistry codec_registry_;
    std::shared_ptr<VideoFrameSinkHub> frame_sink_;
    std::unordered_map<std::string, std::unique_ptr<Session>> sessions_;
};

} // namespace iris::media

