#include "core/media_session_manager.h"

#include "media/capture/ffmpeg_capture.h"
#include "media/control/media_control_impl.h"
#include "media/control/sdp_media_control.h"
#include "media/engine/media_buffer.h"
#include "media/sdp/sdp_components.h"
#include "media/stream/audio_stream.h"
#include "media/stream/stream_internal.h"
#include "media/stream/video_stream.h"

extern "C" {
#include <libavutil/imgutils.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace iris::core {
namespace {

namespace mv = iris::media::v2;

std::uint64_t hashCallId(const CallId& id) {
    std::uint64_t value = 1469598103934665603ULL;
    for (const unsigned char ch : id.value) {
        value ^= ch;
        value *= 1099511628211ULL;
    }
    return value == 0 ? 1 : value;
}

mv::MediaKind toV2(MediaKind kind) {
    return kind == MediaKind::Video
        ? mv::MediaKind::Video
        : mv::MediaKind::Audio;
}

mv::MediaDirection toV2(MediaDirection direction) {
    switch (direction) {
    case MediaDirection::Inactive: return mv::MediaDirection::Inactive;
    case MediaDirection::SendOnly: return mv::MediaDirection::SendOnly;
    case MediaDirection::RecvOnly: return mv::MediaDirection::RecvOnly;
    case MediaDirection::SendRecv: return mv::MediaDirection::SendRecv;
    }
    return mv::MediaDirection::Inactive;
}

MediaDirection fromV2(mv::MediaDirection direction) {
    switch (direction) {
    case mv::MediaDirection::Inactive: return MediaDirection::Inactive;
    case mv::MediaDirection::SendOnly: return MediaDirection::SendOnly;
    case mv::MediaDirection::RecvOnly: return MediaDirection::RecvOnly;
    case mv::MediaDirection::SendRecv: return MediaDirection::SendRecv;
    }
    return MediaDirection::Inactive;
}

mv::CodecType codecType(const std::string& encoding) {
    std::string lower;
    lower.reserve(encoding.size());
    for (const auto ch : encoding) {
        lower.push_back(static_cast<char>(
            std::tolower(static_cast<unsigned char>(ch))));
    }
    if (lower == "opus") {
        return mv::CodecType::Opus;
    }
    if (lower == "h264") {
        return mv::CodecType::H264;
    }
    return mv::CodecType::Unknown;
}

std::string resultError(const mv::Status& status) {
    return status.message.empty() ? "media operation failed" : status.message;
}

class VideoCallbackState final {
public:
    void set(VideoFrameCallback callback) {
        std::lock_guard<std::mutex> lock(mutex_);
        callback_ = std::move(callback);
    }

    VideoFrameCallback get() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return callback_;
    }

private:
    mutable std::mutex mutex_;
    VideoFrameCallback callback_;
};

class VideoCallbackSink final : public mv::IVideoFrameSink {
public:
    VideoCallbackSink(std::shared_ptr<VideoCallbackState> state,
                      CallId callId,
                      VideoFrameKind kind)
        : state_(std::move(state)),
          callId_(std::move(callId)),
          kind_(kind) {
    }

    ~VideoCallbackSink() override {
        sws_freeContext(scaler_);
    }

    void onVideoFrame(const mv::VideoFrame& frame) override {
        const auto callback = state_->get();
        if (!callback || frame.format.width <= 0 ||
            frame.format.height <= 0) {
            return;
        }
        const auto* source = mv::bufferViewData(frame.pixels);
        if (!source) {
            return;
        }
        const auto width = frame.format.width;
        const auto height = frame.format.height;
        auto* mutableSource = const_cast<std::uint8_t*>(source);
        std::uint8_t* planes[3] = {
            mutableSource,
            mutableSource + static_cast<std::size_t>(width) * height,
            mutableSource + static_cast<std::size_t>(width) * height +
                static_cast<std::size_t>(width / 2) * (height / 2),
        };
        scaler_ = sws_getCachedContext(
            scaler_,
            width, height, AV_PIX_FMT_YUV420P,
            width, height, AV_PIX_FMT_BGRA,
            SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!scaler_) {
            return;
        }
        const auto byteCount = av_image_get_buffer_size(
            AV_PIX_FMT_BGRA, width, height, 1);
        if (byteCount <= 0) {
            return;
        }
        std::vector<std::uint8_t> bgra(
            static_cast<std::size_t>(byteCount));
        std::uint8_t* destination[] = {bgra.data()};
        const int sourceStride[] = {
            width, width / 2, width / 2};
        const int destinationStride[] = {width * 4};
        sws_scale(
            scaler_, planes, sourceStride, 0, height,
            destination, destinationStride);
        const auto now = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
        try {
            callback(VideoFrameView{
                callId_,
                kind_,
                bgra.data(),
                bgra.size(),
                width,
                height,
                width * 4,
                now,
            });
        } catch (...) {
        }
    }

private:
    std::shared_ptr<VideoCallbackState> state_;
    CallId callId_;
    VideoFrameKind kind_ = VideoFrameKind::Remote;
    SwsContext* scaler_ = nullptr;
};

class VideoCaptureSplitter final : public mv::IVideoFrameSink {
public:
    VideoCaptureSplitter(
        std::shared_ptr<mv::IVideoFrameSink> stream,
        std::shared_ptr<mv::IVideoFrameSink> preview)
        : stream_(std::move(stream)),
          preview_(std::move(preview)) {
    }

    void onVideoFrame(const mv::VideoFrame& frame) override {
        if (stream_) {
            stream_->onVideoFrame(frame);
        }
        if (preview_) {
            preview_->onVideoFrame(frame);
        }
    }

private:
    std::shared_ptr<mv::IVideoFrameSink> stream_;
    std::shared_ptr<mv::IVideoFrameSink> preview_;
};

mv::SdpCodecPreference codecPreference(
    const MediaSessionConfig& config,
    MediaKind kind) {
    for (const auto& preference : config.codecPreferences) {
        if (preference.kind != kind) {
            continue;
        }
        mv::SdpCodecPreference result;
        result.codecType = codecType(preference.encoding);
        result.encodingName = preference.encoding;
        result.payloadType = preference.payloadType;
        result.attributes["clockRate"] =
            std::to_string(preference.clockRate);
        result.attributes["channels"] =
            std::to_string(preference.channels);
        return result;
    }
    mv::SdpCodecPreference result;
    if (kind == MediaKind::Audio) {
        result.codecType = mv::CodecType::Opus;
        result.encodingName = "opus";
        result.payloadType = 111;
        result.attributes["clockRate"] = "48000";
        result.attributes["channels"] = "2";
    } else {
        result.codecType = mv::CodecType::H264;
        result.encodingName = "H264";
        result.payloadType = 96;
        result.attributes["clockRate"] = "90000";
        result.attributes["channels"] = "0";
    }
    return result;
}

mv::SdpNegotiationInput negotiationInput(
    const MediaSessionConfig& config,
    const MediaRuntimeConfig& runtime) {
    mv::SdpNegotiationInput input;
    input.sessionId = std::to_string(hashCallId(config.callId));
    input.origin.username = "iris";
    input.origin.sessionId = input.sessionId;
    input.origin.sessionVersion = 1;
    input.connection.addressType = "IP4";

    if (config.audioEnabled) {
        mv::SdpMediaCapability audio;
        audio.kind = mv::MediaKind::Audio;
        audio.mid = "audio";
        audio.direction = toV2(config.audioDirection);
        audio.transport.protocol = "RTP/AVP";
        audio.transport.attributes["rtcpMux"] = "1";
        audio.attributes["sampleRate"] =
            std::to_string(config.audioFormat.sampleRate);
        audio.attributes["channels"] =
            std::to_string(config.audioFormat.channels);
        audio.attributes["bitrateKbps"] =
            std::to_string(runtime.audioBitrateKbps);
        audio.codecs.push_back(codecPreference(config, MediaKind::Audio));
        input.media.push_back(std::move(audio));
    }
    if (config.videoEnabled) {
        mv::SdpMediaCapability video;
        video.kind = mv::MediaKind::Video;
        video.mid = "video";
        video.direction = toV2(config.videoDirection);
        video.transport.protocol = "RTP/AVP";
        video.transport.attributes["rtcpMux"] = "1";
        video.attributes["width"] = std::to_string(runtime.videoWidth);
        video.attributes["height"] = std::to_string(runtime.videoHeight);
        video.attributes["frameRateNumerator"] =
            std::to_string(std::max(1, runtime.videoFps));
        video.attributes["frameRateDenominator"] = "1";
        video.attributes["bitrateKbps"] =
            std::to_string(runtime.videoBitrateKbps);
        video.attributes["resolution"] =
            std::to_string(runtime.videoWidth) + "x" +
            std::to_string(runtime.videoHeight);
        video.codecs.push_back(codecPreference(config, MediaKind::Video));
        input.media.push_back(std::move(video));
    }
    return input;
}

} // namespace

