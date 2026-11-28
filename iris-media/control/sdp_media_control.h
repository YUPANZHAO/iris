#pragma once

#include "control/media_control.h"
#include "sdp/sdp.h"

#include <string>

namespace iris::media::control {

struct SdpResult {
    Status status;
    std::string sdp;
};

// Peer-connection-like SDP flow:
//   createOffer() -> setLocalDescription() -> setRemoteDescription(answer)
//   setRemoteDescription(offer) -> createAnswer() -> setLocalDescription()
class SdpMediaControl : public MediaControl {
public:
    explicit SdpMediaControl(sdp::SdpNegotiationInput local);
    ~SdpMediaControl() override = default;

    SdpResult createOffer();
    SdpResult createAnswer();
    Status setLocalDescription(const std::string& sdp);
    Status setRemoteDescription(const std::string& sdp);

    sdp::SdpSessionDescription localDescription() const;
    sdp::SdpSessionDescription remoteDescription() const;

private:
    Status buildStreams(
        const sdp::SdpNegotiationResult& result,
        const sdp::SdpSessionDescription* remote);
    Status makeStream(
        const sdp::SdpNegotiatedMedia& media,
        const sdp::SdpSessionDescription& local,
        const sdp::SdpSessionDescription* remote,
        StreamId id,
        MediaStream* mediaStream) const;

    sdp::SdpNegotiationInput local_;
    sdp::SdpSessionDescription localDescription_;
    sdp::SdpSessionDescription remoteDescription_;
    bool hasLocalDescription_ = false;
    bool hasRemoteDescription_ = false;
    sdp::SdpBuilder builder_;
    sdp::SdpParser parser_;
    sdp::SdpNegotiator negotiator_;
};

} // namespace iris::media::control
