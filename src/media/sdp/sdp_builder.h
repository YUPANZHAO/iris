#pragma once

#include "sdp_types.h"

#include <string>

namespace iris::media::v2 {

class ISdpBuilder {
public:
    virtual ~ISdpBuilder() = default;

    virtual Status build(const SdpSessionDescription& description,
                         std::string* sdp) = 0;
};

} // namespace iris::media::v2
