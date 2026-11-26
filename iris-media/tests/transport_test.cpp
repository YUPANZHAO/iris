#include "transport/rtp.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using namespace iris::media;
using namespace iris::media::transport;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

class RtpCollector final : public RTPTransportSink {
public:
    void onRtpRecv(const RtpParams& params,
                   const EncodedPacket& packet) override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            rtpPackets_.push_back({params, packet});
        }
        condition_.notify_all();
    }

    void onRtcpRecv(const RtcpParams& params) override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            rtcpPackets_.push_back(params);
        }
        condition_.notify_all();
    }

    bool waitForRtp(std::size_t count) const {
        std::unique_lock<std::mutex> lock(mutex_);
        return condition_.wait_for(lock, 3s, [&] {
            return rtpPackets_.size() >= count;
        });
    }

    bool waitForRtcp(std::size_t count) const {
        std::unique_lock<std::mutex> lock(mutex_);
        return condition_.wait_for(lock, 3s, [&] {
            return rtcpPackets_.size() >= count;
        });
    }

    std::vector<std::pair<RtpParams, EncodedPacket>> rtpPackets() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return rtpPackets_;
    }

    std::vector<RtcpParams> rtcpPackets() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return rtcpPackets_;
    }

private:
    mutable std::mutex mutex_;
    mutable std::condition_variable condition_;
    std::vector<std::pair<RtpParams, EncodedPacket>> rtpPackets_;
    std::vector<RtcpParams> rtcpPackets_;
};

class MediaBufferCollector final : public ITransportSink {
public:
    void onTransportData(MediaBuffer data) override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            messages_.push_back(std::move(data));
        }
        condition_.notify_all();
    }

    void onTransportState(const TransportEvent& event) override {
        std::lock_guard<std::mutex> lock(mutex_);
        states_.push_back(event.state);
    }

    bool waitFor(std::size_t count) const {
        std::unique_lock<std::mutex> lock(mutex_);
        return condition_.wait_for(lock, 3s, [&] {
            return messages_.size() >= count;
        });
    }

    bool equals(std::size_t index,
                const std::vector<std::uint8_t>& expected) const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (index >= messages_.size()) {
            return false;
        }
        const auto& message = messages_[index];
        return message && message->size() == expected.size() &&
               std::memcmp(
                   message->data(),
                   expected.data(),
                   expected.size()) == 0;
    }

    std::vector<MediaBuffer> messages() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return messages_;
    }

    bool contains(TransportState state) const {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto value : states_) {
            if (value == state) {
                return true;
            }
        }
        return false;
    }

private:
    mutable std::mutex mutex_;
    mutable std::condition_variable condition_;
    std::vector<MediaBuffer> messages_;
    std::vector<TransportState> states_;
};

MediaBuffer makeMediaBuffer(const std::vector<std::uint8_t>& bytes) {
    return std::make_shared<ByteBuffer>(bytes.data(), bytes.size());
}

EncodedPacket makePacket(StreamId streamId,
                         std::int64_t ptsMicros,
                         const std::vector<std::uint8_t>& payload) {
    EncodedPacket packet;
    packet.streamId = streamId;
    packet.data = makeMediaBuffer(payload);
    packet.ptsMicros = ptsMicros;
    return packet;
}

AttributeMap localOptions(std::uint16_t port = 0) {
    AttributeMap options;
    options["localAddress"] = "127.0.0.1";
    options["localPort"] = std::to_string(port);
    return options;
}

AttributeMap remoteOptions(std::uint16_t port) {
    AttributeMap options;
    options["remoteAddress"] = "127.0.0.1";
    options["remotePort"] = std::to_string(port);
    return options;
}

void addStreamOptions(AttributeMap& options,
                      StreamId streamId,
                      std::uint8_t payloadType,
                      int clockRate) {
    const auto prefix =
        "stream." + std::to_string(streamId) + ".";
    options[prefix + "payloadType"] =
        std::to_string(payloadType);
    options[prefix + "clockRate"] = std::to_string(clockRate);
}

std::uint16_t configureWithFreePort(
    Transport& transport,
    AttributeMap options) {
    for (std::uint16_t port = 39000; port < 39100; ++port) {
        options["localPort"] = std::to_string(port);
        if (transport.configure(options).ok) {
            return port;
        }
    }
    require(false, "no test UDP port is available");
    return 0;
}

std::uint16_t configureWithFreePort(Transport& transport) {
    return configureWithFreePort(transport, localOptions());
}

