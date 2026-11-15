#include "core/call_manager.h"

#include "internal/identifier.h"
#include "sip/state_mapper.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace iris::core {

CallManager::CallManager(RuntimeServices& services,
                         AccountManager& accounts)
    : services_(services), accounts_(accounts) {
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

    CallSnapshot snapshot;
    snapshot.id = call_id;
    snapshot.accountId = account_id;
    snapshot.remoteUri = destination_uri;
    snapshot.state = CallState::Outgoing;
    snapshot.stateText = "outgoing call initiated";

    calls_.emplace(call_id.value, CallRecord{call, snapshot});

    try {
        pj::CallOpParam param(true);
        param.opt.audioCount = 0;
        param.opt.videoCount = 0;
        call->makeCall(destination_uri, param);
    } catch (...) {
        calls_.erase(call_id.value);
        throw;
    }

    publish_call_event(
        snapshot, "outgoing call initiated", 0);
    return call_id;
}

void CallManager::answer_call(const CallId& id) {
    auto call = find_call(id);
    pj::CallOpParam param(false);
    param.statusCode = PJSIP_SC_OK;
    param.opt.audioCount = 0;
    param.opt.videoCount = 0;
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
    pj::CallOpParam param(false);
    call->setHold(param);

    auto iterator = calls_.find(id.value);
    if (iterator != calls_.end()) {
        iterator->second.snapshot.state = CallState::LocallyHeld;
        iterator->second.snapshot.stateText = "locally held";
        publish_call_event(
            iterator->second.snapshot, "local hold requested", 0);
    }
}

void CallManager::resume_call(const CallId& id) {
    auto call = find_call(id);
    pj::CallOpParam param(false);
    param.opt.flag = PJSUA_CALL_UNHOLD;
    call->reinvite(param);

    auto iterator = calls_.find(id.value);
    if (iterator != calls_.end()) {
        iterator->second.snapshot.state = CallState::Confirmed;
        iterator->second.snapshot.stateText = "resume requested";
        publish_call_event(
            iterator->second.snapshot, "call resume requested", 0);
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
                                       int native_call_id) {
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

    if (!services_.post_callback([this, call, info] {
            adopt_incoming_call(call, info);
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
    calls_.clear();
}

void CallManager::call_state_changed(CallId id,
                                     const pj::CallInfo& info) {
    services_.post_callback([this, id, info] {
        update_call(id, info);
    });
}

void CallManager::adopt_incoming_call(
    std::shared_ptr<sip::PjCall> call,
    const pj::CallInfo& info) {
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
    calls_[id.value] = CallRecord{std::move(call), snapshot};

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
        calls_.erase(iterator);
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

