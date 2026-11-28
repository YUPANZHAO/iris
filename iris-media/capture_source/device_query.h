#pragma once

#include "capture_source/capture_source.h"

#include <string>
#include <vector>

namespace iris::media::capture {

struct CaptureDeviceInfo {
    std::string id;
    std::string name;
    std::string backend;
    MediaKind kind = MediaKind::Unknown;
    bool isDefault = false;
};

std::vector<CaptureDeviceInfo> queryCaptureDevices(MediaKind kind);

} // namespace iris::media::capture
