#pragma once

#include "pipeline/stream.h"

#include "transport/rtp_transport.h"

#include <memory>
#include <type_traits>
#include <utility>

namespace iris::media::pipeline {

// Publishes processed RTP payloads as audio or video encoded packets. The
// configured media kind is applied to packets delivered by the transport.
class RtpMediaStream final
    : public Stream,
      public transport::RTPTransportSink,
      public std::enable_shared_from_this<RtpMediaStream> {
public:
    explicit RtpMediaStream(MediaKind mediaKind = MediaKind::Unknown);
    ~RtpMediaStream() override;

    RtpMediaStream(const RtpMediaStream&) = delete;
    RtpMediaStream& operator=(const RtpMediaStream&) = delete;

    Status open() override;
    void close() noexcept override;

    Status setRTPTransport(
        std::shared_ptr<transport::RTPTransport> transport);
    template <
        typename RtpTransportImpl,
        std::enable_if_t<
            std::is_base_of_v<
                transport::RTPTransport,
                RtpTransportImpl>,
            int> = 0>
    Status setRTPTransport(
        std::unique_ptr<RtpTransportImpl> transport) {
        return setRTPTransport(
            std::shared_ptr<transport::RTPTransport>(std::move(transport)));
    }

    MediaKind mediaKind() const noexcept {
        return mediaKind_;
    }

    void onRtpRecv(
        const transport::RtpParams& params,
        const codec::EncodedPacket& packet) override;
    void onRtcpRecv(const transport::RtcpParams& params) override;

private:
    MediaKind mediaKind_ = MediaKind::Unknown;
    std::shared_ptr<transport::RTPTransport> transport_;
};

} // namespace iris::media::pipeline
