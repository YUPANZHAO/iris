#include "media/backend/ffmpeg/ffmpeg_audio_capture_source.h"

#include "media/backend/ffmpeg/capture_sources.h"
#include "media/backend/ffmpeg/ffmpeg_capture_reader.h"

extern "C" {
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <utility>
#include <vector>

namespace iris::media {
namespace {

constexpr int kSampleRate = 48000;
constexpr int kChannels = 2;
constexpr int kFrameSamples = 960;

} // namespace

struct FFmpegAudioCaptureSource::Impl {
    MediaResult configure(const MediaRuntimeConfig& runtime,
                          const MediaSessionConfig&) {
        stop();
        auto resolution =
            resolveCaptureSource(MediaKind::Audio, runtime.audioInput);
        if (!resolution.ok()) {
            return MediaResult::failure(resolution.error);
        }
        source = std::move(resolution.source);
        av_log(
            nullptr, AV_LOG_INFO,
            "Iris audio capture source: %s\n",
            source.isNull ? "empty (silence)" :
                            source.displayName.c_str());
        return MediaResult::success();
    }

    MediaResult start() {
        stop();
        if (!sink) {
            return MediaResult::failure(
                "audio capture source has no frame sink");
        }
        nextPts = 0;
        pcm.clear();
        running.store(true, std::memory_order_release);
        if (!source.isNull) {
            const auto opened = reader.open(
                source, CaptureReaderConfig{MediaKind::Audio});
            if (!opened.ok) {
                running.store(false, std::memory_order_release);
                return opened;
            }
        }
        thread = std::thread([this] {
            if (source.isNull) {
                nullCaptureLoop();
            } else {
                captureLoop();
            }
        });
        return MediaResult::success();
    }

    void stop() noexcept {
        running.store(false, std::memory_order_release);
        reader.abortInput();
        if (thread.joinable() &&
            thread.get_id() != std::this_thread::get_id()) {
            thread.join();
        }
        reader.close();
        swr_free(&resampler);
        pcm.clear();
    }

    void nullCaptureLoop() {
        constexpr auto frameDuration =
            std::chrono::microseconds(
                kFrameSamples * 1000000LL / kSampleRate);
        const auto frameValues = static_cast<std::size_t>(
            kFrameSamples) * kChannels;
        auto nextFrame = std::chrono::steady_clock::now();
        while (running.load(std::memory_order_acquire)) {
            pcm.assign(frameValues, 0);
            emitFrame();
            pcm.clear();
            nextFrame += frameDuration;
            const auto now = std::chrono::steady_clock::now();
            if (nextFrame > now) {
                std::this_thread::sleep_until(nextFrame);
            } else {
                nextFrame = now + frameDuration;
            }
        }
        running.store(false, std::memory_order_release);
    }

    void captureLoop() {
        while (running.load(std::memory_order_acquire) &&
               reader.read([this](const AVFrame& frame) {
                   append(frame);
               })) {
            if (source.format == "lavfi") {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(5));
            }
        }
        running.store(false, std::memory_order_release);
    }

    bool ensureResampler(const AVFrame& frame) {
        if (resampler) {
            return true;
        }
        AVChannelLayout outputLayout;
        av_channel_layout_default(&outputLayout, kChannels);
        AVChannelLayout inputLayout;
        if (frame.ch_layout.nb_channels > 0) {
            av_channel_layout_copy(&inputLayout, &frame.ch_layout);
        } else {
            av_channel_layout_default(&inputLayout, kChannels);
        }
        const int status = swr_alloc_set_opts2(
            &resampler,
            &outputLayout, AV_SAMPLE_FMT_S16, kSampleRate,
            &inputLayout,
            static_cast<AVSampleFormat>(frame.format),
            frame.sample_rate, 0, nullptr);
        av_channel_layout_uninit(&inputLayout);
        av_channel_layout_uninit(&outputLayout);
        return status >= 0 && swr_init(resampler) >= 0;
    }

    void append(const AVFrame& frame) {
        if (!ensureResampler(frame)) {
            return;
        }
        const auto outputCapacity = av_rescale_rnd(
            swr_get_delay(resampler, frame.sample_rate) +
                frame.nb_samples,
            kSampleRate, frame.sample_rate, AV_ROUND_UP);
        std::vector<std::int16_t> converted(
            static_cast<std::size_t>(outputCapacity) * kChannels);
        std::uint8_t* output[] = {
            reinterpret_cast<std::uint8_t*>(converted.data()),
        };
        const auto input =
            const_cast<const std::uint8_t**>(frame.extended_data);
        const auto samples = swr_convert(
            resampler, output, outputCapacity,
            input, frame.nb_samples);
        if (samples <= 0) {
            return;
        }
        converted.resize(
            static_cast<std::size_t>(samples) * kChannels);
        pcm.insert(pcm.end(), converted.begin(), converted.end());
        const auto frameValues =
            static_cast<std::size_t>(kFrameSamples) * kChannels;
        while (pcm.size() >= frameValues) {
            emitFrame();
            pcm.erase(pcm.begin(), pcm.begin() + frameValues);
        }
    }

    void emitFrame() {
        const auto frameValues =
            static_cast<std::size_t>(kFrameSamples) * kChannels;
        if (pcm.size() < frameValues) {
            return;
        }
        if (muted.load(std::memory_order_relaxed)) {
            std::fill(pcm.begin(), pcm.begin() + frameValues, 0);
        }
        if (sink) {
            sink(pipeline::AudioFrameView{
                reinterpret_cast<const std::uint8_t*>(pcm.data()),
                frameValues * sizeof(std::int16_t),
                kSampleRate,
                kChannels,
                kFrameSamples,
                nextPts,
                pipeline::AudioSampleFormat::Signed16,
            });
        }
        nextPts += kFrameSamples;
    }

    CaptureSourceSpec source;
    FfmpegCaptureReader reader;
    SwrContext* resampler = nullptr;
    std::vector<std::int16_t> pcm;
    FrameSink sink;
    std::thread thread;
    std::atomic<bool> running{false};
    std::atomic<bool> muted{false};
    std::int64_t nextPts = 0;
};

FFmpegAudioCaptureSource::FFmpegAudioCaptureSource()
    : impl_(std::make_unique<Impl>()) {
}

FFmpegAudioCaptureSource::~FFmpegAudioCaptureSource() {
    stop();
}

MediaResult FFmpegAudioCaptureSource::configure(
    const MediaRuntimeConfig& runtime,
    const MediaSessionConfig& session) {
    return impl_->configure(runtime, session);
}

MediaResult FFmpegAudioCaptureSource::start() {
    return impl_->start();
}

void FFmpegAudioCaptureSource::stop() noexcept {
    impl_->stop();
}

bool FFmpegAudioCaptureSource::running() const noexcept {
    return impl_->running.load(std::memory_order_acquire);
}

void FFmpegAudioCaptureSource::setFrameSink(FrameSink sink) {
    impl_->sink = std::move(sink);
}

void FFmpegAudioCaptureSource::setMuted(bool muted) {
    impl_->muted.store(muted, std::memory_order_relaxed);
}

} // namespace iris::media
