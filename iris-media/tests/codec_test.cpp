#include "codec/codec.h"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using namespace iris::media::codec;

class PacketCollector final : public IEncodedPacketSink {
public:
    void onEncodedPacket(const EncodedPacket& packet) override {
        packets.push_back(packet);
    }

    std::vector<EncodedPacket> packets;
};

class AudioFrameCollector final : public IAudioFrameSink {
public:
    void onAudioFrame(const AudioRawFrame& frame) override {
        frames.push_back(frame);
    }

    std::vector<AudioRawFrame> frames;
};

class VideoFrameCollector final : public IVideoFrameSink {
public:
    void onVideoFrame(const VideoRawFrame& frame) override {
        frames.push_back(frame);
    }

    std::vector<VideoRawFrame> frames;
};

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

AudioRawFrame audioFrame(std::int64_t ptsMicros,
                         int sampleRate,
                         int channels) {
    const auto byteCount =
        static_cast<std::size_t>(sampleRate / 50) *
        static_cast<std::size_t>(channels) * sizeof(std::int16_t);
    std::vector<std::uint8_t> samples(byteCount, 0);

    AudioRawFrame frame;
    frame.format.sampleRate = sampleRate;
    frame.format.channels = channels;
    frame.format.sampleFormat = AudioSampleFormat::Signed16;
    frame.samples = std::make_shared<iris::media::ByteBuffer>(
        samples.data(), samples.size());
    frame.ptsMicros = ptsMicros;
    return frame;
}

VideoRawFrame videoFrame(std::int64_t ptsMicros,
                         int width,
                         int height) {
    const auto lumaSize =
        static_cast<std::size_t>(width) * height;
    std::vector<std::uint8_t> pixels(
        lumaSize + lumaSize / 2, 0);
    std::memset(
        pixels.data() + lumaSize, 128, lumaSize / 2);

    VideoRawFrame frame;
    frame.format.width = width;
    frame.format.height = height;
    frame.format.frameRate = Rational{30, 1};
    frame.format.pixelFormat = VideoPixelFormat::I420;
    frame.pixels = std::make_shared<iris::media::ByteBuffer>(
        pixels.data(), pixels.size());
    frame.ptsMicros = ptsMicros;
    frame.key = true;
    return frame;
}

void testOpusRoundTrip() {
    auto encoder = std::make_shared<FFmpegOpusEncoder>();
    auto decoder = std::make_shared<FFmpegOpusDecoder>();
    auto packets = std::make_shared<PacketCollector>();
    auto frames = std::make_shared<AudioFrameCollector>();

    AudioEncoderConfig encoderConfig;
    encoderConfig.codecType = CodecType::Opus;
    encoderConfig.inputFormat.sampleRate = 48000;
    encoderConfig.inputFormat.channels = 1;
    encoderConfig.inputFormat.sampleFormat = AudioSampleFormat::Signed16;
    encoderConfig.bitrateKbps = 32;
    encoder->setEncodedPacketSink(packets);
    require(encoder->configure(encoderConfig).ok,
            "Opus encoder configures");
    require(encoder->encode(audioFrame(1000, 48000, 1)).ok,
            "Opus encoder accepts a signed-16 frame");
    require(encoder->flush().ok, "Opus encoder flushes");
    require(!packets->packets.empty(), "Opus encoder emits packets");

    AudioDecoderConfig decoderConfig;
    decoderConfig.codecType = CodecType::Opus;
    decoderConfig.outputFormat.sampleRate = 48000;
    decoderConfig.outputFormat.channels = 1;
    decoderConfig.outputFormat.sampleFormat = AudioSampleFormat::Signed16;
    decoder->setRawFrameSink(frames);
    require(decoder->configure(decoderConfig).ok,
            "Opus decoder configures");
    require(decoder->decode(packets->packets.front()).ok,
            "Opus decoder accepts an encoded packet");
    require(decoder->flush().ok, "Opus decoder flushes");
    require(!frames->frames.empty(), "Opus decoder emits raw frames");
    require(frames->frames.front().samples &&
                frames->frames.front().samples->size() != 0,
            "Opus decoder emits non-empty samples");
}

