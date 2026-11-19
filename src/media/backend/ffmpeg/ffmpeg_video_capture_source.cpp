#include "media/backend/ffmpeg/ffmpeg_video_capture_source.h"

#include "media/backend/ffmpeg/capture_sources.h"
#include "media/backend/ffmpeg/ffmpeg_capture_reader.h"
#include "media/backend/ffmpeg/null_video_image.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>
#include <utility>
#include <vector>

namespace iris::media {
namespace {

std::uint64_t nowMs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

} // namespace

struct FFmpegVideoCaptureSource::Impl {
    MediaResult configure(const MediaRuntimeConfig& runtime,
                          const MediaSessionConfig& session,
                          IVideoFrameSink* previewSink) {
        stop();
        width = std::max(160, runtime.videoWidth);
        height = std::max(90, runtime.videoHeight);
        fps = std::max(1, runtime.videoFps);
        callId = session.callId;
        preview = previewSink;
        auto resolution =
            resolveCaptureSource(MediaKind::Video, runtime.videoInput);
        if (!resolution.ok()) {
            return MediaResult::failure(resolution.error);
        }
        source = std::move(resolution.source);
        if (source.isNull) {
            nullImage = renderNullVideoImage(
                width, height, session.localUri);
        }
        av_log(
            nullptr, AV_LOG_INFO,
            "Iris video capture source: %s\n",
            source.isNull ? "empty (account URI card)" :
                            source.displayName.c_str());
        return MediaResult::success();
    }

