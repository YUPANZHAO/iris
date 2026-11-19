#include "media/pipeline/h264_rtp_packetizer.h"

#include "media/pipeline/h264_bitstream.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace iris::media::pipeline {

void H264RtpPacketizer::setParameterSets(ByteView parameterSets) {
    parameterSets_.clear();
    if (parameterSets.data && parameterSets.size > 0) {
        parameterSets_.assign(
            parameterSets.data, parameterSets.data + parameterSets.size);
    }
}

MediaResult H264RtpPacketizer::packetize(
    const EncodedPacketView& accessUnit,
    std::uint32_t rtpTimestamp,
    const RtpParameters& parameters,
    const RtpDatagramCallback& sink) {
    if (!accessUnit.data || accessUnit.size == 0 || !sink ||
        parameters.mtu <= 2) {
        return MediaResult::failure("invalid H.264 packetizer input");
    }
    const auto nals = splitAnnexB(accessUnit.data, accessUnit.size);
    if (nals.empty()) {
        return MediaResult::failure("H.264 access unit has no NAL units");
    }

    if (containsNalType(nals, 5) && !containsNalType(nals, 7) &&
        !parameterSets_.empty()) {
        const auto parameterNals =
            splitAnnexB(parameterSets_.data(), parameterSets_.size());
        for (const auto& parameterNal : parameterNals) {
            auto result = sendPayload(
                parameterNal.data, parameterNal.size,
                parameters.payloadType, rtpTimestamp, false, sink);
            if (!result.ok) {
                return result;
            }
        }
    }

    for (std::size_t nalIndex = 0; nalIndex < nals.size(); ++nalIndex) {
        const auto nal = nals[nalIndex];
        const bool finalNal = nalIndex + 1 == nals.size();
        if (nal.size <= parameters.mtu) {
            auto result = sendPayload(
                nal.data, nal.size, parameters.payloadType,
                rtpTimestamp, finalNal, sink);
            if (!result.ok) {
                return result;
            }
            continue;
        }

        const std::uint8_t indicator =
            static_cast<std::uint8_t>((nal.data[0] & 0xe0) | 28);
        std::size_t offset = 1;
        bool first = true;
        while (offset < nal.size) {
            const auto fragmentSize =
                std::min(parameters.mtu - 2, nal.size - offset);
            std::vector<std::uint8_t> payload(fragmentSize + 2);
            payload[0] = indicator;
            payload[1] = static_cast<std::uint8_t>(
                (first ? 0x80 : 0) |
                (offset + fragmentSize >= nal.size ? 0x40 : 0) |
                (nal.data[0] & 0x1f));
            std::memcpy(
                payload.data() + 2, nal.data + offset, fragmentSize);
            const bool last = offset + fragmentSize >= nal.size;
            auto result = sendPayload(
                payload.data(), payload.size(), parameters.payloadType,
                rtpTimestamp, finalNal && last, sink);
            if (!result.ok) {
                return result;
            }
            offset += fragmentSize;
            first = false;
        }
    }
    return MediaResult::success();
}

MediaResult H264RtpPacketizer::sendPayload(
    const std::uint8_t* data,
    std::size_t size,
    std::uint8_t payloadType,
    std::uint32_t rtpTimestamp,
    bool marker,
    const RtpDatagramCallback& sink) {
    auto datagram = rtp_.create(
        payloadType, rtpTimestamp, data, size, marker);
    return sink({datagram.data(), datagram.size()});
}

} // namespace iris::media::pipeline
