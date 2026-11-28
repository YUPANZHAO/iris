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
    const transport::RtpParams& params,
    const codec::EncodedPacket& packet) {
    if (mediaKind_ == MediaKind::Video && packet.data &&
        packet.data->data() && packet.data->size() > 0) {
        const auto* bytes = static_cast<const std::uint8_t*>(
            packet.data->data());
        const auto size = packet.data->size();
        bool key = false;
        if (size >= 2 && (bytes[0] & 0x1f) == 28) {
            const auto fuHeader = bytes[1];
            if ((fuHeader & 0x80) != 0) {
                if (h264FragmentInProgress_) {
                    h264AccessUnit_.clear();
                }
                h264FragmentInProgress_ = true;
                h264AccessUnit_.insert(
                    h264AccessUnit_.end(),
                    {0, 0, 0, 1,
                     static_cast<std::uint8_t>(
                         (bytes[0] & 0xe0) |
                         (fuHeader & 0x1f))});
            }
            key = key || (fuHeader & 0x1f) == 5;
            h264AccessUnit_.insert(
                h264AccessUnit_.end(), bytes + 2, bytes + size);
        } else {
            h264AccessUnit_.insert(
                h264AccessUnit_.end(),
                {0, 0, 0, 1});
            h264AccessUnit_.insert(
                h264AccessUnit_.end(), bytes, bytes + size);
            key = (bytes[0] & 0x1f) == 5;
        }
        if (!params.marker || h264AccessUnit_.empty()) {
            return;
        }
        auto data = std::make_shared<ByteBuffer>(
            h264AccessUnit_.data(), h264AccessUnit_.size());
        if (!data || !*data) {
            h264AccessUnit_.clear();
            h264FragmentInProgress_ = false;
            return;
        }
        auto processed = packet;
        processed.kind = MediaKind::Video;
        processed.codecType = codec::CodecType::H264;
        processed.data = std::move(data);
        processed.key = key;
        h264AccessUnit_.clear();
        h264FragmentInProgress_ = false;
        Stream::onEncodedPacket(processed);
        return;
    }

    auto processed = packet;
    if (mediaKind_ != MediaKind::Unknown) {
        processed.kind = mediaKind_;
    }
    Stream::onEncodedPacket(processed);
}

void RtpMediaStream::onRtcpRecv(const transport::RtcpParams&) {
}

} // namespace iris::media::pipeline
