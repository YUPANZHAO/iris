#pragma once

#include "iris/media.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>

namespace iris::media {

struct ByteView {
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
};

struct MediaResult {
    bool ok = false;
    std::string error;

    static MediaResult success() {
        return {true, {}};
    }

    static MediaResult failure(std::string message) {
        return {false, std::move(message)};
    }
};

struct SdpResult {
    bool ok = false;
    std::string sdp;
    std::string error;

    static SdpResult success(std::string value) {
        return {true, std::move(value), {}};
    }

    static SdpResult failure(std::string message) {
        return {false, {}, std::move(message)};
    }
};

struct TransportConfig {
    std::string bindAddress;
    std::string advertiseAddress;
    std::uint16_t rtpPortMin = 0;
    std::uint16_t rtpPortMax = 0;
    bool rtcpMux = true;
};

struct TransportDescription {
    std::string address;
    std::uint16_t rtpPort = 0;
    std::uint16_t rtcpPort = 0;
    bool rtcpMux = true;
    bool secure = false;
};

// The callback runs on the PJLIB ioqueue worker thread. The packet memory is
// valid only during the callback; consumers must copy it before returning.
// Callbacks must not block or wait for the UI/Core thread.
using PacketHandler =
    std::function<void(MediaKind kind, bool isRtcp, ByteView packet)>;

// Transports exchange complete RTP/RTCP datagrams. RTP packetization is owned
// by the media backend, never by the transport and never by both layers.
class IMediaTransport {
public:
    virtual ~IMediaTransport() = default;

    virtual MediaResult open(const TransportConfig& config) = 0;
    virtual void close() noexcept = 0;
    virtual TransportDescription description() const = 0;
    virtual void setPacketHandler(PacketHandler handler) = 0;
    virtual MediaResult sendRtp(MediaKind kind, ByteView packet) = 0;
    virtual MediaResult sendRtcp(MediaKind kind, ByteView packet) = 0;
};

class IMediaPacketSink {
public:
    virtual ~IMediaPacketSink() = default;

    virtual MediaResult sendRtp(MediaKind kind, ByteView packet) = 0;
    virtual MediaResult sendRtcp(MediaKind kind, ByteView packet) = 0;
};

class IVideoFrameSink {
public:
    virtual ~IVideoFrameSink() = default;

    virtual bool active() const noexcept = 0;
    virtual void onVideoFrame(const VideoFrameView& frame) = 0;
};

// The backend owns capture, encoding, packetization, jitter buffering,
// decoding, rendering, loss recovery and congestion control as one coherent
// media implementation. A baseline FFmpeg backend and a future WebRTC backend
// both implement this interface.
class IMediaBackend {
public:
    virtual ~IMediaBackend() = default;

    virtual MediaResult start(const MediaSessionConfig& config,
                              IMediaPacketSink& packetSink) = 0;
    virtual void stop() noexcept = 0;
    virtual MediaResult onReceivedPacket(MediaKind kind,
                                         bool isRtcp,
                                         ByteView packet) = 0;

    virtual MediaResult setAudioMuted(bool muted) = 0;
    virtual MediaResult setVideoMuted(bool muted) = 0;
    virtual MediaResult setTargetBitrate(MediaKind kind,
                                         std::uint32_t bitrateKbps) = 0;
    virtual MediaResult requestVideoKeyFrame() = 0;
    virtual MediaStats stats() const = 0;
};

class ISdpController {
public:
    virtual ~ISdpController() = default;

    virtual SdpResult createOffer(const MediaSessionConfig& config,
                                  const TransportDescription& transport) = 0;
    virtual SdpResult createAnswer(const CallId& callId,
                                   const std::string& remoteOffer,
                                   const TransportDescription& transport) = 0;
    virtual MediaResult applyRemoteAnswer(
        const CallId& callId,
        const std::string& remoteAnswer) = 0;
};

// MediaEngine composes one SDP controller, one transport, and one backend per
// call. Control operations run on the Iris Core thread; media data flows
// directly between backend and transport threads.
class IMediaEngine {
public:
    virtual ~IMediaEngine() = default;

    virtual SdpResult prepareOutgoing(const MediaSessionConfig& config) = 0;
    virtual SdpResult prepareAnswer(const CallId& callId,
                                    const std::string& remoteOffer) = 0;
    virtual MediaResult applyRemoteAnswer(
        const CallId& callId,
        const std::string& remoteAnswer) = 0;
    virtual MediaResult start(const CallId& callId) = 0;
    virtual void stop(const CallId& callId) noexcept = 0;
    virtual MediaStats stats(const CallId& callId) const = 0;
};

} // namespace iris::media

