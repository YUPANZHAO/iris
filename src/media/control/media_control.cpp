#include "media/control/media_control_impl.h"

#include "media/stream/audio_stream.h"
#include "media/stream/video_stream.h"

#include <algorithm>
#include <string>
#include <utility>

namespace iris::media::v2 {
namespace {

Status failure(std::string message) {
    return Status{false, 1, std::move(message)};
}

std::string value(const AttributeMap& map,
                  const std::string& key,
                  const std::string& fallback = {}) {
    const auto iterator = map.find(key);
    return iterator == map.end() ? fallback : iterator->second;
}

int integer(const AttributeMap& map,
            const std::string& key,
            int fallback) {
    const auto iterator = map.find(key);
    if (iterator == map.end()) {
        return fallback;
    }
    try {
        return std::stoi(iterator->second);
    } catch (...) {
        return fallback;
    }
}

CodecType codecType(const std::string& name, MediaKind kind) {
    if (name == "opus" || name == "Opus") {
        return CodecType::Opus;
    }
    if (name == "h264" || name == "H264") {
        return CodecType::H264;
    }
    return kind == MediaKind::Video ? CodecType::H264 : CodecType::Opus;
}

StreamConfig streamConfig(const EncodedPacketStreamPtr& stream) {
    if (auto audio =
            std::dynamic_pointer_cast<AudioEncodedStream>(stream)) {
        return audio->config();
    }
    if (auto video =
            std::dynamic_pointer_cast<VideoEncodedStream>(stream)) {
        return video->config();
    }
    return {};
}

} // namespace

void StreamMuxSink::setStream(
    StreamId id,
    std::weak_ptr<IEncodedPacketStream> stream) {
    std::lock_guard<std::mutex> lock(mutex_);
    streams_[id] = std::move(stream);
}

void StreamMuxSink::removeStream(StreamId id) {
    std::lock_guard<std::mutex> lock(mutex_);
    streams_.erase(id);
}

void StreamMuxSink::onEncodedPacket(const EncodedPacket& packet) {
    std::shared_ptr<IEncodedPacketStream> stream;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto iterator = streams_.find(packet.streamId);
        if (iterator != streams_.end()) {
            stream = iterator->second.lock();
        }
    }
    if (stream) {
        stream->onEncodedPacket(packet);
    }
}

MediaControl::MediaControl() = default;

MediaControl::~MediaControl() {
    stopAll();
    if (transport_) {
        transport_->close();
    }
    streams_.clear();
    mux_.reset();
    transport_.reset();
}

Status MediaControl::initialize(const MediaControlConfig& config) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (initialized_) {
        return failure("media control is already initialized");
    }
    config_ = config;
    transport_ = std::make_shared<RTPTransport>();
    mux_ = std::make_shared<StreamMuxSink>();
    auto result = transport_->open(transportConfig());
    if (!result.ok) {
        transport_.reset();
        mux_.reset();
        return result;
    }
    initialized_ = true;
    return {};
}

TransportConfig MediaControl::transportConfig() const {
    TransportConfig config;
    config.options = config_.options;
    config.endpoint = value(config_.options, "rtp.remoteEndpoint");
    const auto bindAddress =
        value(config_.options, "rtp.bindAddress");
    if (!bindAddress.empty()) {
        config.options["bindAddress"] = bindAddress;
    }
    const auto port = value(config_.options, "rtp.port");
    if (!port.empty()) {
        config.options["port"] = port;
    }
    if (!config.endpoint.empty()) {
        config.options["remoteEndpoint"] = config.endpoint;
    }
    return config;
}

Status MediaControl::configureStream(
    const StreamConfig& configValue,
    EncodedPacketStreamPtr streamValue) {
    if (configValue.kind == MediaKind::Audio) {
        auto stream =
            std::dynamic_pointer_cast<AudioFrameStream>(streamValue);
        if (!stream) {
            return failure("failed to create audio frame stream");
        }
        AudioFrameStreamConfig streamConfig;
        streamConfig.stream = configValue;
        streamConfig.stream.kind = MediaKind::Audio;
        streamConfig.inputFormat.sampleRate = integer(
            configValue.attributes, "sampleRate", 48000);
        streamConfig.inputFormat.channels = integer(
            configValue.attributes, "channels", 2);
        streamConfig.inputFormat.sampleFormat = AudioSampleFormat::Signed16;
        streamConfig.outputFormat = streamConfig.inputFormat;
        streamConfig.encoderType = codecType(
            value(configValue.attributes, "codecType"), MediaKind::Audio);
        streamConfig.decoderType = streamConfig.encoderType;
        streamConfig.bitrateKbps = integer(
            configValue.attributes, "bitrateKbps", 64);
        return stream->configureAudio(streamConfig);
    }
    if (configValue.kind == MediaKind::Video) {
        auto stream =
            std::dynamic_pointer_cast<VideoFrameStream>(streamValue);
        if (!stream) {
            return failure("failed to create video frame stream");
        }
        VideoFrameStreamConfig streamConfig;
        streamConfig.stream = configValue;
        streamConfig.stream.kind = MediaKind::Video;
        streamConfig.inputFormat.width = integer(
            configValue.attributes, "width", 640);
        streamConfig.inputFormat.height = integer(
            configValue.attributes, "height", 360);
        const auto resolution =
            value(configValue.attributes, "resolution");
        const auto separator = resolution.find('x');
        if (separator != std::string::npos) {
            try {
                streamConfig.inputFormat.width =
                    std::stoi(resolution.substr(0, separator));
                streamConfig.inputFormat.height =
                    std::stoi(resolution.substr(separator + 1));
            } catch (...) {
            }
        }
        streamConfig.inputFormat.frameRate = Rational{
            integer(configValue.attributes, "frameRateNumerator", 15),
            std::max(1, integer(
                configValue.attributes, "frameRateDenominator", 1)),
        };
        streamConfig.inputFormat.pixelFormat = VideoPixelFormat::I420;
        streamConfig.outputFormat = VideoPixelFormat::I420;
        streamConfig.encoderType = codecType(
            value(configValue.attributes, "codecType"), MediaKind::Video);
        streamConfig.decoderType = streamConfig.encoderType;
        streamConfig.bitrateKbps = integer(
            configValue.attributes, "bitrateKbps", 800);
        return stream->configureVideo(streamConfig);
    }
    return failure("unsupported stream media kind");
}

