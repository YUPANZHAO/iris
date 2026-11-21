#pragma once

#include "../stream/stream.h"

#include <string>
#include <vector>

namespace iris::media::v2 {

struct MediaControlConfig {
    std::string name = "media-control";
    AttributeMap options;
};

// IMediaControl owns its internal SDP, stream, codec and transport composition.
// Callers do not provide those dependencies. Generic stream creation keeps the
// control plane applicable to additional media types.
class IMediaControl {
public:
    virtual ~IMediaControl() = default;

    virtual Status initialize(const MediaControlConfig& config) = 0;
    // The returned object is the derived stream implementation selected by
    // StreamConfig::kind, for example AudioEncodedStream or AudioFrameStream.
    virtual Status createStream(const StreamConfig& config,
                                EncodedPacketStreamPtr* stream) = 0;
    virtual bool destroyStream(StreamId id) = 0;

    virtual EncodedPacketStreamPtr stream(StreamId id) const = 0;
    virtual std::vector<StreamId> streams() const = 0;

    virtual MediaStats stats() const = 0;
};

} // namespace iris::media::v2
