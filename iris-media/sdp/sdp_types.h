#pragma once

#include "codec/codec_types.h"
#include "media_types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace iris::media::sdp {

using AttributeMap = ::iris::media::AttributeMap;
using CodecType = ::iris::media::codec::CodecType;
using MediaKind = ::iris::media::MediaKind;
using Status = ::iris::media::Status;

enum class SdpDirection {
    Inactive,
    SendOnly,
    RecvOnly,
    SendRecv,
};

struct SdpOrigin {
    std::string username = "-";
    std::uint64_t sessionId = 0;
    std::uint64_t sessionVersion = 0;
    std::string networkType = "IN";
    std::string addressType = "IP4";
    std::string unicastAddress;
};

struct SdpConnection {
    std::string networkType = "IN";
    std::string addressType = "IP4";
    std::string address;
};

struct SdpTransportParameters {
    std::string protocol = "RTP/AVP";
    std::uint16_t dataPort = 0;
    std::uint16_t controlPort = 0;
    AttributeMap attributes;
};

struct SdpCodecDescription {
    CodecType codecType = CodecType::Unknown;
    std::string encodingName;
    int payloadType = -1;
    std::string fmtp;
    AttributeMap attributes;
};

struct SdpHeaderExtension {
    int id = 0;
    std::string uri;
    AttributeMap attributes;
};

struct SdpMediaDescription {
    MediaKind kind = MediaKind::Unknown;
    std::string mid;
    SdpDirection direction = SdpDirection::SendRecv;
    SdpTransportParameters transport;
    std::vector<SdpCodecDescription> codecs;
    std::vector<SdpHeaderExtension> headerExtensions;
    AttributeMap attributes;
    bool rejected = false;
};

struct SdpSessionDescription {
    SdpOrigin origin;
    std::string sessionName = "iris";
    SdpConnection connection;
    std::int64_t startTime = 0;
    std::int64_t stopTime = 0;
    std::vector<SdpMediaDescription> media;
    AttributeMap attributes;
};

struct SdpCodecPreference {
    CodecType codecType = CodecType::Unknown;
    std::string encodingName;
    int payloadType = -1;
    std::string fmtp;
    AttributeMap attributes;
};

struct SdpMediaCapability {
    MediaKind kind = MediaKind::Unknown;
    std::string mid;
    SdpDirection direction = SdpDirection::SendRecv;
    SdpTransportParameters transport;
    std::vector<SdpCodecPreference> codecs;
    std::vector<SdpHeaderExtension> headerExtensions;
    AttributeMap attributes;
};

struct SdpNegotiationInput {
    SdpOrigin origin;
    SdpConnection connection;
    std::string sessionName = "iris";
    std::int64_t startTime = 0;
    std::int64_t stopTime = 0;
    std::vector<SdpMediaCapability> media;
    AttributeMap attributes;
};

struct SdpNegotiatedMedia {
    MediaKind kind = MediaKind::Unknown;
    std::string mid;
    SdpDirection direction = SdpDirection::Inactive;
    SdpTransportParameters transport;
    SdpCodecDescription codec;
    bool rejected = false;
};

struct SdpNegotiationResult {
    Status status;
    SdpSessionDescription description;
    std::vector<SdpNegotiatedMedia> media;
};

} // namespace iris::media::sdp
