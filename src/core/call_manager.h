#pragma once

#include "core/account_manager.h"
#include "core/runtime_services.h"
#include "iris/media.h"
#include "sip/pj_call.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace iris::core {

class MediaSession;

class CallManager final : public sip::CallCallbackHandler {
public:
    CallManager(RuntimeServices& services,
                AccountManager& accounts,
                MediaRuntimeConfig mediaConfig);
    ~CallManager() override;

    CallManager(const CallManager&) = delete;
    CallManager& operator=(const CallManager&) = delete;

    CallId start_call(const AccountId& account_id,
                      const std::string& destination_uri);
    void answer_call(const CallId& id);
    void reject_call(const CallId& id);
    void hangup_call(const CallId& id);
    void hold_call(const CallId& id);
    void resume_call(const CallId& id);

    std::vector<CallSnapshot> calls() const;
    bool has_active_call_for(const AccountId& account_id) const;
    void set_media_input(MediaKind kind, const std::string& source);
    std::string media_input(MediaKind kind) const;
    void set_audio_frame_callback(AudioFrameCallback callback);
    void set_video_frame_callback(VideoFrameCallback callback);
    media::Status set_media_muted(const CallId& id,
                                  MediaKind kind,
                                  bool muted);
    MediaStats media_stats(const CallId& id) const;

    bool audio_enabled() const noexcept {
        return mediaConfig_.audioEnabled;
    }
    bool video_enabled() const noexcept {
        return mediaConfig_.videoEnabled;
    }

    // Called from a PJSUA2 callback thread. The call adapter is created
    // immediately, while map and lifecycle mutation is posted to the core.
    void accept_incoming_call(sip::PjAccount& account,
                              const AccountId& account_id,
                              int native_call_id,
                              const std::string& remoteSdp);

    void hangup_all() noexcept;
    void clear() noexcept;
    bool wait_until_empty(std::chrono::milliseconds timeout);

private:
    enum class PendingOperation {
        None,
        Hold,
        Resume,
    };

    struct CallRecord {
        std::shared_ptr<sip::PjCall> adapter;
        CallSnapshot snapshot;
        std::string answerSdp;
        std::shared_ptr<MediaSession> media;
        bool remoteMediaApplied = false;
        PendingOperation pendingOperation = PendingOperation::None;
    };

    struct MediaCallbackState;

    void call_state_changed(CallId id,
                            const pj::CallInfo& info) override;
    void remote_sdp_received(CallId id,
                             const std::string& sdp) override;
    bool remote_reinvite_received(
        CallId id, const std::string& offer) override;
    std::string negotiate_remote_offer(
        CallId id, const std::string& offer) override;
    void call_offer_failed(CallId id, int statusCode) override;
    void adopt_incoming_call(std::shared_ptr<sip::PjCall> call,
                             const pj::CallInfo& info,
                             std::string remoteSdp);
    void handle_remote_reinvite(const CallId& id,
                                const std::string& offer);
    void apply_remote_sdp(const CallId& id, const std::string& sdp);
    void update_call(const CallId& id, const pj::CallInfo& info);

    std::shared_ptr<sip::PjCall> find_call(const CallId& id);
    void publish_call_event(const CallSnapshot& snapshot,
                            const std::string& reason,
                            int status_code);
    std::shared_ptr<MediaSession> create_media(
        const CallId& id,
        const std::string& localUri);

    RuntimeServices& services_;
    AccountManager& accounts_;
    MediaRuntimeConfig mediaConfig_;
    std::shared_ptr<MediaCallbackState> mediaCallbacks_;
    std::unordered_map<std::string, CallRecord> calls_;
    std::atomic<std::uint64_t> next_call_id_{0};
    std::atomic<std::size_t> call_count_{0};
    mutable std::mutex call_count_mutex_;
    mutable std::condition_variable call_count_condition_;
};

} // namespace iris::core

