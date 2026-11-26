#pragma once

// Cross-platform UDP implementation of the transport boundary.

#include "transport/transport.h"

#include <memory>

namespace iris::media::transport {

class UdpTransport final : public Transport {
public:
    UdpTransport();
    ~UdpTransport() override;

    UdpTransport(const UdpTransport&) = delete;
    UdpTransport& operator=(const UdpTransport&) = delete;

    Status send(MediaBuffer data) override;
    void setSink(
        std::weak_ptr<ITransportSink> sink) noexcept override;
    Status configure(const AttributeMap& options) override;
    Status attach(const AttributeMap& options) override;
    void detach() noexcept override;

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace iris::media::transport
