#include "media/control/sdp_media_control.h"

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

bool parseEndpoint(const std::string& endpoint,
                   std::string* address,
                   std::uint16_t* port) {
    const auto separator = endpoint.rfind(':');
    if (!address || !port || separator == std::string::npos) {
        return false;
    }
    auto parsedAddress = endpoint.substr(0, separator);
    if (parsedAddress == "0.0.0.0" || parsedAddress.empty()) {
        parsedAddress = "127.0.0.1";
    }
    std::uint16_t parsedPort = 0;
    try {
        parsedPort = static_cast<std::uint16_t>(
            std::stoul(endpoint.substr(separator + 1)));
    } catch (...) {
        return false;
    }
    if (parsedPort == 0) {
        return false;
    }
    *address = std::move(parsedAddress);
    *port = parsedPort;
    return true;
}

SdpNegotiationInput toNegotiationInput(
    const SdpSessionDescription& description) {
    SdpNegotiationInput result;
    result.sessionId = description.origin.sessionId;
    result.origin = description.origin;
    result.connection = description.connection;
    for (const auto& media : description.media) {
        SdpMediaCapability capability;
        capability.kind = media.kind;
        capability.mid = media.mid;
        capability.direction = media.direction;
        capability.transport = media.transport;
        capability.headerExtensions = media.headerExtensions;
        capability.attributes = media.attributes;
        for (const auto& codec : media.codecs) {
            SdpCodecPreference preference;
            preference.codecType = codec.codecType;
            preference.encodingName = codec.encodingName;
            preference.payloadType = codec.payloadType;
            preference.attributes = codec.attributes;
            capability.codecs.push_back(std::move(preference));
        }
        result.media.push_back(std::move(capability));
    }
    return result;
}

StreamConfig encodedStreamConfig(
    const EncodedPacketStreamPtr& stream) {
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

StreamId findStream(
    const MediaControl& control,
    MediaKind kind,
    const std::string& mid) {
    for (const auto id : control.streams()) {
        const auto stream = control.stream(id);
        if (!stream) {
            continue;
        }
        const auto config = encodedStreamConfig(stream);
        if (config.kind == kind &&
            (config.name == mid || config.name.empty() || mid.empty())) {
            return id;
        }
    }
    return 0;
}

} // namespace

SDPMediaControl::SDPMediaControl() = default;
SDPMediaControl::~SDPMediaControl() = default;

Status SDPMediaControl::createStreams(
    const SdpNegotiationResult& result,
    const SdpNegotiationInput& local,
    bool replaceExisting) {
    if (!result.status.ok) {
        return result.status;
    }
    if (replaceExisting) {
        for (const auto id : streams()) {
            (void)destroyStream(id);
        }
    }
    for (const auto& media : result.media) {
        if (media.rejected ||
            media.direction == MediaDirection::Inactive) {
            continue;
        }
        const auto capability = std::find_if(
            local.media.begin(), local.media.end(),
            [&](const SdpMediaCapability& candidate) {
                return candidate.kind == media.kind &&
                    (candidate.mid.empty() ||
                     candidate.mid == media.mid);
            });
        if (capability == local.media.end()) {
            continue;
        }

        StreamConfig streamConfig;
        streamConfig.kind = media.kind;
        streamConfig.name = media.mid.empty()
            ? (media.kind == MediaKind::Video ? "video" : "audio")
            : media.mid;
        streamConfig.direction = media.direction;
        streamConfig.attributes = capability->attributes;
        streamConfig.attributes["codecType"] =
            media.codec.codecType == CodecType::H264 ? "h264" : "opus";
        streamConfig.attributes["payloadType"] =
            std::to_string(media.codec.payloadType);
        const auto clock = media.codec.attributes.find("clockRate");
        if (clock != media.codec.attributes.end()) {
            streamConfig.attributes["clockRate"] = clock->second;
        }
        if (media.kind == MediaKind::Audio) {
            streamConfig.attributes["sampleRate"] =
                clock == media.codec.attributes.end()
                    ? "48000" : clock->second;
            const auto channels =
                media.codec.attributes.find("channels");
            if (channels != media.codec.attributes.end()) {
                streamConfig.attributes["channels"] = channels->second;
            }
        } else {
            const auto resolution =
                capability->attributes.find("resolution");
            if (resolution != capability->attributes.end()) {
                streamConfig.attributes["resolution"] =
                    resolution->second;
            }
        }

        EncodedPacketStreamPtr stream;
        const auto status = createStream(streamConfig, &stream);
        if (!status.ok) {
            return status;
        }
    }
    return {};
}

Status SDPMediaControl::createOffer(
    const SdpOfferOptions& options,
    std::string* offerSdp) {
    std::lock_guard<std::mutex> lock(sdpMutex_);
    auto local = options.local;
    auto result = negotiator_.createOffer(local);
    if (!result.status.ok) {
        return result.status;
    }
    auto status = createStreams(
        result, local, options.replaceExistingStreams);
    if (!status.ok) {
        return status;
    }
    status = applyStreamEndpoints(&result.description, nullptr);
    if (!status.ok) {
        return status;
    }
    status = builder_.build(result.description, offerSdp);
    if (!status.ok) {
        return status;
    }
    local_ = std::move(result.description);
    return {};
}

