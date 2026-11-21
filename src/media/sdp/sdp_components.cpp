#include "media/sdp/sdp_components.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace iris::media::v2 {
namespace {

Status failure(std::string message) {
    return Status{false, 1, std::move(message)};
}

std::string encodingName(CodecType codec) {
    switch (codec) {
    case CodecType::Opus: return "opus";
    case CodecType::H264: return "H264";
    case CodecType::Unknown: return {};
    }
    return {};
}

CodecType codecType(const std::string& encoding) {
    std::string lower;
    lower.reserve(encoding.size());
    for (const auto value : encoding) {
        lower.push_back(static_cast<char>(
            std::tolower(static_cast<unsigned char>(value))));
    }
    if (lower == "opus") {
        return CodecType::Opus;
    }
    if (lower == "h264") {
        return CodecType::H264;
    }
    return CodecType::Unknown;
}

std::string directionText(MediaDirection direction) {
    switch (direction) {
    case MediaDirection::SendOnly: return "sendonly";
    case MediaDirection::RecvOnly: return "recvonly";
    case MediaDirection::SendRecv: return "sendrecv";
    case MediaDirection::Inactive: return "inactive";
    }
    return "inactive";
}

MediaDirection parseDirection(const std::string& value) {
    if (value == "sendonly") {
        return MediaDirection::SendOnly;
    }
    if (value == "recvonly") {
        return MediaDirection::RecvOnly;
    }
    if (value == "sendrecv") {
        return MediaDirection::SendRecv;
    }
    return MediaDirection::Inactive;
}

MediaDirection reverseDirection(MediaDirection value) {
    switch (value) {
    case MediaDirection::SendOnly: return MediaDirection::RecvOnly;
    case MediaDirection::RecvOnly: return MediaDirection::SendOnly;
    default: return value;
    }
}

std::string mediaText(MediaKind kind) {
    return kind == MediaKind::Video ? "video" : "audio";
}

MediaKind mediaKind(const std::string& value) {
    if (value == "video") {
        return MediaKind::Video;
    }
    if (value == "audio") {
        return MediaKind::Audio;
    }
    return MediaKind::Unknown;
}

SdpNegotiationResult makeFailure(std::string message) {
    SdpNegotiationResult result;
    result.status = failure(std::move(message));
    return result;
}

std::optional<SdpCodecDescription> selectCodec(
    const SdpMediaCapability& local,
    const SdpMediaDescription& remote) {
    for (const auto& preference : local.codecs) {
        for (const auto& codec : remote.codecs) {
            if (preference.codecType != CodecType::Unknown &&
                preference.codecType == codec.codecType) {
                auto selected = codec;
                if (selected.encodingName.empty()) {
                    selected.encodingName =
                        encodingName(selected.codecType);
                }
                return selected;
            }
            if (!preference.encodingName.empty() &&
                codecType(preference.encodingName) == codec.codecType) {
                auto selected = codec;
                return selected;
            }
        }
    }
    return std::nullopt;
}

SdpCodecDescription localCodec(
    const SdpCodecPreference& preference) {
    SdpCodecDescription result;
    result.codecType = preference.codecType;
    result.encodingName = preference.encodingName.empty()
        ? encodingName(preference.codecType)
        : preference.encodingName;
    result.payloadType = preference.payloadType;
    result.attributes = preference.attributes;
    return result;
}

SdpNegotiationResult negotiate(
    const SdpNegotiationInput& local,
    const SdpSessionDescription* remote,
    bool answer) {
    SdpNegotiationResult result;
    result.description.origin = local.origin;
    result.description.sessionName = "iris";
    result.description.connection = local.connection;
    result.description.attributes = {};

    if (!remote) {
        for (const auto& media : local.media) {
            if (media.codecs.empty()) {
                continue;
            }
            SdpMediaDescription section;
            section.kind = media.kind;
            section.mid = media.mid.empty()
                ? mediaText(media.kind)
                : media.mid;
            section.direction = media.direction;
            section.transport = media.transport;
            section.headerExtensions = media.headerExtensions;
            section.attributes = media.attributes;
            for (const auto& preference : media.codecs) {
                section.codecs.push_back(localCodec(preference));
            }
            result.description.media.push_back(section);
            SdpNegotiatedMedia selected;
            selected.kind = media.kind;
            selected.mid = section.mid;
            selected.direction = media.direction;
            selected.transport = media.transport;
            selected.codec = section.codecs.front();
            result.media.push_back(std::move(selected));
        }
        if (result.description.media.empty()) {
            return makeFailure("local SDP has no media capability");
        }
        return result;
    }

    for (const auto& remoteMedia : remote->media) {
        const auto localIterator = std::find_if(
            local.media.begin(), local.media.end(),
            [&](const SdpMediaCapability& candidate) {
                return candidate.kind == remoteMedia.kind &&
                    (remoteMedia.mid.empty() ||
                     candidate.mid.empty() ||
                     candidate.mid == remoteMedia.mid);
            });
        if (localIterator == local.media.end()) {
            continue;
        }
        auto codec = selectCodec(*localIterator, remoteMedia);
        SdpMediaDescription section;
        section.kind = remoteMedia.kind;
        section.mid = remoteMedia.mid.empty()
            ? localIterator->mid
            : remoteMedia.mid;
        section.transport = localIterator->transport;
        section.headerExtensions = localIterator->headerExtensions;
        section.attributes = localIterator->attributes;
        if (!codec || remoteMedia.rejected ||
            remoteMedia.direction == MediaDirection::Inactive) {
            section.rejected = true;
            section.direction = MediaDirection::Inactive;
            section.transport.dataPort = 0;
            result.description.media.push_back(std::move(section));
            SdpNegotiatedMedia rejected;
            rejected.kind = remoteMedia.kind;
            rejected.mid = section.mid;
            rejected.direction = MediaDirection::Inactive;
            rejected.rejected = true;
            result.media.push_back(std::move(rejected));
            continue;
        }
        section.direction = answer
            ? reverseDirection(remoteMedia.direction)
            : localIterator->direction;
        section.codecs.push_back(*codec);
        result.description.media.push_back(section);
        SdpNegotiatedMedia selected;
        selected.kind = remoteMedia.kind;
        selected.mid = section.mid;
        selected.direction = section.direction;
        selected.transport = section.transport;
        selected.codec = *codec;
        result.media.push_back(std::move(selected));
    }
    if (result.description.media.empty()) {
        return makeFailure("no mutually supported SDP media");
    }
    return result;
}

} // namespace

