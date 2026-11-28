#include "core/media_session.h"

#include "capture_source/capture.h"
#include "codec/codec.h"
#include "control/control.h"
#include "pipeline/capture_stream.h"
#include "transport/udp_port.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace iris::core {
namespace {

namespace media = iris::media;
namespace codec = iris::media::codec;
namespace capture = iris::media::capture;
namespace control = iris::media::control;
namespace sdp = iris::media::sdp;

std::uint64_t hashCallId(const CallId& id) {
    std::uint64_t value = 1469598103934665603ULL;
    for (const unsigned char ch : id.value) {
        value ^= ch;
        value *= 1099511628211ULL;
    }
    return value == 0 ? 1 : value;
}

codec::CodecType codecType(const std::string& encoding) {
    std::string lower;
    lower.reserve(encoding.size());
    for (const auto ch : encoding) {
        lower.push_back(static_cast<char>(
            std::tolower(static_cast<unsigned char>(ch))));
    }
    if (lower == "opus") {
        return codec::CodecType::Opus;
    }
    if (lower == "h264") {
        return codec::CodecType::H264;
    }
    return codec::CodecType::Unknown;
}

codec::CodecType codecType(const control::MediaStream& stream) {
    const auto value = stream.attributes.find("codecType");
    if (value != stream.attributes.end()) {
        return codecType(value->second);
    }
    const auto encoding = stream.attributes.find("encodingName");
    return encoding == stream.attributes.end()
        ? codec::CodecType::Unknown
        : codecType(encoding->second);
}

int integerValue(const std::string& text, int fallback) {
    try {
        return std::stoi(text);
    } catch (...) {
        return fallback;
    }
}

int attributeInteger(
    const media::AttributeMap& attributes,
    const std::string& name,
    int fallback) {
    const auto value = attributes.find(name);
    return value == attributes.end()
        ? fallback
        : integerValue(value->second, fallback);
}

std::string localMediaAddress(const MediaRuntimeConfig& runtime) {
    return runtime.mediaAddress.empty() ||
                   runtime.mediaAddress == "auto"
        ? "127.0.0.1"
        : runtime.mediaAddress;
}

struct SessionCounters {
    std::atomic<std::uint64_t> packetsSent{0};
    std::atomic<std::uint64_t> packetsReceived{0};
    std::atomic<std::uint64_t> bytesSent{0};
    std::atomic<std::uint64_t> bytesReceived{0};
    std::atomic<std::uint64_t> audioFramesDecoded{0};
    std::atomic<std::uint64_t> videoFramesDecoded{0};
};

std::vector<std::uint8_t> toBgra(const codec::VideoRawFrame& frame) {
    const auto width = frame.format.width;
    const auto height = frame.format.height;
    if (width <= 0 || height <= 0 || !frame.pixels ||
        !frame.pixels->data()) {
        return {};
    }
    const auto* source =
        static_cast<const std::uint8_t*>(frame.pixels->data());
    if (frame.format.pixelFormat == codec::VideoPixelFormat::BGRA) {
        return std::vector<std::uint8_t>(
            source, source + frame.pixels->size());
    }

    std::vector<std::uint8_t> output(
        static_cast<std::size_t>(width) * height * 4);
    const auto lumaSize =
        static_cast<std::size_t>(width) * height;
    const auto chromaWidth = (width + 1) / 2;
    const auto chromaHeight = (height + 1) / 2;
    const auto* u = source + lumaSize;
    const auto* v = u +
        static_cast<std::size_t>(chromaWidth) * chromaHeight;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const auto luma =
                static_cast<int>(source[
                    static_cast<std::size_t>(y) * width + x]);
            const auto chromaIndex =
                static_cast<std::size_t>(y / 2) * chromaWidth +
                x / 2;
            int blue = 0;
            int red = 0;
            if (frame.format.pixelFormat ==
                codec::VideoPixelFormat::NV12) {
                blue = static_cast<int>(u[chromaIndex * 2]) - 128;
                red = static_cast<int>(
                          u[chromaIndex * 2 + 1]) -
                    128;
            } else {
                blue = static_cast<int>(u[chromaIndex]) - 128;
                red = static_cast<int>(v[chromaIndex]) - 128;
            }
            const auto c = std::max(0, luma - 16);
            const auto green =
                (298 * c - 100 * blue - 208 * red + 128) >> 8;
            const auto b = (298 * c + 516 * blue + 128) >> 8;
            const auto r = (298 * c + 409 * red + 128) >> 8;
            const auto offset =
                (static_cast<std::size_t>(y) * width + x) * 4;
            output[offset] =
                static_cast<std::uint8_t>(std::clamp(b, 0, 255));
            output[offset + 1] =
                static_cast<std::uint8_t>(std::clamp(green, 0, 255));
            output[offset + 2] =
                static_cast<std::uint8_t>(std::clamp(r, 0, 255));
            output[offset + 3] = 255;
        }
    }
    return output;
}

