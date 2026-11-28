#include "control/control.h"
#include "pipeline/capture_stream.h"
#include "pipeline/rtp_media_stream.h"

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace iris::media;
using namespace iris::media::control;
using namespace iris::media::sdp;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

class TrackingStream final : public pipeline::Stream {
public:
    Status open() override {
        ++openCount;
        return {};
    }

    void close() noexcept override {
        ++closeCount;
    }

    int openCount = 0;
    int closeCount = 0;
};

class TrackingSink final : public MediaControlSink {
public:
    Status onStreamCreate(MediaStream& mediaStream) override {
        ++createCount;
        mids.push_back(mediaStream.mid);
        auto stream = std::make_shared<TrackingStream>();
        streams.push_back(stream);
        mediaStream.stream = std::move(stream);
        return {};
    }

    int createCount = 0;
    std::vector<std::string> mids;
    std::vector<std::shared_ptr<TrackingStream>> streams;
};

class PacketCollector final : public codec::IEncodedPacketSink {
public:
    void onEncodedPacket(const codec::EncodedPacket& packet) override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            packets_.push_back(packet);
        }
        condition_.notify_all();
    }

    bool waitForPacket() {
        std::unique_lock<std::mutex> lock(mutex_);
        return condition_.wait_for(
            lock, std::chrono::seconds(2), [&] {
                return !packets_.empty();
            });
    }

private:
    std::mutex mutex_;
    std::condition_variable condition_;
    std::vector<codec::EncodedPacket> packets_;
};

class ReceiveSink final : public MediaControlSink {
public:
    explicit ReceiveSink(std::shared_ptr<PacketCollector> collector)
        : collector_(std::move(collector)) {
    }

    Status onStreamCreate(MediaStream& mediaStream) override {
        if (mediaStream.receiveStream) {
            mediaStream.receiveStream->setEncodedPacketSink(collector_);
        }
        return {};
    }

private:
    std::shared_ptr<PacketCollector> collector_;
};

SdpCodecPreference opusCodec() {
    SdpCodecPreference codec;
    codec.codecType = codec::CodecType::Opus;
    codec.encodingName = "opus";
    codec.payloadType = 111;
    codec.attributes["clockRate"] = "48000";
    codec.attributes["channels"] = "2";
    return codec;
}

SdpCodecPreference h264Codec() {
    SdpCodecPreference codec;
    codec.codecType = codec::CodecType::H264;
    codec.encodingName = "H264";
    codec.payloadType = 96;
    codec.attributes["clockRate"] = "90000";
    codec.fmtp = "packetization-mode=1";
    return codec;
}

SdpNegotiationInput localInput(
    const std::string& sessionId,
    const std::string& address,
    std::uint16_t audioPort,
    std::uint16_t videoPort) {
    SdpNegotiationInput input;
    input.origin.username = "iris";
    input.origin.sessionId =
        static_cast<std::uint64_t>(std::stoull(sessionId));
    input.origin.sessionVersion = 1;
    input.origin.unicastAddress = address;
    input.connection.address = address;

    SdpMediaCapability audio;
    audio.kind = MediaKind::Audio;
    audio.mid = "audio";
    audio.direction = SdpDirection::SendRecv;
    audio.transport.dataPort = audioPort;
    audio.attributes["bitrateKbps"] = "64";
    audio.codecs.push_back(opusCodec());
    input.media.push_back(std::move(audio));

    SdpMediaCapability video;
    video.kind = MediaKind::Video;
    video.mid = "video";
    video.direction = SdpDirection::SendRecv;
    video.transport.dataPort = videoPort;
    video.attributes["resolution"] = "640x360";
    video.codecs.push_back(h264Codec());
    input.media.push_back(std::move(video));
    return input;
}

void testRtpMediaTransportBinding() {
    auto receiving = localInput(
        "11", "127.0.0.1", 5100, 5102);
    auto sending = localInput(
        "12", "127.0.0.1", 6100, 6102);
    for (auto& media : receiving.media) {
        media.direction = SdpDirection::RecvOnly;
    }
    for (auto& media : sending.media) {
        media.direction = SdpDirection::SendOnly;
    }

    SdpMediaControl receiver(std::move(receiving));
    SdpMediaControl sender(std::move(sending));
    auto packets = std::make_shared<PacketCollector>();
    auto receiveSink = std::make_shared<ReceiveSink>(packets);
    receiver.setMediaControlSink(receiveSink);
    auto offer = receiver.createOffer();
    require(offer.status.ok, offer.status.message.c_str());
    require(receiver.setLocalDescription(offer.sdp).ok,
            "recv-only local description is stored");
    require(sender.setRemoteDescription(offer.sdp).ok,
            "send-only peer stores the recv-only offer");
    auto answer = sender.createAnswer();
    require(answer.status.ok, answer.status.message.c_str());
    require(sender.setLocalDescription(answer.sdp).ok,
            "send-only answer is stored");
    require(receiver.setRemoteDescription(answer.sdp).ok,
            "recv-only peer applies the remote answer");

    const auto streams = receiver.streams();
    require(streams.size() == 2,
            "recv-only negotiation creates RTP media streams");
    require(std::dynamic_pointer_cast<pipeline::RtpMediaStream>(
                streams[0].stream) != nullptr &&
                std::dynamic_pointer_cast<pipeline::RtpMediaStream>(
                    streams[1].stream) != nullptr,
            "recv-only media use RtpMediaStream");
    require(streams[0].receiveStream == streams[0].stream &&
                streams[1].receiveStream == streams[1].stream &&
                streams[0].rtpEngine &&
                streams[1].rtpEngine,
            "each receive stream owns an RTP engine");

    const auto start = receiver.startAll();
    require(start.ok,
            "each RtpMediaStream is bound to an RTPEngine and opens");
    const auto senderStart = sender.startAll();
    require(senderStart.ok,
            "the send-only empty capture pipeline starts");
    require(packets->waitForPacket(),
            "RTP reception reaches the remote RtpMediaStream");
    sender.stopAll();
    receiver.stopAll();
}