void testUdpTransport() {
    auto sink = std::make_shared<MediaBufferCollector>();

    UdpTransport receiver;
    receiver.setSink(sink);
    const auto receiverPort = configureWithFreePort(receiver);
    require(receiver.attach({}).ok, "UDP receiver attaches");

    UdpTransport sender;
    require(sender.configure(localOptions()).ok,
            "UDP sender configures");
    require(sender.attach(remoteOptions(receiverPort)).ok,
            "UDP sender attaches");

    const std::vector<std::uint8_t> message{1, 2, 3, 4};
    require(sender.send(makeMediaBuffer(message)).ok,
            "UDP transport sends a MediaBuffer");
    require(sink->waitFor(1), "UDP transport delivers an inbound message");
    require(sink->equals(0, message),
            "UDP transport preserves message bytes");
    require(sink->contains(TransportState::Configuring) &&
                sink->contains(TransportState::Configured) &&
                sink->contains(TransportState::Attaching) &&
                sink->contains(TransportState::Connected),
            "UDP transport publishes connection states");

    receiver.detach();
    require(sink->contains(TransportState::Detaching) &&
                sink->contains(TransportState::Disconnected),
            "UDP transport publishes disconnect states");
}

void testRTPRoundTrip() {
    constexpr StreamId streamId = 7;
    constexpr std::uint8_t payloadType = 111;
    constexpr int clockRate = 48000;

    RTPEngine receiver;
    auto sink = std::make_shared<RtpCollector>();
    receiver.setSink(sink);
    auto receiverConfigure = localOptions();
    addStreamOptions(
        receiverConfigure, streamId, payloadType, clockRate);
    const auto receiverPort = configureWithFreePort(
        receiver, receiverConfigure);
    require(receiver.attach({}).ok, "RTP receiver attaches");

    RTPEngine sender;
    require(sender.configure(localOptions()).ok,
            "RTP sender configures");
    auto senderAttach = remoteOptions(receiverPort);
    addStreamOptions(
        senderAttach, streamId, payloadType, clockRate);
    require(sender.attach(senderAttach).ok, "RTP sender attaches");

    const std::vector<std::uint8_t> payload1{0x10, 0x20};
    const std::vector<std::uint8_t> payload2{0x30, 0x40};
    require(sender.send(
                RtpParams{false},
                makePacket(streamId, 100000, payload1)).ok,
            "RTP sender sends the first encoded packet");
    require(sender.send(
                RtpParams{true},
                makePacket(streamId, 120000, payload2)).ok,
            "RTP sender sends the second encoded packet");
    require(sink->waitForRtp(2), "RTPEngine delivers encoded packets");

    auto packets = sink->rtpPackets();
    std::sort(
        packets.begin(),
        packets.end(),
        [](const auto& left, const auto& right) {
            return left.second.ptsMicros < right.second.ptsMicros;
        });
    require(packets.size() == 2,
            "RTPEngine emits both encoded packets");
    require(packets[0].first.marker != packets[1].first.marker,
            "RTPEngine preserves the marker field");
    require(packets[0].second.streamId == streamId &&
                packets[1].second.streamId == streamId,
            "RTPEngine restores the bound stream ID");
    require(packets[0].second.ptsMicros == 0 &&
                packets[1].second.ptsMicros == 20000,
            "RTP timestamps are restored to media time");

    UdpTransport rtcpSender;
    require(rtcpSender.configure(localOptions()).ok,
            "RTCP test sender configures");
    require(rtcpSender.attach(remoteOptions(receiverPort)).ok,
            "RTCP test sender attaches");
    const std::vector<std::uint8_t> receiverReport{
        0x80, 201, 0x00, 0x01,
        0x11, 0x22, 0x33, 0x44};
    require(rtcpSender.send(makeMediaBuffer(receiverReport)).ok,
            "RTCP test sender sends a receiver report");
    require(sink->waitForRtcp(1),
            "RTPEngine forwards structured RTCP");
    const auto rtcpPackets = sink->rtcpPackets();
    require(rtcpPackets.size() == 1 &&
                rtcpPackets[0].packetType == 201 &&
                rtcpPackets[0].ssrc == 0x11223344 &&
                rtcpPackets[0].payload &&
                rtcpPackets[0].payload->size() == 4,
            "RTCP params contain packet metadata");

    sender.detach();
    receiver.detach();
}

