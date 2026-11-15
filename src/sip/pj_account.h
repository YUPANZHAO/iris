#pragma once

#include "iris/config.h"
#include "iris/types.h"

#include <pjsua2.hpp>

#include <utility>

namespace iris::sip {

class PjAccount;

class AccountCallbackHandler {
public:
    virtual ~AccountCallbackHandler() = default;

    virtual void registration_changed(
        PjAccount& account,
        const pj::AccountInfo& info,
        const pj::OnRegStateParam& param) = 0;

    virtual void incoming_call(PjAccount& account,
                               int native_call_id,
                               const std::string& remoteSdp) = 0;
};

class PjAccount final : public pj::Account {
public:
    PjAccount(AccountCallbackHandler& handler,
              AccountId id,
              const AccountConfig& config);

    const AccountId& id() const noexcept {
        return id_;
    }

    void onRegState(pj::OnRegStateParam& param) override;
    void onIncomingCall(pj::OnIncomingCallParam& param) override;

private:
    AccountCallbackHandler* handler_;
    AccountId id_;
};

} // namespace iris::sip

