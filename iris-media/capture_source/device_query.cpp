#include "capture_source/device_query.h"

extern "C" {
#include <libavdevice/avdevice.h>
}

#include <algorithm>
#include <array>
#include <string>
#include <vector>

namespace iris::media::capture {
namespace {

struct DeviceBackend {
    const char* format = nullptr;
    const char* name = nullptr;
};

std::vector<DeviceBackend> backends(MediaKind kind) {
#if defined(_WIN32)
    (void)kind;
    return {{"dshow", "ffmpeg/dshow"}};
#elif defined(__APPLE__)
    return {{"avfoundation", "ffmpeg/avfoundation"}};
#else
    if (kind == MediaKind::Video) {
        return {{"v4l2", "ffmpeg/v4l2"}};
    }
    return {{"alsa", "ffmpeg/alsa"}};
#endif
}

AVMediaType mediaType(MediaKind kind) {
    return kind == MediaKind::Video
        ? AVMEDIA_TYPE_VIDEO
        : AVMEDIA_TYPE_AUDIO;
}

bool supports(const AVDeviceInfo* device, AVMediaType type) {
    if (!device || !device->media_types) {
        return false;
    }
    for (int index = 0; index < device->nb_media_types; ++index) {
        if (device->media_types[index] == type) {
            return true;
        }
    }
    return false;
}

} // namespace

std::vector<CaptureDeviceInfo> queryCaptureDevices(MediaKind kind) {
    if (kind == MediaKind::Unknown) {
        return {};
    }
    static const bool initialized = [] {
        avdevice_register_all();
        return true;
    }();
    (void)initialized;

    std::vector<CaptureDeviceInfo> result;
    for (const auto& backend : backends(kind)) {
        const auto* inputFormat =
            av_find_input_format(backend.format);
        if (!inputFormat) {
            continue;
        }
        AVDeviceInfoList* list = nullptr;
        if (avdevice_list_input_sources(
                inputFormat, nullptr, nullptr, &list) < 0 ||
            !list) {
            continue;
        }
        bool foundDefault = false;
        for (int index = 0; index < list->nb_devices; ++index) {
            const auto* device = list->devices[index];
            if (!supports(device, mediaType(kind))) {
                continue;
            }
            CaptureDeviceInfo value;
            value.id = device->device_name ? device->device_name : "";
            value.name = device->device_description
                ? device->device_description
                : value.id;
            if (value.name.empty()) {
                value.name = value.id;
            }
            value.backend = backend.name;
            value.kind = kind;
            value.isDefault = index == list->default_device;
            foundDefault = foundDefault || value.isDefault;
            if (!value.id.empty()) {
                result.push_back(std::move(value));
            }
        }
        avdevice_free_list_devices(&list);
        if (foundDefault) {
            break;
        }
    }
    if (!result.empty() &&
        std::none_of(
            result.begin(),
            result.end(),
            [](const CaptureDeviceInfo& device) {
                return device.isDefault;
            })) {
        result.front().isDefault = true;
    }
    return result;
}

} // namespace iris::media::capture
