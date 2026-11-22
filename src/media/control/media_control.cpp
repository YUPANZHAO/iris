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

std::string reservedStreamKey(const StreamConfig& stream) {
    return stream.name.empty()
        ? "stream." + std::to_string(stream.id)
        : stream.name;
}

std::string endpointPort(const std::string& endpoint) {
    const auto separator = endpoint.rfind(':');
    return separator == std::string::npos
        ? std::string{}
        : endpoint.substr(separator + 1);
}

} // namespace

MediaControl::MediaControl() = default;

MediaControl::~MediaControl() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& entry : streams_) {
        entry.second.stream->shutdown();
        entry.second.transport->close();
    }
    streams_.clear();
}

Status MediaControl::initialize(const MediaControlConfig& config) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (initialized_) {
        return failure("media control is already initialized");
    }
    config_ = config;
    reservedPorts_.clear();
    initialized_ = true;
    return {};
}

AttributeMap MediaControl::transportConfig(
    const StreamConfig& stream,
    const std::string& remoteEndpoint,
    bool useConfiguredPort) const {
    AttributeMap config = config_.options;
    const auto bindAddress =
        value(config_.options, "rtp.bindAddress");
    if (!bindAddress.empty()) {
        config["bindAddress"] = bindAddress;
    }
    const auto configuredPort = value(config_.options, "rtp.port");
    config["port"] =
        useConfiguredPort && !configuredPort.empty()
            ? configuredPort
            : "0";
    if (!remoteEndpoint.empty()) {
        config["remoteEndpoint"] = remoteEndpoint;
    }
    const std::string prefix = "stream.";
    for (auto iterator = config.begin(); iterator != config.end();) {
        if (iterator->first.rfind(prefix, 0) == 0) {
            iterator = config.erase(iterator);
        } else {
            ++iterator;
        }
    }
    const auto codec = codecType(
        value(stream.attributes, "codecType"), stream.kind);
    const auto streamPrefix =
        prefix + std::to_string(stream.id) + ".";
    config[streamPrefix + "payloadType"] = std::to_string(
        std::clamp(
            integer(stream.attributes, "payloadType",
                    codec == CodecType::H264 ? 96 : 111),
            0, 127));
    config[streamPrefix + "clockRate"] = std::to_string(
        std::max(1, integer(
            stream.attributes, "clockRate",
            codec == CodecType::H264 ? 90000 : 48000)));
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
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) {
        return failure("media control is not initialized");
    }
    for (const auto& entry : streams_) {
        if (entry.second.stream->info().state == StreamState::Active) {
            return failure("cannot add a stream while media is active");
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
    EncodedPacketStreamPtr stream;
    if (effective.kind == MediaKind::Audio) {
        stream = std::make_shared<AudioFrameStream>();
    } else if (effective.kind == MediaKind::Video) {
        stream = std::make_shared<VideoFrameStream>();
    } else {
        return failure("unsupported stream media kind");
    }
    auto status = configureStream(effective, stream);
    if (!status.ok) {
        return status;
    }

    auto transport = std::make_shared<RTPTransport>();
    const auto configuredPort = value(config_.options, "rtp.port");
    bool useConfiguredPort = !configuredPort.empty();
    if (useConfiguredPort) {
        for (const auto& entry : streams_) {
            if (endpointPort(entry.second.transport->info().endpoint) ==
                configuredPort) {
                useConfiguredPort = false;
                break;
            }
        }
    }
    auto options =
        transportConfig(effective, {}, useConfiguredPort);
    const auto reservedKey = reservedStreamKey(effective);
    const auto reserved = reservedPorts_.find(reservedKey);
    if (reserved != reservedPorts_.end()) {
        options["port"] = reserved->second;
    }
    status = transport->configure(options);
    if (!status.ok) {
        return status;
    }
    status = stream->attach(transport);
    if (!status.ok) {
        transport->close();
        return status;
    }
    const auto port = endpointPort(transport->info().endpoint);
    if (!port.empty()) {
        reservedPorts_[reservedKey] = port;
    }
    streams_[effective.id] = StreamEntry{
        std::move(stream), std::move(transport), {}};
    *output = streams_[effective.id].stream;
    return {};
}

bool MediaControl::destroyStream(StreamId id) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto iterator = streams_.find(id);
    if (iterator == streams_.end()) {
        return false;
    }
    iterator->second.stream->shutdown();
    iterator->second.transport->close();
    streams_.erase(iterator);
    return true;
}

EncodedPacketStreamPtr MediaControl::stream(StreamId id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto iterator = streams_.find(id);
    return iterator == streams_.end() ? nullptr : iterator->second.stream;
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
    for (auto& entry : streams_) {
        const auto configValue = streamConfig(entry.second.stream);
        auto status = entry.second.transport->open(transportConfig(
            configValue, entry.second.remoteEndpoint, true));
        if (!status.ok) {
            return status;
        }
        status = entry.second.stream->start();
        if (!status.ok) {
            return status;
        }
    }
    return {};
}

void MediaControl::stopAll() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& entry : streams_) {
        entry.second.stream->shutdown();
    }
}

MediaStats MediaControl::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    MediaStats result;
    for (const auto& entry : streams_) {
        const auto streamStats = entry.second.stream->stats();
        result.samplesProduced += streamStats.media.samplesProduced;
        result.samplesConsumed += streamStats.media.samplesConsumed;
        result.samplesDropped += streamStats.media.samplesDropped;
        result.codecErrors += streamStats.media.codecErrors;
    }
    return result;
}

Status MediaControl::setRemoteEndpoint(const std::string& endpoint) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) {
        return failure("media control is not initialized");
    }
    config_.options["rtp.remoteEndpoint"] = endpoint;
    for (auto& entry : streams_) {
        entry.second.remoteEndpoint = endpoint;
    }
    return {};
}

Status MediaControl::setRemoteEndpoint(
    StreamId id,
    const std::string& endpoint) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto iterator = streams_.find(id);
    if (iterator == streams_.end()) {
        return failure("stream does not exist");
    }
    iterator->second.remoteEndpoint = endpoint;
    return {};
}

std::string MediaControl::localEndpoint() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (streams_.empty()) {
        return {};
    }
    return streams_.begin()->second.transport->info().endpoint;
}

std::string MediaControl::localEndpoint(StreamId id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto iterator = streams_.find(id);
    return iterator == streams_.end()
        ? std::string{}
        : iterator->second.transport->info().endpoint;
}

TransportStats MediaControl::transportStats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    TransportStats result;
    for (const auto& entry : streams_) {
        const auto stats = entry.second.transport->stats();
        result.messagesSent += stats.messagesSent;
        result.messagesReceived += stats.messagesReceived;
        result.bytesSent += stats.bytesSent;
        result.bytesReceived += stats.bytesReceived;
        result.rtpPacketsSent += stats.rtpPacketsSent;
        result.rtpPacketsReceived += stats.rtpPacketsReceived;
        result.rtcpPacketsReceived += stats.rtcpPacketsReceived;
        result.sendErrors += stats.sendErrors;
        result.receiveErrors += stats.receiveErrors;
        result.droppedMessages += stats.droppedMessages;
    }
    return result;
}

} // namespace iris::media::v2
