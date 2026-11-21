#pragma once

#include "../media_types.h"

#include "../codec/codec_types.h"

#include <cstdint>
#include <memory>
#include <string>

namespace iris::media::v2 {

enum class TransportState {
    Closed,
    Opening,
    Open,
    Failed,
};

struct TransportConfig {
    std::string endpoint;
    AttributeMap options;
};

struct TransportInfo {
    TransportState state = TransportState::Closed;
    std::string endpoint;
};

struct TransportStats {
    std::uint64_t messagesSent = 0;
    std::uint64_t messagesReceived = 0;
    std::uint64_t bytesSent = 0;
    std::uint64_t bytesReceived = 0;
    std::uint64_t sendErrors = 0;
    std::uint64_t receiveErrors = 0;
    std::uint64_t droppedMessages = 0;
};

class ITransportSink {
public:
    virtual ~ITransportSink() = default;

    virtual void onTransportData(MediaBufferView data) = 0;
    virtual void onTransportError(const Status& status) = 0;
};

// Base transport is deliberately opaque: payload semantics, framing protocol,
// and transport mechanism remain implementation details.
class ITransport {
public:
    virtual ~ITransport() = default;

    virtual Status open(const TransportConfig& config) = 0;
    virtual void close() noexcept = 0;

    virtual TransportInfo info() const = 0;
    virtual TransportStats stats() const = 0;

protected:
    // The implementation may retain the buffer until asynchronous send
    // completion. The caller does not pass transport-specific metadata.
    virtual Status send(MediaBufferView data) = 0;

    virtual void setSink(std::weak_ptr<ITransportSink> sink) = 0;
};

// Transport boundary for already-encoded media. The concrete transport owns
// framing, packetization, serialization, and reconstruction.
class IEncodedPacketTransport : public ITransport {
public:
    ~IEncodedPacketTransport() override = default;

    virtual Status send(const EncodedPacket& packet) = 0;
    virtual void setSink(
        std::weak_ptr<IEncodedPacketSink> sink) = 0;
};

} // namespace iris::media::v2
