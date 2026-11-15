#include "media/backend/ffmpeg/capture_sources.h"

extern "C" {
#include <libavdevice/avdevice.h>
#include <libavutil/error.h>
}

#include <algorithm>
#include <charconv>
#include <string>
#include <utility>
#include <vector>

namespace iris {
namespace {

void ensure_devices_registered() {
    static const bool initialized = [] {
        avdevice_register_all();
        return true;
    }();
    (void)initialized;
}

std::string ffmpeg_error(int error) {
    char message[AV_ERROR_MAX_STRING_SIZE]{};
    if (av_strerror(error, message, sizeof(message)) < 0) {
        return "unknown FFmpeg error";
    }
    return message;
}

AVMediaType media_type(MediaKind kind) {
    return kind == MediaKind::Audio
        ? AVMEDIA_TYPE_AUDIO
        : AVMEDIA_TYPE_VIDEO;
}

bool supports(const AVDeviceInfo* device, AVMediaType type) {
    if (!device) {
        return false;
    }
    for (int index = 0; index < device->nb_media_types; ++index) {
        if (device->media_types[index] == type) {
            return true;
        }
    }
    return false;
}

#ifdef _WIN32
MediaDeviceCatalog enumerate_dshow(MediaKind kind) {
    ensure_devices_registered();
    MediaDeviceCatalog result;
    const AVInputFormat* format = av_find_input_format("dshow");
    if (!format) {
        result.error = "FFmpeg DirectShow input is unavailable";
        return result;
    }

    AVDeviceInfoList* list = nullptr;
    const int status =
        avdevice_list_input_sources(format, nullptr, nullptr, &list);
    if (status < 0 || !list) {
        result.error = "failed to enumerate DirectShow devices: " +
            ffmpeg_error(status);
        return result;
    }

    const auto type = media_type(kind);
    bool found_default = false;
    for (int index = 0; index < list->nb_devices; ++index) {
        const auto* device = list->devices[index];
        if (!supports(device, type)) {
            continue;
        }
        MediaDeviceInfo info;
        info.kind = kind;
        info.id = device->device_name ? device->device_name : "";
        info.name = device->device_description
            ? device->device_description
            : info.id;
        if (info.name.empty()) {
            info.name = info.id;
        }
        info.isDefault = index == list->default_device;
        found_default = found_default || info.isDefault;
        if (!info.id.empty()) {
            result.devices.push_back(std::move(info));
        }
    }

    if (!found_default && !result.devices.empty()) {
        result.devices.front().isDefault = true;
    }
    avdevice_free_list_devices(&list);
    return result;
}
#else
MediaDeviceCatalog enumerate_dshow(MediaKind) {
    MediaDeviceCatalog result;
    result.error = "DirectShow capture is only available on Windows";
    return result;
}
#endif

std::string select_dshow_device(MediaKind kind,
                                const std::string& selector,
                                std::string& display_name,
                                std::string& error) {
    auto catalog = enumerate_dshow(kind);
    if (!catalog.ok()) {
        error = catalog.error;
        return {};
    }
    if (catalog.devices.empty()) {
        error = std::string("no DirectShow ") +
            (kind == MediaKind::Audio ? "microphone" : "camera") +
            " was found";
        return {};
    }

    const MediaDeviceInfo* selected = nullptr;
    if (selector.empty() || selector == "auto") {
        selected = &catalog.devices.front();
        for (const auto& device : catalog.devices) {
            if (device.isDefault) {
                selected = &device;
                break;
            }
        }
    } else {
        int requested_index = -1;
        const auto parsed = std::from_chars(
            selector.data(), selector.data() + selector.size(),
            requested_index);
        if (parsed.ec == std::errc{} &&
            parsed.ptr == selector.data() + selector.size()) {
            if (requested_index < 0 ||
                requested_index >=
                    static_cast<int>(catalog.devices.size())) {
                error = "capture device index is out of range";
                return {};
            }
            selected = &catalog.devices[
                static_cast<std::size_t>(requested_index)];
        } else {
            for (const auto& device : catalog.devices) {
                if (device.id == selector || device.name == selector) {
                    selected = &device;
                    break;
                }
            }
        }
    }

    if (!selected) {
        error = "capture device was not found; use 'media devices' to list ids";
        return {};
    }
    display_name = selected->name;
    return selected->id;
}

} // namespace

MediaDeviceCatalog queryMediaDevices(MediaKind kind) {
    return enumerate_dshow(kind);
}

namespace media {

CaptureSourceResolution resolveCaptureSource(
    MediaKind kind,
    const std::string& configured) {
    CaptureSourceResolution result;
    const std::string input =
        configured.empty() ? std::string("auto") : configured;

    if (input == "null" || input == "none" || input == "empty") {
        result.source.format = "null";
        result.source.displayName = "empty source";
        result.source.isNull = true;
        return result;
    }
    if (input.rfind("lavfi:", 0) == 0) {
        result.source.format = "lavfi";
        result.source.url = input.substr(6);
        result.source.displayName = result.source.url;
        return result;
    }
    if (input.rfind("gdigrab:", 0) == 0 && kind == MediaKind::Video) {
        result.source.format = "gdigrab";
        result.source.url = input.substr(8);
        result.source.displayName = result.source.url;
        return result;
    }
    if (input.rfind("dshow:", 0) == 0) {
        auto url = input.substr(6);
        const std::string prefix =
            kind == MediaKind::Audio ? "audio=" : "video=";
        if (url.rfind(prefix, 0) != 0) {
            url = prefix + url;
        }
        result.source.format = "dshow";
        result.source.url = std::move(url);
        result.source.displayName = result.source.url;
        return result;
    }

    std::string device_error;
    const auto id = select_dshow_device(
        kind, input, result.source.displayName, device_error);
    if (id.empty()) {
        result.error = std::move(device_error);
        return result;
    }
    result.source.format = "dshow";
    result.source.url =
        std::string(kind == MediaKind::Audio ? "audio=" : "video=") + id;
    return result;
}

} // namespace media
} // namespace iris
