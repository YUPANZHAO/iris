#include "media/transport/pj_ioqueue_udp_transport.h"
#include "media/transport/pj_ioqueue_service.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

int main() {
    using namespace iris;
    using namespace iris::media;

    PjIoqueueService io_service;
    PjIoqueueUdpTransport sender(MediaKind::Audio, io_service);
    PjIoqueueUdpTransport receiver(MediaKind::Audio, io_service);

    TransportConfig config;
    config.bindAddress = "127.0.0.1";
    config.advertiseAddress = "127.0.0.1";
    config.rtcpMux = true;
    if (!sender.open(config).ok || !receiver.open(config).ok) {
        std::cerr << "transport open failed\n";
        return 1;
    }

    std::mutex mutex;
    std::condition_variable condition;
    std::vector<bool> received_rtcp;
    std::vector<std::size_t> received_sizes;
    std::vector<std::uint8_t> received_types;
    receiver.setPacketHandler(
        [&](MediaKind kind, bool is_rtcp, ByteView packet) {
            if (kind != MediaKind::Audio || packet.size == 0) {
                return;
            }
            {
                std::lock_guard<std::mutex> lock(mutex);
                received_rtcp.push_back(is_rtcp);
                received_sizes.push_back(packet.size);
                received_types.push_back(
                    packet.size > 1 ? packet.data[1] : 0);
            }
            condition.notify_one();
        });

    const auto sender_description = sender.description();
    const auto receiver_description = receiver.description();
    if (!sender.setRemote(
            receiver_description.address,
            receiver_description.rtpPort).ok ||
        !receiver.setRemote(
            sender_description.address,
            sender_description.rtpPort).ok) {
        std::cerr << "setting remote address failed\n";
        return 1;
    }

    const std::uint8_t rtp[] = {
        0x80, 111, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1,
    };
    const std::uint8_t rtcp[] = {
        0x80, 200, 0, 1, 0, 0, 0, 1,
    };
    if (!sender.sendRtp(
            MediaKind::Audio, ByteView{rtp, sizeof(rtp)}).ok ||
        !sender.sendRtcp(
            MediaKind::Audio, ByteView{rtcp, sizeof(rtcp)}).ok) {
        std::cerr << "transport send failed\n";
        return 1;
    }

    {
        std::unique_lock<std::mutex> lock(mutex);
        if (!condition.wait_for(
                lock, std::chrono::seconds(2),
                [&] { return received_rtcp.size() >= 2; })) {
            std::cerr << "timed out waiting for ioqueue packets\n";
            return 1;
        }
    }

    const bool saw_rtp =
        std::find(received_rtcp.begin(), received_rtcp.end(), false) !=
        received_rtcp.end();
    const bool saw_rtcp =
        std::find(received_rtcp.begin(), received_rtcp.end(), true) !=
        received_rtcp.end();
    if (!saw_rtp || !saw_rtcp) {
        std::cerr << "RTP/RTCP classification failed\n";
        for (const bool is_rtcp : received_rtcp) {
            std::cerr << "  is_rtcp=" << is_rtcp << '\n';
        }
        for (std::size_t index = 0;
             index < received_sizes.size();
             ++index) {
            std::cerr << "  size=" << received_sizes[index]
                      << " type="
                      << static_cast<int>(received_types[index])
                      << '\n';
        }
        return 1;
    }

    std::atomic<bool> stop_racing_sender{false};
    std::thread racing_sender([&] {
        while (!stop_racing_sender.load()) {
            sender.sendRtp(
                MediaKind::Audio, ByteView{rtp, sizeof(rtp)});
        }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    sender.close();
    receiver.close();
    stop_racing_sender.store(true);
    racing_sender.join();
    sender.close();
    receiver.close();

    std::cout << "PJ ioqueue RTP/RTCP transport passed\n";
    return 0;
}
