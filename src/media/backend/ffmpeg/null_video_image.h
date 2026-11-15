#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace iris::media {

struct NullVideoImage {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> bgra;
};

NullVideoImage renderNullVideoImage(
    int width,
    int height,
    const std::string& localUri);

} // namespace iris::media
