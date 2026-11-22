#include "media/codec/ffmpeg_codec_factory.h"
#include "media/codec/ffmpeg_h264_codec.h"
#include "media/codec/ffmpeg_opus_codec.h"
#include "media/control/media_control.h"
#include "media/control/media_control_impl.h"
#include "media/control/sdp_media_control.h"
#include "media/engine/media_buffer.h"
#include "media/sdp/sdp_components.h"
#include "media/codec/codec_types.h"
#include "media/sdp/sdp_types.h"
#include "media/transport/rtp_transport.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

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

namespace {

bool sendUdpDatagram(const std::string& endpoint,
                     const std::vector<std::uint8_t>& payload) {
    const auto separator = endpoint.rfind(':');
    if (separator == std::string::npos || payload.empty()) {
        return false;
    }
    auto host = endpoint.substr(0, separator);
    if (host.size() >= 2 && host.front() == '[' && host.back() == ']') {
        host = host.substr(1, host.size() - 2);
    }
    const auto port = std::stoul(endpoint.substr(separator + 1));
    if (port == 0 || port > 65535) {
        return false;
    }
    sockaddr_in destination{};
    destination.sin_family = AF_INET;
    destination.sin_port = htons(static_cast<std::uint16_t>(port));
    if (inet_pton(
            AF_INET, host.c_str(), &destination.sin_addr) != 1) {
        return false;
    }
#ifdef _WIN32
    const auto socketValue = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socketValue == INVALID_SOCKET) {
        return false;
    }
    const auto sent = ::sendto(
        socketValue,
        reinterpret_cast<const char*>(payload.data()),
        static_cast<int>(payload.size()), 0,
        reinterpret_cast<const sockaddr*>(&destination),
        sizeof(destination));
    closesocket(socketValue);
#else
    const auto socketValue = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socketValue < 0) {
        return false;
    }
    const auto sent = ::sendto(
        socketValue, payload.data(), payload.size(), 0,
        reinterpret_cast<const sockaddr*>(&destination),
        sizeof(destination));
    ::close(socketValue);
#endif
    return sent == static_cast<decltype(sent)>(payload.size());
}

class PacketSink final : public iris::media::v2::IEncodedPacketSink {
public:
    void onEncodedPacket(
        const iris::media::v2::EncodedPacket& packet) override {
        std::lock_guard<std::mutex> lock(mutex_);
        packets_.push_back(packet);
        condition_.notify_all();
    }

    bool wait(std::size_t count,
              int milliseconds = 2000) {
        std::unique_lock<std::mutex> lock(mutex_);
        return condition_.wait_for(
            lock, std::chrono::milliseconds(milliseconds),
            [this, count] { return packets_.size() >= count; });
    }

    std::vector<iris::media::v2::EncodedPacket> packets() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return packets_;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::vector<iris::media::v2::EncodedPacket> packets_;
};

class AudioSink final : public iris::media::v2::IAudioFrameSink {
public:
    void onAudioFrame(const iris::media::v2::AudioFrame& frame) override {
        frames += 1;
        size = iris::media::v2::bufferViewSize(frame.samples);
    }

    std::atomic<int> frames{0};
    std::atomic<std::size_t> size{0};
};

class VideoSink final : public iris::media::v2::IVideoFrameSink {
public:
    void onVideoFrame(const iris::media::v2::VideoFrame& frame) override {
        frames += 1;
        size = iris::media::v2::bufferViewSize(frame.pixels);
    }

    std::atomic<int> frames{0};
    std::atomic<std::size_t> size{0};
};

