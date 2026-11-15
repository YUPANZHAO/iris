#pragma once

#include "iris/types.h"

#include <cstdint>
#include <string>

namespace iris {

enum class EventType {
    AccountStateChanged,
    IncomingCall,
    CallStateChanged,
    MediaStateChanged,
};

struct Event {
    std::uint64_t sequence = 0;
    EventType type = EventType::AccountStateChanged;
    AccountId accountId;
    CallId callId;
    AccountState accountState = AccountState::Created;
    CallState callState = CallState::Outgoing;
    int statusCode = 0;
    std::string reason;
    std::string remoteUri;
};

inline const char* to_string(EventType type) noexcept {
    switch (type) {
    case EventType::AccountStateChanged: return "account-state";
    case EventType::IncomingCall: return "incoming-call";
    case EventType::CallStateChanged: return "call-state";
    case EventType::MediaStateChanged: return "media-state";
    }
    return "unknown";
}

} // namespace iris

