#pragma once

#include "media/engine/media_interfaces.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace iris::media::pipeline {

struct NalUnitView {
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
};

std::vector<NalUnitView> splitAnnexB(const std::uint8_t* data,
                                     std::size_t size);

bool containsNalType(const std::vector<NalUnitView>& nals,
                     std::uint8_t type);

bool detectH264KeyFrame(const std::uint8_t* data, std::size_t size);

void appendH264StartCode(std::vector<std::uint8_t>& output);

// Converts AVCC extradata, or already-Annex-B extradata, into cached SPS/PPS
// NAL units with Annex-B start codes.
std::vector<std::uint8_t> h264ParameterSets(ByteView extradata);

} // namespace iris::media::pipeline
