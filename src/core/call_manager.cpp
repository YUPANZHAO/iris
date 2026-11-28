#include "core/call_manager.h"

#include "core/media_session.h"
#include "internal/identifier.h"
#include "sip/state_mapper.h"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <utility>

namespace iris::core {
namespace {

std::uint64_t now_ms() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

} // namespace

struct CallManager::MediaCallbackState {
    void set(VideoFrameCallback callback) {
        std::lock_guard<std::mutex> lock(mutex);
        value = std::move(callback);
    }

    VideoFrameCallback get() const {
        std::lock_guard<std::mutex> lock(mutex);
        return value;
    }

    void setAudio(AudioFrameCallback callback) {
        std::lock_guard<std::mutex> lock(mutex);
        audioValue = std::move(callback);
    }

    AudioFrameCallback getAudio() const {
        std::lock_guard<std::mutex> lock(mutex);
        return audioValue;
    }

    mutable std::mutex mutex;
    VideoFrameCallback value;
    AudioFrameCallback audioValue;
};

CallManager::CallManager(RuntimeServices& services,
                         AccountManager& accounts,
                         MediaRuntimeConfig mediaConfig)
    : services_(services),
      accounts_(accounts),
      mediaConfig_(std::move(mediaConfig)),
      mediaCallbacks_(std::make_shared<MediaCallbackState>()) {
}

CallManager::~CallManager() = default;

std::shared_ptr<MediaSession> CallManager::create_media(
    const CallId& id,
    const std::string& localUri) {
    const auto callbacks = mediaCallbacks_;
    return std::make_shared<MediaSession>(
        id,
        localUri,
        mediaConfig_,
        [callbacks] {
            return callbacks->getAudio();
        },
        [callbacks] {
            return callbacks->get();
        });
}

CallId CallManager::start_call(const AccountId& account_id,
                               const std::string& destination_uri) {
    if (destination_uri.empty()) {
        throw std::invalid_argument("destination URI must not be empty");
    }

    auto& account = accounts_.account(account_id);
    auto call_id = CallId{
        internal::make_identifier("call", ++next_call_id_)
    };
    auto call = std::make_shared<sip::PjCall>(
        account, *this, call_id, account_id);

    const auto local_uri = accounts_.account_uri(account_id);
    auto media = create_media(call_id, local_uri);
    std::string offerSdp;
    const auto offer = media->prepareOutgoing(&offerSdp);
    if (!offer.ok) {
        throw std::runtime_error(
            "failed to prepare media: " + offer.message);
    }

    CallSnapshot snapshot;
    snapshot.id = call_id;
    snapshot.accountId = account_id;
    snapshot.remoteUri = destination_uri;
    snapshot.state = CallState::Outgoing;
    snapshot.stateText = "outgoing call initiated";
    snapshot.createdAtMs = now_ms();

    calls_.emplace(call_id.value, CallRecord{
        call,
        snapshot,
        offerSdp,
        std::move(media),
        false,
        PendingOperation::None,
    });
    call_count_.fetch_add(1, std::memory_order_release);

    try {
        pj::CallOpParam param(true);
        param.opt.audioCount = mediaConfig_.audioEnabled ? 1 : 0;
        param.opt.videoCount = mediaConfig_.videoEnabled ? 1 : 0;
        param.opt.flag |= PJSUA_CALL_MEDIA_APP_MANAGED;
        param.sdp.wholeSdp = offerSdp;
        call->makeCall(destination_uri, param);
    } catch (...) {
        calls_.erase(call_id.value);
        {
            std::lock_guard<std::mutex> lock(call_count_mutex_);
            call_count_.fetch_sub(1, std::memory_order_release);
        }
        call_count_condition_.notify_all();
        throw;
    }

    publish_call_event(
        snapshot, "outgoing call initiated", 0);
    return call_id;
}

void CallManager::answer_call(const CallId& id) {
    auto iterator = calls_.find(id.value);
    if (iterator == calls_.end()) {
        throw std::out_of_range("unknown or terminated call: " + id.value);
    }
    if (iterator->second.answerSdp.empty()) {
        throw std::runtime_error("media answer is not prepared");
    }
    auto call = iterator->second.adapter;
    pj::CallOpParam param(true);
    param.statusCode = PJSIP_SC_OK;
    param.opt.audioCount = mediaConfig_.audioEnabled ? 1 : 0;
    param.opt.videoCount = mediaConfig_.videoEnabled ? 1 : 0;
    param.opt.flag |= PJSUA_CALL_MEDIA_APP_MANAGED;
    param.sdp.wholeSdp = iterator->second.answerSdp;
    call->answer(param);
}

void CallManager::reject_call(const CallId& id) {
    auto call = find_call(id);
    pj::CallOpParam param(false);
    param.statusCode = PJSIP_SC_BUSY_HERE;
    call->answer(param);
}

void CallManager::hangup_call(const CallId& id) {
    auto call = find_call(id);
    pj::CallOpParam param(false);
    call->hangup(param);
}

void CallManager::hold_call(const CallId& id) {
    auto iterator = calls_.find(id.value);
    if (iterator == calls_.end()) {
        throw std::out_of_range("unknown call: " + id.value);
    }
    const auto result = iterator->second.media->suspend();
    if (!result.ok) {
        throw std::runtime_error(
            "failed to hold media: " + result.message);
    }
    iterator->second.pendingOperation = PendingOperation::None;
    iterator->second.snapshot.state = CallState::LocallyHeld;
    iterator->second.snapshot.stateText = "locally held";
    publish_call_event(
        iterator->second.snapshot, "locally held", 0);
}

void CallManager::resume_call(const CallId& id) {
    auto iterator = calls_.find(id.value);
    if (iterator == calls_.end()) {
        throw std::out_of_range("unknown call: " + id.value);
    }
    const auto result = iterator->second.media->start();
    if (!result.ok) {
        throw std::runtime_error(
            "failed to resume media: " + result.message);
    }
    iterator->second.pendingOperation = PendingOperation::None;
    iterator->second.snapshot.state = CallState::Confirmed;
    iterator->second.snapshot.stateText = "resumed";
    publish_call_event(
        iterator->second.snapshot, "resumed", 0);
}

std::vector<CallSnapshot> CallManager::calls() const {
    std::vector<CallSnapshot> snapshots;
    snapshots.reserve(calls_.size());
    for (const auto& entry : calls_) {
        snapshots.push_back(entry.second.snapshot);
    }
    std::sort(snapshots.begin(), snapshots.end(),
              [](const auto& lhs, const auto& rhs) {
                  return lhs.id.value < rhs.id.value;
              });
    return snapshots;
}

bool CallManager::has_active_call_for(const AccountId& account_id) const {
    for (const auto& entry : calls_) {
        if (entry.second.snapshot.accountId == account_id &&
            entry.second.snapshot.state != CallState::Disconnected) {
            return true;
        }
    }
    return false;
}

void CallManager::accept_incoming_call(sip::PjAccount& account,
                                       const AccountId& account_id,
                                       int native_call_id,
                                       const std::string& remoteSdp) {
    std::shared_ptr<sip::PjCall> call;
    try {
        auto call_id = CallId{
            internal::make_identifier("call", ++next_call_id_)
        };
        call = std::make_shared<sip::PjCall>(
            account, *this, call_id, account_id, native_call_id);
    } catch (...) {
        return;
    }

    pj::CallInfo info;
    try {
        info = call->getInfo();
    } catch (...) {
        return;
    }

    if (!services_.post_callback([this, call, info, remoteSdp] {
            adopt_incoming_call(call, info, remoteSdp);
        })) {
        try {
            pj::CallOpParam reply(false);
            reply.statusCode = PJSIP_SC_SERVICE_UNAVAILABLE;
            call->answer(reply);
        } catch (...) {
        }
    }
}

void CallManager::hangup_all() noexcept {
    for (const auto& entry : calls_) {
        try {
            pj::CallOpParam param(false);
            entry.second.adapter->hangup(param);
        } catch (...) {
        }
    }
}

void CallManager::clear() noexcept {
    for (const auto& entry : calls_) {
        if (entry.second.media) {
            entry.second.media->stop();
        }
    }
    calls_.clear();
    {
        std::lock_guard<std::mutex> lock(call_count_mutex_);
        call_count_.store(0, std::memory_order_release);
    }
    call_count_condition_.notify_all();
}

bool CallManager::wait_until_empty(
    std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(call_count_mutex_);
    return call_count_condition_.wait_for(
        lock, timeout, [this] {
            return call_count_.load(std::memory_order_acquire) == 0;
        });
}

void CallManager::call_state_changed(CallId id,
                                     const pj::CallInfo& info) {
    services_.post_callback([this, id, info] {
        update_call(id, info);
    });
}

void CallManager::remote_sdp_received(CallId id,
                                      const std::string& sdp) {
    services_.post_callback([this, id, sdp] {
        apply_remote_sdp(id, sdp);
    });
}

bool CallManager::remote_reinvite_received(
    CallId id,
    const std::string& offer) {
    return services_.post_callback([this, id, offer] {
        handle_remote_reinvite(id, offer);
    });
}

std::string CallManager::negotiate_remote_offer(
    CallId id,
    const std::string& offer) {
    std::string answer;
    services_.invoke_on_core([this, id, offer, &answer] {
        const auto iterator = calls_.find(id.value);
        if (iterator == calls_.end()) {
            return;
        }
        std::string answerSdp;
        const auto result =
            iterator->second.media->createReinviteAnswer(
                offer, &answerSdp);
        if (!result.ok) {
            publish_call_event(
                iterator->second.snapshot,
                "remote media offer rejected: " + result.message, 488);
            return;
        }
        const auto applied =
            iterator->second.media->applyIncomingOffer(offer);
        if (!applied.ok) {
            publish_call_event(
                iterator->second.snapshot,
                "failed to apply remote media offer: " +
                    applied.message,
                488);
            return;
        }
        answer = std::move(answerSdp);
    });
    return answer;
}

void CallManager::call_offer_failed(CallId id, int statusCode) {
    services_.post_callback([this, id, statusCode] {
        auto iterator = calls_.find(id.value);
        if (iterator == calls_.end() ||
            iterator->second.pendingOperation ==
                PendingOperation::None) {
            return;
        }
        iterator->second.pendingOperation = PendingOperation::None;
        iterator->second.snapshot.stateText =
            "media renegotiation failed";
        publish_call_event(
            iterator->second.snapshot,
            "media renegotiation failed", statusCode);
    });
}

void CallManager::handle_remote_reinvite(
    const CallId& id,
    const std::string& offer) {
    auto iterator = calls_.find(id.value);
    if (iterator == calls_.end()) {
        return;
    }
    auto call = iterator->second.adapter;
    std::string answerSdp;
    const auto answer = offer.empty()
        ? iterator->second.media->createReinviteOffer(
              MediaDirection::SendRecv,
              MediaDirection::SendRecv,
              &answerSdp)
        : iterator->second.media->createReinviteAnswer(
              offer, &answerSdp);
    if (!answer.ok) {
        try {
            pj::CallOpParam reply(false);
            reply.statusCode = PJSIP_SC_NOT_ACCEPTABLE_HERE;
            call->answer(reply);
        } catch (...) {
        }
        publish_call_event(
            iterator->second.snapshot,
            "remote re-INVITE rejected: " + answer.message, 488);
        return;
    }

    try {
        pj::CallOpParam param(false);
        param.statusCode = PJSIP_SC_OK;
        param.opt.audioCount = mediaConfig_.audioEnabled ? 1 : 0;
        param.opt.videoCount = mediaConfig_.videoEnabled ? 1 : 0;
        param.opt.flag |= PJSUA_CALL_MEDIA_APP_MANAGED;
        param.sdp.wholeSdp = std::move(answerSdp);
        call->answer(param);
    } catch (const pj::Error& error) {
        publish_call_event(
            iterator->second.snapshot,
            "failed to answer remote re-INVITE: " +
                std::string(error.info()),
            500);
        return;
    }

    if (!offer.empty()) {
        const auto applied =
            iterator->second.media->applyIncomingOffer(offer);
        if (!applied.ok) {
            publish_call_event(
                iterator->second.snapshot,
                "failed to apply remote re-INVITE media: " +
                    applied.message,
                500);
            return;
        }
    }
}

void CallManager::adopt_incoming_call(
    std::shared_ptr<sip::PjCall> call,
    const pj::CallInfo& info,
    std::string remoteSdp) {
    if (!call) {
        return;
    }

    CallSnapshot snapshot;
    snapshot.id = call->id();
    snapshot.accountId = call->account_id();
    snapshot.remoteUri = info.remoteUri;
    snapshot.state = CallState::Incoming;
    snapshot.stateText = info.stateText;
    snapshot.statusCode = static_cast<int>(info.lastStatusCode);
    snapshot.createdAtMs = now_ms();

    auto id = snapshot.id;
    std::string answerSdp;
    const auto local_uri = accounts_.account_uri(snapshot.accountId);
    auto media = create_media(id, local_uri);
    const auto answer =
        media->prepareAnswer(remoteSdp, &answerSdp);
    if (!answer.ok) {
        answerSdp.clear();
    }
    const bool remoteMediaApplied = !answerSdp.empty();
    calls_[id.value] = CallRecord{
        std::move(call),
        snapshot,
        std::move(answerSdp),
        std::move(media),
        remoteMediaApplied,
        PendingOperation::None,
    };
    call_count_.fetch_add(1, std::memory_order_release);

    Event event;
    event.type = EventType::IncomingCall;
    event.accountId = snapshot.accountId;
    event.callId = snapshot.id;
    event.callState = snapshot.state;
    event.statusCode = snapshot.statusCode;
    event.reason = snapshot.stateText;
    event.remoteUri = snapshot.remoteUri;
    services_.publish_event(std::move(event));
}

void CallManager::apply_remote_sdp(const CallId& id,
                                   const std::string& sdp) {
    auto iterator = calls_.find(id.value);
    if (iterator == calls_.end()) {
        return;
    }
    auto result = iterator->second.media->applyRemoteAnswer(sdp);
    if (!result.ok) {
        iterator->second.pendingOperation = PendingOperation::None;
        publish_call_event(
            iterator->second.snapshot,
            "remote media negotiation failed: " + result.message, 488);
        return;
    }
    iterator->second.remoteMediaApplied = true;
    if (iterator->second.pendingOperation ==
        PendingOperation::Hold) {
        const auto suspended = iterator->second.media->suspend();
        iterator->second.pendingOperation = PendingOperation::None;
        iterator->second.snapshot.state = CallState::LocallyHeld;
        iterator->second.snapshot.stateText = suspended.ok
            ? "locally held"
            : "hold negotiated; media stop failed";
        publish_call_event(
            iterator->second.snapshot,
            iterator->second.snapshot.stateText, 0);
        return;
    }
    if (iterator->second.pendingOperation ==
        PendingOperation::Resume) {
        const auto started = iterator->second.media->start();
        iterator->second.pendingOperation = PendingOperation::None;
        iterator->second.snapshot.state = CallState::Confirmed;
        iterator->second.snapshot.stateText = started.ok
            ? "resumed"
            : "resume negotiated; media start failed";
        publish_call_event(
            iterator->second.snapshot,
            iterator->second.snapshot.stateText,
            started.ok ? 0 : 500);
        return;
    }
    if (iterator->second.snapshot.state == CallState::Confirmed) {
        result = iterator->second.media->start();
        if (!result.ok) {
            publish_call_event(
                iterator->second.snapshot,
                "failed to start media: " + result.message, 500);
        }
    }
}

void CallManager::update_call(const CallId& id,
                              const pj::CallInfo& info) {
    const auto state = sip::map_call_state(info.state);
    CallSnapshot snapshot;

    auto iterator = calls_.find(id.value);
    if (iterator != calls_.end()) {
        snapshot = iterator->second.snapshot;
    } else {
        snapshot.id = id;
        snapshot.state = state;
    }

    snapshot.remoteUri = info.remoteUri;
    snapshot.state = state;
    snapshot.stateText = info.stateText;
    snapshot.statusCode = static_cast<int>(info.lastStatusCode);
    if (state == CallState::Confirmed &&
        snapshot.connectedAtMs == 0) {
        snapshot.connectedAtMs = now_ms();
    }

    if (iterator != calls_.end()) {
        iterator->second.snapshot = snapshot;
    }

    publish_call_event(snapshot, snapshot.stateText, snapshot.statusCode);

    if (state == CallState::Disconnected &&
        iterator != calls_.end()) {
        if (iterator->second.media) {
            iterator->second.media->stop();
        }
        calls_.erase(iterator);
        {
            std::lock_guard<std::mutex> lock(call_count_mutex_);
            call_count_.fetch_sub(1, std::memory_order_release);
        }
        call_count_condition_.notify_all();
    } else if (state == CallState::Confirmed &&
               iterator != calls_.end()) {
        if (iterator->second.remoteMediaApplied) {
            auto result = iterator->second.media->start();
            if (!result.ok) {
                publish_call_event(
                    snapshot,
                    "failed to start media: " + result.message,
                    500);
            }
        }
    }
}

std::shared_ptr<sip::PjCall> CallManager::find_call(const CallId& id) {
    auto iterator = calls_.find(id.value);
    if (iterator == calls_.end()) {
        throw std::out_of_range(
            "unknown or terminated call: " + id.value);
    }
    return iterator->second.adapter;
}

void CallManager::publish_call_event(const CallSnapshot& snapshot,
                                     const std::string& reason,
                                     int status_code) {
    Event event;
    event.type = EventType::CallStateChanged;
    event.accountId = snapshot.accountId;
    event.callId = snapshot.id;
    event.callState = snapshot.state;
    event.statusCode = status_code;
    event.reason = reason;
    event.remoteUri = snapshot.remoteUri;
    services_.publish_event(std::move(event));
}

void CallManager::set_media_input(
    MediaKind kind,
    const std::string& source) {
    if (kind == MediaKind::Audio) {
        mediaConfig_.audioInput = source;
    } else if (kind == MediaKind::Video) {
        mediaConfig_.videoInput = source;
    }
}

std::string CallManager::media_input(MediaKind kind) const {
    return kind == MediaKind::Audio
        ? mediaConfig_.audioInput
        : mediaConfig_.videoInput;
}

void CallManager::set_video_frame_callback(
    VideoFrameCallback callback) {
    mediaCallbacks_->set(std::move(callback));
}

void CallManager::set_audio_frame_callback(
    AudioFrameCallback callback) {
    mediaCallbacks_->setAudio(std::move(callback));
}

media::Status CallManager::set_media_muted(
    const CallId& id,
    MediaKind kind,
    bool muted) {
    const auto iterator = calls_.find(id.value);
    if (iterator == calls_.end() || !iterator->second.media) {
        return media::Status::failure("media session does not exist");
    }
    return iterator->second.media->setMuted(kind, muted);
}

MediaStats CallManager::media_stats(const CallId& id) const {
    const auto iterator = calls_.find(id.value);
    return iterator == calls_.end() || !iterator->second.media
        ? MediaStats{}
        : iterator->second.media->stats();
}

} // namespace iris::core

