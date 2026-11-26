#include "pipeline/rtp_media_stream.h"

namespace iris::media::pipeline {

RtpMediaStream::RtpMediaStream(MediaKind mediaKind)
    : mediaKind_(mediaKind) {
}

RtpMediaStream::~RtpMediaStream() {
    close();
}

Status RtpMediaStream::setRTPTransport(
    std::shared_ptr<transport::RTPTransport> transportValue) {
    if (!transportValue) {
        return Status::failure("RTP transport is null");
    }
    transport_ = std::move(transportValue);
    return {};
}

Status RtpMediaStream::open() {
    const auto self = weak_from_this();
    if (self.expired()) {
        return Status::failure(
            "RTP media stream must be owned by a shared pointer to open");
    }
    if (!transport_) {
        return Status::failure("RTP media stream has no RTP transport");
    }

    transport_->setSink(
        std::weak_ptr<transport::RTPTransportSink>{self});
    return {};
}

void RtpMediaStream::close() noexcept {
    if (transport_) {
        transport_->setSink(
            std::weak_ptr<transport::RTPTransportSink>{});
    }
}

void RtpMediaStream::onRtpRecv(
    const transport::RtpParams&,
    const codec::EncodedPacket& packet) {
    auto processed = packet;
    if (mediaKind_ != MediaKind::Unknown) {
        processed.kind = mediaKind_;
    }
    Stream::onEncodedPacket(processed);
}

void RtpMediaStream::onRtcpRecv(const transport::RtcpParams&) {
}

} // namespace iris::media::pipeline
