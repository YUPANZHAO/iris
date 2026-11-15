#include "sip/pj_call.h"

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

} // namespace iris::sip

