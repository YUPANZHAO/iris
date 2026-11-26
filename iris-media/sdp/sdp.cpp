#include "sdp/sdp.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <utility>

namespace iris::media::sdp {
namespace {

Status failure(std::string message) {
    return Status::failure(std::move(message));
}

bool parseInteger(const std::string& text, int* value) {
    if (!value || text.empty()) {
        return false;
    }
    const auto* begin = text.data();
    const auto* end = begin + text.size();
    const auto result = std::from_chars(begin, end, *value);
    return result.ec == std::errc{} && result.ptr == end;
}

bool parseUnsigned64(const std::string& text, std::uint64_t* value) {
    if (!value || text.empty()) {
        return false;
    }
    const auto* begin = text.data();
    const auto* end = begin + text.size();
    const auto result = std::from_chars(begin, end, *value);
    return result.ec == std::errc{} && result.ptr == end;
}

std::string lower(std::string value) {
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
    return value;
}

std::string encodingName(CodecType codec) {
    switch (codec) {
    case CodecType::Opus:
        return "opus";
    case CodecType::H264:
        return "H264";
    case CodecType::Unknown:
        return {};
    }
    return {};
}

CodecType codecType(const std::string& encoding) {
    const auto normalized = lower(encoding);
    if (normalized == "opus") {
        return CodecType::Opus;
    }
    if (normalized == "h264") {
        return CodecType::H264;
    }
    return CodecType::Unknown;
}

std::string mediaText(MediaKind kind) {
    switch (kind) {
    case MediaKind::Audio:
        return "audio";
    case MediaKind::Video:
        return "video";
    case MediaKind::Unknown:
        return {};
    }
    return {};
}

MediaKind mediaKind(const std::string& text) {
    if (text == "audio") {
        return MediaKind::Audio;
    }
    if (text == "video") {
        return MediaKind::Video;
    }
    return MediaKind::Unknown;
}

std::string directionText(SdpDirection direction) {
    switch (direction) {
    case SdpDirection::SendOnly:
        return "sendonly";
    case SdpDirection::RecvOnly:
        return "recvonly";
    case SdpDirection::SendRecv:
        return "sendrecv";
    case SdpDirection::Inactive:
        return "inactive";
    }
    return "inactive";
}

SdpDirection parseDirection(const std::string& text) {
    if (text == "sendonly") {
        return SdpDirection::SendOnly;
    }
    if (text == "recvonly") {
        return SdpDirection::RecvOnly;
    }
    if (text == "sendrecv") {
        return SdpDirection::SendRecv;
    }
    return SdpDirection::Inactive;
}

bool sends(SdpDirection direction) noexcept {
    return direction == SdpDirection::SendOnly ||
           direction == SdpDirection::SendRecv;
}

bool receives(SdpDirection direction) noexcept {
    return direction == SdpDirection::RecvOnly ||
           direction == SdpDirection::SendRecv;
}

// Resolve the local view of two directional capabilities. Local SendOnly
// means this endpoint can send; remote RecvOnly means the peer can receive.
SdpDirection negotiateDirection(
    SdpDirection local,
    SdpDirection remote) noexcept {
    const bool localSend = sends(local) && receives(remote);
    const bool localReceive = receives(local) && sends(remote);
    if (localSend && localReceive) {
        return SdpDirection::SendRecv;
    }
    if (localSend) {
        return SdpDirection::SendOnly;
    }
    if (localReceive) {
        return SdpDirection::RecvOnly;
    }
    return SdpDirection::Inactive;
}

bool isInternalMediaAttribute(const std::string& name) {
    static const std::set<std::string> names = {
        "bitrateKbps",
        "channels",
        "frameRateDenominator",
        "frameRateNumerator",
        "height",
        "resolution",
        "rtcpMux",
        "sampleRate",
        "width",
    };
    return names.find(name) != names.end();
}

bool isTransportOnlyAttribute(const std::string& name) {
    return name == "connectionAddress" ||
           name == "controlPort" ||
           name == "rtcpMux";
}

bool enabled(const std::string& value) {
    return value == "1" || lower(value) == "true";
}

void writeAttribute(
    std::ostringstream& output,
    const std::string& name,
    const std::string& value) {
    output << "a=" << name;
    if (!value.empty()) {
        output << ":" << value;
    }
    output << "\r\n";
}

std::optional<int> codecClockRate(
    const SdpCodecDescription& codec) {
    const auto clock = codec.attributes.find("clockRate");
    if (clock != codec.attributes.end()) {
        int value = 0;
        if (parseInteger(clock->second, &value) && value > 0) {
            return value;
        }
        return std::nullopt;
    }
    if (codec.codecType == CodecType::Opus) {
        return 48000;
    }
    if (codec.codecType == CodecType::H264) {
        return 90000;
    }
    return std::nullopt;
}

std::string codecEncodingName(
    CodecType type,
    const std::string& explicitName) {
    return explicitName.empty() ? encodingName(type) : explicitName;
}

Status validateDescription(
    const SdpSessionDescription& description) {
    if (description.connection.address.empty()) {
        return failure("SDP connection address is missing");
    }
    if (description.media.empty()) {
        return failure("SDP has no media descriptions");
    }

    std::set<std::string> mids;
    for (const auto& media : description.media) {
        if (media.kind == MediaKind::Unknown) {
            return failure("SDP media kind is unsupported");
        }
        if (!media.mid.empty() && !mids.insert(media.mid).second) {
            return failure("SDP media mid is duplicated");
        }
        if (media.rejected) {
            continue;
        }
        if (media.transport.dataPort == 0) {
            return failure("active SDP media port is zero");
        }
        if (media.codecs.empty()) {
            return failure("active SDP media has no codec");
        }

        std::set<int> payloadTypes;
        for (const auto& codec : media.codecs) {
            if (codec.payloadType < 0 || codec.payloadType > 127) {
                return failure("SDP payload type is out of range");
            }
            if (!payloadTypes.insert(codec.payloadType).second) {
                return failure("SDP payload type is duplicated");
            }
            const auto clock = codec.attributes.find("clockRate");
            if (clock != codec.attributes.end()) {
                int value = 0;
                if (!parseInteger(clock->second, &value) || value <= 0) {
                    return failure("SDP codec clock rate is invalid");
                }
            }
        }
    }
    return {};
}

bool matchingCodec(
    const SdpCodecPreference& local,
    const SdpCodecDescription& remote) {
    if (local.codecType != CodecType::Unknown &&
        remote.codecType != CodecType::Unknown) {
        return local.codecType == remote.codecType;
    }
    const auto localName = lower(
        codecEncodingName(local.codecType, local.encodingName));
    const auto remoteName = lower(
        codecEncodingName(remote.codecType, remote.encodingName));
    return !localName.empty() && localName == remoteName;
}

std::optional<SdpCodecDescription> selectCodec(
    const SdpMediaCapability& local,
    const SdpMediaDescription& remote) {
    for (const auto& preference : local.codecs) {
        for (const auto& remoteCodec : remote.codecs) {
            if (!matchingCodec(preference, remoteCodec)) {
                continue;
            }
            auto selected = remoteCodec;
            if (selected.codecType == CodecType::Unknown) {
                selected.codecType = preference.codecType != CodecType::Unknown
                    ? preference.codecType
                    : codecType(selected.encodingName);
            }
            if (selected.encodingName.empty()) {
                selected.encodingName = codecEncodingName(
                    selected.codecType, preference.encodingName);
            }
            return selected;
        }
    }
    return std::nullopt;
}

SdpCodecDescription advertiseCodec(
    const SdpCodecPreference& preference) {
    SdpCodecDescription result;
    result.codecType = preference.codecType;
    result.encodingName =
        codecEncodingName(preference.codecType, preference.encodingName);
    result.payloadType = preference.payloadType;
    result.fmtp = preference.fmtp;
    result.attributes = preference.attributes;
    return result;
}

SdpMediaDescription localMediaDescription(
    const SdpMediaCapability& capability,
    bool includeAllCodecs) {
    SdpMediaDescription result;
    result.kind = capability.kind;
    result.mid = capability.mid.empty()
        ? mediaText(capability.kind)
        : capability.mid;
    result.direction = capability.direction;
    result.transport = capability.transport;
    result.headerExtensions = capability.headerExtensions;
    result.attributes = capability.attributes;
    if (includeAllCodecs) {
        result.codecs.reserve(capability.codecs.size());
        for (const auto& codec : capability.codecs) {
            result.codecs.push_back(advertiseCodec(codec));
        }
    }
    return result;
}

SdpNegotiationResult negotiationFailure(std::string message) {
    SdpNegotiationResult result;
    result.status = failure(std::move(message));
    return result;
}

void initializeDescription(
    SdpSessionDescription* description,
    const SdpNegotiationInput& local) {
    description->origin = local.origin;
    description->sessionName =
        local.sessionName.empty() ? "iris" : local.sessionName;
    description->connection = local.connection;
    description->startTime = local.startTime;
    description->stopTime = local.stopTime;
    description->attributes = local.attributes;
    if (description->origin.unicastAddress.empty()) {
        description->origin.unicastAddress =
            description->connection.address;
    }
}

const SdpMediaCapability* findLocalMedia(
    const SdpNegotiationInput& local,
    const SdpMediaDescription& remote) {
    if (!remote.mid.empty()) {
        const auto byMid = std::find_if(
            local.media.begin(),
            local.media.end(),
            [&](const SdpMediaCapability& candidate) {
                return candidate.mid == remote.mid;
            });
        if (byMid != local.media.end()) {
            return &*byMid;
        }
    }

    const auto byKind = std::find_if(
        local.media.begin(),
        local.media.end(),
        [&](const SdpMediaCapability& candidate) {
            return candidate.kind == remote.kind;
        });
    return byKind == local.media.end() ? nullptr : &*byKind;
}

SdpNegotiationResult negotiate(
    const SdpNegotiationInput& local,
    const SdpSessionDescription* remote) {
    if (local.media.empty()) {
        return negotiationFailure("local SDP has no media capability");
    }
    if (local.connection.address.empty()) {
        return negotiationFailure("local SDP connection address is empty");
    }

    SdpNegotiationResult result;
    initializeDescription(&result.description, local);

    if (!remote) {
        for (const auto& capability : local.media) {
            if (capability.kind == MediaKind::Unknown ||
                capability.codecs.empty()) {
                continue;
            }
            auto section = localMediaDescription(capability, true);
            result.description.media.push_back(section);

            SdpNegotiatedMedia selected;
            selected.kind = section.kind;
            selected.mid = section.mid;
            selected.direction = section.direction;
            selected.transport = section.transport;
            selected.codec = section.codecs.front();
            result.media.push_back(std::move(selected));
        }
        if (result.description.media.empty()) {
            return negotiationFailure(
                "local SDP has no usable media capability");
        }
        result.status = validateDescription(result.description);
        return result;
    }

    if (remote->media.empty()) {
        return negotiationFailure("remote SDP has no media");
    }

    for (const auto& remoteMedia : remote->media) {
        const auto* localMedia = findLocalMedia(local, remoteMedia);
        SdpMediaDescription section;
        section.kind = remoteMedia.kind;
        section.mid = remoteMedia.mid;
        if (section.mid.empty() && localMedia) {
            section.mid = localMedia->mid;
        }
        if (section.mid.empty()) {
            section.mid = mediaText(section.kind);
        }

        auto selectedCodec = localMedia
            ? selectCodec(*localMedia, remoteMedia)
            : std::nullopt;
        auto direction = localMedia
            ? negotiateDirection(
                  localMedia->direction,
                  remoteMedia.direction)
            : SdpDirection::Inactive;

        const bool rejected =
            !localMedia ||
            remoteMedia.rejected ||
            !selectedCodec ||
            direction == SdpDirection::Inactive;
        if (rejected) {
            section.direction = SdpDirection::Inactive;
            section.rejected = true;
            section.transport.dataPort = 0;
            if (localMedia) {
                section.transport = localMedia->transport;
                section.transport.dataPort = 0;
                section.headerExtensions =
                    localMedia->headerExtensions;
                section.attributes = localMedia->attributes;
            }
            result.description.media.push_back(section);

            SdpNegotiatedMedia rejectedMedia;
            rejectedMedia.kind = section.kind;
            rejectedMedia.mid = section.mid;
            rejectedMedia.direction = SdpDirection::Inactive;
            rejectedMedia.rejected = true;
            result.media.push_back(std::move(rejectedMedia));
            continue;
        }

        section.direction = direction;
        section.transport = localMedia->transport;
        section.headerExtensions = localMedia->headerExtensions;
        section.attributes = localMedia->attributes;
        section.codecs.push_back(*selectedCodec);
        result.description.media.push_back(section);

        SdpNegotiatedMedia selected;
        selected.kind = section.kind;
        selected.mid = section.mid;
        selected.direction = section.direction;
        selected.transport = section.transport;
        selected.codec = *selectedCodec;
        result.media.push_back(std::move(selected));
    }

    if (result.description.media.empty()) {
        return negotiationFailure("no mutually supported SDP media");
    }
    result.status = validateDescription(result.description);
    return result;
}

} // namespace

Status SdpBuilder::build(
    const SdpSessionDescription& description,
    std::string* sdp) const {
    if (!sdp) {
        return failure("SDP output is null");
    }
    const auto valid = validateDescription(description);
    if (!valid.ok) {
        return valid;
    }

    const auto network = description.connection.networkType.empty()
        ? std::string("IN")
        : description.connection.networkType;
    const auto addressType = description.connection.addressType.empty()
        ? std::string("IP4")
        : description.connection.addressType;
    const auto originNetwork = description.origin.networkType.empty()
        ? network
        : description.origin.networkType;
    const auto originAddressType = description.origin.addressType.empty()
        ? addressType
        : description.origin.addressType;
    const auto originAddress =
        description.origin.unicastAddress.empty()
        ? description.connection.address
        : description.origin.unicastAddress;

    std::ostringstream output;
    output << "v=0\r\n";
    output << "o="
           << (description.origin.username.empty()
                   ? "-"
                   : description.origin.username)
           << " " << description.origin.sessionId
           << " " << description.origin.sessionVersion
           << " " << originNetwork
           << " " << originAddressType
           << " " << originAddress
           << "\r\n";
    output << "s="
           << (description.sessionName.empty()
                   ? "iris"
                   : description.sessionName)
           << "\r\n";
    output << "c=" << network << " " << addressType
           << " " << description.connection.address << "\r\n";
    output << "t=" << description.startTime
           << " " << description.stopTime << "\r\n";
    for (const auto& attribute : description.attributes) {
        writeAttribute(output, attribute.first, attribute.second);
    }

    for (const auto& media : description.media) {
        const auto protocol = media.transport.protocol.empty()
            ? std::string("RTP/AVP")
            : media.transport.protocol;
        output << "m=" << mediaText(media.kind) << " "
               << (media.rejected ? 0 : media.transport.dataPort)
               << " " << protocol;
        if (!media.rejected) {
            for (const auto& codec : media.codecs) {
                output << " " << codec.payloadType;
            }
        }
        output << "\r\n";
        output << "c=" << network << " " << addressType
               << " " << description.connection.address << "\r\n";
        if (!media.mid.empty()) {
            writeAttribute(output, "mid", media.mid);
        }
        writeAttribute(output, directionText(media.direction), {});

        if (!media.rejected) {
            for (const auto& codec : media.codecs) {
                const auto encoding =
                    codecEncodingName(
                        codec.codecType, codec.encodingName);
                const auto clock = codecClockRate(codec);
                if (encoding.empty() || !clock) {
                    return failure(
                        "SDP codec mapping is incomplete");
                }

                std::ostringstream mapping;
                mapping << (codec.encodingName.empty()
                    ? encoding
                    : codec.encodingName);
                mapping << "/" << *clock;
                const auto channels =
                    codec.attributes.find("channels");
                if (channels != codec.attributes.end() &&
                    !channels->second.empty()) {
                    mapping << "/" << channels->second;
                }
                writeAttribute(
                    output,
                    "rtpmap",
                    std::to_string(codec.payloadType) + " " +
                        mapping.str());
                if (!codec.fmtp.empty()) {
                    writeAttribute(
                        output,
                        "fmtp",
                        std::to_string(codec.payloadType) + " " +
                            codec.fmtp);
                }
            }
        }

        for (const auto& attribute : media.attributes) {
            if (isInternalMediaAttribute(attribute.first)) {
                continue;
            }
            writeAttribute(
                output, attribute.first, attribute.second);
        }
        for (const auto& attribute : media.transport.attributes) {
            if (isTransportOnlyAttribute(attribute.first)) {
                continue;
            }
            writeAttribute(
                output, attribute.first, attribute.second);
        }

        const auto mediaMux =
            media.attributes.find("rtcpMux");
        const auto transportMux =
            media.transport.attributes.find("rtcpMux");
        const bool rtcpMux =
            (mediaMux != media.attributes.end() &&
             enabled(mediaMux->second)) ||
            (transportMux != media.transport.attributes.end() &&
             enabled(transportMux->second));
        if (rtcpMux) {
            writeAttribute(output, "rtcp-mux", {});
        }

        for (const auto& extension : media.headerExtensions) {
            std::ostringstream value;
            value << extension.id << " " << extension.uri;
            for (const auto& attribute : extension.attributes) {
                value << " " << attribute.first;
                if (!attribute.second.empty()) {
                    value << "=" << attribute.second;
                }
            }
            writeAttribute(output, "extmap", value.str());
        }
    }

    *sdp = output.str();
    return {};
}

Status SdpParser::parse(
    const std::string& sdp,
    SdpSessionDescription* description) const {
    if (!description || sdp.empty()) {
        return failure("SDP input or output is empty");
    }

    SdpSessionDescription result;
    SdpMediaDescription* current = nullptr;
    bool versionSeen = false;
    std::istringstream input(sdp);
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }
        if (line.size() < 2 || line[1] != '=') {
            return failure("invalid SDP line");
        }

