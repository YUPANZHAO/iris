#pragma once

#include "media/sdp/sdp_builder.h"
#include "media/sdp/sdp_negotiator.h"
#include "media/sdp/sdp_parser.h"

namespace iris::media::v2 {

class SdpBuilder final : public ISdpBuilder {
public:
    Status build(const SdpSessionDescription& description,
                 std::string* sdp) override;
};

class SdpParser final : public ISdpParser {
public:
    Status parse(const std::string& sdp,
                 SdpSessionDescription* description) override;
    Status validate(
        const SdpSessionDescription& description) const override;
};

class SdpNegotiator final : public ISdpNegotiator {
public:
    SdpNegotiationResult createOffer(
        const SdpNegotiationInput& local) override;
    SdpNegotiationResult createAnswer(
        const SdpNegotiationInput& local,
        const SdpSessionDescription& remoteOffer) override;
    SdpNegotiationResult applyRemoteAnswer(
        const SdpNegotiationInput& local,
        const SdpSessionDescription& remoteAnswer) override;
};

} // namespace iris::media::v2