    MediaResult start() {
        stop();
        if (!sink) {
            return MediaResult::failure(
                "video capture source has no frame sink");
        }
        nextPts = 0;
        target = av_frame_alloc();
        if (!target) {
            return MediaResult::failure(
                "failed to allocate video capture frame");
        }
        target->format = AV_PIX_FMT_YUV420P;
        target->width = width;
        target->height = height;
        if (av_frame_get_buffer(target, 32) < 0) {
            av_frame_free(&target);
            return MediaResult::failure(
                "failed to allocate video capture buffer");
        }
        running.store(true, std::memory_order_release);
        if (!source.isNull) {
            const auto opened = reader.open(
                source,
                CaptureReaderConfig{
                    MediaKind::Video,
                    width,
                    height,
                    fps,
                });
            if (!opened.ok) {
                running.store(false, std::memory_order_release);
                av_frame_free(&target);
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
        sws_freeContext(scaler);
        scaler = nullptr;
        sws_freeContext(previewScaler);
        previewScaler = nullptr;
        av_frame_free(&target);
    }

    void nullCaptureLoop() {
        const auto frameDuration =
            std::chrono::microseconds(1000000 / fps);
        auto nextFrame = std::chrono::steady_clock::now();
        while (running.load(std::memory_order_acquire)) {
            if (convertBgra(
                    nullImage.bgra.data(),
                    width * 4,
                    width,
                    height)) {
                emitFrame();
            }
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
        auto nextFrameTime = std::chrono::steady_clock::now();
        const auto frameInterval =
            std::chrono::microseconds(1000000 / fps);
        while (running.load(std::memory_order_acquire)) {
            const bool processed = reader.read(
                [this, &nextFrameTime, frameInterval](
                    const AVFrame& decoded) {
                    const auto now =
                        std::chrono::steady_clock::now();
                    if (now < nextFrameTime) {
                        return;
                    }
                    nextFrameTime = now + frameInterval;
                    emitPreview(decoded);
                    if (convert(decoded)) {
                        emitFrame();
                    }
                });
            if (!processed) {
                break;
            }
        }
        running.store(false, std::memory_order_release);
    }

    bool convert(const AVFrame& sourceFrame) {
        if (!target || av_frame_make_writable(target) < 0) {
            return false;
        }
        if (muted.load(std::memory_order_relaxed)) {
            fillMuted();
            return true;
        }
        scaler = sws_getCachedContext(
            scaler,
            sourceFrame.width,
            sourceFrame.height,
            static_cast<AVPixelFormat>(sourceFrame.format),
            width,
            height,
            AV_PIX_FMT_YUV420P,
            SWS_BILINEAR,
            nullptr,
            nullptr,
            nullptr);
        if (!scaler) {
            return false;
        }
        sws_scale(
            scaler,
            sourceFrame.data,
            sourceFrame.linesize,
            0,
            sourceFrame.height,
            target->data,
            target->linesize);
        return true;
    }

    bool convertBgra(const std::uint8_t* data,
                     int stride,
                     int sourceWidth,
                     int sourceHeight) {
        if (!target || !data || av_frame_make_writable(target) < 0) {
            return false;
        }
        if (muted.load(std::memory_order_relaxed)) {
            fillMuted();
            return true;
        }
        scaler = sws_getCachedContext(
            scaler,
            sourceWidth,
            sourceHeight,
            AV_PIX_FMT_BGRA,
            width,
            height,
            AV_PIX_FMT_YUV420P,
            SWS_BILINEAR,
            nullptr,
            nullptr,
            nullptr);
        if (!scaler) {
            return false;
        }
        const std::uint8_t* sourceData[] = {data};
        const int sourceStride[] = {stride};
        sws_scale(
            scaler,
            sourceData,
            sourceStride,
            0,
            sourceHeight,
            target->data,
            target->linesize);
        return true;
    }

    void fillMuted() {
        std::memset(
            target->data[0], 16,
            static_cast<std::size_t>(target->linesize[0]) * height);
        std::memset(
            target->data[1], 128,
            static_cast<std::size_t>(target->linesize[1]) *
                (height / 2));
        std::memset(
            target->data[2], 128,
            static_cast<std::size_t>(target->linesize[2]) *
                (height / 2));
    }

    void emitFrame() {
        if (!sink) {
            return;
        }
        sink(pipeline::VideoFrameView{
            {
                target->data[0],
                target->data[1],
                target->data[2],
            },
            {
                target->linesize[0],
                target->linesize[1],
                target->linesize[2],
            },
            width,
            height,
            nextPts++,
        });
    }

    void emitPreview(const AVFrame& sourceFrame) {
        if (!preview || !preview->active()) {
            return;
        }
        previewScaler = sws_getCachedContext(
            previewScaler,
            sourceFrame.width,
            sourceFrame.height,
            static_cast<AVPixelFormat>(sourceFrame.format),
            sourceFrame.width,
            sourceFrame.height,
            AV_PIX_FMT_BGRA,
            SWS_BILINEAR,
            nullptr,
            nullptr,
            nullptr);
        if (!previewScaler) {
            return;
        }
        const auto byteCount = av_image_get_buffer_size(
            AV_PIX_FMT_BGRA,
            sourceFrame.width,
            sourceFrame.height,
            1);
        if (byteCount <= 0) {
            return;
        }
        std::vector<std::uint8_t> bgra(
            static_cast<std::size_t>(byteCount));
        std::uint8_t* destination[] = {bgra.data()};
        const int lines[] = {sourceFrame.width * 4};
        sws_scale(
            previewScaler,
            sourceFrame.data,
            sourceFrame.linesize,
            0,
            sourceFrame.height,
            destination,
            lines);
        preview->onVideoFrame(VideoFrameView{
            callId,
            VideoFrameKind::LocalPreview,
            bgra.data(),
            bgra.size(),
            sourceFrame.width,
            sourceFrame.height,
            sourceFrame.width * 4,
            nowMs(),
        });
    }

    CaptureSourceSpec source;
    FfmpegCaptureReader reader;
    NullVideoImage nullImage;
    FrameSink sink;
    IVideoFrameSink* preview = nullptr;
    SwsContext* scaler = nullptr;
    SwsContext* previewScaler = nullptr;
    AVFrame* target = nullptr;
    CallId callId;
    int width = 640;
    int height = 360;
    int fps = 15;
    std::int64_t nextPts = 0;
    std::thread thread;
    std::atomic<bool> running{false};
    std::atomic<bool> muted{false};
};

FFmpegVideoCaptureSource::FFmpegVideoCaptureSource()
    : impl_(std::make_unique<Impl>()) {
}

FFmpegVideoCaptureSource::~FFmpegVideoCaptureSource() {
    stop();
}

MediaResult FFmpegVideoCaptureSource::configure(
    const MediaRuntimeConfig& runtime,
    const MediaSessionConfig& session,
    IVideoFrameSink* previewSink) {
    return impl_->configure(runtime, session, previewSink);
}

MediaResult FFmpegVideoCaptureSource::start() {
    return impl_->start();
}

void FFmpegVideoCaptureSource::stop() noexcept {
    impl_->stop();
}

bool FFmpegVideoCaptureSource::running() const noexcept {
    return impl_->running.load(std::memory_order_acquire);
}

void FFmpegVideoCaptureSource::setFrameSink(FrameSink sink) {
    impl_->sink = std::move(sink);
}

void FFmpegVideoCaptureSource::setMuted(bool muted) {
    impl_->muted.store(muted, std::memory_order_relaxed);
}

} // namespace iris::media
