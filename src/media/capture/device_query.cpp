#include "iris/media.h"

#include "media/capture/ffmpeg_capture.h"

namespace iris {

MediaDeviceCatalog queryMediaDevices(MediaKind kind) {
    MediaDeviceCatalog result;
    media::v2::FFmpegCaptureProvider provider;
    const auto sourceKind = kind == MediaKind::Video
        ? media::v2::MediaKind::Video
        : media::v2::MediaKind::Audio;
    for (const auto& source : provider.list(sourceKind)) {
        result.devices.push_back(MediaDeviceInfo{
            kind,
            source.id,
            source.name,
            source.isDefault,
        });
    }
    return result;
}

} // namespace iris