void testMtuFragmentation() {
    constexpr StreamId streamId = 11;
    constexpr std::uint8_t payloadType = 96;
    constexpr int clockRate = 90000;
    constexpr int mtu = 24;

    auto rawSink = std::make_shared<MediaBufferCollector>();
    UdpTransport rawReceiver;
    rawReceiver.setSink(rawSink);
    const auto receiverPort = configureWithFreePort(rawReceiver);
    require(rawReceiver.attach({}).ok, "fragmentation receiver attaches");

    RTPEngine sender;
    auto configure = localOptions();
    configure["mtu"] = std::to_string(mtu);
    addStreamOptions(configure, streamId, payloadType, clockRate);
    require(sender.configure(configure).ok,
            "RTP sender configures MTU");
    auto attach = remoteOptions(receiverPort);
    require(sender.attach(attach).ok, "fragmentation sender attaches");

    std::vector<std::uint8_t> payload(30);
    for (std::size_t index = 0; index < payload.size(); ++index) {
        payload[index] = static_cast<std::uint8_t>(index);
    }
    require(sender.send(
                RtpParams{true},
                makePacket(streamId, 1000, payload)).ok,
            "RTP sender fragments an oversized encoded packet");
    require(rawSink->waitFor(3), "RTP sender emits all fragments");

    auto messages = rawSink->messages();
    std::sort(
        messages.begin(),
        messages.end(),
        [](const MediaBuffer& left, const MediaBuffer& right) {
            const auto* leftBytes =
                static_cast<const std::uint8_t*>(left->data());
            const auto* rightBytes =
                static_cast<const std::uint8_t*>(right->data());
            return ((leftBytes[2] << 8) | leftBytes[3]) <
                   ((rightBytes[2] << 8) | rightBytes[3]);
        });
    require(messages.size() == 3,
            "MTU determines the fragment count");
    std::vector<std::uint8_t> reassembled;
    for (std::size_t index = 0; index < messages.size(); ++index) {
        const auto& message = messages[index];
        require(message && message->size() <= mtu,
                "each RTP datagram respects the MTU");
        const auto* bytes =
            static_cast<const std::uint8_t*>(message->data());
        require((bytes[1] & 0x7f) == payloadType,
                "fragments keep the configured payload type");
        require(((bytes[1] & 0x80) != 0) ==
                    (index + 1 == messages.size()),
                "marker is set only on the final fragment");
        reassembled.insert(
            reassembled.end(),
            bytes + 12,
            bytes + message->size());
    }
    require(reassembled == payload,
            "fragments preserve the encoded payload");

    sender.detach();
    rawReceiver.detach();
}

void testFailures() {
    auto failedSink = std::make_shared<MediaBufferCollector>();
    UdpTransport badAddress;
    badAddress.setSink(failedSink);
    AttributeMap invalidLocal;
    invalidLocal["localAddress"] = "999.999.999.999";
    require(!badAddress.configure(invalidLocal).ok,
            "UDP transport rejects an invalid local address");
    require(failedSink->contains(TransportState::Failed),
            "transport publishes failed configuration state");

    const std::vector<std::uint8_t> payload{1};
    RTPEngine noRemote;
    require(!noRemote.send(
                RtpParams{false},
                makePacket(1, 0, payload)).ok,
            "RTPEngine rejects a packet before configuration");

    auto configured = localOptions();
    addStreamOptions(configured, 1, 111, 48000);
    require(noRemote.configure(configured).ok,
            "RTPEngine configures without a remote");
    require(noRemote.attach({}).ok,
            "RTPEngine attaches for receive-only use");
    require(!noRemote.send(
                RtpParams{false},
                EncodedPacket{}).ok,
            "RTPEngine rejects an empty encoded packet");
    require(!noRemote.send(
                RtpParams{false},
                makePacket(1, 0, payload)).ok,
            "RTPEngine reports a missing UDP remote endpoint");

    Transport& rawTransport = noRemote;
    require(!rawTransport.send(makeMediaBuffer(payload)).ok,
            "RTPEngine does not expose raw transport sending");
    noRemote.detach();
}

} // namespace

int main() {
    static_assert(std::is_abstract_v<Transport>);
    static_assert(std::is_base_of_v<Transport, UdpTransport>);
    static_assert(std::is_base_of_v<Transport, RTPTransport>);
    static_assert(std::is_base_of_v<RTPTransport, RTPEngine>);
    static_assert(std::is_abstract_v<RTPTransport>);
    static_assert(!std::is_abstract_v<UdpTransport>);
    static_assert(!std::is_abstract_v<RTPEngine>);

    testUdpTransport();
    testRTPRoundTrip();
    testMtuFragmentation();
    testFailures();
    std::cout << "iris-media transport tests passed\n";
}