class VideoFrameSink final : public codec::IVideoFrameSink {
public:
    VideoFrameSink(
        MediaSession::VideoCallbackProvider callbacks,
        CallId callId,
        VideoFrameKind kind,
        std::shared_ptr<SessionCounters> counters,
        bool countDecoded)
        : callbacks_(std::move(callbacks)),
          callId_(std::move(callId)),
          kind_(kind),
          counters_(std::move(counters)),
          countDecoded_(countDecoded) {
    }

    void onVideoFrame(const codec::VideoRawFrame& frame) override {
        if (countDecoded_) {
            counters_->videoFramesDecoded.fetch_add(
                1, std::memory_order_relaxed);
        }
        const auto callback =
            callbacks_ ? callbacks_() : VideoFrameCallback{};
        if (!callback) {
            return;
        }
        auto pixels = toBgra(frame);
        if (pixels.empty()) {
            return;
        }
        const auto now = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
        try {
            callback(VideoFrameView{
                callId_,
                kind_,
                pixels.data(),
                pixels.size(),
                frame.format.width,
                frame.format.height,
                frame.format.width * 4,
                now,
            });
        } catch (...) {
        }
    }

private:
    MediaSession::VideoCallbackProvider callbacks_;
    CallId callId_;
    VideoFrameKind kind_ = VideoFrameKind::Remote;
    std::shared_ptr<SessionCounters> counters_;
    bool countDecoded_ = false;
};

class AudioFrameSink final : public codec::IAudioFrameSink {
public:
    AudioFrameSink(
        std::shared_ptr<SessionCounters> counters,
        MediaSession::AudioCallbackProvider callbacks,
        CallId callId)
        : counters_(std::move(counters)),
          callbacks_(std::move(callbacks)),
          callId_(std::move(callId)) {
    }

    void onAudioFrame(const codec::AudioRawFrame& frame) override {
        counters_->audioFramesDecoded.fetch_add(
            1, std::memory_order_relaxed);
        const auto callback =
            callbacks_ ? callbacks_() : AudioFrameCallback{};
        if (!callback || !frame.samples || !frame.samples->data()) {
            return;
        }
        try {
            callback(AudioFrameView{
                callId_,
                frame.format,
                static_cast<const std::uint8_t*>(
                    frame.samples->data()),
                frame.samples->size(),
                frame.ptsMicros,
            });
        } catch (...) {
        }
    }

private:
    std::shared_ptr<SessionCounters> counters_;
    MediaSession::AudioCallbackProvider callbacks_;
    CallId callId_;
};

class CountingEncodedSink final : public codec::IEncodedPacketSink {
public:
    CountingEncodedSink(
        std::shared_ptr<codec::IEncodedPacketSink> target,
        std::shared_ptr<SessionCounters> counters)
        : target_(std::move(target)),
          counters_(std::move(counters)) {
    }

    void onEncodedPacket(const codec::EncodedPacket& packet) override {
        counters_->packetsSent.fetch_add(1, std::memory_order_relaxed);
        if (packet.data) {
            counters_->bytesSent.fetch_add(
                packet.data->size(), std::memory_order_relaxed);
        }
        if (target_) {
            target_->onEncodedPacket(packet);
        }
    }

private:
    std::shared_ptr<codec::IEncodedPacketSink> target_;
    std::shared_ptr<SessionCounters> counters_;
};

