#pragma once

#include "iris/types.h"

#include <pjsua2.hpp>

#include <utility>

namespace iris::sip {

class CallCallbackHandler {
public:
    virtual ~CallCallbackHandler() = default;

    virtual void call_state_changed(CallId id,
                                    const pj::CallInfo& info) = 0;
    virtual void remote_sdp_received(CallId id,
                                     const std::string& sdp) = 0;
    virtual bool remote_reinvite_received(
        CallId id, const std::string& offer) = 0;
    virtual std::string negotiate_remote_offer(
        CallId id, const std::string& offer) = 0;
    virtual void call_offer_failed(CallId id, int statusCode) = 0;
};

class PjCall final : public pj::Call {
public:
    PjCall(pj::Account& account,
           CallCallbackHandler& handler,
           CallId id,
           AccountId account_id,
           int native_call_id = PJSUA_INVALID_ID);

    const CallId& id() const noexcept {
        return id_;
    }

    const AccountId& account_id() const noexcept {
        return account_id_;
    }

    void onCallState(pj::OnCallStateParam& param) override;
    void onCallTsxState(pj::OnCallTsxStateParam& param) override;
    void onCallRxOffer(pj::OnCallRxOfferParam& param) override;
    void onCallRxReinvite(pj::OnCallRxReinviteParam& param) override;
    void onCallTxOffer(pj::OnCallTxOfferParam& param) override;

private:
    CallCallbackHandler* handler_;
    CallId id_;
    AccountId account_id_;
};

} // namespace iris::sip

