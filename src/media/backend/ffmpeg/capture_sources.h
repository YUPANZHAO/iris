#pragma once

#include "iris/media.h"

#include <string>

namespace iris::media {

struct CaptureSourceSpec {
    std::string format;
    std::string url;
    std::string displayName;
    bool isNull = false;
};

struct CaptureSourceResolution {
    CaptureSourceSpec source;
    std::string error;

    bool ok() const noexcept { return error.empty(); }
};

CaptureSourceResolution resolveCaptureSource(
    MediaKind kind,
    const std::string& configured);

} // namespace iris::media
