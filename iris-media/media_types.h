#pragma once

// Types shared by codec and capture-source modules.

#include "engine/memory_pool.h"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>

namespace iris::media {

using StreamId = std::uint64_t;
using AttributeMap = std::map<std::string, std::string>;

enum class MediaKind {
    Unknown,
    Audio,
    Video,
};

struct Status {
    bool ok = true;
    int code = 0;
    std::string message;

    static Status failure(std::string message, int code = -1) {
        return Status{false, code, std::move(message)};
    }
};

using MediaBuffer = std::shared_ptr<ByteBuffer>;

} // namespace iris::media
