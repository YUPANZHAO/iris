#include "sip/pj_account.h"

#include "sip/sip_message.h"

namespace iris::sip {

PjAccount::PjAccount(AccountCallbackHandler& handler,
                     AccountId id,
                     const AccountConfig& config)
    : handler_(&handler), id_(std::move(id)) {
    pj::AccountConfig account_config;
    account_config.idUri = config.idUri;
    account_config.regConfig.registrarUri = config.registrarUri;
    account_config.regConfig.registerOnAdd = false;
    account_config.callConfig.mediaAppManaged = true;

    if (!config.username.empty()) {
        account_config.sipConfig.authCreds.push_back(
            pj::AuthCredInfo(
                "digest",
                config.authRealm,
                config.username,
                PJSIP_CRED_DATA_PLAIN_PASSWD,
                config.password));
    }

    create(account_config);
}

void PjAccount::onRegState(pj::OnRegStateParam& param) {
    pj::AccountInfo info;
    try {
        info = getInfo();
    } catch (...) {
        return;
    }
    handler_->registration_changed(*this, info, param);
}

void PjAccount::onIncomingCall(pj::OnIncomingCallParam& param) {
    handler_->incoming_call(
        *this, param.callId, sip::extract_sdp_body(param.rdata.wholeMsg));
}

} // namespace iris::sip

