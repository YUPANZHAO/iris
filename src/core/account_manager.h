#pragma once

#include "core/runtime_services.h"
#include "sip/pj_account.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace iris::core {

class AccountManager final : public sip::AccountCallbackHandler {
public:
    AccountManager(RuntimeServices& services,
                   IncomingCallSink& incoming_calls);
    ~AccountManager() override;

    AccountManager(const AccountManager&) = delete;
    AccountManager& operator=(const AccountManager&) = delete;

    AccountId add_account(const AccountConfig& config);
    void register_account(const AccountId& id);
    void unregister_account(const AccountId& id);
    void remove_account(const AccountId& id);

    sip::PjAccount& account(const AccountId& id);
    std::vector<AccountSnapshot> accounts() const;

    void unregister_all() noexcept;
    void clear() noexcept;

private:
    struct AccountRecord {
        std::unique_ptr<sip::PjAccount> adapter;
        AccountSnapshot snapshot;
    };

    void registration_changed(
        sip::PjAccount& account,
        const pj::AccountInfo& info,
        const pj::OnRegStateParam& param) override;

    void incoming_call(sip::PjAccount& account,
                       int native_call_id) override;

    AccountRecord& find_account(const AccountId& id);
    void update_registration(const AccountId& id,
                             const pj::AccountInfo& info,
                             pj_status_t transport_status,
                             pjsip_status_code sip_status,
                             const std::string& reason);
    void publish_account_event(const AccountId& id,
                               AccountState state,
                               const std::string& reason,
                               int status_code);

    RuntimeServices& services_;
    IncomingCallSink& incoming_calls_;
    std::unordered_map<std::string, AccountRecord> accounts_;
    std::atomic<std::uint64_t> next_account_id_{0};
};

} // namespace iris::core

