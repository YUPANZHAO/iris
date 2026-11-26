#pragma once

#include "sdp/sdp_types.h"

#include <string>

namespace iris::media::sdp {

class SdpParser {
public:
    Status parse(
        const std::string& sdp,
        SdpSessionDescription* description) const;
    Status validate(
        const SdpSessionDescription& description) const;
};

} // namespace iris::media::sdp
