#pragma once

#include "iris/media.h"

#include <string>
#include <vector>

namespace iris::media {

struct RemoteCodecFormat {
    int payloadType = -1;
    std::string encoding;
    int clockRate = 0;
    int channels = 0;
    std::string fmtp;
};

struct CodecProfile {
    MediaKind kind = MediaKind::Audio;
    std::string id;
    std::string encoding;
    int clockRate = 0;
    int channels = 0;
    int defaultPayloadType = -1;
    std::string fmtp;
    bool acceptsH264Baseline = false;
};

struct CodecSelection {
    const CodecProfile* local = nullptr;
    int payloadType = -1;
    std::string localFmtp;
};

enum class SdpPeer { Offer, Answer };

class CodecRegistry final {
public:
    explicit CodecRegistry(
        const std::vector<MediaCodecPreference>& preferences = {});

    std::vector<CodecProfile> codecs(MediaKind kind) const;
    std::vector<MediaCodecPreference> preferences() const;
    const CodecProfile* findById(const std::string& id) const;

    CodecSelection select(
        MediaKind kind,
        const std::vector<RemoteCodecFormat>& formats,
        SdpPeer peer = SdpPeer::Offer) const;

private:
    static bool matches(const CodecProfile& local,
                        const RemoteCodecFormat& remote);

    std::vector<CodecProfile> codecs_;
};

} // namespace iris::media
