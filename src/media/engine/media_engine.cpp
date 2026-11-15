#include "media/engine/media_engine.h"

#include "media/backend/ffmpeg/ffmpeg_media_backend.h"
#include "media/transport/pj_ioqueue_service.h"
#include "media/transport/pj_ioqueue_udp_transport.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <utility>

namespace iris::media {
namespace {

std::string session_id(const std::string& value) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const unsigned char ch : value) {
        hash ^= ch;
        hash *= 1099511628211ULL;
    }
    hash ^= static_cast<std::uint64_t>(
        std::chrono::system_clock::now()
            .time_since_epoch().count());
    hash *= 1099511628211ULL;
    return std::to_string(hash);
}

} // namespace

class VideoFrameSinkHub final : public IVideoFrameSink {
public:
    void setCallback(VideoFrameCallback callback) {
        std::lock_guard<std::mutex> lock(mutex_);
        callback_ = std::move(callback);
    }

    bool active() const noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        return static_cast<bool>(callback_);
    }

    void onVideoFrame(const VideoFrameView& frame) override {
        VideoFrameCallback callback;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            callback = callback_;
        }
        if (!callback) {
            return;
        }
        try {
            callback(frame);
        } catch (...) {
            // User callbacks must not terminate a media thread.
        }
    }

private:
    mutable std::mutex mutex_;
    VideoFrameCallback callback_;
};

struct MediaEngine::Session final : public IMediaPacketSink {
    Session(CallId value,
            const std::string& localUri,
            const MediaRuntimeConfig& runtime,
            PjIoqueueService& io_service,
            const CodecRegistry& codec_registry)
        : callId(std::move(value)),
          sdpSessionId(session_id(callId.value)),
          audioTransport(MediaKind::Audio, io_service),
          videoTransport(MediaKind::Video, io_service) {
        config.callId = callId;
        config.localUri = localUri;
        config.audioEnabled = runtime.audioEnabled;
        config.videoEnabled = runtime.videoEnabled;
        config.audioDirection = MediaDirection::SendRecv;
        config.videoDirection = MediaDirection::SendRecv;
        config.audioFormat = AudioFormat{48000, 2,
                                         AudioSampleFormat::Signed16};
        config.videoFormat = VideoFormat{
            runtime.videoWidth,
            runtime.videoHeight,
            Rational{runtime.videoFps, 1},
            VideoPixelFormat::I420
        };
        config.codecPreferences = codec_registry.preferences();
    }

    MediaResult sendRtp(MediaKind kind, ByteView packet) override {
        return send(kind, false, packet);
    }

    MediaResult sendRtcp(MediaKind kind, ByteView packet) override {
        return send(kind, true, packet);
    }

    MediaResult send(MediaKind kind, bool rtcp, ByteView packet) {
        if (kind == MediaKind::Audio && audioOpen) {
            return rtcp ? audioTransport.sendRtcp(kind, packet)
                        : audioTransport.sendRtp(kind, packet);
        }
        if (kind == MediaKind::Video && videoOpen) {
            return rtcp ? videoTransport.sendRtcp(kind, packet)
                        : videoTransport.sendRtp(kind, packet);
        }
        return MediaResult::failure("media transport is not open");
    }

    CallId callId;
    std::string sdpSessionId;
    std::uint64_t sdpVersion = 1;
    MediaSessionConfig config;
    PjIoqueueUdpTransport audioTransport;
    PjIoqueueUdpTransport videoTransport;
    bool audioOpen = false;
    bool videoOpen = false;
    bool remoteConfigured = false;
    bool backendDirty = false;
    std::atomic<bool> started{false};
    std::unique_ptr<FfmpegMediaBackend> backend;
};