struct MediaSessionManager::Impl {
    struct Session {
        CallId callId;
        MediaSessionConfig config;
        mv::SDPMediaControl control;
        bool remoteConfigured = false;
        bool active = false;
        bool prepared = false;

        std::shared_ptr<mv::AudioFrameStream> audioStream;
        std::shared_ptr<mv::VideoFrameStream> videoStream;
        std::shared_ptr<VideoCaptureSplitter> videoSplitter;
        std::shared_ptr<mv::IVideoFrameSink> remoteVideoSink;
        std::unique_ptr<mv::IAudioCaptureSource> audioCapture;
        std::unique_ptr<mv::IVideoCaptureSource> videoCapture;
    };

    explicit Impl(MediaRuntimeConfig value)
        : runtime(std::move(value)),
          callbacks(std::make_shared<VideoCallbackState>()) {
    }

    Session& ensure(const CallId& callId) {
        auto [iterator, inserted] =
            sessions.try_emplace(callId.value);
        if (inserted) {
            iterator->second = std::make_unique<Session>();
            iterator->second->callId = callId;
        }
        return *iterator->second;
    }

    Session* find(const CallId& callId) const {
        const auto iterator = sessions.find(callId.value);
        return iterator == sessions.end()
            ? nullptr
            : iterator->second.get();
    }

