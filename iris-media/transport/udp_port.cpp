#include "transport/udp_port.h"

#include <cstdint>
#include <mutex>
#include <set>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace iris::media::transport {

std::uint16_t allocateUdpPort() noexcept {
    static std::mutex mutex;
    static std::set<std::uint16_t> assigned;
    try {
        std::lock_guard<std::mutex> lock(mutex);
        for (int attempt = 0; attempt < 64; ++attempt) {
#ifdef _WIN32
            WSADATA data{};
            if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
                return 0;
            }
            const auto socketValue =
                socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (socketValue == INVALID_SOCKET) {
                WSACleanup();
                continue;
            }
#else
            const auto socketValue =
                socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (socketValue < 0) {
                continue;
            }
#endif

            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            address.sin_port = 0;
            auto result = bind(
                socketValue,
                reinterpret_cast<const sockaddr*>(&address),
                sizeof(address));
            std::uint16_t port = 0;
            if (result == 0) {
#ifdef _WIN32
                int size = sizeof(address);
#else
                socklen_t size = sizeof(address);
#endif
                result = getsockname(
                    socketValue,
                    reinterpret_cast<sockaddr*>(&address),
                    &size);
                if (result == 0) {
                    port = ntohs(address.sin_port);
                }
            }
#ifdef _WIN32
            closesocket(socketValue);
            WSACleanup();
#else
            close(socketValue);
#endif
            if (port != 0 && assigned.insert(port).second) {
                return port;
            }
        }
    } catch (...) {
    }
    return 0;
}

} // namespace iris::media::transport
