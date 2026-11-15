#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace iris::media {

struct RtpHeader {
    bool marker = false;
    std::uint8_t payloadType = 0;
    std::uint16_t sequence = 0;
    std::uint32_t timestamp = 0;
    std::uint32_t ssrc = 0;
    std::size_t payloadOffset = 12;
};

struct RtpPacket {
    RtpHeader header;
    const std::uint8_t* payload = nullptr;
    std::size_t payloadSize = 0;
};

class RtpSender {
public:
    RtpSender();

    std::vector<std::uint8_t> create(std::uint8_t payloadType,
                                     std::uint32_t timestamp,
                                     const std::uint8_t* payload,
                                     std::size_t size,
                                     bool marker);

private:
    std::uint16_t sequence_ = 0;
    std::uint32_t ssrc_ = 0;
};

bool parse_rtp(const std::uint8_t* data,
               std::size_t size,
               RtpPacket& packet);

} // namespace iris::media