    bool initializeControl(Session& session) {
        mv::MediaControlConfig controlConfig;
        controlConfig.name = "iris-" + session.callId.value;
        controlConfig.options["rtp.port"] = "0";
        if (!runtime.mediaAddress.empty() &&
            runtime.mediaAddress != "auto") {
            controlConfig.options["rtp.bindAddress"] =
                runtime.mediaAddress;
        }
        return session.control.initialize(controlConfig).ok;
    }

    void bindStreams(Session& session) {
        session.audioStream.reset();
        session.videoStream.reset();
        for (const auto id : session.control.streams()) {
            auto stream = session.control.stream(id);
            if (!stream) {
                continue;
            }
            if (auto audio =
                    std::dynamic_pointer_cast<mv::AudioFrameStream>(stream)) {
                session.audioStream = std::move(audio);
            } else if (auto video =
                           std::dynamic_pointer_cast<
                               mv::VideoFrameStream>(stream)) {
                session.videoStream = std::move(video);
            }
        }
        if (session.videoStream) {
            session.remoteVideoSink =
                std::make_shared<VideoCallbackSink>(
                    callbacks, session.callId,
                    VideoFrameKind::Remote);
            mv::stream_internal::setVideoOutputSink(
                session.videoStream.get(),
                session.remoteVideoSink);
        }
    }

