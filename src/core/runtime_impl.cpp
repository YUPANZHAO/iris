#include "core/runtime_impl.h"

#include "core/account_manager.h"
#include "core/call_manager.h"
#include "core/media_session_manager.h"

#include <chrono>
#include <stdexcept>
#include <thread>
#include <utility>

namespace iris {

RuntimeImpl::RuntimeImpl(RuntimeConfig config)
    : config_(std::move(config)) {
    dispatcher_.start();
    dispatcher_.invoke([this] { initialize(); }).get();
}

RuntimeImpl::~RuntimeImpl() {
    shutdown();
}

std::string RuntimeImpl::pjproject_version() const {
    return dispatcher_.invoke([this] {
        return endpoint_.version();
    }).get();
}

unsigned RuntimeImpl::sip_port() const {
    return dispatcher_.invoke([this] {
        return endpoint_.sip_port();
    }).get();
}

AccountId RuntimeImpl::add_account(const AccountConfig& config) {
    return dispatcher_.invoke([this, config] {
        return account_manager_->add_account(config);
    }).get();
}

void RuntimeImpl::register_account(const AccountId& id) {
    dispatcher_.invoke([this, id] {
        account_manager_->register_account(id);
    }).get();
}

void RuntimeImpl::unregister_account(const AccountId& id) {
    dispatcher_.invoke([this, id] {
        account_manager_->unregister_account(id);
    }).get();
}

void RuntimeImpl::remove_account(const AccountId& id) {
    dispatcher_.invoke([this, id] {
        if (call_manager_->has_active_call_for(id)) {
            throw std::runtime_error(
                "account still has an active call");
        }
        account_manager_->remove_account(id);
    }).get();
}

CallId RuntimeImpl::start_call(
    const AccountId& account_id,
    const std::string& destination_uri) {
    return dispatcher_.invoke([this, account_id, destination_uri] {
        return call_manager_->start_call(account_id, destination_uri);
    }).get();
}

void RuntimeImpl::answer_call(const CallId& id) {
    dispatcher_.invoke([this, id] {
        call_manager_->answer_call(id);
    }).get();
}

void RuntimeImpl::reject_call(const CallId& id) {
    dispatcher_.invoke([this, id] {
        call_manager_->reject_call(id);
    }).get();
}

void RuntimeImpl::hangup_call(const CallId& id) {
    dispatcher_.invoke([this, id] {
        call_manager_->hangup_call(id);
    }).get();
}

void RuntimeImpl::hold_call(const CallId& id) {
    dispatcher_.invoke([this, id] {
        call_manager_->hold_call(id);
    }).get();
}

void RuntimeImpl::resume_call(const CallId& id) {
    dispatcher_.invoke([this, id] {
        call_manager_->resume_call(id);
    }).get();
}

void RuntimeImpl::set_audio_muted(const CallId& id, bool muted) {
    dispatcher_.invoke([this, id, muted] {
        const auto result = media_sessions_->setAudioMuted(id, muted);
        if (!result.ok) {
            throw std::runtime_error(result.error);
        }
    }).get();
}

void RuntimeImpl::set_video_muted(const CallId& id, bool muted) {
    dispatcher_.invoke([this, id, muted] {
        const auto result = media_sessions_->setVideoMuted(id, muted);
        if (!result.ok) {
            throw std::runtime_error(result.error);
        }
    }).get();
}

void RuntimeImpl::set_media_input(MediaKind kind,
                                  const std::string& source) {
    dispatcher_.invoke([this, kind, source] {
        media_sessions_->setInputSource(kind, source);
    }).get();
}

std::string RuntimeImpl::media_input(MediaKind kind) const {
    return dispatcher_.invoke([this, kind] {
        return media_sessions_->inputSource(kind);
    }).get();
}

void RuntimeImpl::setVideoFrameCallback(
    VideoFrameCallback callback) {
    dispatcher_.invoke(
        [this, callback = std::move(callback)] {
            media_sessions_->setVideoFrameCallback(callback);
        }).get();
}

std::vector<AccountSnapshot> RuntimeImpl::accounts() const {
    return dispatcher_.invoke([this] {
        return account_manager_->accounts();
    }).get();
}

std::vector<CallSnapshot> RuntimeImpl::calls() const {
    return dispatcher_.invoke([this] {
        return call_manager_->calls();
    }).get();
}

MediaStats RuntimeImpl::media_stats(const CallId& id) const {
    return dispatcher_.invoke([this, id] {
        return media_sessions_->stats(id);
    }).get();
}

std::vector<Event> RuntimeImpl::poll_events() {
    std::vector<Event> events;
    std::lock_guard<std::mutex> lock(event_mutex_);
    events.swap(events_);
    return events;
}

void RuntimeImpl::publish_event(Event event) {
    event.sequence = ++next_event_id_;
    if (event.timestampMs == 0) {
        event.timestampMs = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count());
    }
    std::lock_guard<std::mutex> lock(event_mutex_);
    events_.push_back(std::move(event));
}

bool RuntimeImpl::post_callback(std::function<void()> task) {
    return dispatcher_.post(std::move(task));
}

void RuntimeImpl::invoke_on_core(std::function<void()> task) {
    if (dispatcher_.is_worker_thread()) {
        task();
        return;
    }
    dispatcher_.invoke(std::move(task)).get();
}

void RuntimeImpl::accept_incoming_call(sip::PjAccount& account,
                                       const AccountId& account_id,
                                       int native_call_id,
                                       const std::string& remoteSdp) {
    if (call_manager_) {
        call_manager_->accept_incoming_call(
            account, account_id, native_call_id, remoteSdp);
    }
}

void RuntimeImpl::initialize() {
    endpoint_.initialize(config_);
    media_sessions_ = std::make_unique<core::MediaSessionManager>(
        std::move(config_.media));
    account_manager_ =
        std::make_unique<core::AccountManager>(*this, *this);
    call_manager_ =
        std::make_unique<core::CallManager>(
            *this, *account_manager_, *media_sessions_);
}

void RuntimeImpl::shutdown() noexcept {
    if (shutdown_complete_.exchange(true)) {
        return;
    }

    try {
        dispatcher_.invoke([this] {
            if (call_manager_) {
                call_manager_->hangup_all();
            }
            if (account_manager_) {
                account_manager_->unregister_all();
            }
        }).get();

        if (call_manager_) {
            call_manager_->wait_until_empty(
                std::chrono::seconds(3));
        }
        if (account_manager_) {
            account_manager_->wait_until_registration_settled(
                std::chrono::seconds(3));
        }

        dispatcher_.invoke([this] {
            dispatcher_.close_queue();
            if (call_manager_) {
                call_manager_->clear();
            }
            if (account_manager_) {
                account_manager_->clear();
            }
            if (media_sessions_) {
                media_sessions_->stopAll();
            }

            call_manager_.reset();
            media_sessions_.reset();
            account_manager_.reset();
            endpoint_.shutdown();
        }).get();
    } catch (...) {
    }

    dispatcher_.stop();
}

} // namespace iris

