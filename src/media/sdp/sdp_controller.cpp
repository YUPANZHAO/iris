#include "media/sdp/sdp_controller.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <limits>
#include <sstream>
#include <utility>
#include <vector>

extern "C" {
#include <pjmedia/sdp.h>
}

namespace iris::media {
namespace {

class PoolGuard final {
public:
    PoolGuard(pj_pool_factory* factory, const char* name)
        : pool_(factory
              ? pj_pool_create(factory, name, 4096, 4096, nullptr)
              : nullptr) {
    }

    ~PoolGuard() {
        if (pool_) {
            pj_pool_release(pool_);
        }
    }

    PoolGuard(const PoolGuard&) = delete;
    PoolGuard& operator=(const PoolGuard&) = delete;

    pj_pool_t* get() const noexcept { return pool_; }

private:
    pj_pool_t* pool_ = nullptr;
};

std::string string_value(const pj_str_t& value) {
    if (!value.ptr || value.slen <= 0) {
        return {};
    }
    return std::string(
        value.ptr,
        static_cast<std::size_t>(value.slen));
}

template <typename T>
bool parse_unsigned(std::string_view value, T& result) {
    unsigned long long parsed = 0;
    const auto parsed_result = std::from_chars(
        value.data(), value.data() + value.size(), parsed);
    if (parsed_result.ec != std::errc{} ||
        parsed_result.ptr != value.data() + value.size() ||
        parsed > static_cast<unsigned long long>(
            std::numeric_limits<T>::max())) {
        return false;
    }
    result = static_cast<T>(parsed);
    return true;
}

MediaDirection reverse_direction(MediaDirection direction) {
    switch (direction) {
    case MediaDirection::SendOnly: return MediaDirection::RecvOnly;
    case MediaDirection::RecvOnly: return MediaDirection::SendOnly;
    default: return direction;
    }
}

std::string direction_attribute(MediaDirection direction) {
    switch (direction) {
    case MediaDirection::SendOnly: return "a=sendonly";
    case MediaDirection::RecvOnly: return "a=recvonly";
    case MediaDirection::Inactive: return "a=inactive";
    case MediaDirection::SendRecv: return "a=sendrecv";
    }
    return "a=inactive";
}

const pjmedia_sdp_attr* find_attribute(
    unsigned count,
    pjmedia_sdp_attr* const attributes[],
    const char* name) {
    return pjmedia_sdp_attr_find2(count, attributes, name, nullptr);
}

MediaDirection attribute_direction(const pjmedia_sdp_attr* attribute) {
    if (!attribute) {
        return MediaDirection::SendRecv;
    }
    if (pj_strcmp2(&attribute->name, "sendonly") == 0) {
        return MediaDirection::SendOnly;
    }
    if (pj_strcmp2(&attribute->name, "recvonly") == 0) {
        return MediaDirection::RecvOnly;
    }
    if (pj_strcmp2(&attribute->name, "inactive") == 0) {
        return MediaDirection::Inactive;
    }
    return MediaDirection::SendRecv;
}

MediaDirection media_direction(
    const pjmedia_sdp_session* session,
    const pjmedia_sdp_media* media) {
    const char* names[] = {
        "sendrecv", "sendonly", "recvonly", "inactive",
    };
    for (const auto* name : names) {
        if (const auto* attribute = find_attribute(
                media->attr_count, media->attr, name)) {
            return attribute_direction(attribute);
        }
    }
    for (const auto* name : names) {
        if (const auto* attribute = find_attribute(
                session->attr_count, session->attr, name)) {
            return attribute_direction(attribute);
        }
    }
    return MediaDirection::SendRecv;
}

std::string connection_address(
    const pjmedia_sdp_session* session,
    const pjmedia_sdp_media* media) {
    const auto* connection = media->conn ? media->conn : session->conn;
    return connection ? string_value(connection->addr) : std::string();
}

struct MediaCodec {
    CodecProfile profile;
    int payloadType = -1;
};

void append_media(std::ostringstream& sdp,
                  MediaKind kind,
                  const std::string& address,
                  std::uint16_t port,
                  const std::vector<MediaCodec>& codecs,
                  MediaDirection direction) {
    sdp << "m=" << to_string(kind) << " " << port << " RTP/AVP";
    for (const auto& codec : codecs) {
        sdp << " " << codec.payloadType;
    }
    sdp << "\r\n"
        << "c=IN IP4 " << address << "\r\n"
        << "a=rtcp-mux\r\n";
    for (const auto& codec : codecs) {
        sdp << "a=rtpmap:" << codec.payloadType << " "
            << codec.profile.encoding << "/"
            << codec.profile.clockRate;
        if (codec.profile.kind == MediaKind::Audio) {
            sdp << "/" << codec.profile.channels;
        }
        sdp << "\r\n";
        if (!codec.profile.fmtp.empty()) {
            sdp << "a=fmtp:" << codec.payloadType << " "
                << codec.profile.fmtp << "\r\n";
        }
    }
    sdp << direction_attribute(direction) << "\r\n";
}

void append_rejected_media(
    std::ostringstream& sdp,
    const ParsedMediaDescription& media) {
    sdp << "m=" << media.media << " 0 "
        << (media.protocol.empty() ? "RTP/AVP" : media.protocol);
    for (const auto& format : media.formatIds) {
        sdp << " " << format;
    }
    sdp << "\r\n"
        << "a=inactive\r\n";
}

std::vector<MediaCodec> offered_codecs(
    const CodecRegistry& registry,
    MediaKind kind) {
    std::vector<MediaCodec> result;
    for (const auto& profile : registry.codecs(kind)) {
        result.push_back(MediaCodec{profile, profile.defaultPayloadType});
    }
    return result;
}

} // namespace

ParsedSdp parse_sdp(const std::string& text,
                    pj_pool_factory* pool_factory) {
    ParsedSdp result;
    PoolGuard pool(pool_factory, "iris-sdp-parse");
    if (!pool.get() || text.empty()) {
        return result;
    }

    pjmedia_sdp_session* parsed = nullptr;
    std::vector<char> mutable_text(text.begin(), text.end());
    mutable_text.push_back('\0');
    if (pjmedia_sdp_parse(
            pool.get(), mutable_text.data(), text.size(), &parsed) !=
            PJ_SUCCESS ||
        !parsed ||
        pjmedia_sdp_validate2(parsed, PJ_FALSE) != PJ_SUCCESS) {
        return result;
    }

    if (parsed->conn) {
        result.address = string_value(parsed->conn->addr);
    }
    for (unsigned media_index = 0;
         media_index < parsed->media_count;
         ++media_index) {
        const auto* media = parsed->media[media_index];
        const std::string media_name = string_value(media->desc.media);
        ParsedMediaDescription description;
        description.media = media_name;
        description.protocol = string_value(media->desc.transport);
        if (media_name == "audio") {
            description.kind = MediaKind::Audio;
            description.supported = true;
        } else if (media_name == "video") {
            description.kind = MediaKind::Video;
            description.supported = true;
        }
        description.address = connection_address(parsed, media);
        if (description.address.empty()) {
            description.address = result.address;
        }
        description.port = media->desc.port;
        description.direction = media_direction(parsed, media);
        description.rtcpMux =
            find_attribute(media->attr_count, media->attr, "rtcp-mux") !=
            nullptr;
        if (const auto* mid = find_attribute(
                media->attr_count, media->attr, "mid")) {
            description.mid = string_value(mid->value);
        }

        for (unsigned format_index = 0;
             format_index < media->desc.fmt_count;
             ++format_index) {
            int payload_type = 0;
            const auto& format = media->desc.fmt[format_index];
            const auto format_id = string_value(format);
            if (!format_id.empty()) {
                description.formatIds.push_back(format_id);
            }
            if (!parse_unsigned(string_value(format), payload_type) ||
                payload_type < 0 || payload_type > 127) {
                continue;
            }
            pj_str_t rtpmap_name =
                pj_str(const_cast<char*>("rtpmap"));
            const auto* rtpmap_attr = pjmedia_sdp_media_find_attr(
                media, &rtpmap_name, &format);
            if (!rtpmap_attr) {
                continue;
            }
            pjmedia_sdp_rtpmap rtpmap{};
            if (pjmedia_sdp_attr_get_rtpmap(
                    rtpmap_attr, &rtpmap) != PJ_SUCCESS) {
                continue;
            }

            RemoteCodecFormat remote;
            remote.payloadType = payload_type;
            remote.encoding = string_value(rtpmap.enc_name);
            remote.clockRate = static_cast<int>(rtpmap.clock_rate);
            int channels = 0;
            if (rtpmap.param.slen == 0 ||
                parse_unsigned(string_value(rtpmap.param), channels)) {
                remote.channels = channels;
            }
            pj_str_t fmtp_name =
                pj_str(const_cast<char*>("fmtp"));
            if (const auto* fmtp_attr = pjmedia_sdp_media_find_attr(
                    media, &fmtp_name, &format)) {
                pjmedia_sdp_fmtp fmtp{};
                if (pjmedia_sdp_attr_get_fmtp(
                        fmtp_attr, &fmtp) == PJ_SUCCESS) {
                    remote.fmtp = string_value(fmtp.fmt_param);
                }
            }
            description.formats.push_back(std::move(remote));
        }
        result.media.push_back(std::move(description));
    }
    return result;
}

SdpResult create_sdp_offer(
    const MediaSessionConfig& config,
    const LocalSdpPorts& ports,
    const CodecRegistry& codecs,
    pj_pool_factory*,
    const SdpOrigin* origin) {
    if (!config.audioEnabled && !config.videoEnabled) {
        return SdpResult::failure("audio and video are both disabled");
    }
    if (ports.address.empty() ||
        (config.audioEnabled && ports.audioPort == 0) ||
        (config.videoEnabled && ports.videoPort == 0)) {
        return SdpResult::failure("invalid local media transport");
    }

    std::ostringstream sdp;
    const std::string session_id =
        origin && !origin->sessionId.empty()
        ? origin->sessionId
        : "1";
    const auto session_version =
        origin ? origin->version : 1;
    sdp << "v=0\r\n"
        << "o=iris " << session_id << " "
        << session_version << " IN IP4 "
        << ports.address << "\r\n"
        << "s=Iris\r\n"
        << "c=IN IP4 " << ports.address << "\r\n"
        << "t=0 0\r\n";
    if (config.audioEnabled) {
        const auto offered = offered_codecs(codecs, MediaKind::Audio);
        if (offered.empty()) {
            return SdpResult::failure("no audio codec is registered");
        }
        append_media(
            sdp, MediaKind::Audio, ports.address, ports.audioPort,
            offered, config.audioDirection);
    }
    if (config.videoEnabled) {
        const auto offered = offered_codecs(codecs, MediaKind::Video);
        if (offered.empty()) {
            return SdpResult::failure("no video codec is registered");
        }
        append_media(
            sdp, MediaKind::Video, ports.address, ports.videoPort,
            offered, config.videoDirection);
    }
    return SdpResult::success(sdp.str());
}

SdpResult create_sdp_answer(
    const MediaSessionConfig& config,
    const LocalSdpPorts& ports,
    const std::string& remoteOffer,
    const CodecRegistry& codecs,
    pj_pool_factory* pool_factory,
    const SdpOrigin* origin) {
    const auto offer = parse_sdp(remoteOffer, pool_factory);
    if (offer.media.empty()) {
        return SdpResult::failure("remote SDP could not be parsed");
    }

    std::ostringstream sdp;
    const std::string session_id =
        origin && !origin->sessionId.empty()
        ? origin->sessionId
        : "2";
    const auto session_version =
        origin ? origin->version : 1;
    sdp << "v=0\r\n"
        << "o=iris " << session_id << " "
        << session_version << " IN IP4 "
        << ports.address << "\r\n"
        << "s=Iris\r\n"
        << "c=IN IP4 " << ports.address << "\r\n"
        << "t=0 0\r\n";

    bool accepted_any = false;
    for (const auto& media : offer.media) {
        const bool enabled = media.kind == MediaKind::Audio
            ? config.audioEnabled
            : config.videoEnabled;
        const auto selected =
            codecs.select(media.kind, media.formats);
        const bool accepted =
            media.supported &&
            media.protocol == "RTP/AVP" &&
            enabled && selected.local && media.port != 0 &&
            media.direction != MediaDirection::Inactive;
        if (accepted) {
            const auto port = media.kind == MediaKind::Audio
                ? ports.audioPort
                : ports.videoPort;
            if (ports.address.empty() || port == 0) {
                return SdpResult::failure(
                    "invalid local media transport");
            }
            auto profile = *selected.local;
            profile.fmtp = selected.localFmtp;
            append_media(
                sdp, media.kind, ports.address, port,
                {MediaCodec{std::move(profile), selected.payloadType}},
                reverse_direction(media.direction));
            accepted_any = true;
        } else {
            append_rejected_media(sdp, media);
        }
    }

    if (!accepted_any) {
        return SdpResult::failure("no mutually supported media");
    }
    return SdpResult::success(sdp.str());
}

} // namespace iris::media
