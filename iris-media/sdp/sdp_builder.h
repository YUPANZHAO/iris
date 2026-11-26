#pragma once

#include "sdp/sdp_types.h"

#include <string>

namespace iris::media::sdp {

class SdpBuilder {
public:
    Status build(
        const SdpSessionDescription& description,
        std::string* sdp) const;
};

} // namespace iris::media::sdp