class RemoteAudioReceiver final : public codec::IEncodedPacketSink {
public:
    explicit RemoteAudioReceiver(
        std::shared_ptr<SessionCounters> counters,
        MediaSession::AudioCallbackProvider callbacks,
        CallId callId)
        : counters_(std::move(counters)),
          callbacks_(std::move(callbacks)),
          callId_(std::move(callId)) {
    }

    bool configure(const control::MediaStream& stream) {
        codecType_ = codecType(stream);
        codec::FFmpegCodecFactory factory;
        decoder_ = factory.createAudioDecoder(codecType_);
        if (!decoder_) {
            return false;
        }
        codec::AudioDecoderConfig config;
        config.codecType = codecType_;
        config.outputFormat.sampleRate = attributeInteger(
            stream.attributes, "sampleRate", 48000);
        config.outputFormat.channels = attributeInteger(
            stream.attributes, "channels", 2);
        config.outputFormat.sampleFormat =
            codec::AudioSampleFormat::Signed16;
        frameSink_ = std::make_shared<AudioFrameSink>(
            counters_, callbacks_, callId_);
        decoder_->setRawFrameSink(frameSink_);
        return decoder_->configure(config).ok;
    }

    void onEncodedPacket(const codec::EncodedPacket& packet) override {
        counters_->packetsReceived.fetch_add(
            1, std::memory_order_relaxed);
        if (packet.data) {
            counters_->bytesReceived.fetch_add(
                packet.data->size(), std::memory_order_relaxed);
        }
        if (decoder_) {
            auto decoded = packet;
            decoded.kind = media::MediaKind::Audio;
            decoded.codecType = codecType_;
            (void)decoder_->decode(decoded);
        }
    }

private:
    std::shared_ptr<SessionCounters> counters_;
    MediaSession::AudioCallbackProvider callbacks_;
    CallId callId_;
    codec::CodecType codecType_ = codec::CodecType::Unknown;
    std::unique_ptr<codec::AudioDecoder> decoder_;
    std::shared_ptr<AudioFrameSink> frameSink_;
};

class RemoteVideoReceiver final : public codec::IEncodedPacketSink {
public:
    RemoteVideoReceiver(
        std::shared_ptr<SessionCounters> counters,
        MediaSession::VideoCallbackProvider callbacks,
        CallId callId)
        : counters_(std::move(counters)),
          callbacks_(std::move(callbacks)),
          callId_(std::move(callId)) {
    }

    bool configure(const control::MediaStream& stream) {
        codecType_ = codecType(stream);
        codec::FFmpegCodecFactory factory;
        decoder_ = factory.createVideoDecoder(codecType_);
        if (!decoder_) {
            return false;
        }
        codec::VideoDecoderConfig config;
        config.codecType = codecType_;
        config.outputFormat = codec::VideoPixelFormat::BGRA;
        frameSink_ = std::make_shared<VideoFrameSink>(
            callbacks_,
            callId_,
            VideoFrameKind::Remote,
            counters_,
            true);
        decoder_->setRawFrameSink(frameSink_);
        return decoder_->configure(config).ok;
    }

    void onEncodedPacket(const codec::EncodedPacket& packet) override {
        counters_->packetsReceived.fetch_add(
            1, std::memory_order_relaxed);
        if (packet.data) {
            counters_->bytesReceived.fetch_add(
                packet.data->size(), std::memory_order_relaxed);
        }
        if (decoder_) {
            auto decoded = packet;
            decoded.kind = media::MediaKind::Video;
            decoded.codecType = codecType_;
            (void)decoder_->decode(decoded);
        }
    }

private:
    std::shared_ptr<SessionCounters> counters_;
    MediaSession::VideoCallbackProvider callbacks_;
    CallId callId_;
    codec::CodecType codecType_ = codec::CodecType::Unknown;
    std::unique_ptr<codec::VideoDecoder> decoder_;
    std::shared_ptr<VideoFrameSink> frameSink_;
};

struct CaptureInput {
    bool empty = true;
    std::string backend;
    std::string name;
};

bool emptySourceName(const std::string& source) {
    return source.empty() || source == "null" || source == "none" ||
           source == "empty";
}