Status SdpBuilder::build(const SdpSessionDescription& description,
                         std::string* sdp) {
    if (!sdp) {
        return failure("SDP output is null");
    }
    if (description.connection.address.empty()) {
        return failure("SDP connection address is empty");
    }
    std::ostringstream output;
    output << "v=0\r\n";
    output << "o="
           << (description.origin.username.empty()
                   ? "-" : description.origin.username)
           << " "
           << (description.origin.sessionId.empty()
                   ? "0" : description.origin.sessionId)
           << " " << description.origin.sessionVersion
           << " IN IP4 " << description.connection.address << "\r\n";
    output << "s=" << (description.sessionName.empty()
                           ? "iris" : description.sessionName)
           << "\r\n";
    output << "c=IN IP4 " << description.connection.address << "\r\n";
    output << "t=" << description.startTime << " "
           << description.stopTime << "\r\n";
    for (const auto& attribute : description.attributes) {
        output << "a=" << attribute.first;
        if (!attribute.second.empty()) {
            output << ":" << attribute.second;
        }
        output << "\r\n";
    }
    for (const auto& media : description.media) {
        output << "m=" << mediaText(media.kind) << " "
               << (media.rejected ? 0 : media.transport.dataPort)
               << " " << (media.transport.protocol.empty()
                              ? "RTP/AVP" : media.transport.protocol);
        for (const auto& codec : media.codecs) {
            if (!media.rejected) {
                output << " " << codec.payloadType;
            }
        }
        output << "\r\n";
        output << "c=IN IP4 " << description.connection.address << "\r\n";
        output << "a=" << directionText(media.direction) << "\r\n";
        for (const auto& codec : media.codecs) {
            if (media.rejected) {
                continue;
            }
            const auto clock = codec.attributes.find("clockRate");
            const auto channels = codec.attributes.find("channels");
            output << "a=rtpmap:" << codec.payloadType << " "
                   << codec.encodingName << "/"
                   << (clock == codec.attributes.end()
                           ? "0" : clock->second);
            if (channels != codec.attributes.end() &&
                channels->second != "0" &&
                !channels->second.empty()) {
                output << "/" << channels->second;
            }
            output << "\r\n";
            if (!codec.fmtp.empty()) {
                output << "a=fmtp:" << codec.payloadType << " "
                       << codec.fmtp << "\r\n";
            }
        }
        for (const auto& attribute : media.attributes) {
            if (attribute.first == "rtcpMux") {
                continue;
            }
            if (attribute.first == "sampleRate" ||
                attribute.first == "channels" ||
                attribute.first == "bitrateKbps" ||
                attribute.first == "width" ||
                attribute.first == "height" ||
                attribute.first == "frameRateNumerator" ||
                attribute.first == "frameRateDenominator" ||
                attribute.first == "resolution") {
                continue;
            }
            output << "a=" << attribute.first;
            if (!attribute.second.empty()) {
                output << ":" << attribute.second;
            }
            output << "\r\n";
        }
        const auto mux =
            media.transport.attributes.find("rtcpMux");
        if (mux != media.transport.attributes.end() &&
            (mux->second == "1" || mux->second == "true")) {
            output << "a=rtcp-mux\r\n";
        }
        for (const auto& extension : media.headerExtensions) {
            output << "a=extmap:" << extension.id << " "
                   << extension.uri << "\r\n";
        }
    }
    *sdp = output.str();
    return {};
}

