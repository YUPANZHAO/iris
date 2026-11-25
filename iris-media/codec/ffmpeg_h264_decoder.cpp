#include "codec/ffmpeg_h264_decoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <atomic>
#include <climits>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>

namespace iris::media::codec {
namespace {

std::string ffmpegError(int error) {
    char message[AV_ERROR_MAX_STRING_SIZE]{};
    if (av_strerror(error, message, sizeof(message)) < 0) {
        return "unknown FFmpeg error";
    }
    return message;
}

int intParameter(const AttributeMap& values,
                 const std::string& key,
                 int fallback) {
    const auto iterator = values.find(key);
    if (iterator == values.end()) {
        return fallback;
    }
    try {
        return std::stoi(iterator->second);
    } catch (...) {
        return fallback;
    }
}

AVPixelFormat pixelFormat(VideoPixelFormat format) {
    switch (format) {
    case VideoPixelFormat::I420: return AV_PIX_FMT_YUV420P;
    case VideoPixelFormat::NV12: return AV_PIX_FMT_NV12;
    case VideoPixelFormat::BGRA: return AV_PIX_FMT_BGRA;
    }
    return AV_PIX_FMT_YUV420P;
}

} // namespace

struct FFmpegH264Decoder::Impl {
    struct SinkHolder {
        std::mutex mutex;
        std::weak_ptr<IVideoFrameSink> sink;
    };

    ~Impl() {
        resetCodec();
    }

    std::shared_ptr<SinkHolder> sink = std::make_shared<SinkHolder>();
    AVCodecContext* decoder = nullptr;
    SwsContext* converter = nullptr;
    VideoPixelFormat outputFormat = VideoPixelFormat::I420;
    Rational frameRate{0, 1};
    std::atomic<std::uint64_t> errors{0};
    StreamId streamId = 0;

    Status configure(const VideoDecoderConfig& config) {
        resetCodec();
        if (config.codecType != CodecType::H264) {
            return Status::failure("H.264 decoder requires H.264 configuration");
        }
        const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
        if (!codec) {
            return Status::failure("H.264 decoder is unavailable");
        }
        decoder = avcodec_alloc_context3(codec);
        if (!decoder) {
            return Status::failure("failed to allocate H.264 decoder");
        }
        decoder->thread_count = 1;
        const auto result = avcodec_open2(decoder, codec, nullptr);
        if (result < 0) {
            resetCodec();
            return Status::failure(
                "failed to open H.264 decoder: " +
                ffmpegError(result));
        }
        outputFormat = config.outputFormat;
        frameRate.numerator = intParameter(
            config.parameters, "frameRateNumerator", 0);
        frameRate.denominator = std::max(
            1,
            intParameter(config.parameters, "frameRateDenominator", 1));
        return {};
    }

    Status decode(const EncodedPacket& value) {
        if (!decoder) {
            return Status::failure("H.264 decoder is not configured");
        }
        if (value.kind != MediaKind::Video ||
            value.codecType != CodecType::H264) {
            return Status::failure("H.264 decoder requires an H.264 video packet");
        }
        if (!value.data) {
            return Status::failure("H.264 decoder received an empty buffer");
        }
        streamId = value.streamId;
        const auto* data =
            static_cast<const std::uint8_t*>(value.data->data());
        const auto size = value.data->size();
        if (!data || size == 0 || size > INT_MAX) {
            return Status::failure("invalid H.264 packet");
        }

        AVPacket* input = av_packet_alloc();
        if (!input) {
            return Status::failure("failed to allocate H.264 input packet");
        }
        const auto alloc = av_new_packet(input, static_cast<int>(size));
        if (alloc < 0) {
            av_packet_free(&input);
            return Status::failure("failed to allocate H.264 packet payload");
        }
        std::memcpy(input->data, data, size);
        input->pts = value.ptsMicros;

        auto result = avcodec_send_packet(decoder, input);
        av_packet_free(&input);
        if (result == AVERROR(EAGAIN)) {
            const auto drained = drain(value.ptsMicros);
            if (!drained.ok) {
                return drained;
            }
            AVPacket* retry = av_packet_alloc();
            if (!retry) {
                return Status::failure("failed to allocate H.264 retry packet");
            }
            const auto retryAlloc =
                av_new_packet(retry, static_cast<int>(size));
            if (retryAlloc < 0) {
                av_packet_free(&retry);
                return Status::failure(
                    "failed to allocate H.264 retry payload");
            }
            std::memcpy(retry->data, data, size);
            retry->pts = value.ptsMicros;
            result = avcodec_send_packet(decoder, retry);
            av_packet_free(&retry);
        }
        if (result < 0 && result != AVERROR_EOF) {
            errors.fetch_add(1);
            return Status::failure(
                "failed to submit H.264 packet: " +
                ffmpegError(result));
        }
        return drain(value.ptsMicros);
    }

