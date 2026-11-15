#include "core/call_manager.h"

#include "internal/identifier.h"
#include "media/engine/media_engine.h"
#include "sip/state_mapper.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace iris::core {

CallManager::CallManager(RuntimeServices& services,
                         AccountManager& accounts,
                         iris::media::MediaEngine& media)
    : services_(services), accounts_(accounts), media_(media) {
}

CallManager::~CallManager() = default;

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
    auto offer = media_.prepareOutgoing(call_id, local_uri);
    if (!offer.ok) {
        throw std::runtime_error("failed to prepare media: " + offer.error);
    }

    CallSnapshot snapshot;
    snapshot.id = call_id;
    snapshot.accountId = account_id;
    snapshot.remoteUri = destination_uri;
    snapshot.state = CallState::Outgoing;
    snapshot.stateText = "outgoing call initiated";

    calls_.emplace(call_id.value, CallRecord{
        call, snapshot, offer.sdp, false, PendingOperation::None
    });
    call_count_.fetch_add(1, std::memory_order_release);

    try {
        pj::CallOpParam param(true);
        param.opt.audioCount = media_.audioEnabled() ? 1 : 0;
        param.opt.videoCount = media_.videoEnabled() ? 1 : 0;
        param.opt.flag |= PJSUA_CALL_MEDIA_APP_MANAGED;
        param.sdp.wholeSdp = offer.sdp;
        call->makeCall(destination_uri, param);
    } catch (...) {
        media_.stop(call_id);
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
    param.opt.audioCount = media_.audioEnabled() ? 1 : 0;
    param.opt.videoCount = media_.videoEnabled() ? 1 : 0;
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
    auto call = find_call(id);
    auto offer = media_.createReinviteOffer(
        id, MediaDirection::SendOnly, MediaDirection::SendOnly);
    if (!offer.ok) {
        throw std::runtime_error(
            "failed to create hold SDP: " + offer.error);
    }
    pj::CallOpParam param(false);
    param.opt.audioCount = media_.audioEnabled() ? 1 : 0;
    param.opt.videoCount = media_.videoEnabled() ? 1 : 0;
    param.opt.flag |= PJSUA_CALL_MEDIA_APP_MANAGED;
    param.sdp.wholeSdp = std::move(offer.sdp);

    auto iterator = calls_.find(id.value);
    if (iterator == calls_.end()) {
        throw std::out_of_range("unknown call: " + id.value);
    }
    try {
        call->reinvite(param);
        iterator->second.pendingOperation = PendingOperation::Hold;
        iterator->second.snapshot.stateText = "local hold requested";
        publish_call_event(
            iterator->second.snapshot, "local hold requested", 0);
    } catch (...) {
        iterator->second.pendingOperation = PendingOperation::None;
        throw;
    }
}