namespace {

TransportConfig transport_config(const MediaRuntimeConfig& runtime) {
    TransportConfig config;
    if (runtime.mediaAddress != "auto" &&
        !runtime.mediaAddress.empty()) {
        config.bindAddress = runtime.mediaAddress;
        config.advertiseAddress = runtime.mediaAddress;
    } else {
        config.bindAddress = "0.0.0.0";
        config.advertiseAddress = "auto";
    }
    config.rtcpMux = true;
    return config;
}

struct SelectedMedia {
    const ParsedMediaDescription* description = nullptr;
    CodecSelection codec;
};

SelectedMedia select_media(
    const ParsedSdp& sdp,
    MediaKind kind,
    const CodecRegistry& registry) {
    for (const auto& media : sdp.media) {
        if (media.kind != kind || !media.supported ||
            media.protocol != "RTP/AVP" || media.port == 0 ||
            media.direction == MediaDirection::Inactive) {
            continue;
        }
        const auto selection = registry.select(kind, media.formats);
        if (selection.local) {
            return SelectedMedia{&media, selection};
        }
    }
    return {};
}

MediaDirection local_direction_for_remote(MediaDirection direction) {
    switch (direction) {
    case MediaDirection::SendOnly: return MediaDirection::RecvOnly;
    case MediaDirection::RecvOnly: return MediaDirection::SendOnly;
    default: return direction;
    }
}

void set_negotiated_codec(MediaSessionConfig& config,
                          MediaKind kind,
                          const CodecSelection& selection) {
    if (!selection.local) {
        return;
    }
    config.codecPreferences.erase(
        std::remove_if(
            config.codecPreferences.begin(),
            config.codecPreferences.end(),
            [kind](const auto& preference) {
                return preference.kind == kind;
            }),
        config.codecPreferences.end());
    config.codecPreferences.push_back(MediaCodecPreference{
        kind,
        selection.local->encoding,
        selection.local->clockRate,
        selection.local->channels,
        selection.payloadType,
        selection.local->id,
        selection.local->fmtp,
    });
}

} // namespace

MediaEngine::MediaEngine(MediaRuntimeConfig runtime,
                         PjIoqueueService& io_service)
    : runtime_(std::move(runtime)),
      io_service_(io_service),
      codec_registry_(runtime_.codecPreferences),
      frame_sink_(std::make_shared<VideoFrameSinkHub>()) {
}

MediaEngine::~MediaEngine() {
    stopAll();
}

SdpResult MediaEngine::prepareOutgoing(
    const CallId& callId,
    const std::string& localUri) {
    stop(callId);
    auto session = std::make_unique<Session>(
        callId, localUri, runtime_, io_service_, codec_registry_);
    const auto base_transport = transport_config(runtime_);

    if (session->config.audioEnabled) {
        auto result = session->audioTransport.open(base_transport);
        if (!result.ok) {
            return SdpResult::failure(result.error);
        }
        session->audioOpen = true;
    }
    if (session->config.videoEnabled) {
        auto result = session->videoTransport.open(base_transport);
        if (!result.ok) {
            return SdpResult::failure(result.error);
        }
        session->videoOpen = true;
    }

    LocalSdpPorts ports;
    const auto& first = session->audioOpen
        ? session->audioTransport.description()
        : session->videoTransport.description();
    ports.address = first.address;
    ports.audioPort = session->audioOpen
        ? session->audioTransport.description().rtpPort
        : 0;
    ports.videoPort = session->videoOpen
        ? session->videoTransport.description().rtpPort
        : 0;
    const SdpOrigin origin{
        session->sdpSessionId, session->sdpVersion};
    auto result = create_sdp_offer(
        session->config, ports, codec_registry_,
        io_service_.pool_factory(), &origin);
    if (!result.ok) {
        return result;
    }

    session->audioTransport.setPacketHandler(
        [session = session.get()](MediaKind kind, bool rtcp, ByteView packet) {
            if (session->started && session->backend) {
                session->backend->onReceivedPacket(kind, rtcp, packet);
            }
        });
    session->videoTransport.setPacketHandler(
        [session = session.get()](MediaKind kind, bool rtcp, ByteView packet) {
            if (session->started && session->backend) {
                session->backend->onReceivedPacket(kind, rtcp, packet);
            }
        });
    sessions_[callId.value] = std::move(session);
    return result;
}

