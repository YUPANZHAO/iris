#pragma once

#include "media/control/media_control_impl.h"
#include "media/sdp/sdp_components.h"

#include <mutex>
#include <string>

namespace iris::media::v2 {

struct SdpOfferOptions {
    SdpNegotiationInput local;
    bool replaceExistingStreams = false;
};

struct SdpAnswerOptions {
    SdpNegotiationInput local;
    std::string remoteOffer;
    bool startImmediately = false;
};

class SDPMediaControl final : public MediaControl {
public:
    SDPMediaControl();
    ~SDPMediaControl() override;

    Status createOffer(const SdpOfferOptions& options,
                       std::string* offerSdp);
    Status createAnswer(const SdpAnswerOptions& options,
                        std::string* answerSdp);
    Status applyRemoteOffer(const std::string& remoteOffer,
                            const SdpNegotiationInput& local,
                            bool startImmediately);
    Status applyRemoteAnswer(const std::string& remoteAnswer);
    Status renegotiate(const SdpNegotiationInput& local);

    SdpSessionDescription localDescription() const;
    SdpSessionDescription remoteDescription() const;

private:
    Status createStreams(const SdpNegotiationResult& result,
                         const SdpNegotiationInput& local,
                         bool replaceExisting);
    Status applyStreamEndpoints(
        SdpSessionDescription* local,
        const SdpSessionDescription* remote);

    mutable std::mutex sdpMutex_;
    SdpBuilder builder_;
    SdpParser parser_;
    SdpNegotiator negotiator_;
    SdpSessionDescription local_;
    SdpSessionDescription remote_;
};

} // namespace iris::media::v2
