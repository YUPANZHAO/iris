#pragma once

#include <cstdint>

namespace iris::media::transport {

// Reserves a currently available loopback UDP port for SDP advertisement.
// The port is released before this function returns.
std::uint16_t allocateUdpPort() noexcept;

} // namespace iris::media::transport