SdpResult MediaEngine::prepareAnswer(const CallId& callId,
                                     const std::string& localUri,
                                     const std::string& remoteOffer) {
    stop(callId);
    const auto offer = parse_sdp(
        remoteOffer, io_service_.pool_factory());
    if (offer.media.empty()) {
        return SdpResult::failure("remote offer has no supported media");
    }

    auto session = std::make_unique<Session>(
        callId, localUri, runtime_, io_service_, codec_registry_);
    const auto offered_audio = select_media(
        offer, MediaKind::Audio, codec_registry_);
    const auto offered_video = select_media(
        offer, MediaKind::Video, codec_registry_);
    session->config.audioEnabled =
        runtime_.audioEnabled && offered_audio.description;
    session->config.videoEnabled =
        runtime_.videoEnabled && offered_video.description;
    const auto base_transport = transport_config(runtime_);

    if (session->config.audioEnabled) {
        auto result = session->audioTransport.open(base_transport);
        if (!result.ok) {
            return SdpResult::failure(result.error);
        }
        session->audioOpen = true;
    }
    if (session->config.videoEnabled) {
        auto result = session->videoTransport.open(base_transport);
        if (!result.ok) {
            return SdpResult::failure(result.error);
        }
        session->videoOpen = true;
    }

    LocalSdpPorts ports;
    const auto& first = session->audioOpen
        ? session->audioTransport.description()
        : session->videoTransport.description();
    ports.address = first.address;
    ports.audioPort = session->audioOpen
        ? session->audioTransport.description().rtpPort
        : 0;
    ports.videoPort = session->videoOpen
        ? session->videoTransport.description().rtpPort
        : 0;
    const SdpOrigin origin{
        session->sdpSessionId, session->sdpVersion};
    auto result = create_sdp_answer(
        session->config, ports, remoteOffer, codec_registry_,
        io_service_.pool_factory(), &origin);
    if (!result.ok) {
        return result;
    }

    if (offered_audio.description && session->audioOpen) {
        set_negotiated_codec(
            session->config, MediaKind::Audio, offered_audio.codec);
        session->config.audioDirection =
            local_direction_for_remote(
                offered_audio.description->direction);
        auto remote = session->audioTransport.setRemote(
            offered_audio.description->address,
            offered_audio.description->port);
        if (!remote.ok) {
            return SdpResult::failure(remote.error);
        }
    }
    if (offered_video.description && session->videoOpen) {
        set_negotiated_codec(
            session->config, MediaKind::Video, offered_video.codec);
        session->config.videoDirection =
            local_direction_for_remote(
                offered_video.description->direction);
        auto remote = session->videoTransport.setRemote(
            offered_video.description->address,
            offered_video.description->port);
        if (!remote.ok) {
            return SdpResult::failure(remote.error);
        }
    }
    session->remoteConfigured = true;

    session->audioTransport.setPacketHandler(
        [session = session.get()](MediaKind kind, bool rtcp, ByteView packet) {
            if (session->started && session->backend) {
                session->backend->onReceivedPacket(kind, rtcp, packet);
            }
        });
    session->videoTransport.setPacketHandler(
        [session = session.get()](MediaKind kind, bool rtcp, ByteView packet) {
            if (session->started && session->backend) {
                session->backend->onReceivedPacket(kind, rtcp, packet);
            }
        });
    sessions_[callId.value] = std::move(session);
    return result;
}

MediaResult MediaEngine::applyRemoteAnswer(
    const CallId& callId,
    const std::string& remoteAnswer) {
    auto* session = findSession(callId);
    if (!session) {
        return MediaResult::failure("media session does not exist");
    }
    const auto answer = parse_sdp(
        remoteAnswer, io_service_.pool_factory());
    const bool wasStarted = session->started.load(
        std::memory_order_acquire);
    const auto selected_audio = select_media(
        answer, MediaKind::Audio, codec_registry_);
    const auto selected_video = select_media(
        answer, MediaKind::Video, codec_registry_);
    if (session->audioOpen) {
        if (!selected_audio.description ||
            selected_audio.description->port == 0) {
            session->config.audioEnabled = false;
        } else {
            set_negotiated_codec(
                session->config, MediaKind::Audio,
                selected_audio.codec);
            session->config.audioDirection =
                local_direction_for_remote(
                    selected_audio.description->direction);
            auto result = session->audioTransport.setRemote(
                selected_audio.description->address,
                selected_audio.description->port);
            if (!result.ok) {
                return result;
            }
        }
    }
    if (session->videoOpen) {
        if (!selected_video.description ||
            selected_video.description->port == 0) {
            session->config.videoEnabled = false;
        } else {
            set_negotiated_codec(
                session->config, MediaKind::Video,
                selected_video.codec);
            session->config.videoDirection =
                local_direction_for_remote(
                    selected_video.description->direction);
            auto result = session->videoTransport.setRemote(
                selected_video.description->address,
                selected_video.description->port);
            if (!result.ok) {
                return result;
            }
        }
    }
    session->remoteConfigured =
        session->config.audioEnabled || session->config.videoEnabled;
    session->backendDirty = wasStarted;
    if (!session->remoteConfigured) {
        suspend(callId);
        return MediaResult::failure(
            "remote endpoint rejected all media");
    }
    return MediaResult::success();
}

