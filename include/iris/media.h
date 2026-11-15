#pragma once

#include "iris/types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace iris {

enum class MediaKind {
    Audio,
    Video,
};

enum class MediaDirection {
    Inactive,
    SendOnly,
    RecvOnly,
    SendRecv,
};

enum class MediaSessionState {
    Idle,
    Preparing,
    Negotiating,
    Starting,
    Active,
    Reconfiguring,
    Stopping,
    Stopped,
    Failed,
};

enum class AudioSampleFormat {
    Signed16,
    Float32,
};

enum class VideoPixelFormat {
    I420,
    NV12,
    BGRA,
};

struct Rational {
    int numerator = 0;
    int denominator = 1;
};

struct AudioFormat {
    int sampleRate = 0;
    int channels = 0;
    AudioSampleFormat sampleFormat = AudioSampleFormat::Signed16;
};

struct VideoFormat {
    int width = 0;
    int height = 0;
    Rational frameRate;
    VideoPixelFormat pixelFormat = VideoPixelFormat::I420;
};

struct MediaCodecPreference {
    MediaKind kind = MediaKind::Audio;
    std::string encoding;
    int clockRate = 0;
    int channels = 0;
    int payloadType = -1;
    std::string id;
    std::string fmtp;
};

struct MediaDeviceConfig {
    std::string audioCaptureId;
    std::string audioRenderId;
    std::string videoCaptureId;
    std::string videoRenderId;
};

struct MediaRuntimeConfig {
    bool audioEnabled = true;
    bool videoEnabled = true;
    bool renderAudio = true;
    bool renderVideo = true;
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

struct MediaDeviceInfo {
    MediaKind kind = MediaKind::Audio;
    std::string id;
    std::string name;
    bool isDefault = false;
};

struct MediaDeviceCatalog {
    std::vector<MediaDeviceInfo> devices;
    std::string error;

    bool ok() const noexcept { return error.empty(); }
};

// Enumerates the platform capture devices available to the FFmpeg backend.
// Device ids can be passed to Runtime::setMediaInput() or iris-cli.
MediaDeviceCatalog queryMediaDevices(MediaKind kind);

struct MediaSessionConfig {
    CallId callId;
    std::string localUri;
    bool audioEnabled = true;
    bool videoEnabled = true;
    MediaDirection audioDirection = MediaDirection::SendRecv;
    MediaDirection videoDirection = MediaDirection::SendRecv;
    AudioFormat audioFormat;
    VideoFormat videoFormat;
    MediaDeviceConfig devices;
    std::vector<MediaCodecPreference> codecPreferences;
};

struct NegotiatedMediaStream {
    MediaKind kind = MediaKind::Audio;
    std::string mid;
    std::string encoding;
    int clockRate = 0;
    int channels = 0;
    int payloadType = -1;
    MediaDirection direction = MediaDirection::Inactive;

    // RTCP multiplexing, RTX, SSRC and RTP header extensions will be added
    // when the corresponding SDP/RTP milestone is implemented.
    bool rtcpMux = false;
};

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

inline const char* to_string(MediaKind kind) noexcept {
    switch (kind) {
    case MediaKind::Audio: return "audio";
    case MediaKind::Video: return "video";
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

inline const char* to_string(MediaSessionState state) noexcept {
    switch (state) {
    case MediaSessionState::Idle: return "idle";
    case MediaSessionState::Preparing: return "preparing";
    case MediaSessionState::Negotiating: return "negotiating";
    case MediaSessionState::Starting: return "starting";
    case MediaSessionState::Active: return "active";
    case MediaSessionState::Reconfiguring: return "reconfiguring";
    case MediaSessionState::Stopping: return "stopping";
    case MediaSessionState::Stopped: return "stopped";
    case MediaSessionState::Failed: return "failed";
    }
    return "unknown";
}

} // namespace iris