Status SdpParser::parse(const std::string& sdp,
                        SdpSessionDescription* description) {
    if (!description || sdp.empty()) {
        return failure("SDP input or output is empty");
    }

    SdpSessionDescription result;
    std::istringstream input(sdp);
    std::string line;
    SdpMediaDescription* current = nullptr;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.size() < 2 || line[1] != '=') {
            continue;
        }
        const char type = line[0];
        const auto value = line.substr(2);
        if (type == 'o') {
            std::istringstream fields(value);
            std::uint64_t version = 0;
            fields >> result.origin.username
                   >> result.origin.sessionId
                   >> version
                   >> result.origin.networkType
                   >> result.origin.addressType
                   >> result.origin.unicastAddress;
            result.origin.sessionVersion = version;
            if (result.connection.address.empty()) {
                result.connection.address =
                    result.origin.unicastAddress;
                result.connection.addressType =
                    result.origin.addressType;
                result.connection.networkType =
                    result.origin.networkType;
            }
        } else if (type == 's') {
            result.sessionName = value;
        } else if (type == 'c') {
            std::istringstream fields(value);
            std::string addressType;
            std::string address;
            fields >> result.connection.networkType
                   >> addressType
                   >> address;
            result.connection.addressType = addressType;
            result.connection.address = address;
            if (current) {
                current->transport.attributes["connectionAddress"] =
                    address;
            }
        } else if (type == 'm') {
            std::istringstream fields(value);
            std::string kind;
            int port = 0;
            std::string protocol;
            fields >> kind >> port >> protocol;
            SdpMediaDescription section;
            section.kind = mediaKind(kind);
            section.transport.protocol = protocol;
            section.transport.dataPort =
                static_cast<std::uint16_t>(std::max(0, port));
            section.rejected = port == 0;
            std::string payload;
            while (fields >> payload) {
                try {
                    SdpCodecDescription codec;
                    codec.payloadType = std::stoi(payload);
                    section.codecs.push_back(std::move(codec));
                } catch (...) {
                    section.attributes["format." + payload] = payload;
                }
            }
            result.media.push_back(std::move(section));
            current = &result.media.back();
        } else if (type == 'a') {
            const auto separator = value.find(':');
            const auto name = value.substr(0, separator);
            const auto attribute =
                separator == std::string::npos
                    ? std::string()
                    : value.substr(separator + 1);
            if (name == "sendrecv" ||
                name == "sendonly" ||
                name == "recvonly" ||
                name == "inactive") {
                if (current) {
                    current->direction = parseDirection(name);
                }
            } else if (name == "mid" && current) {
                current->mid = attribute;
            } else if (name == "rtcp-mux" && current) {
                current->transport.attributes["rtcpMux"] = "1";
                current->transport.controlPort =
                    current->transport.dataPort;
            } else if (name == "rtpmap" && current) {
                std::istringstream fields(attribute);
                int payloadType = -1;
                std::string mapping;
                fields >> payloadType >> mapping;
                const auto first = mapping.find('/');
                const auto second = first == std::string::npos
                    ? std::string::npos
                    : mapping.find('/', first + 1);
                const auto encoding = mapping.substr(0, first);
                for (auto& codec : current->codecs) {
                    if (codec.payloadType != payloadType) {
                        continue;
                    }
                    codec.codecType = codecType(encoding);
                    codec.encodingName = encoding;
                    if (first != std::string::npos) {
                        codec.attributes["clockRate"] = mapping.substr(
                            first + 1,
                            second == std::string::npos
                                ? std::string::npos
                                : second - first - 1);
                    }
                    if (second != std::string::npos) {
                        codec.attributes["channels"] =
                            mapping.substr(second + 1);
                    }
                }
            } else if (name == "fmtp" && current) {
                std::istringstream fields(attribute);
                int payloadType = -1;
                fields >> payloadType;
                std::string fmtp;
                std::getline(fields, fmtp);
                if (!fmtp.empty() && fmtp.front() == ' ') {
                    fmtp.erase(fmtp.begin());
                }
                for (auto& codec : current->codecs) {
                    if (codec.payloadType == payloadType) {
                        codec.fmtp = fmtp;
                    }
                }
            } else if (name == "extmap" && current) {
                std::istringstream fields(attribute);
                SdpHeaderExtension extension;
                fields >> extension.id >> extension.uri;
                current->headerExtensions.push_back(std::move(extension));
            } else if (name == "ice-ufrag" ||
                       name == "ice-pwd" ||
                       name == "fingerprint") {
                if (current) {
                    current->transport.attributes[name] = attribute;
                }
            } else if (current) {
                current->attributes[name] = attribute;
            } else {
                result.attributes[name] = attribute;
            }
        }
    }
    if (result.media.empty()) {
        return failure("SDP has no parseable media");
    }
    *description = std::move(result);
    return this->validate(*description);
}

Status SdpParser::validate(
    const SdpSessionDescription& description) const {
    if (description.connection.address.empty()) {
        return failure("SDP connection address is missing");
    }
    if (description.media.empty()) {
        return failure("SDP has no media descriptions");
    }
    for (const auto& media : description.media) {
        if (media.kind == MediaKind::Unknown) {
            return failure("SDP media kind is unsupported");
        }
        if (!media.rejected && media.transport.dataPort == 0) {
            return failure("SDP media port is zero");
        }
        if (!media.rejected && media.codecs.empty()) {
            return failure("SDP media has no codec");
        }
    }
    return {};
}

SdpNegotiationResult SdpNegotiator::createOffer(
    const SdpNegotiationInput& local) {
    return negotiate(local, nullptr, false);
}

SdpNegotiationResult SdpNegotiator::createAnswer(
    const SdpNegotiationInput& local,
    const SdpSessionDescription& remoteOffer) {
    return negotiate(local, &remoteOffer, true);
}

SdpNegotiationResult SdpNegotiator::applyRemoteAnswer(
    const SdpNegotiationInput& local,
    const SdpSessionDescription& remoteAnswer) {
    return negotiate(local, &remoteAnswer, false);
}

} // namespace iris::media::v2