SdpResult MediaEngine::createReinviteOffer(
    const CallId& callId,
    MediaDirection audioDirection,
    MediaDirection videoDirection) {
    auto* session = findSession(callId);
    if (!session) {
        return SdpResult::failure("media session does not exist");
    }

    auto config = session->config;
    config.audioDirection = audioDirection;
    config.videoDirection = videoDirection;
    LocalSdpPorts ports;
    const auto& first = session->audioOpen
        ? session->audioTransport.description()
        : session->videoTransport.description();
    ports.address = first.address;
    ports.audioPort = session->audioOpen
        ? session->audioTransport.description().rtpPort
        : 0;
    ports.videoPort = session->videoOpen
        ? session->videoTransport.description().rtpPort
        : 0;
    const SdpOrigin origin{
        session->sdpSessionId, ++session->sdpVersion};
    return create_sdp_offer(
        config, ports, codec_registry_,
        io_service_.pool_factory(), &origin);
}

SdpResult MediaEngine::createReinviteAnswer(
    const CallId& callId,
    const std::string& remoteOffer) {
    auto* session = findSession(callId);
    if (!session) {
        return SdpResult::failure("media session does not exist");
    }
    const auto offer =
        parse_sdp(remoteOffer, io_service_.pool_factory());
    if (offer.media.empty()) {
        return SdpResult::failure("remote offer has no media");
    }

    const auto offered_audio =
        select_media(offer, MediaKind::Audio, codec_registry_);
    const auto offered_video =
        select_media(offer, MediaKind::Video, codec_registry_);
    auto config = session->config;
    config.audioEnabled =
        runtime_.audioEnabled && offered_audio.description;
    config.videoEnabled =
        runtime_.videoEnabled && offered_video.description;
    if (!config.audioEnabled && !config.videoEnabled) {
        return SdpResult::failure("no mutually supported media");
    }

    const auto base_transport = transport_config(runtime_);
    if (config.audioEnabled && !session->audioOpen) {
        auto result = session->audioTransport.open(base_transport);
        if (!result.ok) {
            return SdpResult::failure(result.error);
        }
        session->audioOpen = true;
    }
    if (config.videoEnabled && !session->videoOpen) {
        auto result = session->videoTransport.open(base_transport);
        if (!result.ok) {
            return SdpResult::failure(result.error);
        }
        session->videoOpen = true;
    }

    LocalSdpPorts ports;
    const auto& first = session->audioOpen
        ? session->audioTransport.description()
        : session->videoTransport.description();
    ports.address = first.address;
    ports.audioPort = session->audioOpen
        ? session->audioTransport.description().rtpPort
        : 0;
    ports.videoPort = session->videoOpen
        ? session->videoTransport.description().rtpPort
        : 0;
    const SdpOrigin origin{
        session->sdpSessionId, session->sdpVersion};
    return create_sdp_answer(
        config, ports, remoteOffer, codec_registry_,
        io_service_.pool_factory(), &origin);
}

MediaResult MediaEngine::applyIncomingOffer(
    const CallId& callId,
    const std::string& remoteOffer) {
    auto* session = findSession(callId);
    if (!session) {
        return MediaResult::failure("media session does not exist");
    }
    const auto offer =
        parse_sdp(remoteOffer, io_service_.pool_factory());
    const auto selected_audio =
        select_media(offer, MediaKind::Audio, codec_registry_);
    const auto selected_video =
        select_media(offer, MediaKind::Video, codec_registry_);

    session->config.audioEnabled =
        runtime_.audioEnabled && selected_audio.description &&
        session->audioOpen;
    session->config.videoEnabled =
        runtime_.videoEnabled && selected_video.description &&
        session->videoOpen;
    if (session->config.audioEnabled) {
        set_negotiated_codec(
            session->config, MediaKind::Audio, selected_audio.codec);
        session->config.audioDirection =
            local_direction_for_remote(
                selected_audio.description->direction);
        auto result = session->audioTransport.setRemote(
            selected_audio.description->address,
            selected_audio.description->port);
        if (!result.ok) {
            return result;
        }
    }
    if (session->config.videoEnabled) {
        set_negotiated_codec(
            session->config, MediaKind::Video, selected_video.codec);
        session->config.videoDirection =
            local_direction_for_remote(
                selected_video.description->direction);
        auto result = session->videoTransport.setRemote(
            selected_video.description->address,
            selected_video.description->port);
        if (!result.ok) {
            return result;
        }
    }
    session->remoteConfigured =
        session->config.audioEnabled || session->config.videoEnabled;
    session->backendDirty = session->started.load(
        std::memory_order_acquire);
    if (!session->remoteConfigured) {
        suspend(callId);
        return MediaResult::failure(
            "remote endpoint rejected all media");
    }
    return MediaResult::success();
}

