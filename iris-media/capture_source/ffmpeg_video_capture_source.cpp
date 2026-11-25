#include "capture_source/ffmpeg_video_capture_source.h"

#include "capture_source/ffmpeg_capture_utils.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace iris::media::capture {
namespace {

std::string ffmpegError(int error) {
    char message[AV_ERROR_MAX_STRING_SIZE]{};
    if (av_strerror(error, message, sizeof(message)) < 0) {
        return "unknown FFmpeg error";
    }
    return message;
}

AVPixelFormat pixelFormat(VideoPixelFormat format) {
    switch (format) {
    case VideoPixelFormat::I420:
        return AV_PIX_FMT_YUV420P;
    case VideoPixelFormat::NV12:
        return AV_PIX_FMT_NV12;
    case VideoPixelFormat::BGRA:
        return AV_PIX_FMT_BGRA;
    }
    return AV_PIX_FMT_NONE;
}

Rational frameRate(AVRational value) {
    if (value.num <= 0 || value.den <= 0) {
        return {};
    }
    return Rational{value.num, value.den};
}

std::int64_t timestampMicros(std::int64_t timestamp,
                             AVRational timeBase) {
    return av_rescale_q(timestamp, timeBase, AVRational{1, 1000000});
}

std::size_t planeSize(int width, int height) {
    if (width <= 0 || height <= 0) {
        return 0;
    }
    return static_cast<std::size_t>(width) *
           static_cast<std::size_t>(height);
}

std::size_t frameByteCount(const VideoFormat& format) {
    const auto luma = planeSize(format.width, format.height);
    switch (format.pixelFormat) {
    case VideoPixelFormat::I420:
    case VideoPixelFormat::NV12:
        return luma +
               planeSize((format.width + 1) / 2,
                         (format.height + 1) / 2) *
                   2;
    case VideoPixelFormat::BGRA:
        return luma * 4;
    }
    return 0;
}

bool copyPlane(std::uint8_t* destination,
               std::size_t offset,
               const std::uint8_t* source,
               int sourceLineSize,
               int rowBytes,
               int rows) {
    if (!destination || !source || sourceLineSize == 0 ||
        rowBytes < 0 || rows < 0) {
        return false;
    }
    const auto stride = static_cast<std::size_t>(
        sourceLineSize < 0 ? -sourceLineSize : sourceLineSize);
    for (int row = 0; row < rows; ++row) {
        const auto sourceRow = sourceLineSize < 0
            ? source + (rows - 1 - row) * stride
            : source + row * stride;
        std::memcpy(destination + offset +
                        static_cast<std::size_t>(row) *
                            static_cast<std::size_t>(rowBytes),
                    sourceRow,
                    static_cast<std::size_t>(rowBytes));
    }
    return true;
}

bool copyFrame(const AVFrame& source,
               const VideoFormat& format,
               ByteBuffer& destination) {
    auto* target =
        static_cast<std::uint8_t*>(destination.data());
    if (!target || destination.size() == 0 ||
        source.width != format.width ||
        source.height != format.height) {
        return false;
    }

    const auto width = format.width;
    const auto height = format.height;
    switch (format.pixelFormat) {
    case VideoPixelFormat::I420: {
        const auto luma = planeSize(width, height);
        const auto chromaWidth = (width + 1) / 2;
        const auto chromaHeight = (height + 1) / 2;
        if (!copyPlane(target, 0, source.data[0],
                       source.linesize[0], width, height)) {
            return false;
        }
        if (!copyPlane(target, luma, source.data[1],
                       source.linesize[1], chromaWidth,
                       chromaHeight)) {
            return false;
        }
        return copyPlane(
            target, luma + planeSize(chromaWidth, chromaHeight),
            source.data[2], source.linesize[2], chromaWidth,
            chromaHeight);
    }
    case VideoPixelFormat::NV12: {
        const auto luma = planeSize(width, height);
        if (!copyPlane(target, 0, source.data[0],
                       source.linesize[0], width, height)) {
            return false;
        }
        return copyPlane(target, luma, source.data[1],
                         source.linesize[1], width,
                         (height + 1) / 2);
    }
    case VideoPixelFormat::BGRA:
        return copyPlane(target, 0, source.data[0],
                         source.linesize[0], width * 4, height);
    }
    return false;
}

} // namespace

