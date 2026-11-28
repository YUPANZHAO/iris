#include "iris/media.h"

#include "capture_source/device_query.h"

namespace iris {

MediaDeviceCatalog queryMediaDevices(MediaKind kind) {
    MediaDeviceCatalog result;
    result.devices = media::capture::queryCaptureDevices(kind);
    return result;
}

} // namespace iris