    MediaResult startCaptures(Session& session) {
        if (session.audioStream && session.config.audioEnabled &&
            !session.audioCapture) {
            mv::CaptureConfig config;
            config.kind = mv::MediaKind::Audio;
            config.source = runtime.audioInput;
            mv::Status status;
            auto source = captureProvider.create(config, &status);
            auto* audio = dynamic_cast<mv::IAudioCaptureSource*>(
                source.get());
            if (!audio) {
                if (!status.ok) {
                    return MediaResult::failure(status.message);
                }
                return MediaResult::failure(
                    "capture provider returned a non-audio source");
            }
            mv::AudioCaptureConfig typed;
            typed.format.sampleRate =
                session.config.audioFormat.sampleRate;
            typed.format.channels = session.config.audioFormat.channels;
            audio->setAudioSink(session.audioStream);
            if (!audio->configure(typed).ok ||
                !audio->open(config).ok ||
                !audio->start().ok) {
                return MediaResult::failure(
                    "failed to start audio capture");
            }
            session.audioCapture.reset(audio);
            source.release();
        }

        if (session.videoStream && session.config.videoEnabled &&
            !session.videoCapture) {
            mv::CaptureConfig config;
            config.kind = mv::MediaKind::Video;
            config.source = runtime.videoInput;
            mv::Status status;
            auto source = captureProvider.create(config, &status);
            auto* video = dynamic_cast<mv::IVideoCaptureSource*>(
                source.get());
            if (!video) {
                if (!status.ok) {
                    return MediaResult::failure(status.message);
                }
                return MediaResult::failure(
                    "capture provider returned a non-video source");
            }
            mv::VideoCaptureConfig typed;
            typed.format.width = runtime.videoWidth;
            typed.format.height = runtime.videoHeight;
            typed.format.frameRate = mv::Rational{
                std::max(1, runtime.videoFps), 1};
            typed.format.pixelFormat = mv::VideoPixelFormat::I420;
            auto preview = std::make_shared<VideoCallbackSink>(
                callbacks, session.callId,
                VideoFrameKind::LocalPreview);
            session.videoSplitter =
                std::make_shared<VideoCaptureSplitter>(
                    std::static_pointer_cast<
                        mv::IVideoFrameSink>(session.videoStream),
                    preview);
            video->setVideoSink(session.videoSplitter);
            if (!video->configure(typed).ok ||
                !video->open(config).ok ||
                !video->start().ok) {
                return MediaResult::failure(
                    "failed to start video capture");
            }
            session.videoCapture.reset(video);
            source.release();
        }
        return MediaResult::success();
    }

    void stopCaptures(Session& session) noexcept {
        if (session.audioCapture) {
            session.audioCapture->stop();
            session.audioCapture->close();
            session.audioCapture.reset();
        }
        if (session.videoCapture) {
            session.videoCapture->stop();
            session.videoCapture->close();
            session.videoCapture.reset();
        }
        session.videoSplitter.reset();
    }

    MediaStats stats(const Session& session) const {
        MediaStats result;
        if (!session.remoteConfigured) {
            return result;
        }
        const auto& control =
            const_cast<mv::SDPMediaControl&>(session.control);
        const auto ids = control.streams();
        for (const auto id : ids) {
            auto stream = control.stream(id);
            if (!stream) {
                continue;
            }
            const auto stats = stream->stats();
            result.packetsSent += stats.transport.messagesSent;
            result.packetsReceived += stats.transport.messagesReceived;
            result.bytesSent += stats.transport.bytesSent;
            result.bytesReceived += stats.transport.bytesReceived;
            result.sendErrors += stats.transport.sendErrors;
            if (auto video =
                    std::dynamic_pointer_cast<
                        mv::VideoFrameStream>(stream)) {
                result.videoFramesDecoded +=
                    stats.media.samplesProduced;
            } else {
                result.audioFramesDecoded +=
                    stats.media.samplesProduced;
            }
        }
        return result;
    }

    MediaRuntimeConfig runtime;
    mv::FFmpegCaptureProvider captureProvider;
    std::shared_ptr<VideoCallbackState> callbacks;
    std::unordered_map<std::string, std::unique_ptr<Session>> sessions;
};

MediaSessionManager::MediaSessionManager(MediaRuntimeConfig runtime)
    : runtime_(std::move(runtime)),
      impl_(std::make_unique<Impl>(runtime_)) {
}

MediaSessionManager::~MediaSessionManager() {
    stopAll();
}

SdpResult MediaSessionManager::prepareOutgoing(
    const CallId& callId,
    const std::string& localUri) {
    stop(callId);
    auto& session = impl_->ensure(callId);
    session.config.callId = callId;
    session.config.localUri = localUri;
    session.config.audioEnabled = runtime_.audioEnabled;
    session.config.videoEnabled = runtime_.videoEnabled;
    session.config.audioDirection = MediaDirection::SendRecv;
    session.config.videoDirection = MediaDirection::SendRecv;
    session.config.audioFormat = AudioFormat{
        48000, 2, AudioSampleFormat::Signed16};
    session.config.videoFormat = VideoFormat{
        runtime_.videoWidth,
        runtime_.videoHeight,
        Rational{runtime_.videoFps, 1},
        VideoPixelFormat::I420,
    };
    session.config.codecPreferences = runtime_.codecPreferences;
    if (!impl_->initializeControl(session)) {
        return SdpResult::failure("failed to initialize media control");
    }
    mv::SdpOfferOptions options;
    options.local = negotiationInput(session.config, runtime_);
    options.replaceExistingStreams = true;
    std::string sdp;
    const auto status = session.control.createOffer(options, &sdp);
    if (!status.ok) {
        return SdpResult::failure(resultError(status));
    }
    impl_->bindStreams(session);
    session.prepared = true;
    return SdpResult::success(std::move(sdp));
}