void testDefaultCapturePipeline() {
    SdpMediaControl caller(localInput(
        "21", "127.0.0.1", 5500, 5502));
    SdpMediaControl callee(localInput(
        "22", "127.0.0.1", 6500, 6502));
    auto offer = caller.createOffer();
    require(offer.status.ok, offer.status.message.c_str());
    require(caller.setLocalDescription(offer.sdp).ok,
            "default capture caller stores its offer");
    require(callee.setRemoteDescription(offer.sdp).ok,
            "default capture callee stores the offer");
    auto answer = callee.createAnswer();
    require(answer.status.ok, answer.status.message.c_str());
    require(callee.setLocalDescription(answer.sdp).ok,
            "default capture callee stores its answer");
    require(caller.setRemoteDescription(answer.sdp).ok,
            "default capture caller applies the answer");

    const auto streams = caller.streams();
    require(streams.size() == 2 &&
                std::dynamic_pointer_cast<
                    pipeline::CaptureAudioStream>(
                    streams[0].stream) != nullptr &&
                std::dynamic_pointer_cast<
                    pipeline::CaptureVideoStream>(
                    streams[1].stream) != nullptr,
            "send media use CaptureStream with empty sources");

    const auto start = caller.startAll();
    require(start.ok, start.message.c_str());
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    caller.stopAll();
    require(!caller.started(),
            "the default capture pipeline starts and stops cleanly");
}

} // namespace

int main() {
    SdpMediaControl caller(localInput(
        "1", "127.0.0.1", 5000, 5002));
    SdpMediaControl callee(localInput(
        "2", "192.0.2.20", 6000, 6002));
    auto callerSink = std::make_shared<TrackingSink>();
    auto calleeSink = std::make_shared<TrackingSink>();
    caller.setMediaControlSink(callerSink);
    callee.setMediaControlSink(calleeSink);

    auto offer = caller.createOffer();
    require(offer.status.ok, offer.status.message.c_str());
    require(offer.sdp.find("m=audio 5000") != std::string::npos &&
                offer.sdp.find("m=video 5002") != std::string::npos,
            "createOffer emits the configured media");
    require(caller.setLocalDescription(offer.sdp).ok,
            "setLocalDescription stores the offer");

    require(callee.setRemoteDescription(offer.sdp).ok,
            "setRemoteDescription stores the remote offer");
    auto answer = callee.createAnswer();
    require(answer.status.ok, answer.status.message.c_str());
    require(answer.sdp.find("m=audio 6000") != std::string::npos &&
                answer.sdp.find("m=video 6002") != std::string::npos,
            "createAnswer emits the negotiated answer");
    require(callee.setLocalDescription(answer.sdp).ok,
            "setLocalDescription stores the answer");
    require(calleeSink->createCount == 2,
            "answer negotiation notifies the callee sink");

    require(caller.setRemoteDescription(answer.sdp).ok,
            "setRemoteDescription applies the remote answer");
    require(callerSink->createCount == 2,
            "answer application notifies the caller sink");

    const auto streams = caller.streams();
    require(streams.size() == 2,
            "negotiated streams are managed by the control object");
    require(streams[0].mid == "audio" &&
                streams[0].attributes.at("codecType") == "opus" &&
                streams[0].attributes.at("payloadType") == "111" &&
                streams[0].attributes.at("direction") == "sendrecv",
            "audio stream exposes negotiated attributes");
    require(streams[1].mid == "video" &&
                streams[1].attributes.at("codecType") == "h264" &&
                streams[1].attributes.at("payloadType") == "96",
            "video stream exposes negotiated attributes");
    require(streams[0].receiveStream &&
                streams[1].receiveStream &&
                streams[0].rtpEngine &&
                streams[1].rtpEngine &&
                streams[0].encodedPacketSink &&
                streams[1].encodedPacketSink,
            "sendrecv media contain receive, RTP engine, and send routing");
    require(caller.stream(streams[0].id) == callerSink->streams[0],
            "the sink can replace the stream selected for sending");

    const auto start = caller.startAll();
    require(start.ok, start.message.c_str());
    require(caller.started(), "the control reports its started state");
    require(callerSink->streams[0]->openCount == 1 &&
                callerSink->streams[1]->openCount == 1,
            "startAll opens every managed stream");
    caller.stopAll();
    require(callerSink->streams[0]->closeCount == 1 &&
                callerSink->streams[1]->closeCount == 1,
            "stopAll closes every managed stream");

    testRtpMediaTransportBinding();
    testDefaultCapturePipeline();

    std::cout << "iris-media control tests passed\n";
    return EXIT_SUCCESS;
}
