#include "capture_source/capture.h"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>
#include <type_traits>
#include <vector>

namespace {

using namespace iris::media::capture;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

template <typename Source>
bool waitUntilStopped(const Source& source) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (source.running() &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return !source.running();
}

template <typename Collector>
bool waitForFrames(const Collector& collector,
                   std::size_t count) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (collector.size() < count &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return collector.size() >= count;
}

class AudioCollector final : public AudioFrameSink {
public:
    void onAudioFrame(const AudioFrame& frame) override {
        std::lock_guard<std::mutex> lock(mutex);
        frames.push_back(frame);
    }

    std::size_t size() const {
        std::lock_guard<std::mutex> lock(mutex);
        return frames.size();
    }

    AudioFrame front() const {
        std::lock_guard<std::mutex> lock(mutex);
        return frames.front();
    }

private:
    mutable std::mutex mutex;
    std::vector<AudioFrame> frames;
};

class VideoCollector final : public VideoFrameSink {
public:
    void onVideoFrame(const VideoFrame& frame) override {
        std::lock_guard<std::mutex> lock(mutex);
        frames.push_back(frame);
    }

    std::size_t size() const {
        std::lock_guard<std::mutex> lock(mutex);
        return frames.size();
    }

    VideoFrame front() const {
        std::lock_guard<std::mutex> lock(mutex);
        return frames.front();
    }

private:
    mutable std::mutex mutex;
    std::vector<VideoFrame> frames;
};

void testConfigurationFailures() {
    auto video = std::make_unique<FFmpegVideoCaptureSource>();
    require(!video->start().ok,
            "an unconfigured video source rejects start");

    VideoCaptureConfig videoConfig;
    require(!video->configure(videoConfig).ok,
            "an empty video input URL is rejected");

    auto audio = std::make_unique<FFmpegAudioCaptureSource>();
    require(!audio->start().ok,
            "an unconfigured audio source rejects start");

    AudioCaptureConfig audioConfig;
    require(!audio->configure(audioConfig).ok,
            "an empty audio input URL is rejected");
}

void testVideoCapture() {
    auto source = std::make_shared<FFmpegVideoCaptureSource>();
    auto collector = std::make_shared<VideoCollector>();
    source->setFrameSink(collector);

    VideoCaptureConfig config;
    config.backend = "lavfi";
    config.name =
        "testsrc2=size=160x90:rate=30:duration=0.25";
    config.outputFormat.width = 80;
    config.outputFormat.height = 48;
    config.outputFormat.frameRate = iris::media::codec::Rational{30, 1};
    config.outputFormat.pixelFormat =
        iris::media::codec::VideoPixelFormat::BGRA;

    const auto configured = source->configure(config);
    require(configured.ok, configured.message.c_str());
    const auto started = source->start();
    require(started.ok, started.message.c_str());
    require(waitUntilStopped(*source),
            "the finite video input stops at end of stream");
    source->stop();
    require(source->lastError().empty(), source->lastError().c_str());
    require(collector->size() != 0,
            "the video source emits decoded frames");

    const auto frame = collector->front();
    require(frame.format.width == 80 && frame.format.height == 48,
            "the video source converts frame dimensions");
    require(frame.format.pixelFormat ==
                iris::media::codec::VideoPixelFormat::BGRA,
            "the video source converts the pixel format");
    require(frame.pixels && frame.pixels->size() == 80 * 48 * 4,
            "the video source emits a packed BGRA buffer");
}

void testAudioCapture() {
    auto source = std::make_shared<FFmpegAudioCaptureSource>();
    auto collector = std::make_shared<AudioCollector>();
    source->setFrameSink(collector);

    AudioCaptureConfig config;
    config.backend = "lavfi";
    config.name =
        "sine=frequency=440:sample_rate=48000:duration=0.2";
    config.outputFormat.sampleRate = 16000;
    config.outputFormat.channels = 1;
    config.outputFormat.sampleFormat =
        iris::media::codec::AudioSampleFormat::Signed16;

    const auto configured = source->configure(config);
    require(configured.ok, configured.message.c_str());
    const auto started = source->start();
    require(started.ok, started.message.c_str());
    require(waitUntilStopped(*source),
            "the finite audio input stops at end of stream");
    source->stop();
    require(source->lastError().empty(), source->lastError().c_str());
    require(collector->size() != 0,
            "the audio source emits decoded frames");

    const auto frame = collector->front();
    require(frame.format.sampleRate == 16000,
            "the audio source resamples the output");
    require(frame.format.channels == 1,
            "the audio source converts the channel count");
    require(frame.format.sampleFormat ==
                iris::media::codec::AudioSampleFormat::Signed16,
            "the audio source converts the sample format");
    require(frame.samples && frame.samples->size() != 0 &&
                frame.samples->size() % sizeof(std::int16_t) == 0,
            "the audio source emits a signed-16 sample buffer");
}

void testStop() {
    auto source = std::make_shared<FFmpegVideoCaptureSource>();
    auto collector = std::make_shared<VideoCollector>();
    source->setFrameSink(collector);

    VideoCaptureConfig config;
    config.backend = "lavfi";
    config.name = "testsrc2=size=32x32:rate=5";
    config.outputFormat.width = 32;
    config.outputFormat.height = 32;
    config.outputFormat.pixelFormat =
        iris::media::codec::VideoPixelFormat::I420;

    const auto configured = source->configure(config);
    require(configured.ok, configured.message.c_str());
    const auto started = source->start();
    require(started.ok, started.message.c_str());
    source->stop();
    require(!source->running(),
            "stop terminates an unbounded video source");
}

void testEmptyAudioCapture() {
    auto source = std::make_shared<EmptyAudioCaptureSource>("silence");
    auto collector = std::make_shared<AudioCollector>();
    source->setFrameSink(collector);

    AudioCaptureConfig config;
    config.outputFormat.sampleRate = 8000;
    config.outputFormat.channels = 1;
    config.outputFormat.sampleFormat =
        iris::media::codec::AudioSampleFormat::Signed16;
    require(source->configure(config).ok,
            "empty audio source configures");
    require(source->start().ok, "empty audio source starts");
    require(waitForFrames(*collector, 1),
            "empty audio source emits a frame");
    source->stop();

    const auto frame = collector->front();
    require(frame.format.sampleRate == 8000 &&
                frame.format.channels == 1,
            "empty audio source preserves the requested format");
    bool silent = frame.samples && frame.samples->size() > 0;
    if (silent) {
        const auto* samples = static_cast<const std::uint8_t*>(
            frame.samples->data());
        for (std::size_t index = 0;
             index < frame.samples->size();
             ++index) {
            silent = silent && samples[index] == 0;
        }
    }
    require(silent, "empty audio source emits silence");
}

void testEmptyVideoCapture() {
    auto source =
        std::make_shared<EmptyVideoCaptureSource>("Iris 100");
    auto collector = std::make_shared<VideoCollector>();
    source->setFrameSink(collector);

    VideoCaptureConfig config;
    config.outputFormat.width = 160;
    config.outputFormat.height = 90;
    config.outputFormat.frameRate =
        iris::media::codec::Rational{10, 1};
    config.outputFormat.pixelFormat =
        iris::media::codec::VideoPixelFormat::BGRA;
    require(source->configure(config).ok,
            "empty video source configures");
    require(source->start().ok, "empty video source starts");
    require(waitForFrames(*collector, 1),
            "empty video source emits a frame");
    source->stop();

    const auto frame = collector->front();
    require(frame.pixels &&
                frame.pixels->size() == 160 * 90 * 4,
            "empty video source emits a BGRA frame");
    const auto* pixels = static_cast<const std::uint8_t*>(
        frame.pixels->data());
    std::size_t whitePixels = 0;
    std::size_t darkPixels = 0;
    for (std::size_t offset = 0;
         offset < frame.pixels->size();
         offset += 4) {
        if (pixels[offset] >= 240 &&
            pixels[offset + 1] >= 240 &&
            pixels[offset + 2] >= 240) {
            ++whitePixels;
        }
        if (pixels[offset] < 64 &&
            pixels[offset + 1] < 64 &&
            pixels[offset + 2] < 64) {
            ++darkPixels;
        }
    }
    require(whitePixels > 0 && darkPixels > 0,
            "empty video source draws white text on a dark canvas");
}

} // namespace

int main() {
    static_assert(std::is_same_v<Status, iris::media::Status>);
    static_assert(std::is_same_v<
                  AttributeMap, iris::media::AttributeMap>);
    static_assert(std::is_abstract_v<CaptureSource>);
    static_assert(std::is_abstract_v<AudioCaptureSource>);
    static_assert(std::is_abstract_v<VideoCaptureSource>);
    static_assert(!std::is_abstract_v<FFmpegAudioCaptureSource>);
    static_assert(!std::is_abstract_v<FFmpegVideoCaptureSource>);
    static_assert(!std::is_abstract_v<EmptyAudioCaptureSource>);
    static_assert(!std::is_abstract_v<EmptyVideoCaptureSource>);
    static_assert(std::is_base_of_v<
                  AudioCaptureSource, FFmpegAudioCaptureSource>);
    static_assert(std::is_base_of_v<
                  VideoCaptureSource, FFmpegVideoCaptureSource>);

    testConfigurationFailures();
    testVideoCapture();
    testAudioCapture();
    testStop();
    testEmptyAudioCapture();
    testEmptyVideoCapture();
    std::cout << "iris-media capture source tests passed\n";
    return EXIT_SUCCESS;
}