Status SDPMediaControl::createAnswer(
    const SdpAnswerOptions& options,
    std::string* answerSdp) {
    std::lock_guard<std::mutex> lock(sdpMutex_);
    SdpSessionDescription offer;
    auto status = parser_.parse(options.remoteOffer, &offer);
    if (!status.ok) {
        return status;
    }
    auto local = options.local;
    auto result = negotiator_.createAnswer(local, offer);
    if (!result.status.ok) {
        return result.status;
    }
    status = createStreams(result, local, true);
    if (!status.ok) {
        return status;
    }
    status = applyStreamEndpoints(&result.description, &offer);
    if (!status.ok) {
        return status;
    }
    status = builder_.build(result.description, answerSdp);
    if (!status.ok) {
        return status;
    }
    if (options.startImmediately) {
        status = startAll();
        if (!status.ok) {
            return status;
        }
    }
    local_ = std::move(result.description);
    remote_ = std::move(offer);
    return {};
}

Status SDPMediaControl::applyRemoteOffer(
    const std::string& remoteOffer,
    const SdpNegotiationInput& localValue,
    bool startImmediately) {
    std::lock_guard<std::mutex> lock(sdpMutex_);
    SdpSessionDescription offer;
    auto status = parser_.parse(remoteOffer, &offer);
    if (!status.ok) {
        return status;
    }
    auto local = localValue;
    auto result = negotiator_.createAnswer(local, offer);
    if (!result.status.ok) {
        return result.status;
    }
    status = createStreams(result, local, true);
    if (!status.ok) {
        return status;
    }
    status = applyStreamEndpoints(&result.description, &offer);
    if (!status.ok) {
        return status;
    }
    if (startImmediately) {
        status = startAll();
        if (!status.ok) {
            return status;
        }
    }
    local_ = std::move(result.description);
    remote_ = std::move(offer);
    return {};
}

Status SDPMediaControl::applyRemoteAnswer(
    const std::string& remoteAnswer) {
    std::lock_guard<std::mutex> lock(sdpMutex_);
    SdpSessionDescription answer;
    auto status = parser_.parse(remoteAnswer, &answer);
    if (!status.ok) {
        return status;
    }
    const auto local = toNegotiationInput(local_);
    auto result = negotiator_.applyRemoteAnswer(local, answer);
    if (!result.status.ok) {
        return result.status;
    }
    status = createStreams(result, local, true);
    if (!status.ok) {
        return status;
    }
    status = applyStreamEndpoints(&result.description, &answer);
    if (!status.ok) {
        return status;
    }
    local_ = std::move(result.description);
    remote_ = std::move(answer);
    return {};
}

Status SDPMediaControl::renegotiate(
    const SdpNegotiationInput& local) {
    std::lock_guard<std::mutex> lock(sdpMutex_);
    auto result = negotiator_.createAnswer(local, remote_);
    if (!result.status.ok) {
        return result.status;
    }
    auto status = createStreams(result, local, true);
    if (!status.ok) {
        return status;
    }
    return applyStreamEndpoints(&result.description, &remote_);
}

Status SDPMediaControl::applyStreamEndpoints(
    SdpSessionDescription* local,
    const SdpSessionDescription* remote) {
    if (!local) {
        return failure("local SDP description is null");
    }
    for (auto& media : local->media) {
        if (media.rejected) {
            continue;
        }
        const auto id = findStream(*this, media.kind, media.mid);
        if (id == 0) {
            return failure("SDP media has no matching stream");
        }
        std::string address;
        std::uint16_t port = 0;
        if (!parseEndpoint(localEndpoint(id), &address, &port)) {
            return failure("stream has no usable local endpoint");
        }
        media.transport.dataPort = port;
        if (local->connection.address.empty()) {
            local->connection.address = address;
        }
        if (!remote) {
            continue;
        }
        const auto remoteMedia = std::find_if(
            remote->media.begin(), remote->media.end(),
            [&](const SdpMediaDescription& candidate) {
                return candidate.kind == media.kind &&
                    (candidate.mid.empty() || media.mid.empty() ||
                     candidate.mid == media.mid);
            });
        if (remoteMedia == remote->media.end() ||
            remoteMedia->rejected ||
            remoteMedia->transport.dataPort == 0) {
            return failure("remote SDP media has no usable endpoint");
        }
        if (remote->connection.address.empty()) {
            return failure("remote SDP connection address is empty");
        }
        const auto status = setRemoteEndpoint(
            id, remote->connection.address + ":" +
                std::to_string(remoteMedia->transport.dataPort));
        if (!status.ok) {
            return status;
        }
    }
    return {};
}

SdpSessionDescription SDPMediaControl::localDescription() const {
    std::lock_guard<std::mutex> lock(sdpMutex_);
    return local_;
}

SdpSessionDescription SDPMediaControl::remoteDescription() const {
    std::lock_guard<std::mutex> lock(sdpMutex_);
    return remote_;
}

} // namespace iris::media::v2
