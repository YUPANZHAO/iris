#include "sip/pj_endpoint.h"

#include <pjsua2.hpp>
#include <pjsua-lib/pjsua.h>

#include <exception>

namespace iris::sip {
namespace {

bool udp_port_available(unsigned port) {
    if (port == 0) {
        return true;
    }
    pj_sock_t socket = PJ_INVALID_SOCKET;
    if (pj_sock_socket(pj_AF_INET(), pj_SOCK_DGRAM(), 0, &socket) !=
        PJ_SUCCESS) {
        return false;
    }
    pj_sockaddr address{};
    const pj_str_t any = pj_str(const_cast<char*>("0.0.0.0"));
    auto available = false;
    if (pj_sockaddr_init(
            pj_AF_INET(), &address, &any,
            static_cast<pj_uint16_t>(port)) == PJ_SUCCESS) {
        available =
            pj_sock_bind(
                socket, &address, pj_sockaddr_get_len(&address)) ==
            PJ_SUCCESS;
    }
    pj_sock_close(socket);
    return available;
}

} // namespace

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
        transport.port = udp_port_available(config.sipPort)
            ? config.sipPort
            : 0;
        pj::TransportId transport_id = PJSUA_INVALID_ID;
        try {
            transport_id =
                endpoint_->transportCreate(PJSIP_TRANSPORT_UDP, transport);
        } catch (const pj::Error&) {
            if (config.sipPort == 0) {
                throw;
            }
            // Let the operating system choose an available loopback-capable
            // UDP port. This allows several Iris clients on one machine.
            transport.port = 0;
            transport_id =
                endpoint_->transportCreate(PJSIP_TRANSPORT_UDP, transport);
        }
        pjsua_transport_info transport_info{};
        if (pjsua_transport_get_info(
                transport_id, &transport_info) == PJ_SUCCESS) {
            sip_port_ = pj_sockaddr_get_port(
                &transport_info.local_addr);
        }

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
    sip_port_ = 0;
}

std::string PjEndpoint::version() const {
    if (!endpoint_) {
        return "unknown";
    }
    auto version = endpoint_->libVersion();
    return version.full.empty() ? std::string("unknown") : version.full;
}

} // namespace iris::sip

