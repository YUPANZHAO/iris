#include "iris/runtime.h"

#include "core/runtime_impl.h"

namespace iris {

Runtime::Runtime(const RuntimeConfig& config)
    : impl_(std::make_unique<RuntimeImpl>(config)) {
}

Runtime::~Runtime() = default;

Runtime::Runtime(Runtime&&) noexcept = default;

Runtime& Runtime::operator=(Runtime&&) noexcept = default;

std::string Runtime::version() const {
    return IRIS_VERSION;
}

std::string Runtime::pjprojectVersion() const {
    return impl_->pjproject_version();
}

AccountId Runtime::addAccount(const AccountConfig& config) {
    return impl_->add_account(config);
}

void Runtime::registerAccount(const AccountId& id) {
    impl_->register_account(id);
}

void Runtime::unregisterAccount(const AccountId& id) {
    impl_->unregister_account(id);
}

void Runtime::removeAccount(const AccountId& id) {
    impl_->remove_account(id);
}

CallId Runtime::startCall(const AccountId& account_id,
                          const std::string& destination_uri) {
    return impl_->start_call(account_id, destination_uri);
}

void Runtime::answerCall(const CallId& id) {
    impl_->answer_call(id);
}

void Runtime::rejectCall(const CallId& id) {
    impl_->reject_call(id);
}

void Runtime::hangupCall(const CallId& id) {
    impl_->hangup_call(id);
}

void Runtime::holdCall(const CallId& id) {
    impl_->hold_call(id);
}

void Runtime::resumeCall(const CallId& id) {
    impl_->resume_call(id);
}

std::vector<AccountSnapshot> Runtime::accounts() const {
    return impl_->accounts();
}

std::vector<CallSnapshot> Runtime::calls() const {
    return impl_->calls();
}

std::vector<Event> Runtime::pollEvents() {
    return impl_->poll_events();
}

} // namespace iris