media::Status resolveInput(
    MediaKind kind,
    const std::string& configured,
    CaptureInput* output) {
    if (!output) {
        return media::Status::failure("capture input output is null");
    }
    const auto source = configured.empty() ? "auto" : configured;
    if (emptySourceName(source)) {
        *output = {};
        return {};
    }
    if (source.rfind("lavfi:", 0) == 0) {
        output->empty = false;
        output->backend = "lavfi";
        output->name = source.substr(6);
        return {};
    }
    if (source.rfind("dshow:", 0) == 0) {
        output->empty = false;
        output->backend = "dshow";
        output->name = source.substr(6);
        if (kind == MediaKind::Audio &&
            output->name.rfind("audio=", 0) != 0) {
            output->name = "audio=" + output->name;
        }
        if (kind == MediaKind::Video &&
            output->name.rfind("video=", 0) != 0) {
            output->name = "video=" + output->name;
        }
        return {};
    }
    if (source.rfind("gdigrab:", 0) == 0 &&
        kind == MediaKind::Video) {
        output->empty = false;
        output->backend = "gdigrab";
        output->name = source.substr(8);
        return {};
    }

    const auto devices =
        capture::queryCaptureDevices(kind);
    const capture::CaptureDeviceInfo* selected = nullptr;
    if (source == "auto") {
        if (devices.empty()) {
            *output = {};
            return {};
        }
        selected = &devices.front();
        for (const auto& device : devices) {
            if (device.isDefault) {
                selected = &device;
                break;
            }
        }
    } else {
        try {
            std::size_t consumed = 0;
            const auto index = std::stoul(source, &consumed);
            if (consumed == source.size() &&
                index < devices.size()) {
                selected = &devices[index];
            }
        } catch (...) {
        }
        if (!selected) {
            for (const auto& device : devices) {
                if (device.id == source || device.name == source) {
                    selected = &device;
                    break;
                }
            }
        }
    }
    if (!selected) {
        return media::Status::failure("capture device was not found");
    }
    output->empty = false;
    output->backend = selected->backend;
    const auto prefix = "ffmpeg/";
    if (output->backend.rfind(prefix, 0) == 0) {
        output->backend.erase(0, std::strlen(prefix));
    }
    output->name = selected->id;
    if (output->backend == "dshow") {
        output->name =
            std::string(kind == MediaKind::Audio
                            ? "audio="
                            : "video=") +
            output->name;
    }
    return {};
}

struct SessionConfig {
    CallId callId;
    std::string localUri;
    bool audioEnabled = true;
    bool videoEnabled = true;
    MediaDirection audioDirection = MediaDirection::SendRecv;
    MediaDirection videoDirection = MediaDirection::SendRecv;
    AudioFormat audioFormat;
    VideoFormat videoFormat;
    std::vector<MediaCodecPreference> codecPreferences;
};

struct SessionState {
    SessionConfig config;
    MediaRuntimeConfig runtime;
    std::shared_ptr<SessionCounters> counters =
        std::make_shared<SessionCounters>();
    std::vector<std::shared_ptr<void>> keepAlive;
};

class SessionMediaSink final : public control::MediaControlSink {
public:
    SessionMediaSink(
        std::shared_ptr<SessionState> state,
        MediaSession::AudioCallbackProvider audioCallbacks,
        MediaSession::VideoCallbackProvider videoCallbacks)
        : state_(std::move(state)),
          audioCallbacks_(std::move(audioCallbacks)),
          videoCallbacks_(std::move(videoCallbacks)) {
    }