SdpResult MediaSessionManager::prepareAnswer(
    const CallId& callId,
    const std::string& localUri,
    const std::string& remoteOffer) {
    stop(callId);
    auto& session = impl_->ensure(callId);
    session.config.callId = callId;
    session.config.localUri = localUri;
    session.config.audioEnabled = runtime_.audioEnabled;
    session.config.videoEnabled = runtime_.videoEnabled;
    session.config.audioDirection = MediaDirection::SendRecv;
    session.config.videoDirection = MediaDirection::SendRecv;
    session.config.audioFormat = AudioFormat{
        48000, 2, AudioSampleFormat::Signed16};
    session.config.videoFormat = VideoFormat{
        runtime_.videoWidth,
        runtime_.videoHeight,
        Rational{runtime_.videoFps, 1},
        VideoPixelFormat::I420,
    };
    session.config.codecPreferences = runtime_.codecPreferences;
    if (!impl_->initializeControl(session)) {
        return SdpResult::failure("failed to initialize media control");
    }
    mv::SdpAnswerOptions options;
    options.local = negotiationInput(session.config, runtime_);
    options.remoteOffer = remoteOffer;
    std::string sdp;
    const auto status = session.control.createAnswer(options, &sdp);
    if (!status.ok) {
        return SdpResult::failure(resultError(status));
    }
    impl_->bindStreams(session);
    session.remoteConfigured = true;
    session.prepared = true;
    return SdpResult::success(std::move(sdp));
}

MediaResult MediaSessionManager::applyRemoteAnswer(
    const CallId& callId,
    const std::string& remoteAnswer) {
    auto* session = impl_->find(callId);
    if (!session) {
        return MediaResult::failure("media session does not exist");
    }
    const auto status =
        session->control.applyRemoteAnswer(remoteAnswer);
    if (!status.ok) {
        return MediaResult::failure(resultError(status));
    }
    impl_->bindStreams(*session);
    session->remoteConfigured = true;
    return MediaResult::success();
}

MediaResult MediaSessionManager::start(const CallId& callId) {
    auto* session = impl_->find(callId);
    if (!session) {
        return MediaResult::failure("media session does not exist");
    }
    const auto status = session->control.startAll();
    if (!status.ok) {
        return MediaResult::failure(resultError(status));
    }
    const auto capture = impl_->startCaptures(*session);
    if (!capture.ok) {
        return capture;
    }
    session->active = true;
    return MediaResult::success();
}

MediaResult MediaSessionManager::suspend(const CallId& callId) {
    auto* session = impl_->find(callId);
    if (!session) {
        return MediaResult::failure("media session does not exist");
    }
    impl_->stopCaptures(*session);
    session->active = false;
    return MediaResult::success();
}

MediaResult MediaSessionManager::reconcileActivity(const CallId& callId) {
    auto* session = impl_->find(callId);
    if (!session) {
        return MediaResult::failure("media session does not exist");
    }
    return session->active ? start(callId) : suspend(callId);
}

void MediaSessionManager::stop(const CallId& callId) noexcept {
    const auto iterator = impl_->sessions.find(callId.value);
    if (iterator == impl_->sessions.end()) {
        return;
    }
    auto& session = *iterator->second;
    impl_->stopCaptures(session);
    session.control.stopAll();
    if (session.videoStream) {
        mv::stream_internal::setVideoOutputSink(
            session.videoStream.get(),
            std::weak_ptr<mv::IVideoFrameSink>{});
    }
    impl_->sessions.erase(iterator);
}