    Status flush() {
        if (!decoder) {
            return {};
        }
        const auto result = avcodec_send_packet(decoder, nullptr);
        if (result < 0 && result != AVERROR_EOF) {
            return Status::failure(
                "failed to flush H.264 decoder: " +
                ffmpegError(result));
        }
        return drain(0);
    }

    Status drain(std::int64_t fallbackPts) {
        AVFrame* decoded = av_frame_alloc();
        if (!decoded) {
            return Status::failure("failed to allocate H.264 output frame");
        }
        while (true) {
            const auto result = avcodec_receive_frame(decoder, decoded);
            if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) {
                av_frame_free(&decoded);
                return {};
            }
            if (result < 0) {
                av_frame_free(&decoded);
                errors.fetch_add(1);
                return Status::failure(
                    "failed to decode H.264 packet: " +
                    ffmpegError(result));
            }
            if (!convertAndEmit(decoded, fallbackPts)) {
                errors.fetch_add(1);
            }
            av_frame_unref(decoded);
        }
    }

    bool convertAndEmit(const AVFrame* frame,
                        std::int64_t fallbackPts) {
        const auto destinationFormat = pixelFormat(outputFormat);
        converter = sws_getCachedContext(
            converter,
            frame->width,
            frame->height,
            static_cast<AVPixelFormat>(frame->format),
            frame->width,
            frame->height,
            destinationFormat,
            SWS_BILINEAR,
            nullptr,
            nullptr,
            nullptr);
        if (!converter) {
            return false;
        }
        const auto byteCount = av_image_get_buffer_size(
            destinationFormat, frame->width, frame->height, 1);
        if (byteCount <= 0) {
            return false;
        }
        auto buffer =
            std::make_shared<ByteBuffer>(static_cast<std::size_t>(byteCount));
        if (!buffer || !*buffer) {
            return false;
        }

        std::uint8_t* destination[4] = {
            static_cast<std::uint8_t*>(buffer->data()),
            nullptr,
            nullptr,
            nullptr};
        int strides[4] = {frame->width, 0, 0, 0};
        if (outputFormat == VideoPixelFormat::BGRA) {
            strides[0] = frame->width * 4;
        } else if (outputFormat == VideoPixelFormat::I420) {
            destination[1] = destination[0] +
                static_cast<std::size_t>(frame->width) * frame->height;
            destination[2] = destination[1] +
                static_cast<std::size_t>(frame->width / 2) *
                    (frame->height / 2);
            strides[1] = frame->width / 2;
            strides[2] = frame->width / 2;
        } else {
            destination[1] = destination[0] +
                static_cast<std::size_t>(frame->width) * frame->height;
            strides[1] = frame->width;
        }
        const auto converted = sws_scale(
            converter,
            frame->data,
            frame->linesize,
            0,
            frame->height,
            destination,
            strides);
        if (converted <= 0) {
            return false;
        }

        auto sinkValue = [&] {
            std::lock_guard<std::mutex> lock(sink->mutex);
            return sink->sink.lock();
        }();
        if (!sinkValue) {
            return true;
        }

        VideoRawFrame output;
        output.streamId = streamId;
        output.format.width = frame->width;
        output.format.height = frame->height;
        output.format.frameRate = frameRate;
        output.format.pixelFormat = outputFormat;
        output.pixels = std::move(buffer);
        output.ptsMicros =
            frame->pts == AV_NOPTS_VALUE ? fallbackPts : frame->pts;
        output.key = (frame->flags & AV_FRAME_FLAG_KEY) != 0;
        sinkValue->onVideoFrame(output);
        return true;
    }

    void resetCodec() noexcept {
        sws_freeContext(converter);
        converter = nullptr;
        avcodec_free_context(&decoder);
        streamId = 0;
    }
};

FFmpegH264Decoder::FFmpegH264Decoder()
    : impl_(std::make_unique<Impl>()) {
}

FFmpegH264Decoder::~FFmpegH264Decoder() = default;

FFmpegH264Decoder::FFmpegH264Decoder(FFmpegH264Decoder&&) noexcept = default;

FFmpegH264Decoder& FFmpegH264Decoder::operator=(
    FFmpegH264Decoder&&) noexcept = default;

Status FFmpegH264Decoder::configure(const VideoDecoderConfig& config) {
    return impl_->configure(config);
}

Status FFmpegH264Decoder::decode(const EncodedPacket& packet) {
    return impl_->decode(packet);
}

Status FFmpegH264Decoder::flush() {
    return impl_->flush();
}

void FFmpegH264Decoder::setRawFrameSink(
    std::weak_ptr<IVideoFrameSink> sink) noexcept {
    std::lock_guard<std::mutex> lock(impl_->sink->mutex);
    impl_->sink->sink = std::move(sink);
}

void FFmpegH264Decoder::reset() noexcept {
    std::lock_guard<std::mutex> lock(impl_->sink->mutex);
    impl_->resetCodec();
}

} // namespace iris::media::codec