    media::Status onStreamCreate(
        control::MediaStream& stream) override {
        if (stream.encodedPacketSink) {
            auto counting = std::make_shared<CountingEncodedSink>(
                stream.encodedPacketSink, state_->counters);
            stream.encodedPacketSink = counting;
            state_->keepAlive.push_back(counting);
        }

        if (auto audio =
                std::dynamic_pointer_cast<
                    media::pipeline::CaptureAudioStream>(
                    stream.stream)) {
            auto status = configureAudioCapture(*audio, stream);
            if (!status.ok) {
                return status;
            }
        }
        if (auto video =
                std::dynamic_pointer_cast<
                    media::pipeline::CaptureVideoStream>(
                    stream.stream)) {
            auto status = configureVideoCapture(*video, stream);
            if (!status.ok) {
                return status;
            }
            auto preview = std::make_shared<VideoFrameSink>(
                videoCallbacks_,
                state_->config.callId,
                VideoFrameKind::LocalPreview,
                state_->counters,
                false);
            video->setPreviewSink(preview);
            state_->keepAlive.push_back(preview);
        }

        if (stream.receiveStream) {
            auto status =
                configureReceiver(stream);
            if (!status.ok) {
                return status;
            }
        }
        return {};
    }

private:
    media::Status configureAudioCapture(
        media::pipeline::CaptureAudioStream& stream,
        const control::MediaStream& info) {
        CaptureInput input;
        auto status = resolveInput(
            MediaKind::Audio, state_->runtime.audioInput, &input);
        if (!status.ok || input.empty) {
            return status;
        }
        auto source =
            std::make_shared<capture::FFmpegAudioCaptureSource>();
        capture::AudioCaptureConfig config;
        config.backend = input.backend;
        config.name = input.name;
        config.outputFormat.sampleRate = attributeInteger(
            info.attributes, "sampleRate", 48000);
        config.outputFormat.channels = attributeInteger(
            info.attributes, "channels", 2);
        config.outputFormat.sampleFormat =
            codec::AudioSampleFormat::Signed16;
        status = source->configure(config);
        if (status.ok) {
            status = stream.setCapture(std::move(source));
        }
        return status;
    }

    media::Status configureVideoCapture(
        media::pipeline::CaptureVideoStream& stream,
        const control::MediaStream& info) {
        CaptureInput input;
        auto status = resolveInput(
            MediaKind::Video, state_->runtime.videoInput, &input);
        if (!status.ok || input.empty) {
            return status;
        }
        auto source =
            std::make_shared<capture::FFmpegVideoCaptureSource>();
        capture::VideoCaptureConfig config;
        config.backend = input.backend;
        config.name = input.name;
        config.outputFormat.width = attributeInteger(
            info.attributes, "width",
            state_->runtime.videoWidth);
        config.outputFormat.height = attributeInteger(
            info.attributes, "height",
            state_->runtime.videoHeight);
        const auto resolution =
            info.attributes.find("resolution");
        if (resolution != info.attributes.end()) {
            const auto separator = resolution->second.find('x');
            if (separator != std::string::npos) {
                config.outputFormat.width = std::max(
                    1, integerValue(
                           resolution->second.substr(0, separator),
                           config.outputFormat.width));
                config.outputFormat.height = std::max(
                    1, integerValue(
                           resolution->second.substr(separator + 1),
                           config.outputFormat.height));
            }
        }
        config.outputFormat.frameRate = codec::Rational{
            std::max(
                1,
                attributeInteger(
                    info.attributes, "frameRateNumerator", 15)),
            std::max(
                1,
                attributeInteger(
                    info.attributes, "frameRateDenominator", 1)),
        };
        config.outputFormat.pixelFormat =
            codec::VideoPixelFormat::I420;
        status = source->configure(config);
        if (status.ok) {
            status = stream.setCapture(std::move(source));
        }
        return status;
    }

    media::Status configureReceiver(
        const control::MediaStream& stream) {
        if (stream.kind == media::MediaKind::Audio) {
            auto receiver =
                std::make_shared<RemoteAudioReceiver>(
                    state_->counters,
                    audioCallbacks_,
                    state_->config.callId);
            if (!receiver->configure(stream)) {
                return media::Status::failure(
                    "failed to configure remote audio decoder");
            }
            stream.receiveStream->setEncodedPacketSink(receiver);
            state_->keepAlive.push_back(receiver);
        } else {
            auto receiver =
                std::make_shared<RemoteVideoReceiver>(
                    state_->counters,
                    videoCallbacks_,
                    state_->config.callId);
            if (!receiver->configure(stream)) {
                return media::Status::failure(
                    "failed to configure remote video decoder");
            }
            stream.receiveStream->setEncodedPacketSink(receiver);
            state_->keepAlive.push_back(receiver);
        }
        return {};
    }

    std::shared_ptr<SessionState> state_;
    MediaSession::AudioCallbackProvider audioCallbacks_;
    MediaSession::VideoCallbackProvider videoCallbacks_;
};

