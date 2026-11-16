#include "media/sdp/codec_registry.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <iomanip>
#include <iterator>
#include <map>
#include <optional>
#include <sstream>

namespace iris::media {
namespace {

std::string lower(std::string value) {
    std::transform(
        value.begin(), value.end(), value.begin(),
        [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
    return value;
}

std::map<std::string, std::string> fmtp_parameters(
    const std::string& fmtp) {
    std::map<std::string, std::string> result;
    std::istringstream stream(fmtp);
    std::string item;
    while (std::getline(stream, item, ';')) {
        const auto separator = item.find('=');
        std::string name = separator == std::string::npos
            ? item
            : item.substr(0, separator);
        std::string value = separator == std::string::npos
            ? std::string()
            : item.substr(separator + 1);
        name.erase(
            std::remove_if(
                name.begin(), name.end(),
                [](unsigned char ch) { return std::isspace(ch); }),
            name.end());
        const auto first = value.find_first_not_of(" \t");
        value = first == std::string::npos
            ? std::string()
            : value.substr(first, value.find_last_not_of(" \t") - first + 1);
        if (!name.empty()) {
            result[lower(name)] = value;
        }
    }
    return result;
}

enum class H264Profile {
    ConstrainedBaseline, Baseline, Main, Extended, High, ConstrainedHigh,
    High10, High422, High444, High10Intra, High422Intra, High444Intra,
    Cavlc444Intra,
};

struct H264Format {
    H264Profile profile;
    unsigned profileBytes;
    unsigned levelIdc;
    bool level1b;

    unsigned levelRank() const {
        return level1b ? 105 : levelIdc * 10;
    }
};

using FmtpParameters = std::map<std::string, std::string>;

std::string parameter(const FmtpParameters& params, const char* name,
                      const char* fallback) {
    const auto it = params.find(name);
    return it == params.end() ? fallback : it->second;
}

std::optional<H264Format> parse_h264(const FmtpParameters& params) {
    // RFC 6184 defaults to Baseline, Level 1, when the value is absent.
    const auto value = parameter(params, "profile-level-id", "42000a");
    unsigned id = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(),
                                        id, 16);
    if (value.size() != 6 || parsed.ec != std::errc{} ||
        parsed.ptr != value.data() + value.size()) {
        return std::nullopt;
    }
    const unsigned profile_idc = id >> 16;
    const unsigned flags = (id >> 8) & 0xff;
    const unsigned level = id & 0xff;
    const bool legacy = profile_idc == 0x42 || profile_idc == 0x4d ||
                        profile_idc == 0x58;
    const bool level1b = legacy ? level == 11 && (flags & 0x10) != 0
                                : level == 9;
    constexpr unsigned levels[] = {
        10, 11, 12, 13, 20, 21, 22, 30, 31, 32, 40, 41, 42, 50, 51, 52, 60, 61, 62
    };
    if (!level1b && std::find(std::begin(levels), std::end(levels), level) ==
                        std::end(levels)) {
        return std::nullopt;
    }

    // RFC 6184 Table 5. The mask selects the significant constraint flags.
    using Profile = H264Profile;
    constexpr struct {
        unsigned idc, mask, flags;
        Profile profile;
    } patterns[] = {
        {0x42, 0x4f, 0x40, Profile::ConstrainedBaseline},
        {0x4d, 0x8f, 0x80, Profile::ConstrainedBaseline},
        {0x58, 0xcf, 0xc0, Profile::ConstrainedBaseline},
        {0x42, 0x4f, 0x00, Profile::Baseline},
        {0x58, 0xcf, 0x80, Profile::Baseline},
        {0x4d, 0xaf, 0x00, Profile::Main},
        {0x58, 0xcf, 0x00, Profile::Extended},
        {0x64, 0xff, 0x00, Profile::High},
        {0x64, 0xff, 0x0c, Profile::ConstrainedHigh},
        {0x6e, 0xff, 0x00, Profile::High10},
        {0x7a, 0xff, 0x00, Profile::High422},
        {0xf4, 0xff, 0x00, Profile::High444},
        {0x6e, 0xff, 0x10, Profile::High10Intra},
        {0x7a, 0xff, 0x10, Profile::High422Intra},
        {0xf4, 0xff, 0x10, Profile::High444Intra},
        {0x2c, 0xff, 0x10, Profile::Cavlc444Intra},
    };
    for (const auto& pattern : patterns) {
        if (profile_idc == pattern.idc &&
            (flags & pattern.mask) == pattern.flags) {
            return H264Format{pattern.profile, id >> 8, level, level1b};
        }
    }
    return std::nullopt;
}

struct H264Negotiation {
    std::string fmtp;
    bool sameProfile;
    bool sameLevel;
};

std::optional<H264Negotiation> negotiate_h264(
    const CodecProfile& local, const RemoteCodecFormat& remote,
    SdpPeer peer) {
    auto local_params = fmtp_parameters(local.fmtp);
    const auto remote_params = fmtp_parameters(remote.fmtp);
    const auto local_mode = parameter(local_params, "packetization-mode", "0");
    const auto remote_mode = parameter(remote_params, "packetization-mode", "0");
    if (local_mode != remote_mode ||
        (local_mode != "0" && local_mode != "1" && local_mode != "2")) {
        return std::nullopt;
    }

    const auto local_format = parse_h264(local_params);
    const auto remote_format = parse_h264(remote_params);
    if (!local_format || !remote_format) {
        return std::nullopt;
    }
    // The default backend encodes Constrained Baseline and can decode
    // Baseline. A subset bitstream satisfies a Baseline offer.
    const bool baseline_offer = peer == SdpPeer::Offer &&
        local.acceptsH264Baseline &&
        local_format->profile == H264Profile::ConstrainedBaseline &&
        remote_format->profile == H264Profile::Baseline;
    if (local_format->profile != remote_format->profile && !baseline_offer) {
        return std::nullopt;
    }

    const bool asymmetric =
        parameter(local_params, "level-asymmetry-allowed", "0") == "1" &&
        parameter(remote_params, "level-asymmetry-allowed", "0") == "1";
    if (peer == SdpPeer::Answer && !asymmetric &&
        remote_format->levelRank() > local_format->levelRank()) {
        return std::nullopt;
    }
    const auto& level = asymmetric ||
            local_format->levelRank() <= remote_format->levelRank()
        ? *local_format : *remote_format;
    unsigned profile = remote_format->profileBytes;
    unsigned level_idc = level.levelIdc;
    const auto idc = profile >> 8;
    if (idc == 0x42 || idc == 0x4d || idc == 0x58) {
        if (level.level1b) {
            profile |= 0x10;
            level_idc = 11;
        } else if (level_idc == 11) {
            profile &= ~0x10u;
        }
    } else if (level.level1b) {
        level_idc = 9;
    }

    std::ostringstream profile_level;
    profile_level << std::hex << std::setw(6) << std::setfill('0')
                  << ((profile << 8) | level_idc);
    local_params["profile-level-id"] = profile_level.str();
    if (!asymmetric) {
        local_params.erase("level-asymmetry-allowed");
    }
    if (profile != local_format->profileBytes ||
        level.levelRank() != local_format->levelRank()) {
        // Parameter sets belong to the original profile and level.
        local_params.erase("sprop-parameter-sets");
        local_params.erase("sprop-level-parameter-sets");
    }
    std::string fmtp;
    for (const auto& [name, value] : local_params) {
        if (!fmtp.empty()) {
            fmtp += ';';
        }
        fmtp += name + '=' + value;
    }
    return H264Negotiation{
        std::move(fmtp), !baseline_offer,
        local_format->levelRank() == remote_format->levelRank(),
    };
}

CodecProfile default_audio_codec() {
    return CodecProfile{
        MediaKind::Audio,
        "opus",
        "opus",
        48000,
        2,
        111,
        "minptime=10;useinbandfec=1;maxaveragebitrate=48000",
    };
}

CodecProfile default_video_codec() {
    return CodecProfile{
        MediaKind::Video,
        "h264-baseline",
        "H264",
        90000,
        0,
        96,
        "packetization-mode=1;profile-level-id=42e01f;"
        "level-asymmetry-allowed=1",
        true,
    };
}

CodecProfile profile_from_preference(
    const MediaCodecPreference& preference) {
    const auto encoding = lower(preference.encoding);
    if (preference.kind == MediaKind::Audio && encoding == "opus") {
        auto profile = default_audio_codec();
        if (!preference.id.empty()) {
            profile.id = preference.id;
        }
        if (preference.clockRate > 0) {
            profile.clockRate = preference.clockRate;
        }
        if (preference.channels > 0) {
            profile.channels = preference.channels;
        }
        if (preference.payloadType >= 0 &&
            preference.payloadType <= 127) {
            profile.defaultPayloadType = preference.payloadType;
        }
        if (!preference.fmtp.empty()) {
            profile.fmtp = preference.fmtp;
            if (!fmtp_parameters(profile.fmtp).count("maxaveragebitrate")) {
                profile.fmtp += ";maxaveragebitrate=48000";
            }
        }
        return profile;
    }
    if (preference.kind == MediaKind::Video && encoding == "h264") {
        auto profile = default_video_codec();
        if (!preference.id.empty()) {
            profile.id = preference.id;
        }
        if (preference.clockRate > 0) {
            profile.clockRate = preference.clockRate;
        }
        if (preference.payloadType >= 0 &&
            preference.payloadType <= 127) {
            profile.defaultPayloadType = preference.payloadType;
        }
        if (!preference.fmtp.empty()) {
            profile.fmtp = preference.fmtp;
        }
        return profile;
    }
    return CodecProfile{
        preference.kind,
        preference.id,
        preference.encoding,
        preference.clockRate,
        preference.channels,
        preference.payloadType,
        preference.fmtp,
    };
}

} // namespace

CodecRegistry::CodecRegistry(
    const std::vector<MediaCodecPreference>& preferences) {
    if (preferences.empty()) {
        codecs_.push_back(default_audio_codec());
        codecs_.push_back(default_video_codec());
        return;
    }
    for (const auto& preference : preferences) {
        const auto profile = profile_from_preference(preference);
        if (profile.defaultPayloadType >= 0 &&
            profile.defaultPayloadType <= 127) {
            codecs_.push_back(profile);
        }
    }
}

std::vector<CodecProfile> CodecRegistry::codecs(
    MediaKind kind) const {
    std::vector<CodecProfile> result;
    for (const auto& codec : codecs_) {
        if (codec.kind == kind) {
            result.push_back(codec);
        }
    }
    return result;
}

std::vector<MediaCodecPreference> CodecRegistry::preferences() const {
    std::vector<MediaCodecPreference> result;
    result.reserve(codecs_.size());
    for (const auto& codec : codecs_) {
        MediaCodecPreference preference;
        preference.kind = codec.kind;
        preference.encoding = codec.encoding;
        preference.clockRate = codec.clockRate;
        preference.channels = codec.channels;
        preference.payloadType = codec.defaultPayloadType;
        preference.fmtp = codec.fmtp;
        result.push_back(std::move(preference));
    }
    return result;
}

const CodecProfile* CodecRegistry::findById(
    const std::string& id) const {
    for (const auto& codec : codecs_) {
        if (codec.id == id) {
            return &codec;
        }
    }
    return nullptr;
}

CodecSelection CodecRegistry::select(
    MediaKind kind,
    const std::vector<RemoteCodecFormat>& formats, SdpPeer peer) const {
    for (const auto& local : codecs_) {
        if (local.kind != kind) {
            continue;
        }
        CodecSelection fallback;
        int fallback_rank = 0;
        for (const auto& remote : formats) {
            if (remote.payloadType < 0 || remote.payloadType > 127 ||
                lower(local.encoding) != lower(remote.encoding) ||
                local.clockRate != remote.clockRate) {
                continue;
            }
            if (lower(local.encoding) == "h264") {
                auto negotiated = negotiate_h264(local, remote, peer);
                if (negotiated) {
                    CodecSelection selection{
                        &local, remote.payloadType, std::move(negotiated->fmtp)};
                    if (negotiated->sameProfile && negotiated->sameLevel) {
                        return selection;
                    }
                    const int rank = negotiated->sameProfile ? 2 : 1;
                    if (rank > fallback_rank) {
                        fallback_rank = rank;
                        fallback = std::move(selection);
                    }
                }
            } else if (matches(local, remote)) {
                return CodecSelection{&local, remote.payloadType, local.fmtp};
            }
        }
        if (fallback.local) {
            return fallback;
        }
    }
    return {};
}

bool CodecRegistry::matches(
    const CodecProfile& local,
    const RemoteCodecFormat& remote) {
    const auto encoding = lower(local.encoding);
    if (encoding == "opus") {
        return remote.channels == 0 ||
            remote.channels == local.channels;
    }
    return local.fmtp.empty() || remote.fmtp.empty() ||
        lower(local.fmtp) == lower(remote.fmtp);
}

} // namespace iris::media
