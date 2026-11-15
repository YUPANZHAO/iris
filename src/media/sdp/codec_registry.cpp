#include "media/sdp/codec_registry.h"

#include <algorithm>
#include <cctype>
#include <map>
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
        value.erase(
            std::remove_if(
                value.begin(), value.end(),
                [](unsigned char ch) { return std::isspace(ch); }),
            value.end());
        if (!name.empty()) {
            result[lower(name)] = lower(value);
        }
    }
    return result;
}

bool h264_matches(const CodecProfile& local,
                  const RemoteCodecFormat& remote) {
    const auto local_params = fmtp_parameters(local.fmtp);
    const auto remote_params = fmtp_parameters(remote.fmtp);

    const auto remote_packetization =
        remote_params.find("packetization-mode");
    const auto local_packetization =
        local_params.find("packetization-mode");
    if (local_packetization != local_params.end() &&
        local_packetization->second == "1" &&
        (remote_packetization == remote_params.end() ||
         remote_packetization->second != "1")) {
        return false;
    }

    const auto local_profile =
        local_params.find("profile-level-id");
    const auto remote_profile =
        remote_params.find("profile-level-id");
    return local_profile == local_params.end() ||
        remote_profile == remote_params.end() ||
        local_profile->second == remote_profile->second;
}

CodecProfile default_audio_codec() {
    return CodecProfile{
        MediaKind::Audio,
        "opus",
        "opus",
        48000,
        2,
        111,
        "minptime=10;useinbandfec=1",
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
    const std::vector<RemoteCodecFormat>& formats) const {
    for (const auto& local : codecs_) {
        if (local.kind != kind) {
            continue;
        }
        for (const auto& remote : formats) {
            if (remote.payloadType >= 0 &&
                remote.payloadType <= 127 &&
                matches(local, remote)) {
                return CodecSelection{
                    &local,
                    remote.payloadType,
                    remote.fmtp,
                };
            }
        }
    }
    return {};
}

bool CodecRegistry::matches(
    const CodecProfile& local,
    const RemoteCodecFormat& remote) {
    if (lower(local.encoding) != lower(remote.encoding) ||
        local.clockRate != remote.clockRate) {
        return false;
    }
    const auto encoding = lower(local.encoding);
    if (encoding == "opus") {
        return remote.channels == 0 ||
            remote.channels == local.channels;
    }
    if (encoding == "h264") {
        return h264_matches(local, remote);
    }
    return local.fmtp.empty() || remote.fmtp.empty() ||
        lower(local.fmtp) == lower(remote.fmtp);
}

} // namespace iris::media
