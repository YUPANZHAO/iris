#pragma once

#include <string>

namespace iris::sip {

std::string extract_sdp_body(const std::string& sip_message);

} // namespace iris::sip

