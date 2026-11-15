#include "media/transport/rtp.h"

#include <chrono>
#include <random>

namespace iris::media {

RtpSender::RtpSender() {
    std::random_device device;
    std::mt19937 generator(device());
    std::uniform_int_distribution<std::uint32_t> distribution;
    ssrc_ = distribution(generator);
    sequence_ = static_cast<std::uint16_t>(distribution(generator));
}

std::vector<std::uint8_t> RtpSender::create(
    std::uint8_t payloadType,
    std::uint32_t timestamp,
    const std::uint8_t* payload,
    std::size_t size,
    bool marker) {
    std::vector<std::uint8_t> packet(12 + size);
    packet[0] = 0x80;
    packet[1] = static_cast<std::uint8_t>(
        (marker ? 0x80 : 0) | (payloadType & 0x7f));
    packet[2] = static_cast<std::uint8_t>(sequence_ >> 8);
    packet[3] = static_cast<std::uint8_t>(sequence_);
    packet[4] = static_cast<std::uint8_t>(timestamp >> 24);
    packet[5] = static_cast<std::uint8_t>(timestamp >> 16);
    packet[6] = static_cast<std::uint8_t>(timestamp >> 8);
    packet[7] = static_cast<std::uint8_t>(timestamp);
    packet[8] = static_cast<std::uint8_t>(ssrc_ >> 24);
    packet[9] = static_cast<std::uint8_t>(ssrc_ >> 16);
    packet[10] = static_cast<std::uint8_t>(ssrc_ >> 8);
    packet[11] = static_cast<std::uint8_t>(ssrc_);
    if (size > 0 && payload) {
        std::memcpy(packet.data() + 12, payload, size);
    }
    ++sequence_;
    return packet;
}

bool parse_rtp(const std::uint8_t* data,
               std::size_t size,
               RtpPacket& packet) {
    if (!data || size < 12 || (data[0] >> 6) != 2) {
        return false;
    }
    const auto csrc_count = data[0] & 0x0f;
    const std::size_t header_size = 12 + csrc_count * 4;
    if (size < header_size) {
        return false;
    }
    packet.header.marker = (data[1] & 0x80) != 0;
    packet.header.payloadType = data[1] & 0x7f;
    packet.header.sequence =
        static_cast<std::uint16_t>((data[2] << 8) | data[3]);
    packet.header.timestamp =
        (static_cast<std::uint32_t>(data[4]) << 24) |
        (static_cast<std::uint32_t>(data[5]) << 16) |
        (static_cast<std::uint32_t>(data[6]) << 8) |
        data[7];
    packet.header.ssrc =
        (static_cast<std::uint32_t>(data[8]) << 24) |
        (static_cast<std::uint32_t>(data[9]) << 16) |
        (static_cast<std::uint32_t>(data[10]) << 8) |
        data[11];

    std::size_t offset = header_size;
    if (data[0] & 0x10) {
        if (size < offset + 4) {
            return false;
        }
        const auto extension_words =
            static_cast<std::size_t>((data[offset + 2] << 8) | data[offset + 3]);
        offset += 4 + extension_words * 4;
        if (offset > size) {
            return false;
        }
    }
    std::size_t padding = 0;
    if (data[0] & 0x20) {
        padding = data[size - 1];
        if (padding == 0 || padding > size - offset) {
            return false;
        }
    }
    packet.header.payloadOffset = offset;
    packet.payload = data + offset;
    packet.payloadSize = size - offset - padding;
    return true;
}

} // namespace iris::media

