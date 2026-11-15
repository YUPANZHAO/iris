#include "sip/sip_message.h"

#include <algorithm>
#include <cctype>

namespace iris::sip {

std::string extract_sdp_body(const std::string& sip_message) {
    auto separator = sip_message.find("\r\n\r\n");
    std::size_t offset = 4;
    if (separator == std::string::npos) {
        separator = sip_message.find("\n\n");
        offset = 2;
    }
    if (separator == std::string::npos) {
        return {};
    }
    auto body = sip_message.substr(separator + offset);
    while (!body.empty() &&
           std::isspace(static_cast<unsigned char>(body.front()))) {
        body.erase(body.begin());
    }
    return body.rfind("v=0", 0) == 0 ? body : std::string{};
}

} // namespace iris::sip