sdp::SdpCodecPreference codecPreference(
    const SessionConfig& config,
    MediaKind kind) {
    for (const auto& preference : config.codecPreferences) {
        if (preference.kind != kind) {
            continue;
        }
        sdp::SdpCodecPreference result;
        result.codecType = codecType(preference.encoding);
        result.encodingName = preference.encoding;
        result.payloadType = preference.payloadType;
        result.fmtp = preference.fmtp;
        result.attributes["clockRate"] =
            std::to_string(preference.clockRate);
        result.attributes["channels"] =
            std::to_string(preference.channels);
        return result;
    }
    sdp::SdpCodecPreference result;
    if (kind == MediaKind::Audio) {
        result.codecType = codec::CodecType::Opus;
        result.encodingName = "opus";
        result.payloadType = 111;
        result.attributes["clockRate"] = "48000";
        result.attributes["channels"] = "2";
    } else {
        result.codecType = codec::CodecType::H264;
        result.encodingName = "H264";
        result.payloadType = 96;
        result.attributes["clockRate"] = "90000";
    }
    return result;
}

media::Status negotiationInput(
    const SessionConfig& config,
    const MediaRuntimeConfig& runtime,
    sdp::SdpNegotiationInput* output) {
    if (!output) {
        return media::Status::failure("SDP input output is null");
    }
    sdp::SdpNegotiationInput input;
    input.origin.username = "iris";
    input.origin.sessionId = hashCallId(config.callId);
    input.origin.sessionVersion = 1;
    input.origin.unicastAddress = localMediaAddress(runtime);
    input.connection.address = localMediaAddress(runtime);
    input.sessionName = "iris";

    if (config.audioEnabled) {
        const auto port = media::transport::allocateUdpPort();
        if (port == 0) {
            return media::Status::failure(
                "failed to allocate an audio media port");
        }
        sdp::SdpMediaCapability audio;
        audio.kind = media::MediaKind::Audio;
        audio.mid = "audio";
        audio.direction = config.audioDirection;
        audio.transport.dataPort = port;
        audio.attributes["sampleRate"] =
            std::to_string(config.audioFormat.sampleRate);
        audio.attributes["channels"] =
            std::to_string(config.audioFormat.channels);
        audio.attributes["bitrateKbps"] =
            std::to_string(runtime.audioBitrateKbps);
        audio.attributes["displayText"] = config.localUri;
        audio.codecs.push_back(
            codecPreference(config, MediaKind::Audio));
        input.media.push_back(std::move(audio));
    }
    if (config.videoEnabled) {
        const auto port = media::transport::allocateUdpPort();
        if (port == 0) {
            return media::Status::failure(
                "failed to allocate a video media port");
        }
        sdp::SdpMediaCapability video;
        video.kind = media::MediaKind::Video;
        video.mid = "video";
        video.direction = config.videoDirection;
        video.transport.dataPort = port;
        video.attributes["width"] =
            std::to_string(runtime.videoWidth);
        video.attributes["height"] =
            std::to_string(runtime.videoHeight);
        video.attributes["frameRateNumerator"] =
            std::to_string(std::max(1, runtime.videoFps));
        video.attributes["frameRateDenominator"] = "1";
        video.attributes["bitrateKbps"] =
            std::to_string(runtime.videoBitrateKbps);
        video.attributes["resolution"] =
            std::to_string(runtime.videoWidth) + "x" +
            std::to_string(runtime.videoHeight);
        video.attributes["displayText"] = config.localUri;
        video.codecs.push_back(
            codecPreference(config, MediaKind::Video));
        input.media.push_back(std::move(video));
    }
    *output = std::move(input);
    return {};
}

} // namespace

struct MediaSession::Impl {
    Impl(CallId id,
         std::string uri,
         MediaRuntimeConfig runtimeValue,
         AudioCallbackProvider audioProvider,
         VideoCallbackProvider videoProvider)
        : state(std::make_shared<SessionState>()),
          runtime(std::move(runtimeValue)),
          audioCallbacks(std::move(audioProvider)),
          videoCallbacks(std::move(videoProvider)) {
        state->runtime = runtime;
        state->config.callId = id;
        state->config.localUri = std::move(uri);
        state->config.audioEnabled = runtime.audioEnabled;
        state->config.videoEnabled = runtime.videoEnabled;
        state->config.audioFormat = AudioFormat{
            48000, 2, AudioSampleFormat::Signed16};
        state->config.videoFormat = VideoFormat{
            runtime.videoWidth,
            runtime.videoHeight,
            Rational{std::max(1, runtime.videoFps), 1},
            VideoPixelFormat::I420,
        };
        state->config.codecPreferences = runtime.codecPreferences;
        sink = std::make_shared<SessionMediaSink>(
            state, audioCallbacks, videoCallbacks);
    }

