#pragma once

#include "core/runtime_services.h"
#include "core/task_dispatcher.h"
#include "iris/config.h"
#include "iris/events.h"
#include "iris/types.h"
#include "sip/pj_endpoint.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace iris::core {
class AccountManager;
class CallManager;
}

namespace iris {

class RuntimeImpl final : public core::RuntimeServices,
                          public core::IncomingCallSink {
public:
    explicit RuntimeImpl(RuntimeConfig config);
    ~RuntimeImpl() override;

    RuntimeImpl(const RuntimeImpl&) = delete;
    RuntimeImpl& operator=(const RuntimeImpl&) = delete;

    std::string pjproject_version() const;
    unsigned sip_port() const;

    AccountId add_account(const AccountConfig& config);
    void register_account(const AccountId& id);
    void unregister_account(const AccountId& id);
    void remove_account(const AccountId& id);

    CallId start_call(const AccountId& account_id,
                      const std::string& destination_uri);
    void answer_call(const CallId& id);
    void reject_call(const CallId& id);
    void hangup_call(const CallId& id);
    void hold_call(const CallId& id);
    void resume_call(const CallId& id);
    void set_audio_muted(const CallId& id, bool muted);
    void set_video_muted(const CallId& id, bool muted);
    void set_media_input(MediaKind kind, const std::string& source);
    std::string media_input(MediaKind kind) const;
    void setAudioFrameCallback(AudioFrameCallback callback);
    void setVideoFrameCallback(VideoFrameCallback callback);

    std::vector<AccountSnapshot> accounts() const;
    std::vector<CallSnapshot> calls() const;
    MediaStats media_stats(const CallId& id) const;
    std::vector<Event> poll_events();

    void publish_event(Event event) override;
    bool post_callback(std::function<void()> task) override;
    void invoke_on_core(std::function<void()> task) override;
    void accept_incoming_call(sip::PjAccount& account,
                              const AccountId& account_id,
                              int native_call_id,
                              const std::string& remoteSdp) override;

private:
    void initialize();
    void shutdown() noexcept;

    RuntimeConfig config_;
    mutable core::TaskDispatcher dispatcher_;
    sip::PjEndpoint endpoint_;
    std::unique_ptr<core::AccountManager> account_manager_;
    std::unique_ptr<core::CallManager> call_manager_;

    mutable std::mutex event_mutex_;
    std::vector<Event> events_;
    std::atomic<std::uint64_t> next_event_id_{0};
    std::atomic<bool> shutdown_complete_{false};
};

} // namespace iris

