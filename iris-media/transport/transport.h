#pragma once

// Message-oriented transport boundary used by protocol implementations.

#include "media_types.h"

#include <memory>

namespace iris::media::transport {

enum class TransportState {
    Idle,
    Configuring,
    Configured,
    Attaching,
    Connected,
    Detaching,
    Disconnected,
    Failed,
};

struct TransportEvent {
    TransportState state = TransportState::Idle;
    Status status;
};

class ITransportSink {
public:
    virtual ~ITransportSink() = default;

    // Takes ownership of one received transport message.
    virtual void onTransportData(MediaBuffer data) = 0;

    // Reports configuration, connection, and failure state changes.
    virtual void onTransportState(const TransportEvent&) {
    }
};

class Transport {
public:
    virtual ~Transport() = default;

    // Sends one transport message and transfers its ownership to the
    // implementation.
    virtual Status send(MediaBuffer data) = 0;

    // Assigns the receiver for inbound messages. Passing an empty weak pointer
    // detaches the current sink.
    virtual void setSink(
        std::weak_ptr<ITransportSink> sink) noexcept = 0;

    // Configures local information such as the bind address and port.
    virtual Status configure(const AttributeMap& options) = 0;

    // Attaches remote connection information such as peer address and port,
    // then starts receiving.
    virtual Status attach(const AttributeMap& options) = 0;

    // Closes the active connection. The transport may be configured again
    // afterwards.
    virtual void detach() noexcept = 0;
};

} // namespace iris::media::transport
