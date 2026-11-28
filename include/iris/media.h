#pragma once

#include "capture_source/device_query.h"
#include "codec/codec_types.h"
#include "iris/types.h"
#include "media_types.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace iris {

using MediaKind = media::MediaKind;
using MediaDirection = media::MediaDirection;
using AudioSampleFormat = media::codec::AudioSampleFormat;
using VideoPixelFormat = media::codec::VideoPixelFormat;
using Rational = media::codec::Rational;
using AudioFormat = media::codec::AudioFormat;
using VideoFormat = media::codec::VideoFormat;

enum class VideoFrameKind {
    Remote,
    LocalPreview,
};

struct MediaCodecPreference {
    MediaKind kind = MediaKind::Audio;
    std::string encoding;
    int clockRate = 0;
    int channels = 0;
    int payloadType = -1;
    std::string fmtp;
};

struct MediaRuntimeConfig {
    bool audioEnabled = true;
    bool videoEnabled = true;
    std::string mediaAddress = "auto";
    std::string audioInput = "auto";
    std::string videoInput = "auto";
    std::vector<MediaCodecPreference> codecPreferences;
    int audioBitrateKbps = 64;
    int videoBitrateKbps = 800;
    int videoWidth = 640;
    int videoHeight = 360;
    int videoFps = 15;
};

using MediaDeviceInfo = media::capture::CaptureDeviceInfo;

struct MediaDeviceCatalog {
    std::vector<MediaDeviceInfo> devices;
    std::string error;

    bool ok() const noexcept { return error.empty(); }
};

// Enumerates the platform capture devices available to the FFmpeg backend.
// Device ids can be passed to Runtime::setMediaInput() or iris-cli.
MediaDeviceCatalog queryMediaDevices(MediaKind kind);

struct MediaStats {
    std::uint64_t packetsSent = 0;
    std::uint64_t sendErrors = 0;
    std::uint64_t packetsReceived = 0;
    std::uint64_t packetsLost = 0;
    std::uint64_t bytesSent = 0;
    std::uint64_t bytesReceived = 0;
    std::uint64_t audioFramesDecoded = 0;
    std::uint64_t videoFramesDecoded = 0;
    std::uint32_t roundTripTimeMs = 0;
    std::uint32_t jitterMs = 0;
    std::uint32_t estimatedSendBitrateKbps = 0;
    std::uint32_t estimatedReceiveBitrateKbps = 0;
};

struct VideoFrameView {
    CallId callId;
    VideoFrameKind kind = VideoFrameKind::Remote;
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
    int width = 0;
    int height = 0;
    int stride = 0;
    std::uint64_t timestampMs = 0;
};

struct AudioFrameView {
    CallId callId;
    AudioFormat format;
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
    std::int64_t ptsMicros = 0;
};

// Called on a media decode/capture thread. The frame bytes are valid only
// for the duration of the call; consumers that retain a frame must copy it.
using VideoFrameCallback =
    std::function<void(const VideoFrameView& frame)>;
using AudioFrameCallback =
    std::function<void(const AudioFrameView& frame)>;

inline const char* to_string(MediaKind kind) noexcept {
    switch (kind) {
    case MediaKind::Audio: return "audio";
    case MediaKind::Video: return "video";
    case MediaKind::Unknown: return "unknown";
    }
    return "unknown";
}

inline const char* to_string(MediaDirection direction) noexcept {
    switch (direction) {
    case MediaDirection::Inactive: return "inactive";
    case MediaDirection::SendOnly: return "send-only";
    case MediaDirection::RecvOnly: return "recv-only";
    case MediaDirection::SendRecv: return "send-recv";
    }
    return "unknown";
}

} // namespace iris

