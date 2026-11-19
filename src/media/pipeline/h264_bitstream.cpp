#include "media/pipeline/h264_bitstream.h"

#include <algorithm>
#include <initializer_list>

namespace iris::media::pipeline {
namespace {

bool startsWithAnnexBStartCode(const std::uint8_t* data,
                               std::size_t size) {
    return (size >= 4 && data[0] == 0 && data[1] == 0 &&
            data[2] == 0 && data[3] == 1) ||
        (size >= 3 && data[0] == 0 && data[1] == 0 && data[2] == 1);
}

} // namespace

std::vector<NalUnitView> splitAnnexB(const std::uint8_t* data,
                                    std::size_t size) {
    std::vector<NalUnitView> result;
    std::vector<std::size_t> starts;
    if (!data || size == 0) {
        return result;
    }
    for (std::size_t index = 0; index + 3 <= size; ++index) {
        if (index + 4 <= size && data[index] == 0 &&
            data[index + 1] == 0 && data[index + 2] == 0 &&
            data[index + 3] == 1) {
            starts.push_back(index + 4);
            index += 3;
        } else if (data[index] == 0 && data[index + 1] == 0 &&
                   data[index + 2] == 1) {
            starts.push_back(index + 3);
            index += 2;
        }
    }
    if (starts.empty()) {
        result.push_back({data, size});
        return result;
    }
    for (std::size_t index = 0; index < starts.size(); ++index) {
        const auto begin = starts[index];
        auto end = index + 1 < starts.size()
            ? starts[index + 1] - 3
            : size;
        while (end > begin && data[end - 1] == 0) {
            --end;
        }
        if (end > begin) {
            result.push_back({data + begin, end - begin});
        }
    }
    return result;
}

bool containsNalType(const std::vector<NalUnitView>& nals,
                     std::uint8_t type) {
    return std::any_of(
        nals.begin(), nals.end(), [type](const NalUnitView& nal) {
            return nal.size > 0 && (nal.data[0] & 0x1f) == type;
        });
}

bool detectH264KeyFrame(const std::uint8_t* data, std::size_t size) {
    const auto nals = splitAnnexB(data, size);
    return containsNalType(nals, 5);
}

void appendH264StartCode(std::vector<std::uint8_t>& output) {
    output.insert(
        output.end(), std::initializer_list<std::uint8_t>{0, 0, 0, 1});
}

std::vector<std::uint8_t> h264ParameterSets(ByteView extradata) {
    std::vector<std::uint8_t> result;
    const auto* data = extradata.data;
    const auto size = extradata.size;
    if (!data || size == 0) {
        return result;
    }
    if (startsWithAnnexBStartCode(data, size)) {
        result.assign(data, data + size);
        return result;
    }
    if (size < 7 || data[0] != 1) {
        return result;
    }
    std::size_t offset = 5;
    const int spsCount = data[offset++] & 0x1f;
    for (int index = 0; index < spsCount; ++index) {
        if (offset + 2 > size) {
            return {};
        }
        const auto length =
            static_cast<std::size_t>((data[offset] << 8) |
                                     data[offset + 1]);
        offset += 2;
        if (length == 0 || offset + length > size) {
            return {};
        }
        appendH264StartCode(result);
        result.insert(result.end(), data + offset, data + offset + length);
        offset += length;
    }
    if (offset >= size) {
        return {};
    }
    const int ppsCount = data[offset++];
    for (int index = 0; index < ppsCount; ++index) {
        if (offset + 2 > size) {
            return {};
        }
        const auto length =
            static_cast<std::size_t>((data[offset] << 8) |
                                     data[offset + 1]);
        offset += 2;
        if (length == 0 || offset + length > size) {
            return {};
        }
        appendH264StartCode(result);
        result.insert(result.end(), data + offset, data + offset + length);
        offset += length;
    }
    return result;
}

} // namespace iris::media::pipeline