    media::Status makeControl(
        std::unique_ptr<control::SdpMediaControl>* output) {
        sdp::SdpNegotiationInput input;
        auto status = negotiationInput(
            state->config, runtime, &input);
        if (!status.ok) {
            return status;
        }
        auto value =
            std::make_unique<control::SdpMediaControl>(
                std::move(input));
        value->setMediaControlSink(sink);
        *output = std::move(value);
        return {};
    }

    std::shared_ptr<SessionState> state;
    MediaRuntimeConfig runtime;
    AudioCallbackProvider audioCallbacks;
    VideoCallbackProvider videoCallbacks;
    std::shared_ptr<SessionMediaSink> sink;
    std::unique_ptr<control::SdpMediaControl> control;
    std::unique_ptr<control::SdpMediaControl> pendingControl;
    std::string pendingOffer;
    bool active = false;
};

MediaSession::MediaSession(
    CallId callId,
    std::string localUri,
    MediaRuntimeConfig runtime,
    AudioCallbackProvider audioCallbacks,
    VideoCallbackProvider videoCallbacks)
    : impl_(std::make_unique<Impl>(
          std::move(callId),
          std::move(localUri),
          std::move(runtime),
          std::move(audioCallbacks),
          std::move(videoCallbacks))) {
}

MediaSession::~MediaSession() {
    stop();
}

media::Status MediaSession::prepareOutgoing(std::string* sdp) {
    if (!sdp) {
        return media::Status::failure("SDP output is null");
    }
    std::unique_ptr<control::SdpMediaControl> control;
    auto status = impl_->makeControl(&control);
    if (!status.ok) {
        return status;
    }
    auto offer = control->createOffer();
    if (!offer.status.ok) {
        return offer.status;
    }
    status = control->setLocalDescription(offer.sdp);
    if (!status.ok) {
        return status;
    }
    impl_->control = std::move(control);
    *sdp = std::move(offer.sdp);
    return {};
}

media::Status MediaSession::prepareAnswer(
    const std::string& remoteOffer,
    std::string* sdp) {
    if (!sdp) {
        return media::Status::failure("SDP output is null");
    }
    std::unique_ptr<control::SdpMediaControl> control;
    auto status = impl_->makeControl(&control);
    if (!status.ok) {
        return status;
    }
    status = control->setRemoteDescription(remoteOffer);
    if (!status.ok) {
        return status;
    }
    auto answer = control->createAnswer();
    if (!answer.status.ok) {
        return answer.status;
    }
    status = control->setLocalDescription(answer.sdp);
    if (!status.ok) {
        return status;
    }
    impl_->control = std::move(control);
    *sdp = std::move(answer.sdp);
    return {};
}

media::Status MediaSession::applyRemoteAnswer(
    const std::string& remoteAnswer) {
    if (!impl_->control) {
        return media::Status::failure("media session is not prepared");
    }
    const auto status =
        impl_->control->setRemoteDescription(remoteAnswer);
    return status;
}

media::Status MediaSession::start() {
    if (!impl_->control) {
        return media::Status::failure("media session is not prepared");
    }
    const auto status = impl_->control->startAll();
    if (status.ok) {
        impl_->active = true;
    }
    return status;
}

media::Status MediaSession::suspend() {
    if (!impl_->control) {
        return media::Status::failure("media session is not prepared");
    }
    impl_->control->stopAll();
    impl_->active = false;
    return {};
}

void MediaSession::stop() noexcept {
    if (impl_->control) {
        impl_->control->stopAll();
    }
    if (impl_->pendingControl) {
        impl_->pendingControl->stopAll();
    }
    impl_->active = false;
}