void testH264RoundTrip() {
    auto encoder = std::make_shared<FFmpegH264Encoder>();
    auto decoder = std::make_shared<FFmpegH264Decoder>();
    auto packets = std::make_shared<PacketCollector>();
    auto frames = std::make_shared<VideoFrameCollector>();

    VideoEncoderConfig encoderConfig;
    encoderConfig.codecType = CodecType::H264;
    encoderConfig.inputFormat.width = 16;
    encoderConfig.inputFormat.height = 16;
    encoderConfig.inputFormat.frameRate = Rational{30, 1};
    encoderConfig.inputFormat.pixelFormat = VideoPixelFormat::I420;
    encoderConfig.bitrateKbps = 300;
    encoder->setEncodedPacketSink(packets);
    require(encoder->configure(encoderConfig).ok,
            "H.264 encoder configures");
    require(encoder->encode(videoFrame(1000, 16, 16)).ok,
            "H.264 encoder accepts an I420 frame");
    require(encoder->flush().ok, "H.264 encoder flushes");
    require(!packets->packets.empty(), "H.264 encoder emits packets");
    require(packets->packets.front().key,
            "H.264 first packet is a key frame");

    VideoDecoderConfig decoderConfig;
    decoderConfig.codecType = CodecType::H264;
    decoderConfig.outputFormat = VideoPixelFormat::BGRA;
    decoder->setRawFrameSink(frames);
    require(decoder->configure(decoderConfig).ok,
            "H.264 decoder configures");
    require(decoder->decode(packets->packets.front()).ok,
            "H.264 decoder accepts an encoded packet");
    require(!frames->frames.empty(), "H.264 decoder emits raw frames");
    require(
        frames->frames.front().format.pixelFormat ==
            VideoPixelFormat::BGRA,
        "H.264 decoder honors the output pixel format");
    require(frames->frames.front().pixels &&
                frames->frames.front().pixels->size() != 0,
            "H.264 decoder emits non-empty pixels");
}

void testFactory() {
    FFmpegCodecFactory factory;
    auto h264 = factory.createVideoEncoder(CodecType::H264);
    auto opus = factory.createAudioEncoder(CodecType::Opus);
    auto h264Decoder = factory.createVideoDecoder(CodecType::H264);
    auto opusDecoder = factory.createAudioDecoder(CodecType::Opus);

    require(dynamic_cast<FFmpegH264Encoder*>(h264.get()) != nullptr,
            "factory creates the H.264 encoder");
    require(dynamic_cast<FFmpegOpusEncoder*>(opus.get()) != nullptr,
            "factory creates the Opus encoder");
    require(dynamic_cast<FFmpegH264Decoder*>(h264Decoder.get()) != nullptr,
            "factory creates the H.264 decoder");
    require(dynamic_cast<FFmpegOpusDecoder*>(opusDecoder.get()) != nullptr,
            "factory creates the Opus decoder");
}

void testStatusFailure() {
    const auto defaultFailure = Status::failure("default failure");
    require(!defaultFailure.ok, "failure status is not successful");
    require(defaultFailure.code == -1,
            "failure status uses the default error code");
    require(defaultFailure.message == "default failure",
            "failure status preserves the message");

    const auto customFailure = Status::failure("custom failure", 42);
    require(customFailure.code == 42,
            "failure status accepts a custom error code");
}

} // namespace

int main() {
    static_assert(std::is_abstract_v<AudioEncoder>);
    static_assert(std::is_abstract_v<VideoEncoder>);
    static_assert(std::is_abstract_v<AudioDecoder>);
    static_assert(std::is_abstract_v<VideoDecoder>);
    static_assert(std::is_same_v<
                  MediaBuffer,
                  std::shared_ptr<iris::media::ByteBuffer>>);
    static_assert(std::is_same_v<
                  iris::media::MediaBuffer, MediaBuffer>);
    static_assert(std::is_same_v<
                  iris::media::Status, Status>);
    static_assert(!std::is_abstract_v<FFmpegH264Encoder>);
    static_assert(!std::is_abstract_v<FFmpegOpusEncoder>);
    static_assert(!std::is_abstract_v<FFmpegH264Decoder>);
    static_assert(!std::is_abstract_v<FFmpegOpusDecoder>);

    testFactory();
    testStatusFailure();
    testOpusRoundTrip();
    testH264RoundTrip();
    std::cout << "iris-media codec tests passed\n";
    return EXIT_SUCCESS;
}
