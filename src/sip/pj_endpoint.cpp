#include "sip/pj_endpoint.h"

#include <pjsua2.hpp>

#include <exception>

namespace iris::sip {

PjEndpoint::~PjEndpoint() {
    shutdown();
}

void PjEndpoint::initialize(const RuntimeConfig& config) {
    if (endpoint_) {
        return;
    }

    endpoint_ = std::make_unique<pj::Endpoint>();
    bool created = false;
    try {
        endpoint_->libCreate();
        created = true;

        pj::EpConfig endpoint_config;
        endpoint_config.uaConfig.userAgent =
            std::string("libiris/") + IRIS_VERSION;
        endpoint_config.uaConfig.threadCnt = 1;
        endpoint_config.medConfig.hasIoqueue = true;
        endpoint_config.medConfig.threadCnt = 1;
        endpoint_config.logConfig.level = config.logLevel;
        endpoint_config.logConfig.consoleLevel = config.logLevel;

        endpoint_->libInit(endpoint_config);

        pj::TransportConfig transport;
        transport.port = config.sipPort;
        endpoint_->transportCreate(PJSIP_TRANSPORT_UDP, transport);

        endpoint_->libStart();
        endpoint_->libRegisterThread("iris-core");
        started_ = true;

        if (config.nullAudioDevice) {
            endpoint_->audDevManager().setNullDev();
        }
    } catch (...) {
        if (created) {
            try {
                endpoint_->libDestroy();
            } catch (...) {
            }
        }
        endpoint_.reset();
        started_ = false;
        throw;
    }
}

void PjEndpoint::shutdown() noexcept {
    if (!endpoint_) {
        return;
    }

    if (started_) {
        try {
            endpoint_->libDestroy();
        } catch (...) {
        }
    }
    endpoint_.reset();
    started_ = false;
}

std::string PjEndpoint::version() const {
    if (!endpoint_) {
        return "unknown";
    }
    auto version = endpoint_->libVersion();
    return version.full.empty() ? std::string("unknown") : version.full;
}

} // namespace iris::sip