        const char type = line[0];
        const auto value = line.substr(2);
        if (type == 'v') {
            if (value != "0") {
                return failure("unsupported SDP version");
            }
            versionSeen = true;
        } else if (type == 'o') {
            std::istringstream fields(value);
            std::uint64_t sessionId = 0;
            std::uint64_t version = 0;
            if (!(fields >> result.origin.username >> sessionId >>
                  version >> result.origin.networkType >>
                  result.origin.addressType >>
                  result.origin.unicastAddress)) {
                return failure("invalid SDP origin");
            }
            result.origin.sessionId = sessionId;
            result.origin.sessionVersion = version;
        } else if (type == 's') {
            result.sessionName = value;
        } else if (type == 'c') {
            std::istringstream fields(value);
            std::string network;
            std::string addressType;
            std::string address;
            if (!(fields >> network >> addressType >> address)) {
                return failure("invalid SDP connection");
            }
            if (current) {
                current->transport.attributes["connectionAddress"] =
                    address;
            } else {
                result.connection.networkType = network;
                result.connection.addressType = addressType;
                result.connection.address = address;
            }
        } else if (type == 't') {
            std::int64_t start = 0;
            std::int64_t stop = 0;
            std::istringstream fields(value);
            if (!(fields >> start >> stop)) {
                return failure("invalid SDP timing");
            }
            result.startTime = start;
            result.stopTime = stop;
        } else if (type == 'm') {
            std::istringstream fields(value);
            std::string kind;
            std::string portText;
            std::string protocol;
            if (!(fields >> kind >> portText >> protocol)) {
                return failure("invalid SDP media");
            }
            const auto slash = portText.find('/');
            if (slash != std::string::npos) {
                portText.erase(slash);
            }
            int port = 0;
            if (!parseInteger(portText, &port) ||
                port < 0 || port > 65535) {
                return failure("invalid SDP media port");
            }

            SdpMediaDescription media;
            media.kind = mediaKind(kind);
            media.transport.protocol = protocol;
            media.transport.dataPort =
                static_cast<std::uint16_t>(port);
            media.rejected = port == 0;

            std::string payload;
            while (fields >> payload) {
                int payloadType = 0;
                if (!parseInteger(payload, &payloadType)) {
                    media.attributes["format." + payload] = payload;
                    continue;
                }
                if (payloadType < 0 || payloadType > 127) {
                    return failure("invalid SDP payload type");
                }
                SdpCodecDescription codec;
                codec.payloadType = payloadType;
                media.codecs.push_back(std::move(codec));
            }
            result.media.push_back(std::move(media));
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
            } else if (name == "rtcp" && current) {
                int controlPort = 0;
                if (!parseInteger(attribute, &controlPort) ||
                    controlPort < 0 || controlPort > 65535) {
                    return failure("invalid SDP RTCP port");
                }
                current->transport.controlPort =
                    static_cast<std::uint16_t>(controlPort);
            } else if (name == "rtpmap" && current) {
                std::istringstream fields(attribute);
                int payloadType = -1;
                std::string mapping;
                if (!(fields >> payloadType >> mapping)) {
                    return failure("invalid SDP rtpmap");
                }
                const auto first = mapping.find('/');
                const auto second = first == std::string::npos
                    ? std::string::npos
                    : mapping.find('/', first + 1);
                const auto encoding = mapping.substr(0, first);
                if (encoding.empty()) {
                    return failure("SDP rtpmap has no encoding");
                }

                bool found = false;
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
                    found = true;
                }
                if (!found) {
                    return failure(
                        "SDP rtpmap payload is not listed");
                }
            } else if (name == "fmtp" && current) {
                std::istringstream fields(attribute);
                int payloadType = -1;
                std::string fmtp;
                if (!(fields >> payloadType)) {
                    return failure("invalid SDP fmtp");
                }
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
                if (!(fields >> extension.id >> extension.uri)) {
                    return failure("invalid SDP extmap");
                }
                std::string extensionAttribute;
                while (fields >> extensionAttribute) {
                    const auto equals =
                        extensionAttribute.find('=');
                    extension.attributes[
                        extensionAttribute.substr(0, equals)] =
                        equals == std::string::npos
                        ? std::string()
                        : extensionAttribute.substr(equals + 1);
                }
                current->headerExtensions.push_back(
                    std::move(extension));
            } else if (
                current &&
                (name == "ice-ufrag" ||
                 name == "ice-pwd" ||
                 name == "ice-options" ||
                 name == "fingerprint" ||
                 name == "setup")) {
                current->transport.attributes[name] = attribute;
            } else if (current) {
                current->attributes[name] = attribute;
            } else {
                result.attributes[name] = attribute;
            }
        } else if (type == 'b') {
            if (current) {
                current->attributes["bandwidth"] = value;
            } else {
                result.attributes["bandwidth"] = value;
            }
        }
    }

    if (!versionSeen) {
        return failure("SDP version is missing");
    }
    const auto valid = validate(result);
    if (!valid.ok) {
        return valid;
    }
    *description = std::move(result);
    return {};
}

Status SdpParser::validate(
    const SdpSessionDescription& description) const {
    return validateDescription(description);
}

SdpNegotiationResult SdpNegotiator::createOffer(
    const SdpNegotiationInput& local) const {
    return negotiate(local, nullptr);
}

SdpNegotiationResult SdpNegotiator::createAnswer(
    const SdpNegotiationInput& local,
    const SdpSessionDescription& remoteOffer) const {
    return negotiate(local, &remoteOffer);
}

SdpNegotiationResult SdpNegotiator::applyRemoteAnswer(
    const SdpNegotiationInput& local,
    const SdpSessionDescription& remoteAnswer) const {
    return negotiate(local, &remoteAnswer);
}

} // namespace iris::media::sdp
