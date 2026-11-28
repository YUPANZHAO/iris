#include "pipeline/pipeline.h"

#include <algorithm>
#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <memory>
#include <utility>

namespace {

using namespace iris::media;
using namespace iris::media::codec;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

class PacketCollector final : public IEncodedPacketSink {
public:
    void onEncodedPacket(const EncodedPacket& packet) override {
        ++count;
        kind = packet.kind;
        lastPacket = packet;
    }

    int count = 0;
    MediaKind kind = MediaKind::Unknown;
    EncodedPacket lastPacket;
};

class MockRtpTransport final : public transport::RTPTransport {
public:
    Status send(
        const transport::RtpParams&,
        const codec::EncodedPacket&) override {
        return {};
    }

    void setSink(
        std::weak_ptr<transport::RTPTransportSink> sink) noexcept override {
        sink_ = std::move(sink);
    }

    Status configure(const AttributeMap&) override {
        return {};
    }

    Status attach(const AttributeMap&) override {
        return {};
    }

    void detach() noexcept override {
    }

    void receive(
        const codec::EncodedPacket& packet,
        bool marker = false) {
        const auto sink = sink_.lock();
        require(static_cast<bool>(sink), "RTP stream is connected");
        sink->onRtpRecv(transport::RtpParams{marker}, packet);
    }

    bool hasSink() const noexcept {
        return !sink_.expired();
    }

private:
    std::weak_ptr<transport::RTPTransportSink> sink_;
};

class MockAudioCapture final : public capture::AudioCaptureSource {
public:
    Status configure(const capture::AudioCaptureConfig&) override {
        return {};
    }

    Status start() override {
        running_ = true;
        ++startCount;
        return {};
    }

    void stop() noexcept override {
        if (running_) {
            ++stopCount;
        }
        running_ = false;
    }

    bool running() const noexcept override {
        return running_;
    }

    std::string lastError() const override {
        return {};
    }

    void setFrameSink(std::weak_ptr<capture::AudioFrameSink> sink) noexcept
        override {
        sink_ = std::move(sink);
    }

    capture::AudioFormat outputFormat() const noexcept override {
        return {};
    }

    void emit() {
        const auto sink = sink_.lock();
        require(static_cast<bool>(sink), "audio capture sink is connected");
        sink->onAudioFrame(capture::AudioFrame{});
    }

    bool hasSink() const noexcept {
        return !sink_.expired();
    }

    int startCount = 0;
    int stopCount = 0;

private:
    bool running_ = false;
    std::weak_ptr<capture::AudioFrameSink> sink_;
};

class MockVideoCapture final : public capture::VideoCaptureSource {
public:
    Status configure(const capture::VideoCaptureConfig&) override {
        return {};
    }

    Status start() override {
        running_ = true;
        ++startCount;
        return {};
    }

    void stop() noexcept override {
        if (running_) {
            ++stopCount;
        }
        running_ = false;
    }

    bool running() const noexcept override {
        return running_;
    }

    std::string lastError() const override {
        return {};
    }

    void setFrameSink(std::weak_ptr<capture::VideoFrameSink> sink) noexcept
        override {
        sink_ = std::move(sink);
    }

    capture::VideoFormat outputFormat() const noexcept override {
        return {};
    }

    void emit() {
        const auto sink = sink_.lock();
        require(static_cast<bool>(sink), "video capture sink is connected");
        sink->onVideoFrame(capture::VideoFrame{});
    }

    bool hasSink() const noexcept {
        return !sink_.expired();
    }

    int startCount = 0;
    int stopCount = 0;

private:
    bool running_ = false;
    std::weak_ptr<capture::VideoFrameSink> sink_;
};

class MockAudioEncoder final : public AudioEncoder {
public:
    Status configure(const AudioEncoderConfig&) override {
        return {};
    }

    Status encode(const AudioRawFrame&) override {
        ++encodeCount;
        const auto sink = sink_.lock();
        require(static_cast<bool>(sink), "audio encoder sink is connected");
        EncodedPacket packet;
        packet.kind = MediaKind::Audio;
        sink->onEncodedPacket(packet);
        return {};
    }

    Status flush() override {
        ++flushCount;
        return {};
    }

