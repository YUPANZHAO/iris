#include "media/pipeline/opus_rtp_packetizer.h"

namespace iris::media::pipeline {

MediaResult OpusRtpPacketizer::packetize(
    const EncodedPacketView& packet,
    std::uint32_t rtpTimestamp,
    const RtpParameters& parameters,
    const RtpDatagramCallback& sink) {
    if (!packet.data || packet.size == 0 || !sink) {
        return MediaResult::failure("invalid Opus packetizer input");
    }
    auto datagram = rtp_.create(
        parameters.payloadType, rtpTimestamp,
        packet.data, packet.size, true);
    return sink({datagram.data(), datagram.size()});
}

} // namespace iris::media::pipeline
