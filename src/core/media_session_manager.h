#pragma once

#include "iris/config.h"
#include "iris/media.h"

#include <memory>
#include <string>
#include <utility>

namespace iris::core {

struct MediaResult {
    bool ok = false;
    std::string error;

    static MediaResult success() {
        return {true, {}};
    }

    static MediaResult failure(std::string message) {
        return {false, std::move(message)};
    }
};

struct SdpResult {
    bool ok = false;
    std::string sdp;
    std::string error;

    static SdpResult success(std::string value) {
        return {true, std::move(value), {}};
    }

    static SdpResult failure(std::string message) {
        return {false, {}, std::move(message)};
    }
};

// Compatibility facade used by the SIP layer. Internally it uses only the v2
// media implementation under src/media.
class MediaSessionManager {
public:
    explicit MediaSessionManager(MediaRuntimeConfig runtime);
    ~MediaSessionManager();

    MediaSessionManager(const MediaSessionManager&) = delete;
    MediaSessionManager& operator=(const MediaSessionManager&) = delete;

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

    bool audioEnabled() const noexcept {
        return runtime_.audioEnabled;
    }

    bool videoEnabled() const noexcept {
        return runtime_.videoEnabled;
    }

private:
    struct Impl;
    MediaRuntimeConfig runtime_;
    std::unique_ptr<Impl> impl_;
};

} // namespace iris::core
