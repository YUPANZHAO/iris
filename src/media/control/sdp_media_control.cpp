#include "media/control/sdp_media_control.h"

#include <algorithm>
#include <string>
#include <utility>

namespace iris::media::v2 {
namespace {

Status failure(std::string message) {
    return Status{false, 1, std::move(message)};
}

void applyLocalEndpoint(const std::string& endpoint,
                        SdpNegotiationInput* input) {
    const auto separator = endpoint.rfind(':');
    if (!input || separator == std::string::npos) {
        return;
    }
    auto address = endpoint.substr(0, separator);
    if (address == "0.0.0.0" || address.empty()) {
        address = "127.0.0.1";
    }
    std::uint16_t port = 0;
    try {
        port = static_cast<std::uint16_t>(
            std::stoul(endpoint.substr(separator + 1)));
    } catch (...) {
        return;
    }
    if (input->connection.address.empty()) {
        input->connection.address = address;
    }
    for (auto& media : input->media) {
        if (media.transport.dataPort == 0) {
            media.transport.dataPort = port;
        }
    }
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
    applyLocalEndpoint(localEndpoint(), &local);
    auto result = negotiator_.createOffer(local);
    if (!result.status.ok) {
        return result.status;
    }
    auto status = builder_.build(result.description, offerSdp);
    if (!status.ok) {
        return status;
    }
    status = createStreams(
        result, local, options.replaceExistingStreams);
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
    applyLocalEndpoint(localEndpoint(), &local);
    auto result = negotiator_.createAnswer(local, offer);
    if (!result.status.ok) {
        return result.status;
    }
    status = builder_.build(result.description, answerSdp);
    if (!status.ok) {
        return status;
    }
    status = updateRemoteEndpoint(offer);
    if (!status.ok) {
        return status;
    }
    status = createStreams(result, local, true);
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
    applyLocalEndpoint(localEndpoint(), &local);
    auto result = negotiator_.createAnswer(local, offer);
    if (!result.status.ok) {
        return result.status;
    }
    status = updateRemoteEndpoint(offer);
    if (!status.ok) {
        return status;
    }
    status = createStreams(result, local, true);
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
    status = updateRemoteEndpoint(answer);
    if (!status.ok) {
        return status;
    }
    status = createStreams(result, local, true);
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
    return createStreams(result, local, true);
}

Status SDPMediaControl::updateRemoteEndpoint(
    const SdpSessionDescription& remote) {
    for (const auto& media : remote.media) {
        if (media.transport.dataPort != 0 &&
            !remote.connection.address.empty()) {
            return setRemoteEndpoint(
                remote.connection.address + ":" +
                std::to_string(media.transport.dataPort));
        }
    }
    return failure("remote SDP has no usable media endpoint");
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