bool testCodec() {
    class EncodedSink final
        : public iris::media::v2::IEncodedPacketSink {
    public:
        void onEncodedPacket(
            const iris::media::v2::EncodedPacket& packetValue) override {
            packet = packetValue;
        }
        iris::media::v2::EncodedPacket packet;
    } encodedSink;

    const auto encodedSinkHolder =
        std::shared_ptr<EncodedSink>(&encodedSink, [](EncodedSink*) {});
    iris::media::v2::FFmpegOpusAudioEncoder encoder;
    encoder.setSink(encodedSinkHolder);
    iris::media::v2::AudioEncoderConfig encoderConfig;
    encoderConfig.codecType = iris::media::v2::CodecType::Opus;
    encoderConfig.inputFormat.sampleRate = 48000;
    encoderConfig.inputFormat.channels = 2;
    encoderConfig.inputFormat.sampleFormat =
        iris::media::v2::AudioSampleFormat::Signed16;
    encoderConfig.bitrateKbps = 64;
    if (!encoder.configure(encoderConfig).ok) {
        return false;
    }

    constexpr std::size_t samples = 960;
    constexpr std::size_t channels = 2;
    std::vector<std::int16_t> pcm(samples * channels, 0);
    auto buffer = iris::media::v2::copyToMediaBuffer(
        reinterpret_cast<const std::uint8_t*>(pcm.data()),
        pcm.size() * sizeof(std::int16_t));
    iris::media::v2::AudioFrame frame;
    frame.format = encoderConfig.inputFormat;
    frame.samples = buffer;
    if (!encoder.encode(frame).ok || !encodedSink.packet.data.buffer) {
        return false;
    }

    AudioSink decoded;
    const auto decodedHolder =
        std::shared_ptr<AudioSink>(&decoded, [](AudioSink*) {});
    iris::media::v2::FFmpegOpusAudioDecoder decoder;
    decoder.setSink(decodedHolder);
    iris::media::v2::AudioDecoderConfig decoderConfig;
    decoderConfig.codecType = iris::media::v2::CodecType::Opus;
    decoderConfig.outputFormat = encoderConfig.inputFormat;
    if (!decoder.configure(decoderConfig).ok ||
        !decoder.decode(encodedSink.packet).ok) {
        return false;
    }
    (void)encoder.flush();
    return decoded.frames.load() > 0 && decoded.size.load() > 0;
}

