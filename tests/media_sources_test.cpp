#include "iris/iris.h"

#include <iostream>

int main() {
    const auto audioDevices =
        iris::queryMediaDevices(iris::MediaKind::Audio);
    const auto videoDevices =
        iris::queryMediaDevices(iris::MediaKind::Video);
    std::cout << "audio devices: " << audioDevices.devices.size()
              << ", video devices: " << videoDevices.devices.size()
              << '\n';

    iris::RuntimeConfig config;
    config.media.audioInput = "null";
    config.media.videoInput = "null";
    config.media.renderAudio = false;
    config.media.renderVideo = false;
    iris::Runtime runtime(config);
    runtime.setMediaInput(iris::MediaKind::Audio, "null");
    runtime.setMediaInput(iris::MediaKind::Video, "null");

    if (runtime.mediaInput(iris::MediaKind::Audio) != "null" ||
        runtime.mediaInput(iris::MediaKind::Video) != "null") {
        std::cerr << "runtime media input selection was not retained\n";
        return 1;
    }
    std::cout << "media source selection API passed\n";
    return 0;
}
