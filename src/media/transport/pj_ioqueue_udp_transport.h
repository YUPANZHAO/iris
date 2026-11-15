#pragma once

#include "media/engine/media_interfaces.h"

#include <cstdint>
#include <memory>

namespace iris::media {

class PjIoqueueService;

class PjIoqueueUdpTransport final : public IMediaTransport {
public:
    PjIoqueueUdpTransport(MediaKind media_kind,
                          PjIoqueueService& service);
    ~PjIoqueueUdpTransport() override;

    PjIoqueueUdpTransport(const PjIoqueueUdpTransport&) = delete;
    PjIoqueueUdpTransport& operator=(const PjIoqueueUdpTransport&) =
        delete;

    MediaResult open(const TransportConfig& config) override;
    void close() noexcept override;
    TransportDescription description() const override;
    void setPacketHandler(PacketHandler handler) override;
    MediaResult sendRtp(MediaKind kind, ByteView packet) override;
    MediaResult sendRtcp(MediaKind kind, ByteView packet) override;

    MediaResult setRemote(const std::string& address,
                          std::uint16_t port);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace iris::media