    void setEncodedPacketSink(
        std::weak_ptr<IEncodedPacketSink> sink) noexcept override {
        sink_ = std::move(sink);
    }

    void reset() noexcept override {
    }

    bool hasSink() const noexcept {
        return !sink_.expired();
    }

    int encodeCount = 0;
    int flushCount = 0;

private:
    std::weak_ptr<IEncodedPacketSink> sink_;
};

class MockVideoEncoder final : public VideoEncoder {
public:
    Status configure(const VideoEncoderConfig&) override {
        return {};
    }

    Status encode(const VideoRawFrame&) override {
        ++encodeCount;
        const auto sink = sink_.lock();
        require(static_cast<bool>(sink), "video encoder sink is connected");
        EncodedPacket packet;
        packet.kind = MediaKind::Video;
        sink->onEncodedPacket(packet);
        return {};
    }

    Status flush() override {
        ++flushCount;
        return {};
    }

    void setEncodedPacketSink(
        std::weak_ptr<IEncodedPacketSink> sink) noexcept override {
        sink_ = std::move(sink);
    }

    void reset() noexcept override {
    }

    bool hasSink() const noexcept {
        return !sink_.expired();
    }

    int encodeCount = 0;
    int flushCount = 0;

private:
    std::weak_ptr<IEncodedPacketSink> sink_;
};

void testAudioPipeline() {
    auto stream = std::make_shared<pipeline::CaptureAudioStream>();
    auto capture = std::make_unique<MockAudioCapture>();
    auto encoder = std::make_unique<MockAudioEncoder>();
    auto* capturePointer = capture.get();
    auto* encoderPointer = encoder.get();
    auto collector = std::make_shared<PacketCollector>();

    require(stream->setCapture(std::move(capture)).ok,
            "audio capture source is accepted");
    require(stream->setEncoder(std::move(encoder)).ok,
            "audio encoder is accepted");
    stream->setEncodedPacketSink(collector);
    require(stream->open().ok, "audio stream opens");
    require(capturePointer->startCount == 1,
            "opening starts the audio capture source");

    capturePointer->emit();
    require(encoderPointer->encodeCount == 1,
            "audio frames are passed to the encoder");
    require(collector->count == 1 &&
                collector->kind == MediaKind::Audio,
            "encoded audio packets reach the stream sink");

    stream->close();
    require(capturePointer->stopCount == 1,
            "closing stops the audio capture source");
    require(encoderPointer->flushCount == 1,
            "closing flushes the audio encoder");
    require(!capturePointer->hasSink() && !encoderPointer->hasSink(),
            "closing disconnects the audio components");
}

void testVideoPipeline() {
    auto stream = std::make_shared<pipeline::CaptureVideoStream>();
    auto capture = std::make_unique<MockVideoCapture>();
    auto encoder = std::make_unique<MockVideoEncoder>();
    auto* capturePointer = capture.get();
    auto* encoderPointer = encoder.get();
    auto collector = std::make_shared<PacketCollector>();

    require(stream->setCapture(std::move(capture)).ok,
            "video capture source is accepted");
    require(stream->setEncoder(std::move(encoder)).ok,
            "video encoder is accepted");
    stream->setEncodedPacketSink(collector);
    require(stream->open().ok, "video stream opens");
    require(capturePointer->startCount == 1,
            "opening starts the video capture source");

    capturePointer->emit();
    require(encoderPointer->encodeCount == 1,
            "video frames are passed to the encoder");
    require(collector->count == 1 &&
                collector->kind == MediaKind::Video,
            "encoded video packets reach the stream sink");

    stream->close();
    require(capturePointer->stopCount == 1,
            "closing stops the video capture source");
    require(encoderPointer->flushCount == 1,
            "closing flushes the video encoder");
    require(!capturePointer->hasSink() && !encoderPointer->hasSink(),
            "closing disconnects the video components");
}

void testRtpStreams() {
    const std::uint8_t payload[] = {0x12, 0x34, 0x56};
    const std::uint8_t videoPayload[] = {0x65, 0x34, 0x56};

    auto audio = std::make_shared<pipeline::RtpMediaStream>(
        MediaKind::Audio);
    auto audioTransport = std::make_shared<MockRtpTransport>();
    auto audioCollector = std::make_shared<PacketCollector>();
    require(audio->setRTPTransport(audioTransport).ok,
            "RTP audio transport is accepted");
    audio->setEncodedPacketSink(audioCollector);
    require(audio->open().ok, "RTP audio stream opens");

    EncodedPacket audioPacket;
    audioPacket.streamId = 11;
    audioPacket.codecType = CodecType::Opus;
    audioPacket.data = std::make_shared<ByteBuffer>(
        payload, sizeof(payload));
    audioTransport->receive(audioPacket);
    require(audioCollector->count == 1 &&
                audioCollector->kind == MediaKind::Audio,
            "RTP audio stream publishes audio packets");
    require(audioCollector->lastPacket.streamId == 11 &&
                audioCollector->lastPacket.codecType == CodecType::Opus &&
                audioCollector->lastPacket.data &&
                audioCollector->lastPacket.data->size() == sizeof(payload),
            "RTP audio stream preserves the processed payload");

    audio->close();
    require(!audioTransport->hasSink(),
            "closing disconnects the RTP audio stream");

    auto video = std::make_shared<pipeline::RtpMediaStream>(
        MediaKind::Video);
    auto videoTransport = std::make_shared<MockRtpTransport>();
    auto videoCollector = std::make_shared<PacketCollector>();
    require(video->setRTPTransport(videoTransport).ok,
            "RTP video transport is accepted");
    video->setEncodedPacketSink(videoCollector);
    require(video->open().ok, "RTP video stream opens");

    EncodedPacket videoPacket;
    videoPacket.streamId = 22;
    videoPacket.codecType = CodecType::H264;
    videoPacket.data = std::make_shared<ByteBuffer>(
        videoPayload, sizeof(videoPayload));
    videoTransport->receive(videoPacket, true);
    require(videoCollector->count == 1 &&
                videoCollector->kind == MediaKind::Video,
            "RTP video stream publishes video packets");
    require(videoCollector->lastPacket.streamId == 22 &&
                videoCollector->lastPacket.codecType == CodecType::H264 &&
                videoCollector->lastPacket.data &&
                videoCollector->lastPacket.data->size() == 7,
            "RTP video stream preserves the processed payload");

    video->close();
    require(!videoTransport->hasSink(),
            "closing disconnects the RTP video stream");
}

void testH264ParameterSetsSurviveFuA() {
    auto stream = std::make_shared<pipeline::RtpMediaStream>(
        MediaKind::Video);
    auto transport = std::make_shared<MockRtpTransport>();
    auto collector = std::make_shared<PacketCollector>();
    require(stream->setRTPTransport(transport).ok,
            "H.264 RTP transport is accepted");
    stream->setEncodedPacketSink(collector);
    require(stream->open().ok, "H.264 RTP stream opens");

    const auto receive = [&](std::initializer_list<std::uint8_t> bytes,
                             bool marker = false) {
        EncodedPacket packet;
        packet.streamId = 22;
        packet.codecType = CodecType::H264;
        packet.data = std::make_shared<ByteBuffer>(bytes.begin(), bytes.size());
        transport->receive(packet, marker);
    };

    receive({0x67, 0xaa});
    receive({0x68, 0xbb});
    receive({0x7c, 0x85, 0x11});
    receive({0x7c, 0x05, 0x22});
    receive({0x7c, 0x45, 0x33}, true);

    require(collector->count == 1,
            "one H.264 access unit is emitted after the marker");
    require(collector->lastPacket.key,
            "a fragmented IDR access unit remains a key frame");
    const std::uint8_t expected[] = {
        0, 0, 0, 1, 0x67, 0xaa,
        0, 0, 0, 1, 0x68, 0xbb,
        0, 0, 0, 1, 0x65, 0x11, 0x22, 0x33,
    };
    const auto* actual = static_cast<const std::uint8_t*>(
        collector->lastPacket.data->data());
    require(
        collector->lastPacket.data->size() == sizeof(expected) &&
            std::equal(
                actual,
                actual + sizeof(expected),
                std::begin(expected)),
        "SPS, PPS and fragmented IDR share one access unit");

    stream->close();
}

} // namespace

int main() {
    testAudioPipeline();
    testVideoPipeline();
    testRtpStreams();
    testH264ParameterSetsSurviveFuA();
    std::cout << "pipeline tests passed\n";
    return EXIT_SUCCESS;
}
