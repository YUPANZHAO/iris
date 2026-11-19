#include "media/pipeline/h264_rtp_packetizer.h"
#include "media/pipeline/h264_bitstream.h"
#include "media/pipeline/opus_rtp_packetizer.h"
#include "media/transport/rtp.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

struct DatagramCapture {
    std::vector<std::vector<std::uint8_t>> datagrams;

    iris::media::MediaResult operator()(
        iris::media::ByteView datagram) {
        datagrams.emplace_back(
            datagram.data, datagram.data + datagram.size);
        return iris::media::MediaResult::success();
    }

    iris::media::RtpPacket parse(std::size_t index) const {
        iris::media::RtpPacket packet;
        const auto& datagram = datagrams.at(index);
        if (!iris::media::parse_rtp(
                datagram.data(), datagram.size(), packet)) {
            throw std::runtime_error("generated RTP is invalid");
        }
        return packet;
    }
};

bool testOpus() {
    const std::uint8_t opus[] = {0xfc, 0x11, 0x22};
    DatagramCapture capture;
    iris::media::pipeline::OpusRtpPacketizer packetizer;
    const auto result = packetizer.packetize(
        {opus, sizeof(opus), 960, false},
        48000,
        {111, 48000, 1200},
        std::ref(capture));
    if (!result.ok || capture.datagrams.size() != 1) {
        return false;
    }
    const auto packet = capture.parse(0);
    return packet.header.payloadType == 111 &&
        packet.header.timestamp == 48000 &&
        packet.header.marker &&
        packet.payloadSize == sizeof(opus) &&
        std::equal(
            opus, opus + sizeof(opus), packet.payload);
}

bool testH264ParameterSetsAndSingleNal() {
    const std::uint8_t parameterSets[] = {
        0, 0, 0, 1, 0x67, 0x42, 0x00, 0x1f,
        0, 0, 1, 0x68, 0xce, 0x06,
    };
    const std::uint8_t idr[] = {
        0, 0, 0, 1, 0x65, 0x88, 0x84,
    };
    DatagramCapture capture;
    iris::media::pipeline::H264RtpPacketizer packetizer;
    packetizer.setParameterSets(
        {parameterSets, sizeof(parameterSets)});
    const auto result = packetizer.packetize(
        {idr, sizeof(idr), 1, true},
        6000,
        {96, 90000, 1200},
        std::ref(capture));
    if (!result.ok || capture.datagrams.size() != 3) {
        return false;
    }
    const auto sps = capture.parse(0);
    const auto pps = capture.parse(1);
    const auto nal = capture.parse(2);
    return (sps.payload[0] & 0x1f) == 7 &&
        (pps.payload[0] & 0x1f) == 8 &&
        (nal.payload[0] & 0x1f) == 5 &&
        !sps.header.marker && !pps.header.marker &&
        nal.header.marker &&
        sps.header.timestamp == 6000 &&
        pps.header.timestamp == 6000 &&
        nal.header.timestamp == 6000;
}

bool testH264FuA() {
    std::vector<std::uint8_t> accessUnit = {
        0, 0, 0, 1, 0x65,
    };
    accessUnit.resize(3001, 0x55);
    DatagramCapture capture;
    iris::media::pipeline::H264RtpPacketizer packetizer;
    const auto result = packetizer.packetize(
        {accessUnit.data(), accessUnit.size(), 0, true},
        9000,
        {96, 90000, 1200},
        std::ref(capture));
    if (!result.ok || capture.datagrams.size() != 3) {
        return false;
    }
    const auto first = capture.parse(0);
    const auto middle = capture.parse(1);
    const auto last = capture.parse(2);
    return (first.payload[0] & 0x1f) == 28 &&
        (first.payload[1] & 0x80) != 0 &&
        (first.payload[1] & 0x40) == 0 &&
        (first.payload[1] & 0x1f) == 5 &&
        (middle.payload[1] & 0xc0) == 0 &&
        (last.payload[1] & 0x40) != 0 &&
        (last.payload[1] & 0x1f) == 5 &&
        !first.header.marker && !middle.header.marker &&
        last.header.marker &&
        first.payloadSize <= 1200 &&
        middle.payloadSize <= 1200 &&
        last.payloadSize <= 1200;
}

bool testAvccParameterSets() {
    const std::uint8_t extradata[] = {
        1, 0x42, 0x00, 0x1f, 0xff,
        0xe1,
        0x00, 0x02, 0x67, 0x42,
        0x01,
        0x00, 0x01, 0x68,
    };
    const auto parameterSets =
        iris::media::pipeline::h264ParameterSets(
            {extradata, sizeof(extradata)});
    const auto nals = iris::media::pipeline::splitAnnexB(
        parameterSets.data(), parameterSets.size());
    return nals.size() == 2 &&
        (nals[0].data[0] & 0x1f) == 7 &&
        (nals[1].data[0] & 0x1f) == 8;
}

} // namespace

int main() {
    if (!testOpus()) {
        std::cerr << "Opus RTP packetizer failed\n";
        return 1;
    }
    if (!testH264ParameterSetsAndSingleNal()) {
        std::cerr << "H.264 RTP packetizer single-NAL test failed\n";
        return 1;
    }
    if (!testH264FuA()) {
        std::cerr << "H.264 RTP packetizer FU-A test failed\n";
        return 1;
    }
    if (!testAvccParameterSets()) {
        std::cerr << "H.264 AVCC parameter-set parsing failed\n";
        return 1;
    }
    std::cout << "RTP packetizer tests passed\n";
    return 0;
}
