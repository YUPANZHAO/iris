#pragma once

#include "sdp_types.h"

#include <string>

namespace iris::media::v2 {

class ISdpParser {
public:
    virtual ~ISdpParser() = default;

    virtual Status parse(const std::string& sdp,
                         SdpSessionDescription* description) = 0;
    virtual Status validate(
        const SdpSessionDescription& description) const = 0;
};

} // namespace iris::media::v2
