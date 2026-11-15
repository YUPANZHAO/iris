#include "core/runtime_impl.h"

#include "core/account_manager.h"
#include "core/call_manager.h"

#include <chrono>
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

std::vector<Event> RuntimeImpl::poll_events() {
    std::vector<Event> events;
    std::lock_guard<std::mutex> lock(event_mutex_);
    events.swap(events_);
    return events;
}

void RuntimeImpl::publish_event(Event event) {
    event.sequence = ++next_event_id_;
    std::lock_guard<std::mutex> lock(event_mutex_);
    events_.push_back(std::move(event));
}

bool RuntimeImpl::post_callback(std::function<void()> task) {
    return dispatcher_.post(std::move(task));
}

void RuntimeImpl::accept_incoming_call(sip::PjAccount& account,
                                       const AccountId& account_id,
                                       int native_call_id) {
    if (call_manager_) {
        call_manager_->accept_incoming_call(
            account, account_id, native_call_id);
    }
}

void RuntimeImpl::initialize() {
    endpoint_.initialize(config_);
    account_manager_ =
        std::make_unique<core::AccountManager>(*this, *this);
    call_manager_ =
        std::make_unique<core::CallManager>(*this, *account_manager_);
}

void RuntimeImpl::shutdown() noexcept {
    if (shutdown_complete_.exchange(true)) {
        return;
    }

    try {
        dispatcher_.invoke([this] {
            dispatcher_.close_queue();

            if (call_manager_) {
                call_manager_->hangup_all();
            }
            if (account_manager_) {
                account_manager_->unregister_all();
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(250));

            if (call_manager_) {
                call_manager_->clear();
            }
            if (account_manager_) {
                account_manager_->clear();
            }

            call_manager_.reset();
            account_manager_.reset();
            endpoint_.shutdown();
        }).get();
    } catch (...) {
    }

    dispatcher_.stop();
}

} // namespace iris