void CallManager::resume_call(const CallId& id) {
    auto call = find_call(id);
    auto offer = media_.createReinviteOffer(
        id, MediaDirection::SendRecv, MediaDirection::SendRecv);
    if (!offer.ok) {
        throw std::runtime_error(
            "failed to create resume SDP: " + offer.error);
    }
    pj::CallOpParam param(false);
    param.opt.audioCount = media_.audioEnabled() ? 1 : 0;
    param.opt.videoCount = media_.videoEnabled() ? 1 : 0;
    param.opt.flag |= PJSUA_CALL_MEDIA_APP_MANAGED;
    param.sdp.wholeSdp = std::move(offer.sdp);

    auto iterator = calls_.find(id.value);
    if (iterator == calls_.end()) {
        throw std::out_of_range("unknown call: " + id.value);
    }
    try {
        call->reinvite(param);
        iterator->second.pendingOperation = PendingOperation::Resume;
        iterator->second.snapshot.stateText = "resume requested";
        publish_call_event(
            iterator->second.snapshot, "call resume requested", 0);
    } catch (...) {
        iterator->second.pendingOperation = PendingOperation::None;
        throw;
    }
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
        media_.stop(CallId{entry.first});
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
        auto result = media_.createReinviteAnswer(id, offer);
        if (!result.ok) {
            publish_call_event(
                iterator->second.snapshot,
                "remote media offer rejected: " + result.error, 488);
            return;
        }
        const auto applied = media_.applyIncomingOffer(id, offer);
        if (!applied.ok) {
            publish_call_event(
                iterator->second.snapshot,
                "failed to apply remote media offer: " +
                    applied.error,
                488);
            return;
        }
        const auto activity = media_.reconcileActivity(id);
        if (!activity.ok) {
            publish_call_event(
                iterator->second.snapshot,
                "failed to update media activity: " +
                    activity.error,
                500);
            return;
        }
        answer = std::move(result.sdp);
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
    auto answer = offer.empty()
        ? media_.createReinviteOffer(
              id, MediaDirection::SendRecv,
              MediaDirection::SendRecv)
        : media_.createReinviteAnswer(id, offer);
    if (!answer.ok) {
        try {
            pj::CallOpParam reply(false);
            reply.statusCode = PJSIP_SC_NOT_ACCEPTABLE_HERE;
            call->answer(reply);
        } catch (...) {
        }
        publish_call_event(
            iterator->second.snapshot,
            "remote re-INVITE rejected: " + answer.error, 488);
        return;
    }

    try {
        pj::CallOpParam param(false);
        param.statusCode = PJSIP_SC_OK;
        param.opt.audioCount = media_.audioEnabled() ? 1 : 0;
        param.opt.videoCount = media_.videoEnabled() ? 1 : 0;
        param.opt.flag |= PJSUA_CALL_MEDIA_APP_MANAGED;
        param.sdp.wholeSdp = std::move(answer.sdp);
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
        const auto applied = media_.applyIncomingOffer(id, offer);
        if (!applied.ok) {
            publish_call_event(
                iterator->second.snapshot,
                "failed to apply remote re-INVITE media: " +
                    applied.error,
                500);
            return;
        }
        const auto activity = media_.reconcileActivity(id);
        if (!activity.ok) {
            publish_call_event(
                iterator->second.snapshot,
                "failed to update media activity: " +
                    activity.error,
                500);
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

    auto id = snapshot.id;
    std::string answerSdp;
    const auto local_uri = accounts_.account_uri(snapshot.accountId);
    auto answer = media_.prepareAnswer(id, local_uri, remoteSdp);
    if (answer.ok) {
        answerSdp = std::move(answer.sdp);
    }
    const bool remoteMediaApplied = !answerSdp.empty();
    calls_[id.value] = CallRecord{
        std::move(call), snapshot, std::move(answerSdp),
        remoteMediaApplied, PendingOperation::None
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
    auto result = media_.applyRemoteAnswer(id, sdp);
    if (!result.ok) {
        iterator->second.pendingOperation = PendingOperation::None;
        publish_call_event(
            iterator->second.snapshot,
            "remote media negotiation failed: " + result.error, 488);
        return;
    }
    iterator->second.remoteMediaApplied = true;
    if (iterator->second.pendingOperation ==
        PendingOperation::Hold) {
        const auto suspended = media_.suspend(id);
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
        const auto started = media_.start(id);
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
        result = media_.reconcileActivity(id);
        if (!result.ok) {
            publish_call_event(
                iterator->second.snapshot,
                "failed to start media: " + result.error, 500);
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

    if (iterator != calls_.end()) {
        iterator->second.snapshot = snapshot;
    }

    publish_call_event(snapshot, snapshot.stateText, snapshot.statusCode);

    if (state == CallState::Disconnected &&
        iterator != calls_.end()) {
        media_.stop(id);
        calls_.erase(iterator);
        {
            std::lock_guard<std::mutex> lock(call_count_mutex_);
            call_count_.fetch_sub(1, std::memory_order_release);
        }
        call_count_condition_.notify_all();
    } else if (state == CallState::Confirmed &&
               iterator != calls_.end()) {
        if (iterator->second.remoteMediaApplied) {
            auto result = media_.reconcileActivity(id);
            if (!result.ok) {
                publish_call_event(
                    snapshot, "failed to start media: " + result.error, 500);
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

} // namespace iris::core

