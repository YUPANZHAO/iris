#pragma once

#include "../media_types.h"

#include "../transport/transport.h"

#include <cstddef>
#include <string>

namespace iris::media::v2 {

// This is the generic stream description. It contains no audio/video formats,
// codec implementation choice, RTP details, or transport implementation name.
struct StreamConfig {
    StreamId id = 0;
    MediaKind kind = MediaKind::Unknown;
    std::string name;
    MediaDirection direction = MediaDirection::SendRecv;
    AttributeMap attributes;
};

struct StreamInfo {
    StreamState state = StreamState::Idle;
    StreamConfig stream;
    TransportInfo transport;
};

struct StreamStats {
    MediaStats media;
    TransportStats transport;
};

} // namespace iris::media::v2
