#pragma once

#include "iris/events.h"
#include "iris/types.h"

#include <functional>
#include <string>

namespace iris::sip {
class PjAccount;
}

namespace iris::core {

class RuntimeServices {
public:
    virtual ~RuntimeServices() = default;

    // Must only publish from the Runtime core thread.
    virtual void publish_event(Event event) = 0;
    virtual bool post_callback(std::function<void()> task) = 0;
    virtual void invoke_on_core(std::function<void()> task) = 0;
};

class IncomingCallSink {
public:
    virtual ~IncomingCallSink() = default;

    virtual void accept_incoming_call(sip::PjAccount& account,
                                      const AccountId& account_id,
                                      int native_call_id,
                                      const std::string& remoteSdp) = 0;
};

} // namespace iris::core

