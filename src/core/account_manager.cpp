#include "core/account_manager.h"

#include "internal/identifier.h"

#include <algorithm>
#include <stdexcept>

namespace iris::core {

AccountManager::AccountManager(RuntimeServices& services,
                               IncomingCallSink& incoming_calls)
    : services_(services), incoming_calls_(incoming_calls) {
}

AccountManager::~AccountManager() = default;

AccountId AccountManager::add_account(const AccountConfig& config) {
    if (config.idUri.empty()) {
        throw std::invalid_argument("account idUri must not be empty");
    }

    auto domain_id = AccountId{
        internal::make_identifier("account", ++next_account_id_)
    };
    auto account = std::make_unique<sip::PjAccount>(
        *this, domain_id, config);

    AccountSnapshot snapshot;
    snapshot.id = domain_id;
    snapshot.name = config.name;
    snapshot.idUri = config.idUri;
    snapshot.registrarUri = config.registrarUri;
    snapshot.state = AccountState::Created;

    accounts_.emplace(domain_id.value, AccountRecord{
        std::move(account), std::move(snapshot)
    });
    publish_account_event(
        domain_id, AccountState::Created, "account created", 0);
    return domain_id;
}

void AccountManager::register_account(const AccountId& id) {
    auto& record = find_account(id);
    if (record.snapshot.registrarUri.empty()) {
        throw std::invalid_argument(
            "account has no registrar and cannot register");
    }

    record.snapshot.state = AccountState::Registering;
    record.snapshot.statusText = "registration in progress";
    record.snapshot.statusCode = 0;
    record.adapter->setRegistration(true);
    publish_account_event(
        id, AccountState::Registering, "registration started", 0);
}

void AccountManager::unregister_account(const AccountId& id) {
    auto& record = find_account(id);
    record.snapshot.state = AccountState::Unregistering;
    record.snapshot.statusText = "unregistration in progress";
    record.adapter->setRegistration(false);
    publish_account_event(
        id, AccountState::Unregistering, "unregistration started", 0);
}

void AccountManager::remove_account(const AccountId& id) {
    find_account(id);
    auto iterator = accounts_.find(id.value);
    try {
        iterator->second.adapter->setRegistration(false);
    } catch (...) {
    }
    accounts_.erase(iterator);
    publish_account_event(id, AccountState::Removed, "account removed", 0);
}

sip::PjAccount& AccountManager::account(const AccountId& id) {
    return *find_account(id).adapter;
}

std::vector<AccountSnapshot> AccountManager::accounts() const {
    std::vector<AccountSnapshot> snapshots;
    snapshots.reserve(accounts_.size());
    for (const auto& entry : accounts_) {
        snapshots.push_back(entry.second.snapshot);
    }
    std::sort(snapshots.begin(), snapshots.end(),
              [](const auto& lhs, const auto& rhs) {
                  return lhs.id.value < rhs.id.value;
              });
    return snapshots;
}

void AccountManager::unregister_all() noexcept {
    for (const auto& entry : accounts_) {
        try {
            entry.second.adapter->setRegistration(false);
        } catch (...) {
        }
    }
}

void AccountManager::clear() noexcept {
    accounts_.clear();
}

void AccountManager::registration_changed(
    sip::PjAccount& account,
    const pj::AccountInfo& info,
    const pj::OnRegStateParam& param) {
    auto id = account.id();
    auto transport_status = param.status;
    auto sip_status = param.code;
    auto reason = param.reason;

    services_.post_callback(
        [this, id, info, transport_status, sip_status, reason] {
            update_registration(
                id, info, transport_status, sip_status, reason);
        });
}

void AccountManager::incoming_call(sip::PjAccount& account,
                                   int native_call_id) {
    // PJSUA2 owns this callback thread. The call manager creates the adapter
    // now and schedules all shared-state mutation on the Iris core thread.
    incoming_calls_.accept_incoming_call(
        account, account.id(), native_call_id);
}

AccountManager::AccountRecord& AccountManager::find_account(
    const AccountId& id) {
    auto iterator = accounts_.find(id.value);
    if (iterator == accounts_.end()) {
        throw std::out_of_range("unknown account: " + id.value);
    }
    return iterator->second;
}

void AccountManager::update_registration(
    const AccountId& id,
    const pj::AccountInfo& info,
    pj_status_t transport_status,
    pjsip_status_code sip_status,
    const std::string& reason) {
    auto iterator = accounts_.find(id.value);
    if (iterator == accounts_.end()) {
        return;
    }

    AccountState state;
    if (info.regIsActive) {
        state = AccountState::Registered;
    } else if (sip_status == 0 && transport_status == PJ_SUCCESS) {
        state = AccountState::Unregistered;
    } else {
        state = AccountState::RegistrationFailed;
    }

    auto& snapshot = iterator->second.snapshot;
    snapshot.state = state;
    snapshot.statusCode = static_cast<int>(info.regStatus);
    snapshot.statusText = info.regStatusText.empty()
        ? reason
        : info.regStatusText;
    snapshot.registered = info.regIsActive;

    auto event_reason = reason.empty() ? snapshot.statusText : reason;
    publish_account_event(
        id, state, event_reason, snapshot.statusCode);
}

void AccountManager::publish_account_event(const AccountId& id,
                                           AccountState state,
                                           const std::string& reason,
                                           int status_code) {
    Event event;
    event.type = EventType::AccountStateChanged;
    event.accountId = id;
    event.accountState = state;
    event.statusCode = status_code;
    event.reason = reason;
    services_.publish_event(std::move(event));
}

} // namespace iris::core

