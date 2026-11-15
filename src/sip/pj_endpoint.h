#pragma once

#include "iris/config.h"

#include <memory>
#include <string>

namespace pj {
class Endpoint;
}

namespace iris::sip {

class PjEndpoint {
public:
    PjEndpoint() = default;
    PjEndpoint(const PjEndpoint&) = delete;
    PjEndpoint& operator=(const PjEndpoint&) = delete;
    ~PjEndpoint();

    void initialize(const RuntimeConfig& config);
    void shutdown() noexcept;
    std::string version() const;

private:
    std::unique_ptr<pj::Endpoint> endpoint_;
    bool started_ = false;
};

} // namespace iris::sip