struct FFmpegVideoCaptureSource::Impl
    : public std::enable_shared_from_this<
          FFmpegVideoCaptureSource::Impl> {
    ~Impl() {
        stop();
        reset();
    }

    Status configure(const VideoCaptureConfig& value) {
        std::lock_guard<std::mutex> lock(lifecycleMutex);
        if (running.load()) {
            return Status::failure("cannot configure a running video source");
        }
        if (value.name.empty()) {
            return Status::failure("video source input URL must not be empty");
        }
        {
            std::lock_guard<std::mutex> configLock(configMutex);
            config = value;
            configured = true;
        }
        setOutputFormat(value.outputFormat);
        clearError();
        return {};
    }

    Status start() {
        std::lock_guard<std::mutex> lock(lifecycleMutex);
        if (running.load()) {
            return Status::failure("video source is already running");
        }
        if (worker.joinable()) {
            if (worker.get_id() == std::this_thread::get_id()) {
                return Status::failure(
                    "cannot restart a video source from its callback");
            }
            worker.join();
        }

        VideoCaptureConfig value;
        {
            std::lock_guard<std::mutex> configLock(configMutex);
            if (!configured) {
                return Status::failure("video source is not configured");
            }
            value = config;
        }

        clearError();
        interrupted.store(false);
        const auto prepared = prepare(value);
        if (!prepared.ok) {
            reset();
            return prepared;
        }

        running.store(true);
        const auto self = shared_from_this();
        try {
            worker = std::thread([self] {
                self->run();
            });
        } catch (const std::exception& error) {
            running.store(false);
            reset();
            return Status::failure(
                "failed to start video capture thread: " +
                std::string(error.what()));
        }
        return {};
    }

    void stop() noexcept {
        std::unique_lock<std::mutex> lock(lifecycleMutex);
        running.store(false);
        interrupted.store(true);
        if (!worker.joinable()) {
            return;
        }
        if (worker.get_id() == std::this_thread::get_id()) {
            return;
        }
        try {
            worker.join();
        } catch (...) {
        }
    }

    Status prepare(const VideoCaptureConfig& value) {
        reset();
        detail::ensureDevicesRegistered();

        const AVInputFormat* inputFormat = nullptr;
        if (!value.backend.empty()) {
            inputFormat =
                av_find_input_format(value.backend.c_str());
            if (!inputFormat) {
                return Status::failure(
                    "unknown FFmpeg input format: " +
                    value.backend);
            }
        }

        AVDictionary* inputOptions = nullptr;
        for (const auto& option : value.options) {
            const auto status = av_dict_set(
                &inputOptions, option.first.c_str(),
                option.second.c_str(), 0);
            if (status < 0) {
                av_dict_free(&inputOptions);
                return Status::failure(
                    "failed to set video input option '" +
                    option.first + "': " + ffmpegError(status));
            }
        }

        formatContext = avformat_alloc_context();
        if (!formatContext) {
            av_dict_free(&inputOptions);
            return Status::failure("failed to allocate FFmpeg input context");
        }
        formatContext->interrupt_callback.callback =
            &detail::interruptCapture;
        formatContext->interrupt_callback.opaque =
            &interrupted;

        auto status = avformat_open_input(
            &formatContext, value.name.c_str(), inputFormat,
            &inputOptions);
        av_dict_free(&inputOptions);
        if (status < 0) {
            reset();
            return Status::failure(
                "failed to open video input: " + ffmpegError(status));
        }

        status = avformat_find_stream_info(formatContext, nullptr);
        if (status < 0) {
            reset();
            return Status::failure(
                "failed to inspect video input: " +
                ffmpegError(status));
        }

        const AVCodec* decoder = nullptr;
        streamIndex = av_find_best_stream(
            formatContext, AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
        if (streamIndex < 0 || !decoder) {
            reset();
            return Status::failure(
                "video input does not contain a decodable video stream");
        }

        const auto* stream = formatContext->streams[streamIndex];
        decoderContext = avcodec_alloc_context3(decoder);
        packet = av_packet_alloc();
        decodedFrame = av_frame_alloc();
        convertedFrame = av_frame_alloc();
        if (!decoderContext || !packet || !decodedFrame ||
            !convertedFrame) {
            reset();
            return Status::failure("failed to allocate video capture resources");
        }

        status = avcodec_parameters_to_context(
            decoderContext, stream->codecpar);
        if (status < 0) {
            reset();
            return Status::failure(
                "failed to initialize video decoder: " +
                ffmpegError(status));
        }
        decoderContext->pkt_timebase = stream->time_base;
        status = avcodec_open2(decoderContext, decoder, nullptr);
        if (status < 0) {
            reset();
            return Status::failure(
                "failed to open video decoder: " +
                ffmpegError(status));
        }

        streamTimeBase =
            stream->time_base.num > 0 && stream->time_base.den > 0
                ? stream->time_base
                : AVRational{1, 1000000};

        auto concreteFormat = value.outputFormat;
        if (concreteFormat.width <= 0) {
            concreteFormat.width = decoderContext->width;
        }
        if (concreteFormat.height <= 0) {
            concreteFormat.height = decoderContext->height;
        }
        if (concreteFormat.frameRate.numerator <= 0 ||
            concreteFormat.frameRate.denominator <= 0) {
            concreteFormat.frameRate =
                frameRate(stream->avg_frame_rate);
            if (concreteFormat.frameRate.numerator <= 0) {
                concreteFormat.frameRate =
                    frameRate(stream->r_frame_rate);
            }
            if (concreteFormat.frameRate.numerator <= 0) {
                concreteFormat.frameRate =
                    frameRate(decoderContext->framerate);
            }
            if (concreteFormat.frameRate.numerator <= 0) {
                concreteFormat.frameRate = Rational{30, 1};
            }
        }
        if (pixelFormat(concreteFormat.pixelFormat) ==
                AV_PIX_FMT_NONE ||
            concreteFormat.width <= 0 ||
            concreteFormat.height <= 0) {
            reset();
            return Status::failure("invalid video output format");
        }
        setOutputFormat(concreteFormat);
        return {};
    }

    void run() noexcept {
        bool reachedEnd = false;
        try {
            while (running.load()) {
                av_packet_unref(packet);
                const auto readStatus = av_read_frame(
                    formatContext, packet);
                if (readStatus == AVERROR(EAGAIN)) {
                    continue;
                }
                if (readStatus == AVERROR_EXIT || !running.load()) {
                    break;
                }
                if (readStatus == AVERROR_EOF) {
                    reachedEnd = true;
                    break;
                }
                if (readStatus < 0) {
                    setError(
                        "failed to read video input: " +
                        ffmpegError(readStatus));
                    break;
                }
                if (packet->stream_index == streamIndex &&
                    !decodePacket()) {
                    break;
                }
            }

            if (reachedEnd && running.load()) {
                const auto status =
                    avcodec_send_packet(decoderContext, nullptr);
                if (status < 0 && status != AVERROR_EOF) {
                    setError(
                        "failed to drain video decoder: " +
                        ffmpegError(status));
                } else if (!receiveFrames()) {
                    // receiveFrames() records the concrete error.
                }
            }
        } catch (const std::exception& error) {
            setError(
                "video frame callback failed: " +
                std::string(error.what()));
        } catch (...) {
            setError("video frame callback failed");
        }

        running.store(false);
        reset();
    }

    bool decodePacket() {
        auto status =
            avcodec_send_packet(decoderContext, packet);
        if (status == AVERROR(EAGAIN)) {
            if (!receiveFrames()) {
                return false;
            }
            status =
                avcodec_send_packet(decoderContext, packet);
        }
        if (status < 0) {
            setError(
                "failed to decode video packet: " +
                ffmpegError(status));
            return false;
        }
        return receiveFrames();
    }

    bool receiveFrames() {
        while (true) {
            const auto status =
                avcodec_receive_frame(decoderContext, decodedFrame);
            if (status == AVERROR(EAGAIN) ||
                status == AVERROR_EOF) {
                return true;
            }
            if (status < 0) {
                setError(
                    "failed to receive video frame: " +
                    ffmpegError(status));
                return false;
            }
            const auto delivered = deliverFrame(*decodedFrame);
            av_frame_unref(decodedFrame);
            if (!delivered.ok) {
                setError(delivered.message);
                return false;
            }
        }
    }

    Status deliverFrame(const AVFrame& source) {
        if (!running.load() || source.width <= 0 ||
            source.height <= 0 ||
            source.format == AV_PIX_FMT_NONE) {
            return {};
        }

        auto targetFormat = getOutputFormat();
        if (targetFormat.width <= 0) {
            targetFormat.width = source.width;
        }
        if (targetFormat.height <= 0) {
            targetFormat.height = source.height;
        }
        if (targetFormat.width <= 0 ||
            targetFormat.height <= 0) {
            return Status::failure("video stream has invalid dimensions");
        }

        const auto targetPixelFormat =
            pixelFormat(targetFormat.pixelFormat);
        if (targetPixelFormat == AV_PIX_FMT_NONE) {
            return Status::failure("unsupported video pixel format");
        }
        scaler = sws_getCachedContext(
            scaler, source.width, source.height,
            static_cast<AVPixelFormat>(source.format),
            targetFormat.width, targetFormat.height,
            targetPixelFormat, SWS_BILINEAR, nullptr, nullptr,
            nullptr);
        if (!scaler) {
            return Status::failure("failed to configure video conversion");
        }

        av_frame_unref(convertedFrame);
        convertedFrame->format = targetPixelFormat;
        convertedFrame->width = targetFormat.width;
        convertedFrame->height = targetFormat.height;
        auto status = av_frame_get_buffer(convertedFrame, 32);
        if (status < 0) {
            return Status::failure(
                "failed to allocate converted video frame: " +
                ffmpegError(status));
        }
        status = sws_scale(
            scaler, source.data, source.linesize, 0,
            source.height, convertedFrame->data,
            convertedFrame->linesize);
        if (status <= 0) {
            return Status::failure("failed to convert video frame");
        }

        const auto size = frameByteCount(targetFormat);
        if (size == 0) {
            return Status::failure("invalid converted video buffer size");
        }
        auto pixels =
            std::make_shared<ByteBuffer>(size);
        if (!pixels || !*pixels ||
            !copyFrame(*convertedFrame, targetFormat, *pixels) ||
            !pixels->setSize(size)) {
            return Status::failure("failed to copy converted video frame");
        }

        std::int64_t pts = lastPts;
        auto timestamp = source.best_effort_timestamp;
        if (timestamp == AV_NOPTS_VALUE) {
            timestamp = source.pts;
        }
        if (timestamp != AV_NOPTS_VALUE) {
            const auto converted =
                timestampMicros(timestamp, streamTimeBase);
            if (!hasTimestampOrigin) {
                timestampOrigin = converted;
                hasTimestampOrigin = true;
            }
            pts = std::max<std::int64_t>(
                0, converted - timestampOrigin);
        } else {
            const auto fps = targetFormat.frameRate;
            pts += (1000000LL * fps.denominator) /
                   std::max(1, fps.numerator);
        }
        lastPts = pts;
        setOutputFormat(targetFormat);

        auto sinkValue = [&] {
            std::lock_guard<std::mutex> lock(sinkMutex);
            return sink.lock();
        }();
        if (!sinkValue) {
            return {};
        }

        VideoFrame frame;
        frame.format = targetFormat;
        frame.pixels = std::move(pixels);
        frame.ptsMicros = pts;
        frame.key = true;
        sinkValue->onVideoFrame(frame);
        return {};
    }

    void reset() noexcept {
        av_frame_free(&convertedFrame);
        av_frame_free(&decodedFrame);
        av_packet_free(&packet);
        avcodec_free_context(&decoderContext);
        if (formatContext) {
            avformat_close_input(&formatContext);
        }
        sws_freeContext(scaler);
        scaler = nullptr;
        streamIndex = -1;
        streamTimeBase = {};
        lastPts = 0;
        timestampOrigin = 0;
        hasTimestampOrigin = false;
    }

    void setError(std::string value) {
        std::lock_guard<std::mutex> lock(errorMutex);
        error = std::move(value);
    }

    void clearError() {
        std::lock_guard<std::mutex> lock(errorMutex);
        error.clear();
    }

    std::string getError() const {
        std::lock_guard<std::mutex> lock(errorMutex);
        return error;
    }

    void setOutputFormat(const VideoFormat& value) {
        std::lock_guard<std::mutex> lock(formatMutex);
        output = value;
    }

    VideoFormat getOutputFormat() const {
        std::lock_guard<std::mutex> lock(formatMutex);
        return output;
    }

    std::mutex lifecycleMutex;
    mutable std::mutex configMutex;
    mutable std::mutex sinkMutex;
    mutable std::mutex errorMutex;
    mutable std::mutex formatMutex;

    VideoCaptureConfig config;
    bool configured = false;
    VideoFormat output;
    std::weak_ptr<VideoFrameSink> sink;
    std::string error;

    std::atomic<bool> running{false};
    std::atomic<bool> interrupted{false};
    std::thread worker;

    AVFormatContext* formatContext = nullptr;
    AVCodecContext* decoderContext = nullptr;
    AVPacket* packet = nullptr;
    AVFrame* decodedFrame = nullptr;
    AVFrame* convertedFrame = nullptr;
    SwsContext* scaler = nullptr;
    int streamIndex = -1;
    AVRational streamTimeBase{};
    std::int64_t lastPts = 0;
    std::int64_t timestampOrigin = 0;
    bool hasTimestampOrigin = false;
};

FFmpegVideoCaptureSource::FFmpegVideoCaptureSource()
    : impl_(std::make_shared<Impl>()) {
}

FFmpegVideoCaptureSource::~FFmpegVideoCaptureSource() = default;

FFmpegVideoCaptureSource::FFmpegVideoCaptureSource(
    FFmpegVideoCaptureSource&&) noexcept = default;

FFmpegVideoCaptureSource& FFmpegVideoCaptureSource::operator=(
    FFmpegVideoCaptureSource&&) noexcept = default;

Status FFmpegVideoCaptureSource::configure(
    const VideoCaptureConfig& config) {
    return impl_->configure(config);
}

Status FFmpegVideoCaptureSource::start() {
    return impl_->start();
}

void FFmpegVideoCaptureSource::stop() noexcept {
    impl_->stop();
}

bool FFmpegVideoCaptureSource::running() const noexcept {
    return impl_->running.load();
}

std::string FFmpegVideoCaptureSource::lastError() const {
    return impl_->getError();
}

void FFmpegVideoCaptureSource::setFrameSink(
    std::weak_ptr<VideoFrameSink> sink) noexcept {
    std::lock_guard<std::mutex> lock(impl_->sinkMutex);
    impl_->sink = std::move(sink);
}

VideoFormat FFmpegVideoCaptureSource::outputFormat() const noexcept {
    return impl_->getOutputFormat();
}

} // namespace iris::media::capture
