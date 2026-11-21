#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>

namespace iris::media::v2 {

using StreamId = std::uint64_t;
using SessionId = std::string;
using AttributeMap = std::map<std::string, std::string>;

enum class MediaKind {
    Unknown,
    Audio,
    Video,
};

enum class MediaDirection {
    Inactive,
    SendOnly,
    RecvOnly,
    SendRecv,
};

enum class StreamState {
    Idle,
    Preparing,
    Negotiating,
    Starting,
    Active,
    Reconfiguring,
    Stopping,
    Stopped,
    Failed,
};

struct Status {
    bool ok = true;
    int code = 0;
    std::string message;
};

// Buffer leases are the only ownership primitive below media-type-specific
// interfaces. Storage may be pooled or provided by another allocator.
class IBufferLease {
public:
    virtual ~IBufferLease() = default;

    virtual std::uint8_t* data() noexcept = 0;
    virtual const std::uint8_t* data() const noexcept = 0;
    virtual std::size_t size() const noexcept = 0;
    virtual std::size_t capacity() const noexcept = 0;
};

using MediaBuffer = std::shared_ptr<IBufferLease>;

struct MediaBufferView {
    MediaBuffer buffer;
    std::size_t offset = 0;
    std::size_t size = 0;
};

struct MediaStats {
    std::uint64_t samplesProduced = 0;
    std::uint64_t samplesConsumed = 0;
    std::uint64_t samplesDropped = 0;
    std::uint64_t codecErrors = 0;
};

} // namespace iris::media::v2
