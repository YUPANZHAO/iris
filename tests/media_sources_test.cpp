#include "iris/iris.h"
#include "media/backend/ffmpeg/null_video_image.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>

namespace {

void print_catalog(iris::MediaKind kind,
                   const iris::MediaDeviceCatalog& catalog) {
    std::cout << iris::to_string(kind) << " devices:\n";
    if (!catalog.ok()) {
        std::cout << "  error: " << catalog.error << '\n';
        return;
    }
    if (catalog.devices.empty()) {
        std::cout << "  none\n";
        return;
    }
    for (std::size_t index = 0;
         index < catalog.devices.size();
         ++index) {
        const auto& device = catalog.devices[index];
        std::cout << "  [" << index << "]"
                  << (device.isDefault ? " (default)" : "")
                  << " " << device.name << '\n'
                  << "      id: " << device.id << '\n';
    }
}

} // namespace

int main() {
    const auto audio_devices =
        iris::queryMediaDevices(iris::MediaKind::Audio);
    const auto video_devices =
        iris::queryMediaDevices(iris::MediaKind::Video);
    print_catalog(iris::MediaKind::Audio, audio_devices);
    print_catalog(iris::MediaKind::Video, video_devices);

    iris::RuntimeConfig config;
    config.media.audioInput = "null";
    config.media.videoInput = "null";
    iris::Runtime runtime(config);
    runtime.setMediaInput(iris::MediaKind::Audio, "null");
    runtime.setMediaInput(iris::MediaKind::Video, "null");

    if (runtime.mediaInput(iris::MediaKind::Audio) != "null" ||
        runtime.mediaInput(iris::MediaKind::Video) != "null") {
        std::cerr << "runtime media input selection was not retained\n";
        return 1;
    }

    const auto image = iris::media::renderNullVideoImage(
        640, 360, "sip:alice@example.com");
    std::size_t white_pixels = 0;
    int minimum_x = image.width;
    int maximum_x = 0;
    int minimum_y = image.height;
    int maximum_y = 0;
    for (int y = 0; y < image.height; ++y) {
        for (int x = 0; x < image.width; ++x) {
            const auto index =
                (static_cast<std::size_t>(y) * image.width + x) * 4;
            if (image.bgra[index] > 245 &&
                image.bgra[index + 1] > 245 &&
                image.bgra[index + 2] > 245) {
                ++white_pixels;
                minimum_x = std::min(minimum_x, x);
                maximum_x = std::max(maximum_x, x);
                minimum_y = std::min(minimum_y, y);
                maximum_y = std::max(maximum_y, y);
            }
        }
    }
    const int text_center_x = (minimum_x + maximum_x) / 2;
    const int text_center_y = (minimum_y + maximum_y) / 2;
    if (image.width != 640 || image.height != 360 ||
        image.bgra.size() != 640u * 360u * 4u ||
        image.bgra[0] > 60 || white_pixels < 20 ||
        std::abs(text_center_x - image.width / 2) > 8 ||
        std::abs(text_center_y - image.height / 2) > 8) {
        std::cerr << "account URI card rendering failed\n";
        return 1;
    }

    std::cout << "media source selection API passed\n";
    return 0;
}
