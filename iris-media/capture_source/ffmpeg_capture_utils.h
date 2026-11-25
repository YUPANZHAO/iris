#pragma once

extern "C" {
#include <libavdevice/avdevice.h>
}

#include <atomic>
#include <mutex>

namespace iris::media::capture::detail {

inline void ensureDevicesRegistered() {
    static std::once_flag registered;
    std::call_once(registered, [] {
        avdevice_register_all();
    });
}

inline int interruptCapture(void* opaque) {
    const auto* interrupted =
        static_cast<const std::atomic<bool>*>(opaque);
    return interrupted && interrupted->load() ? 1 : 0;
}

} // namespace iris::media::capture::detail
