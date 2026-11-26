#pragma once

#include "sdp/sdp_types.h"

namespace iris::media::sdp {

class SdpNegotiator {
public:
    SdpNegotiationResult createOffer(
        const SdpNegotiationInput& local) const;
    SdpNegotiationResult createAnswer(
        const SdpNegotiationInput& local,
        const SdpSessionDescription& remoteOffer) const;
    SdpNegotiationResult applyRemoteAnswer(
        const SdpNegotiationInput& local,
        const SdpSessionDescription& remoteAnswer) const;
};

} // namespace iris::media::sdp
