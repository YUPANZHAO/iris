#pragma once

#include <ostream>
#include <cstdint>
#include <string>

namespace iris {

struct AccountId {
    std::string value;

    bool empty() const noexcept { return value.empty(); }
    const std::string& str() const noexcept { return value; }

    friend bool operator==(const AccountId& lhs, const AccountId& rhs) noexcept {
        return lhs.value == rhs.value;
    }

    friend bool operator!=(const AccountId& lhs, const AccountId& rhs) noexcept {
        return !(lhs == rhs);
    }
};

struct CallId {
    std::string value;

    bool empty() const noexcept { return value.empty(); }
    const std::string& str() const noexcept { return value; }

    friend bool operator==(const CallId& lhs, const CallId& rhs) noexcept {
        return lhs.value == rhs.value;
    }

    friend bool operator!=(const CallId& lhs, const CallId& rhs) noexcept {
        return !(lhs == rhs);
    }
};

inline std::ostream& operator<<(std::ostream& stream, const AccountId& id) {
    return stream << id.value;
}

inline std::ostream& operator<<(std::ostream& stream, const CallId& id) {
    return stream << id.value;
}

enum class AccountState {
    Created,
    Registering,
    Registered,
    Unregistering,
    Unregistered,
    RegistrationFailed,
    Removed,
};

enum class CallState {
    Incoming,
    Outgoing,
    Early,
    Confirmed,
    LocallyHeld,
    RemotelyHeld,
    Disconnected,
};

inline const char* to_string(AccountState state) noexcept {
    switch (state) {
    case AccountState::Created: return "created";
    case AccountState::Registering: return "registering";
    case AccountState::Registered: return "registered";
    case AccountState::Unregistering: return "unregistering";
    case AccountState::Unregistered: return "unregistered";
    case AccountState::RegistrationFailed: return "registration-failed";
    case AccountState::Removed: return "removed";
    }
    return "unknown";
}

inline const char* to_string(CallState state) noexcept {
    switch (state) {
    case CallState::Incoming: return "incoming";
    case CallState::Outgoing: return "outgoing";
    case CallState::Early: return "early";
    case CallState::Confirmed: return "confirmed";
    case CallState::LocallyHeld: return "locally-held";
    case CallState::RemotelyHeld: return "remotely-held";
    case CallState::Disconnected: return "disconnected";
    }
    return "unknown";
}

struct AccountSnapshot {
    AccountId id;
    std::string name;
    std::string idUri;
    std::string registrarUri;
    AccountState state = AccountState::Created;
    int statusCode = 0;
    std::string statusText;
    bool registered = false;
};

struct CallSnapshot {
    CallId id;
    AccountId accountId;
    std::string remoteUri;
    std::string stateText;
    CallState state = CallState::Outgoing;
    int statusCode = 0;
    std::uint64_t createdAtMs = 0;
    std::uint64_t connectedAtMs = 0;
};

} // namespace iris