Status MediaControl::createStream(
    const StreamConfig& configValue,
    EncodedPacketStreamPtr* output) {
    if (!output) {
        return failure("stream output is null");
    }
    EncodedPacketStreamPtr result;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_ || !transport_) {
        return failure("media control is not initialized");
    }
    for (const auto& stream : streams_) {
        if (stream.second->info().state == StreamState::Active) {
            return failure(
                "cannot add a stream while media is active");
        }
    }
    StreamConfig effective = configValue;
    if (effective.id == 0) {
        StreamId next = 1;
        for (const auto& entry : streams_) {
            next = std::max(next, entry.first + 1);
        }
        effective.id = next;
    }
    if (effective.kind == MediaKind::Audio) {
        result = std::make_shared<AudioFrameStream>();
    } else if (effective.kind == MediaKind::Video) {
        result = std::make_shared<VideoFrameStream>();
    } else {
        return failure("unsupported stream media kind");
    }
    auto status = configureStream(effective, result);
    if (!status.ok) {
        return status;
    }
    streams_[effective.id] = result;
    const auto codec = codecType(
        value(effective.attributes, "codecType"), effective.kind);
    const auto payloadType = static_cast<std::uint8_t>(std::clamp(
        integer(effective.attributes, "payloadType",
                codec == CodecType::H264 ? 96 : 111),
        0, 127));
    const auto clockRate = integer(
        effective.attributes, "clockRate",
        codec == CodecType::H264 ? 90000 : 48000);
    status = transport_->configureStream(
        effective.id, effective.kind, codec, payloadType, clockRate);
    if (!status.ok) {
        streams_.erase(effective.id);
        return status;
    }
    if (!result->attach(transport_).ok) {
        streams_.erase(effective.id);
        return failure("failed to attach stream transport");
    }
    updateMux();
    *output = result;
    return {};
}

void MediaControl::updateMux() {
    if (!transport_ || !mux_) {
        return;
    }
    for (const auto& entry : streams_) {
        mux_->setStream(entry.first, entry.second);
    }
    transport_->setSink(mux_);
}

bool MediaControl::destroyStream(StreamId id) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto iterator = streams_.find(id);
    if (iterator == streams_.end()) {
        return false;
    }
    iterator->second->shutdown();
    if (mux_) {
        mux_->removeStream(id);
    }
    streams_.erase(iterator);
    return true;
}

EncodedPacketStreamPtr MediaControl::stream(StreamId id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto iterator = streams_.find(id);
    return iterator == streams_.end() ? nullptr : iterator->second;
}

std::vector<StreamId> MediaControl::streams() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<StreamId> result;
    result.reserve(streams_.size());
    for (const auto& entry : streams_) {
        result.push_back(entry.first);
    }
    return result;
}

Status MediaControl::startAll() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (streams_.empty()) {
        return failure("media control has no streams");
    }
    Status result;
    for (const auto& entry : streams_) {
        result = entry.second->start();
        if (!result.ok) {
            break;
        }
    }
    return result;
}

void MediaControl::stopAll() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& entry : streams_) {
        entry.second->shutdown();
    }
}

MediaStats MediaControl::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    MediaStats result;
    for (const auto& entry : streams_) {
        const auto streamStats = entry.second->stats();
        result.samplesProduced += streamStats.media.samplesProduced;
        result.samplesConsumed += streamStats.media.samplesConsumed;
        result.samplesDropped += streamStats.media.samplesDropped;
        result.codecErrors += streamStats.media.codecErrors;
    }
    return result;
}

Status MediaControl::setRemoteEndpoint(const std::string& endpoint) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_ || !transport_) {
        return failure("media control is not initialized");
    }
    config_.options["rtp.remoteEndpoint"] = endpoint;
    auto status = transport_->setRemoteEndpoint(endpoint);
    if (!status.ok) {
        return status;
    }
    for (const auto& entry : streams_) {
        const auto streamConfig =
            ::iris::media::v2::streamConfig(entry.second);
        const auto codec = codecType(
            value(streamConfig.attributes, "codecType"),
            streamConfig.kind);
        (void)transport_->configureStream(
            entry.first,
            streamConfig.kind,
            codec,
            static_cast<std::uint8_t>(std::clamp(
                integer(streamConfig.attributes, "payloadType",
                        codec == CodecType::H264 ? 96 : 111),
                0, 127)),
            integer(streamConfig.attributes, "clockRate",
                    codec == CodecType::H264 ? 90000 : 48000));
    }
    return {};
}

std::string MediaControl::localEndpoint() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return transport_ ? transport_->info().endpoint : std::string();
}

TransportStats MediaControl::transportStats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return transport_ ? transport_->stats() : TransportStats{};
}

} // namespace iris::media::v2