bool testRtpTransport() {
    {
        iris::media::v2::RTPTransport unconfigured;
        if (unconfigured.open(iris::media::v2::AttributeMap{}).ok) {
            return false;
        }
    }

    iris::media::v2::RTPTransport receiver;
    iris::media::v2::AttributeMap receiverConfig;
    receiverConfig["port"] = "0";
    receiverConfig["stream.1.payloadType"] = "97";
    receiverConfig["stream.1.clockRate"] = "90000";
    receiverConfig["stream.2.payloadType"] = "112";
    receiverConfig["stream.2.clockRate"] = "48000";
    if (!receiver.configure(receiverConfig).ok ||
        receiver.info().state !=
            iris::media::v2::TransportState::Configured) {
        return false;
    }
    const auto receiverEndpoint = receiver.info().endpoint;
    if (!receiver.open(iris::media::v2::AttributeMap{}).ok) {
        return false;
    }

    iris::media::v2::RTPTransport sender;
    iris::media::v2::AttributeMap senderConfig;
    senderConfig["port"] = "0";
    if (!sender.configure(senderConfig).ok) {
        return false;
    }
    iris::media::v2::AttributeMap senderOpenConfig;
    senderOpenConfig["remoteEndpoint"] = receiverEndpoint;
    senderOpenConfig["stream.1.payloadType"] = "97";
    senderOpenConfig["stream.1.clockRate"] = "90000";
    senderOpenConfig["stream.2.payloadType"] = "112";
    senderOpenConfig["stream.2.clockRate"] = "48000";
    if (!sender.open(senderOpenConfig).ok ||
        sender.info().state != iris::media::v2::TransportState::Open) {
        return false;
    }

    PacketSink sink;
    const auto sinkHolder =
        std::shared_ptr<PacketSink>(&sink, [](PacketSink*) {});
    receiver.setSink(sinkHolder);

    // The bytes deliberately resemble an Annex-B H.264 access unit. RTP
    // transport must forward them unchanged without inspecting the payload.
    std::vector<std::uint8_t> opaque(
        3000, 0x5a);
    opaque[0] = 0;
    opaque[1] = 0;
    opaque[2] = 0;
    opaque[3] = 1;
    opaque[4] = 0x65;
    iris::media::v2::EncodedPacket first;
    first.streamId = 1;
    first.kind = iris::media::v2::MediaKind::Video;
    first.codecType = iris::media::v2::CodecType::H264;
    first.key = true;
    first.data = iris::media::v2::copyToMediaBuffer(
        opaque.data(), opaque.size());
    first.ptsMicros = 20000;
    if (!first.data.buffer) {
        std::cerr << "packet buffer allocation failed\n";
        return false;
    }
    const auto sent = sender.send(first);
    if (!sent.ok) {
        std::cerr << "RTP send failed: " << sent.message << '\n';
        return false;
    }
    std::vector<std::uint8_t> audio(160, 0x33);
    iris::media::v2::EncodedPacket second;
    second.streamId = 2;
    second.kind = iris::media::v2::MediaKind::Audio;
    second.codecType = iris::media::v2::CodecType::Opus;
    second.key = false;
    second.data = iris::media::v2::copyToMediaBuffer(
        audio.data(), audio.size());
    second.ptsMicros = 20000;
    if (!second.data.buffer) {
        return false;
    }
    if (!sender.send(second).ok || !sink.wait(2)) {
        const auto senderStats = sender.stats();
        const auto receiverStats = receiver.stats();
        std::cerr << "RTP receive timeout"
                  << " sender=" << sender.info().endpoint
                  << " receiver=" << receiver.info().endpoint
                  << " sent=" << senderStats.messagesSent
                  << " received=" << receiverStats.messagesReceived
                  << " receiveErrors=" << receiverStats.receiveErrors
                  << '\n';
        return false;
    }
    const auto received = sink.packets();
    const auto sawFirst = std::find_if(
        received.begin(), received.end(),
        [](const auto& value) { return value.streamId == 1; });
    const auto sawSecond = std::find_if(
        received.begin(), received.end(),
        [](const auto& value) { return value.streamId == 2; });
    auto sameBytes = [](const iris::media::v2::EncodedPacket& value,
                        const std::vector<std::uint8_t>& expected) {
        const auto size = iris::media::v2::bufferViewSize(value.data);
        return size == expected.size() &&
            std::memcmp(
                iris::media::v2::bufferViewData(value.data),
                expected.data(), expected.size()) == 0;
    };
    const auto senderStats = sender.stats();
    const auto receiverStats = receiver.stats();
    bool valid = received.size() == 2 &&
        sawFirst != received.end() &&
        sawSecond != received.end() &&
        sawFirst->kind == iris::media::v2::MediaKind::Unknown &&
        sawFirst->codecType == iris::media::v2::CodecType::Unknown &&
        sawFirst->key == false &&
        sawFirst->ptsMicros == first.ptsMicros &&
        sameBytes(*sawFirst, opaque) &&
        sawSecond->kind == iris::media::v2::MediaKind::Unknown &&
        sawSecond->codecType == iris::media::v2::CodecType::Unknown &&
        sawSecond->ptsMicros == second.ptsMicros &&
        sameBytes(*sawSecond, audio) &&
        senderStats.messagesSent == 2 &&
        senderStats.rtpPacketsSent == 2 &&
        receiverStats.messagesReceived == 2 &&
        receiverStats.rtpPacketsReceived == 2 &&
        receiverStats.rtcpPacketsReceived == 0;
    if (!valid) {
        std::cerr << "unexpected RTP result count=" << received.size();
        for (const auto& value : received) {
            std::cerr << " stream=" << value.streamId
                      << " bytes="
                      << iris::media::v2::bufferViewSize(value.data);
        }
        std::cerr << '\n';
    }
    if (!valid) {
        return false;
    }

    // A receiver report is managed by RTPTransport and must never be emitted
    // as an EncodedPacket.
    const std::vector<std::uint8_t> receiverReport = {
        0x80, 201, 0, 1, 0x12, 0x34, 0x56, 0x78,
    };
    if (!sendUdpDatagram(receiverEndpoint, receiverReport)) {
        std::cerr << "failed to send RTCP receiver report\n";
        return false;
    }
    for (int retry = 0; retry < 100; ++retry) {
        if (receiver.stats().rtcpPacketsReceived != 0) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    valid = receiver.stats().rtcpPacketsReceived == 1 &&
        sink.packets().size() == 2;
    if (!valid) {
        std::cerr << "RTCP was not managed internally\n";
    }
    return valid;
}

bool testVideoCodec() {
    class EncodedSink final
        : public iris::media::v2::IEncodedPacketSink {
    public:
        void onEncodedPacket(
            const iris::media::v2::EncodedPacket& value) override {
            packet = value;
        }
        iris::media::v2::EncodedPacket packet;
    } encodedSink;
    const auto encodedHolder =
        std::shared_ptr<EncodedSink>(&encodedSink, [](EncodedSink*) {});

    iris::media::v2::FFmpegH264VideoEncoder encoder;
    encoder.setSink(encodedHolder);
    iris::media::v2::VideoEncoderConfig config;
    config.codecType = iris::media::v2::CodecType::H264;
    config.inputFormat.width = 320;
    config.inputFormat.height = 180;
    config.inputFormat.frameRate = {15, 1};
    config.inputFormat.pixelFormat = iris::media::v2::VideoPixelFormat::I420;
    config.bitrateKbps = 300;
    if (!encoder.configure(config).ok) {
        return false;
    }
    std::vector<std::uint8_t> i420(
        static_cast<std::size_t>(320) * 180 * 3 / 2, 128);
    std::fill(i420.begin(), i420.begin() + 320 * 180, 16);
    iris::media::v2::VideoFrame frame;
    frame.format = config.inputFormat;
    frame.pixels = iris::media::v2::copyToMediaBuffer(
        i420.data(), i420.size());
    if (!frame.pixels.buffer ||
        !encoder.encode(frame).ok) {
        return false;
    }
    (void)encoder.flush();
    if (!encodedSink.packet.data.buffer) {
        return false;
    }

    VideoSink decoded;
    const auto decodedHolder =
        std::shared_ptr<VideoSink>(&decoded, [](VideoSink*) {});
    iris::media::v2::FFmpegH264VideoDecoder decoder;
    decoder.setSink(decodedHolder);
    iris::media::v2::VideoDecoderConfig decoderConfig;
    decoderConfig.codecType = iris::media::v2::CodecType::H264;
    decoderConfig.outputFormat = iris::media::v2::VideoPixelFormat::I420;
    if (!decoder.configure(decoderConfig).ok ||
        !decoder.decode(encodedSink.packet).ok) {
        return false;
    }
    return decoded.frames.load() > 0 && decoded.size.load() > 0;
}

bool testSdp() {
    iris::media::v2::SdpNegotiationInput input;
    input.sessionId = "1234";
    input.origin.sessionId = "1234";
    input.origin.unicastAddress = "127.0.0.1";
    input.connection.address = "127.0.0.1";

    iris::media::v2::SdpMediaCapability audio;
    audio.kind = iris::media::v2::MediaKind::Audio;
    audio.mid = "audio";
    audio.transport.dataPort = 4000;
    iris::media::v2::SdpCodecPreference opus;
    opus.codecType = iris::media::v2::CodecType::Opus;
    opus.encodingName = "opus";
    opus.payloadType = 111;
    opus.attributes["clockRate"] = "48000";
    opus.attributes["channels"] = "2";
    audio.codecs.push_back(opus);
    input.media.push_back(audio);

    iris::media::v2::SdpNegotiator negotiator;
    auto negotiated = negotiator.createOffer(input);
    if (!negotiated.status.ok) {
        std::cerr << "offer negotiation failed: "
                  << negotiated.status.message << '\n';
        return false;
    }
    iris::media::v2::SdpBuilder builder;
    std::string text;
    if (!builder.build(negotiated.description, &text).ok) {
        std::cerr << "SDP build failed\n";
        return false;
    }
    iris::media::v2::SdpParser parser;
    iris::media::v2::SdpSessionDescription parsed;
    const auto parsedStatus = parser.parse(text, &parsed);
    if (!parsedStatus.ok) {
        std::cerr << "SDP parse failed: " << parsedStatus.message << '\n';
        return false;
    }
    return
        parsed.media.size() == 1 &&
        parsed.media.front().kind ==
            iris::media::v2::MediaKind::Audio;
}

bool testControl() {
    iris::media::v2::MediaControl control;
    iris::media::v2::MediaControlConfig config;
    config.options["rtp.port"] = "0";
    if (!control.initialize(config).ok) {
        return false;
    }
    iris::media::v2::StreamConfig streamConfigValue;
    streamConfigValue.id = 7;
    streamConfigValue.kind = iris::media::v2::MediaKind::Audio;
    streamConfigValue.name = "audio";
    streamConfigValue.attributes["codecType"] = "opus";
    streamConfigValue.attributes["sampleRate"] = "48000";
    streamConfigValue.attributes["channels"] = "2";
    streamConfigValue.attributes["payloadType"] = "111";
    iris::media::v2::EncodedPacketStreamPtr stream;
    if (!control.createStream(streamConfigValue, &stream).ok || !stream) {
        return false;
    }
    iris::media::v2::StreamConfig videoConfigValue;
    videoConfigValue.id = 8;
    videoConfigValue.kind = iris::media::v2::MediaKind::Video;
    videoConfigValue.name = "video";
    videoConfigValue.attributes["codecType"] = "h264";
    videoConfigValue.attributes["payloadType"] = "96";
    videoConfigValue.attributes["clockRate"] = "90000";
    videoConfigValue.attributes["resolution"] = "320x180";
    iris::media::v2::EncodedPacketStreamPtr videoStream;
    if (!control.createStream(videoConfigValue, &videoStream).ok ||
        !videoStream) {
        return false;
    }
    const auto audioEndpoint = control.localEndpoint(7);
    const auto videoEndpoint = control.localEndpoint(8);
    if (audioEndpoint.empty() || videoEndpoint.empty() ||
        audioEndpoint == videoEndpoint ||
        control.streams().size() != 2 ||
        control.stream(7) != stream ||
        control.stream(8) != videoStream ||
        !control.startAll().ok) {
        return false;
    }
    if (!control.destroyStream(7) || control.stream(7) ||
        !control.destroyStream(8) || control.stream(8)) {
        return false;
    }
    control.stopAll();

    iris::media::v2::SDPMediaControl sdpControl;
    if (!sdpControl.initialize(config).ok) {
        return false;
    }
    iris::media::v2::SdpOfferOptions options;
    options.local.sessionId = "9876";
    options.local.origin.sessionId = "9876";
    options.local.origin.unicastAddress = "127.0.0.1";
    options.local.connection.address = "127.0.0.1";
    iris::media::v2::SdpMediaCapability audio;
    audio.kind = iris::media::v2::MediaKind::Audio;
    audio.mid = "audio";
    audio.transport.dataPort = 5004;
    iris::media::v2::SdpCodecPreference opus;
    opus.codecType = iris::media::v2::CodecType::Opus;
    opus.encodingName = "opus";
    opus.payloadType = 111;
    opus.attributes["clockRate"] = "48000";
    opus.attributes["channels"] = "2";
    audio.codecs.push_back(opus);
    options.local.media.push_back(audio);
    std::string offer;
    if (!sdpControl.createOffer(options, &offer).ok ||
        offer.find("m=audio") == std::string::npos ||
        sdpControl.streams().empty()) {
        return false;
    }
    return true;
}

} // namespace

int main() {
    if (!testCodec()) {
        std::cerr << "media v2 codec test failed\n";
        return 1;
    }
    if (!testRtpTransport()) {
        std::cerr << "media v2 RTP transport test failed\n";
        return 1;
    }
    if (!testVideoCodec()) {
        std::cerr << "media v2 video codec test failed\n";
        return 1;
    }
    if (!testSdp()) {
        std::cerr << "media v2 SDP test failed\n";
        return 1;
    }
    if (!testControl()) {
        std::cerr << "media v2 control test failed\n";
        return 1;
    }
    std::cout << "media v2 tests passed\n";
    return 0;
}
