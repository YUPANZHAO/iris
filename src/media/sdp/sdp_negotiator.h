#pragma once

#include "sdp_types.h"

namespace iris::media::v2 {

// Negotiation is pure with respect to I/O: it consumes parsed descriptions and
// local capabilities, then returns a description for IMediaControl to build.
// Payload types supplied by a remote offer take precedence over local defaults.
class ISdpNegotiator {
public:
    virtual ~ISdpNegotiator() = default;

    virtual SdpNegotiationResult createOffer(
        const SdpNegotiationInput& local) = 0;
    virtual SdpNegotiationResult createAnswer(
        const SdpNegotiationInput& local,
        const SdpSessionDescription& remoteOffer) = 0;
    virtual SdpNegotiationResult applyRemoteAnswer(
        const SdpNegotiationInput& local,
        const SdpSessionDescription& remoteAnswer) = 0;
};

} // namespace iris::media::v2
