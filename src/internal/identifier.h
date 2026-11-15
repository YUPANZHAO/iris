#pragma once

#include <cstdint>
#include <string>

namespace iris::internal {

inline std::string make_identifier(const char* prefix, std::uint64_t value) {
    return std::string(prefix) + "-" + std::to_string(value);
}

} // namespace iris::internal

