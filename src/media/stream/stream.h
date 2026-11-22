#pragma once

#include "stream_types.h"

#include "../codec/codec_types.h"
#include "../transport/transport.h"

#include <memory>

namespace iris::media::v2 {

// IStream contains only lifecycle, generic identity/state, information and
// statistics. Media-specific behavior derives from it.
class IStream {
public:
    virtual ~IStream() = default;

    virtual Status configure(const StreamConfig& config) = 0;
    virtual Status start() = 0;
    virtual void shutdown() noexcept = 0;

    virtual StreamInfo info() const = 0;
    virtual StreamStats stats() const = 0;
};

// Generic stream branch for encoded media. A stream is directly the sink for
// exactly one encoded-packet transport.
class IEncodedPacketStream
    : public IStream,
      public IEncodedPacketSink,
      public std::enable_shared_from_this<IEncodedPacketStream> {
public:
    ~IEncodedPacketStream() override = default;

    virtual Status attach(
        std::shared_ptr<IEncodedPacketTransport> transport) = 0;
    virtual void detach() noexcept = 0;
    virtual Status sendEncodedPacket(const EncodedPacket& packet) = 0;
};

using EncodedPacketStreamPtr = std::shared_ptr<IEncodedPacketStream>;

} // namespace iris::media::v2
