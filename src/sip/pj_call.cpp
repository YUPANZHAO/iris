#include "sip/pj_call.h"

#include "sip/sip_message.h"

namespace iris::sip {

PjCall::PjCall(pj::Account& account,
               CallCallbackHandler& handler,
               CallId id,
               AccountId account_id,
               int native_call_id)
    : pj::Call(account, native_call_id),
      handler_(&handler),
      id_(std::move(id)),
      account_id_(std::move(account_id)) {
}

void PjCall::onCallState(pj::OnCallStateParam&) {
    pj::CallInfo info;
    try {
        info = getInfo();
    } catch (...) {
        return;
    }
    handler_->call_state_changed(id_, info);
}

void PjCall::onCallTsxState(pj::OnCallTsxStateParam& param) {
    auto& state = param.e.body.tsxState;
    if (state.type != PJSIP_EVENT_RX_MSG ||
        state.tsx.method != "INVITE" ||
        state.tsx.statusCode < 200) {
        return;
    }
    if (state.tsx.statusCode >= 300) {
        handler_->call_offer_failed(
            id_, static_cast<int>(state.tsx.statusCode));
        return;
    }
    auto sdp = sip::extract_sdp_body(state.src.rdata.wholeMsg);
    if (!sdp.empty()) {
        handler_->remote_sdp_received(id_, sdp);
    }
}

void PjCall::onCallRxOffer(pj::OnCallRxOfferParam& param) {
    if (param.offer.wholeSdp.empty()) {
        return;
    }
    auto answer =
        handler_->negotiate_remote_offer(id_, param.offer.wholeSdp);
    if (answer.empty()) {
        param.statusCode = PJSIP_SC_NOT_ACCEPTABLE_HERE;
        return;
    }
    param.answer.wholeSdp = std::move(answer);
    param.statusCode = PJSIP_SC_OK;
    param.opt.flag |= PJSUA_CALL_MEDIA_APP_MANAGED;
}

void PjCall::onCallRxReinvite(pj::OnCallRxReinviteParam& param) {
    if (handler_->remote_reinvite_received(
            id_, param.offer.wholeSdp)) {
        param.isAsync = true;
        param.opt.flag |= PJSUA_CALL_MEDIA_APP_MANAGED;
    } else {
        param.isAsync = false;
        param.statusCode = PJSIP_SC_INTERNAL_SERVER_ERROR;
    }
}

void PjCall::onCallTxOffer(pj::OnCallTxOfferParam& param) {
    param.opt.flag |= PJSUA_CALL_MEDIA_APP_MANAGED;
}

} // namespace iris::sip

