#pragma once

#include "iris/media.h"

#include <functional>
#include <memory>
#include <string>

namespace iris::core {

// Owns the media state for one call. CallManager already owns the call map, so
// this type deliberately has no session registry of its own.
class MediaSession {
public:
    using AudioCallbackProvider =
        std::function<AudioFrameCallback()>;
    using VideoCallbackProvider =
        std::function<VideoFrameCallback()>;

    MediaSession(CallId callId,
                 std::string localUri,
                 MediaRuntimeConfig runtime,
                 AudioCallbackProvider audioCallbacks,
                 VideoCallbackProvider videoCallbacks);
    ~MediaSession();

    MediaSession(const MediaSession&) = delete;
    MediaSession& operator=(const MediaSession&) = delete;

    media::Status prepareOutgoing(std::string* sdp);
    media::Status prepareAnswer(const std::string& remoteOffer,
                                std::string* sdp);
    media::Status applyRemoteAnswer(const std::string& remoteAnswer);
    media::Status start();
    media::Status suspend();
    void stop() noexcept;

    media::Status setMuted(MediaKind kind, bool muted);
    MediaStats stats() const;

    media::Status createReinviteOffer(
        MediaDirection audioDirection,
        MediaDirection videoDirection,
        std::string* sdp);
    media::Status createReinviteAnswer(
        const std::string& remoteOffer,
        std::string* sdp);
    media::Status applyIncomingOffer(const std::string& remoteOffer);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace iris::core
