#include "capture_source/capture.h"

#include <chrono>
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
    static_assert(std::is_base_of_v<
                  AudioCaptureSource, FFmpegAudioCaptureSource>);
    static_assert(std::is_base_of_v<
                  VideoCaptureSource, FFmpegVideoCaptureSource>);

    testConfigurationFailures();
    testVideoCapture();
    testAudioCapture();
    testStop();
    std::cout << "iris-media capture source tests passed\n";
    return EXIT_SUCCESS;
}