MediaResult MediaEngine::start(const CallId& callId) {
    auto* session = findSession(callId);
    if (!session) {
        return MediaResult::failure("media session does not exist");
    }
    if (session->started) {
        return MediaResult::success();
    }
    if (!session->remoteConfigured) {
        return MediaResult::failure("remote media is not configured");
    }
    session->backend =
        std::make_unique<FfmpegMediaBackend>(
            runtime_, frame_sink_.get());
    auto result = session->backend->start(session->config, *session);
    if (!result.ok) {
        session->backend.reset();
        return result;
    }
    session->started = true;
    session->backendDirty = false;
    return MediaResult::success();
}

MediaResult MediaEngine::suspend(const CallId& callId) {
    auto* session = findSession(callId);
    if (!session) {
        return MediaResult::failure("media session does not exist");
    }
    session->started.store(false, std::memory_order_release);
    if (session->backend) {
        session->backend->stop();
        session->backend.reset();
    }
    return MediaResult::success();
}

MediaResult MediaEngine::reconcileActivity(const CallId& callId) {
    auto* session = findSession(callId);
    if (!session) {
        return MediaResult::failure("media session does not exist");
    }
    const bool audioActive =
        session->config.audioEnabled &&
        session->config.audioDirection != MediaDirection::Inactive;
    const bool videoActive =
        session->config.videoEnabled &&
        session->config.videoDirection != MediaDirection::Inactive;
    if (!audioActive && !videoActive) {
        return suspend(callId);
    }
    if (session->started && !session->backendDirty) {
        return MediaResult::success();
    }
    if (session->backend) {
        const auto suspended = suspend(callId);
        if (!suspended.ok) {
            return suspended;
        }
    }
    return start(callId);
}

void MediaEngine::stop(const CallId& callId) noexcept {
    auto iterator = sessions_.find(callId.value);
    if (iterator == sessions_.end()) {
        return;
    }
    iterator->second->started.store(false, std::memory_order_release);
    iterator->second->audioTransport.close();
    iterator->second->videoTransport.close();
    if (iterator->second->backend) {
        iterator->second->backend->stop();
        iterator->second->backend.reset();
    }
    sessions_.erase(iterator);
}

void MediaEngine::stopAll() noexcept {
    for (auto& entry : sessions_) {
        entry.second->started.store(false, std::memory_order_release);
        entry.second->audioTransport.close();
        entry.second->videoTransport.close();
        if (entry.second->backend) {
            entry.second->backend->stop();
            entry.second->backend.reset();
        }
    }
    sessions_.clear();
}

MediaResult MediaEngine::setAudioMuted(const CallId& callId, bool muted) {
    auto* session = findSession(callId);
    return session && session->backend
        ? session->backend->setAudioMuted(muted)
        : MediaResult::failure("media session is not active");
}

MediaResult MediaEngine::setVideoMuted(const CallId& callId, bool muted) {
    auto* session = findSession(callId);
    return session && session->backend
        ? session->backend->setVideoMuted(muted)
        : MediaResult::failure("media session is not active");
}

void MediaEngine::setInputSource(MediaKind kind,
                                 const std::string& source) {
    auto& input = kind == MediaKind::Audio
        ? runtime_.audioInput
        : runtime_.videoInput;
    input = source.empty() ? "auto" : source;
}

std::string MediaEngine::inputSource(MediaKind kind) const {
    return kind == MediaKind::Audio
        ? runtime_.audioInput
        : runtime_.videoInput;
}

void MediaEngine::setVideoFrameCallback(
    VideoFrameCallback callback) {
    frame_sink_->setCallback(std::move(callback));
}

MediaStats MediaEngine::stats(const CallId& callId) const {
    const auto* session = findSession(callId);
    return session && session->backend
        ? session->backend->stats()
        : MediaStats{};
}

MediaEngine::Session& MediaEngine::ensureSession(const CallId& callId) {
    auto iterator = sessions_.find(callId.value);
    if (iterator == sessions_.end()) {
        throw std::runtime_error("media session does not exist");
    }
    return *iterator->second;
}

MediaEngine::Session* MediaEngine::findSession(
    const CallId& callId) const {
    auto iterator = sessions_.find(callId.value);
    return iterator == sessions_.end() ? nullptr : iterator->second.get();
}

} // namespace iris::media