media::Status MediaSession::setMuted(
    MediaKind kind,
    bool muted) {
    if (!impl_->control) {
        return media::Status::failure("media session is not prepared");
    }
    for (const auto& stream : impl_->control->streams()) {
        if (kind == MediaKind::Audio) {
            if (auto audio =
                    std::dynamic_pointer_cast<
                        media::pipeline::CaptureAudioStream>(
                        stream.stream)) {
                audio->setMuted(muted);
                return {};
            }
        } else if (auto video =
                       std::dynamic_pointer_cast<
                           media::pipeline::CaptureVideoStream>(
                           stream.stream)) {
            video->setMuted(muted);
            return {};
        }
    }
    return media::Status::failure(
        kind == MediaKind::Audio
            ? "audio stream is not available"
            : "video stream is not available");
}

MediaStats MediaSession::stats() const {
    const auto& counters = *impl_->state->counters;
    MediaStats result;
    result.packetsSent =
        counters.packetsSent.load(std::memory_order_relaxed);
    result.packetsReceived =
        counters.packetsReceived.load(std::memory_order_relaxed);
    result.bytesSent =
        counters.bytesSent.load(std::memory_order_relaxed);
    result.bytesReceived =
        counters.bytesReceived.load(std::memory_order_relaxed);
    result.audioFramesDecoded =
        counters.audioFramesDecoded.load(std::memory_order_relaxed);
    result.videoFramesDecoded =
        counters.videoFramesDecoded.load(std::memory_order_relaxed);
    return result;
}

media::Status MediaSession::createReinviteOffer(
    MediaDirection audioDirection,
    MediaDirection videoDirection,
    std::string* sdp) {
    if (!sdp || !impl_->control) {
        return media::Status::failure("media session is not prepared");
    }
    auto description = impl_->control->localDescription();
    ++description.origin.sessionVersion;
    for (auto& mediaDesc : description.media) {
        mediaDesc.direction =
            mediaDesc.kind == media::MediaKind::Audio
                ? audioDirection
                : videoDirection;
    }
    sdp::SdpBuilder builder;
    auto status = builder.build(description, sdp);
    if (!status.ok) {
        return status;
    }
    status = impl_->control->setLocalDescription(*sdp);
    if (status.ok) {
        impl_->state->config.audioDirection = audioDirection;
        impl_->state->config.videoDirection = videoDirection;
    }
    return status;
}

media::Status MediaSession::createReinviteAnswer(
    const std::string& remoteOffer,
    std::string* sdp) {
    if (!sdp) {
        return media::Status::failure("SDP output is null");
    }
    std::unique_ptr<control::SdpMediaControl> pending;
    auto status = impl_->makeControl(&pending);
    if (!status.ok) {
        return status;
    }
    status = pending->setRemoteDescription(remoteOffer);
    if (!status.ok) {
        return status;
    }
    auto answer = pending->createAnswer();
    if (!answer.status.ok) {
        return answer.status;
    }
    status = pending->setLocalDescription(answer.sdp);
    if (!status.ok) {
        return status;
    }
    impl_->pendingControl = std::move(pending);
    impl_->pendingOffer = remoteOffer;
    *sdp = std::move(answer.sdp);
    return {};
}

media::Status MediaSession::applyIncomingOffer(
    const std::string& remoteOffer) {
    const bool wasActive = impl_->active;
    if (impl_->control) {
        impl_->control->stopAll();
    }

    std::unique_ptr<control::SdpMediaControl> replacement;
    if (impl_->pendingControl &&
        impl_->pendingOffer == remoteOffer) {
        replacement = std::move(impl_->pendingControl);
        impl_->pendingOffer.clear();
    } else {
        auto status = impl_->makeControl(&replacement);
        if (!status.ok) {
            return status;
        }
        status = replacement->setRemoteDescription(remoteOffer);
        if (!status.ok) {
            return status;
        }
        auto answer = replacement->createAnswer();
        if (!answer.status.ok) {
            return answer.status;
        }
        status = replacement->setLocalDescription(answer.sdp);
        if (!status.ok) {
            return status;
        }
    }

    impl_->control = std::move(replacement);
    impl_->active = false;
    return wasActive ? start() : media::Status{};
}

} // namespace iris::core
