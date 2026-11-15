#pragma once

#include "core/account_manager.h"
#include "core/runtime_services.h"
#include "sip/pj_call.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace iris::core {

class CallManager final : public sip::CallCallbackHandler {
public:
    CallManager(RuntimeServices& services, AccountManager& accounts);
    ~CallManager() override;

    CallManager(const CallManager&) = delete;
    CallManager& operator=(const CallManager&) = delete;

    CallId start_call(const AccountId& account_id,
                      const std::string& destination_uri);
    void answer_call(const CallId& id);
    void reject_call(const CallId& id);
    void hangup_call(const CallId& id);
    void hold_call(const CallId& id);
    void resume_call(const CallId& id);

    std::vector<CallSnapshot> calls() const;
    bool has_active_call_for(const AccountId& account_id) const;

    // Called from a PJSUA2 callback thread. The call adapter is created
    // immediately, while map and lifecycle mutation is posted to the core.
    void accept_incoming_call(sip::PjAccount& account,
                              const AccountId& account_id,
                              int native_call_id);

    void hangup_all() noexcept;
    void clear() noexcept;

private:
    struct CallRecord {
        std::shared_ptr<sip::PjCall> adapter;
        CallSnapshot snapshot;
    };

    void call_state_changed(CallId id,
                            const pj::CallInfo& info) override;
    void adopt_incoming_call(std::shared_ptr<sip::PjCall> call,
                             const pj::CallInfo& info);
    void update_call(const CallId& id, const pj::CallInfo& info);

    std::shared_ptr<sip::PjCall> find_call(const CallId& id);
    void publish_call_event(const CallSnapshot& snapshot,
                            const std::string& reason,
                            int status_code);

    RuntimeServices& services_;
    AccountManager& accounts_;
    std::unordered_map<std::string, CallRecord> calls_;
    std::atomic<std::uint64_t> next_call_id_{0};
};

} // namespace iris::core