void MediaSessionManager::stopAll() noexcept {
    std::vector<std::string> ids;
    ids.reserve(impl_->sessions.size());
    for (const auto& entry : impl_->sessions) {
        ids.push_back(entry.first);
    }
    for (const auto& id : ids) {
        stop(CallId{id});
    }
}

MediaResult MediaSessionManager::setAudioMuted(
    const CallId& callId,
    bool muted) {
    auto* session = impl_->find(callId);
    if (!session || !session->audioStream) {
        return MediaResult::failure("audio stream is not available");
    }
    const auto status = session->audioStream->setMuted(muted);
    return status.ok ? MediaResult::success()
                     : MediaResult::failure(status.message);
}

MediaResult MediaSessionManager::setVideoMuted(
    const CallId& callId,
    bool muted) {
    auto* session = impl_->find(callId);
    if (!session || !session->videoStream) {
        return MediaResult::failure("video stream is not available");
    }
    const auto status = session->videoStream->setMuted(muted);
    return status.ok ? MediaResult::success()
                     : MediaResult::failure(status.message);
}

void MediaSessionManager::setInputSource(MediaKind kind,
                                 const std::string& source) {
    if (kind == MediaKind::Audio) {
        runtime_.audioInput = source;
    } else {
        runtime_.videoInput = source;
    }
}

std::string MediaSessionManager::inputSource(MediaKind kind) const {
    return kind == MediaKind::Audio
        ? runtime_.audioInput
        : runtime_.videoInput;
}

void MediaSessionManager::setVideoFrameCallback(VideoFrameCallback callback) {
    impl_->callbacks->set(std::move(callback));
}

MediaStats MediaSessionManager::stats(const CallId& callId) const {
    auto* session = impl_->find(callId);
    return session ? impl_->stats(*session) : MediaStats{};
}

SdpResult MediaSessionManager::createReinviteOffer(
    const CallId& callId,
    MediaDirection audioDirection,
    MediaDirection videoDirection) {
    auto* session = impl_->find(callId);
    if (!session) {
        return SdpResult::failure("media session does not exist");
    }
    auto description = session->control.localDescription();
    ++description.origin.sessionVersion;
    for (auto& media : description.media) {
        media.direction = media.kind == mv::MediaKind::Audio
            ? toV2(audioDirection)
            : toV2(videoDirection);
    }
    std::string sdp;
    mv::SdpBuilder builder;
    const auto status = builder.build(description, &sdp);
    if (!status.ok) {
        return SdpResult::failure(resultError(status));
    }
    session->config.audioDirection = audioDirection;
    session->config.videoDirection = videoDirection;
    return SdpResult::success(std::move(sdp));
}

SdpResult MediaSessionManager::createReinviteAnswer(
    const CallId& callId,
    const std::string& remoteOffer) {
    auto* session = impl_->find(callId);
    if (!session) {
        return SdpResult::failure("media session does not exist");
    }
    mv::SdpSessionDescription offer;
    mv::SdpParser parser;
    auto status = parser.parse(remoteOffer, &offer);
    if (!status.ok) {
        return SdpResult::failure(resultError(status));
    }
    mv::SdpNegotiator negotiator;
    auto result = negotiator.createAnswer(
        negotiationInput(session->config, runtime_), offer);
    if (!result.status.ok) {
        return SdpResult::failure(resultError(result.status));
    }
    std::string sdp;
    mv::SdpBuilder builder;
    status = builder.build(result.description, &sdp);
    if (!status.ok) {
        return SdpResult::failure(resultError(status));
    }
    return SdpResult::success(std::move(sdp));
}

MediaResult MediaSessionManager::applyIncomingOffer(
    const CallId& callId,
    const std::string& remoteOffer) {
    auto* session = impl_->find(callId);
    if (!session) {
        return MediaResult::failure("media session does not exist");
    }
    const bool wasActive = session->active;
    impl_->stopCaptures(*session);
    const auto status = session->control.applyRemoteOffer(
        remoteOffer,
        negotiationInput(session->config, runtime_),
        false);
    if (!status.ok) {
        return MediaResult::failure(resultError(status));
    }
    impl_->bindStreams(*session);
    session->remoteConfigured = true;
    if (wasActive) {
        return start(callId);
    }
    return MediaResult::success();
}

} // namespace iris::core
