#pragma once

#include "media/engine/media_interfaces.h"

#include <cstddef>
#include <cstdint>
#include <functional>

namespace iris::media::pipeline {

enum class AudioSampleFormat {
    Signed16,
};

// Views are valid only for the duration of the synchronous callback that
// receives them. Consumers that cross a thread boundary must copy the data.
struct AudioFrameView {
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
    int sampleRate = 0;
    int channels = 0;
    int samples = 0;
    int64_t pts = 0;
    AudioSampleFormat format = AudioSampleFormat::Signed16;
};

// Encoder input is always planar I420. Capture adapters own format conversion.
struct VideoFrameView {
    const std::uint8_t* data[3]{};
    int stride[3]{};
    int width = 0;
    int height = 0;
    int64_t pts = 0;
};

struct EncodedPacketView {
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
    int64_t pts = 0;
    bool keyFrame = false;
};

struct RtpParameters {
    std::uint8_t payloadType = 0;
    std::uint32_t clockRate = 0;
    std::size_t mtu = 1200;
};

using EncodedPacketCallback =
    std::function<void(const EncodedPacketView& packet)>;
using RtpDatagramCallback =
    std::function<MediaResult(ByteView datagram)>;

} // namespace iris::media::pipeline
