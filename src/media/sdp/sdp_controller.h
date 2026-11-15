#pragma once

#include "iris/media.h"
#include "media/engine/media_interfaces.h"
#include "media/sdp/codec_registry.h"

#include <pjlib.h>

#include <cstdint>
#include <string>
#include <vector>

namespace iris::media {

struct ParsedMediaDescription {
    std::string media;
    std::string protocol;
    bool supported = false;
    MediaKind kind = MediaKind::Audio;
    std::string address;
    std::uint16_t port = 0;
    std::string mid;
    bool rtcpMux = false;
    std::vector<std::string> formatIds;
    std::vector<RemoteCodecFormat> formats;
    MediaDirection direction = MediaDirection::Inactive;
};

struct ParsedSdp {
    std::string address;
    std::vector<ParsedMediaDescription> media;
};

struct LocalSdpPorts {
    std::string address;
    std::uint16_t audioPort = 0;
    std::uint16_t videoPort = 0;
};

struct SdpOrigin {
    std::string sessionId;
    std::uint64_t version = 1;
};

ParsedSdp parse_sdp(const std::string& text,
                    pj_pool_factory* pool_factory);
SdpResult create_sdp_offer(const MediaSessionConfig& config,
                           const LocalSdpPorts& ports,
                           const CodecRegistry& codecs,
                           pj_pool_factory* pool_factory,
                           const SdpOrigin* origin = nullptr);
SdpResult create_sdp_answer(const MediaSessionConfig& config,
                            const LocalSdpPorts& ports,
                            const std::string& remoteOffer,
                            const CodecRegistry& codecs,
                            pj_pool_factory* pool_factory,
                            const SdpOrigin* origin = nullptr);

} // namespace iris::media

