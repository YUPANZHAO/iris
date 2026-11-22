#include "media/codec/ffmpeg_h264_codec.h"

#include "media/engine/media_buffer.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace iris::media::v2 {
namespace {

Status failure(std::string message) {
    return Status{false, 1, std::move(message)};
}

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

struct NalUnitView {
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
};

void appendStartCode(std::vector<std::uint8_t>& output) {
    output.insert(output.end(), {0, 0, 0, 1});
}

std::vector<NalUnitView> splitNals(const std::uint8_t* data,
                                   std::size_t size) {
    std::vector<NalUnitView> result;
    std::vector<std::size_t> starts;
    if (!data || size == 0) {
        return result;
    }
    for (std::size_t index = 0; index + 3 <= size; ++index) {
        if (index + 4 <= size &&
            data[index] == 0 && data[index + 1] == 0 &&
            data[index + 2] == 0 && data[index + 3] == 1) {
            starts.push_back(index + 4);
            index += 3;
        } else if (data[index] == 0 &&
                   data[index + 1] == 0 &&
                   data[index + 2] == 1) {
            starts.push_back(index + 3);
            index += 2;
        }
    }
    if (starts.empty()) {
        result.push_back({data, size});
        return result;
    }
    for (std::size_t index = 0; index < starts.size(); ++index) {
        const auto begin = starts[index];
        auto end = index + 1 < starts.size()
            ? starts[index + 1] - 3
            : size;
        while (end > begin && data[end - 1] == 0) {
            --end;
        }
        if (end > begin) {
            result.push_back({data + begin, end - begin});
        }
    }
    return result;
}

bool containsNal(const std::vector<NalUnitView>& nals,
                 std::uint8_t type) {
    return std::any_of(
        nals.begin(), nals.end(), [type](const NalUnitView& nal) {
            return nal.size > 0 &&
                (nal.data[0] & 0x1f) == type;
        });
}

std::vector<std::uint8_t> buildParameterSets(
    const std::uint8_t* data,
    std::size_t size) {
    if (!data || size == 0) {
        return {};
    }
    const bool annexB =
        (size >= 3 && data[0] == 0 && data[1] == 0 &&
         data[2] == 1) ||
        (size >= 4 && data[0] == 0 && data[1] == 0 &&
         data[2] == 0 && data[3] == 1);
    if (annexB) {
        return std::vector<std::uint8_t>(data, data + size);
    }
    if (size < 7 || data[0] != 1) {
        return {};
    }
    std::vector<std::uint8_t> result;
    std::size_t offset = 5;
    const int spsCount = data[offset++] & 0x1f;
    for (int index = 0; index < spsCount; ++index) {
        if (offset + 2 > size) {
            return {};
        }
        const auto length =
            (static_cast<std::size_t>(data[offset]) << 8) |
            data[offset + 1];
        offset += 2;
        if (length == 0 || offset + length > size) {
            return {};
        }
        appendStartCode(result);
        result.insert(
            result.end(), data + offset, data + offset + length);
        offset += length;
    }
    if (offset >= size) {
        return {};
    }
    const int ppsCount = data[offset++];
    for (int index = 0; index < ppsCount; ++index) {
        if (offset + 2 > size) {
            return {};
        }
        const auto length =
            (static_cast<std::size_t>(data[offset]) << 8) |
            data[offset + 1];
        offset += 2;
        if (length == 0 || offset + length > size) {
            return {};
        }
        appendStartCode(result);
        result.insert(
            result.end(), data + offset, data + offset + length);
        offset += length;
    }
    return result;
}

} // namespace

struct FFmpegH264VideoEncoder::Impl {
    ~Impl() {
        reset();
    }

    Status configure(const VideoEncoderConfig& config) {
        reset();
        if (config.codecType != CodecType::H264) {
            return failure("video encoder requires H.264");
        }
        if (config.inputFormat.width <= 0 ||
            config.inputFormat.height <= 0 ||
            config.inputFormat.frameRate.numerator <= 0 ||
            config.inputFormat.frameRate.denominator <= 0 ||
            config.inputFormat.pixelFormat !=
                VideoPixelFormat::I420) {
            return failure(
                "H.264 encoder requires positive I420 frame format");
        }
        const AVCodec* codec =
            avcodec_find_encoder_by_name("libopenh264");
        if (!codec) {
            return failure("libopenh264 encoder is unavailable");
        }
        context = avcodec_alloc_context3(codec);
        frame = av_frame_alloc();
        packet = av_packet_alloc();
        if (!context || !frame || !packet) {
            reset();
            return failure("failed to allocate H.264 encoder");
        }
        const auto fps = std::max(
            1,
            config.inputFormat.frameRate.numerator /
                std::max(
                    1,
                    config.inputFormat.frameRate.denominator));
        context->width = config.inputFormat.width;
        context->height = config.inputFormat.height;
        context->pix_fmt = AV_PIX_FMT_YUV420P;
        context->time_base = AVRational{1, fps};
        context->framerate = AVRational{fps, 1};
        context->bit_rate =
            std::max(100000, config.bitrateKbps * 1000);
        context->gop_size = std::max(
            1, intParameter(config.parameters, "gopSize", fps));
        context->max_b_frames = 0;
        context->thread_count = 1;
        context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        av_opt_set_int(
            context->priv_data, "allow_skip_frames", 1, 0);
        const auto status = avcodec_open2(context, codec, nullptr);
        if (status < 0) {
            reset();
            return failure(
                "failed to open OpenH264 encoder: " +
                ffmpegError(status));
        }
        parameterSets = buildParameterSets(
            context->extradata,
            context->extradata_size > 0
                ? static_cast<std::size_t>(context->extradata_size)
                : 0);
        return {};
    }

    Status encode(const VideoFrame& input) {
        if (!context) {
            return failure("H.264 encoder is not configured");
        }
        auto* data = mutableBufferViewData(input.pixels);
        const auto size = bufferViewSize(input.pixels);
        const auto width = input.format.width;
        const auto height = input.format.height;
        if (!data || width <= 0 || height <= 0 ||
            input.format.pixelFormat != VideoPixelFormat::I420) {
            return failure("invalid H.264 I420 input frame");
        }
        const auto expected =
            static_cast<std::size_t>(width) * height * 3 / 2;
        if (size < expected) {
            return failure(
                "truncated H.264 I420 input frame");
        }
        frame->format = AV_PIX_FMT_YUV420P;
        frame->width = width;
        frame->height = height;
        frame->pts = input.ptsMicros;
        frame->data[0] = data;
        frame->data[1] =
            data + static_cast<std::size_t>(width) * height;
        frame->data[2] =
            frame->data[1] +
            static_cast<std::size_t>(width / 2) * (height / 2);
        frame->linesize[0] = width;
        frame->linesize[1] = width / 2;
        frame->linesize[2] = width / 2;
        lastPts = input.ptsMicros;
        const auto status = avcodec_send_frame(context, frame);
        if (status < 0) {
            errors.fetch_add(1);
            return failure(
                "failed to submit H.264 frame: " +
                ffmpegError(status));
        }
        return drain();
    }

    Status flush() {
        if (!context) {
            return {};
        }
        const auto status = avcodec_send_frame(context, nullptr);
        if (status < 0 && status != AVERROR_EOF) {
            return failure(
                "failed to flush H.264 encoder: " +
                ffmpegError(status));
        }
        return drain();
    }

    void reset() noexcept {
        parameterSets.clear();
        av_packet_free(&packet);
        av_frame_free(&frame);
        avcodec_free_context(&context);
        lastPts = 0;
    }

    void emit(const std::uint8_t* data,
              std::size_t size,
              std::int64_t ptsMicros,
              bool key) {
        auto sinkValue = [&] {
            std::lock_guard<std::mutex> lock(sinkMutex);
            return sink.lock();
        }();
        if (!sinkValue) {
            return;
        }

        const auto nals = splitNals(data, size);
        const bool hasSps = containsNal(nals, 7);
        const bool hasPps = containsNal(nals, 8);
        MediaBufferView payload;
        if (key && !hasSps && !hasPps &&
            !parameterSets.empty()) {
            auto buffer = allocateMediaBuffer(
                parameterSets.size() + size);
            if (!buffer) {
                errors.fetch_add(1);
                return;
            }
            std::memcpy(
                buffer->data(),
                parameterSets.data(),
                parameterSets.size());
            std::memcpy(
                buffer->data() + parameterSets.size(),
                data,
                size);
            payload = MediaBufferView{
                std::move(buffer),
                0,
                parameterSets.size() + size};
        } else {
            payload = copyToMediaBuffer(data, size);
        }
        if (!payload.buffer) {
            errors.fetch_add(1);
            return;
        }
        EncodedPacket value;
        value.kind = MediaKind::Video;
        value.codecType = CodecType::H264;
        value.data = std::move(payload);
        value.ptsMicros = ptsMicros;
        value.key = key;
        sinkValue->onEncodedPacket(value);
    }

    Status drain() {
        while (true) {
            const auto status =
                avcodec_receive_packet(context, packet);
            if (status == AVERROR(EAGAIN) ||
                status == AVERROR_EOF) {
                return {};
            }
            if (status < 0) {
                errors.fetch_add(1);
                return failure(
                    "failed to receive H.264 packet: " +
                    ffmpegError(status));
            }
            const auto nals = splitNals(
                packet->data,
                static_cast<std::size_t>(packet->size));
            const bool key =
                (packet->flags & AV_PKT_FLAG_KEY) != 0 ||
                containsNal(nals, 5);
            emit(
                packet->data,
                static_cast<std::size_t>(packet->size),
                packet->pts == AV_NOPTS_VALUE
                    ? lastPts : packet->pts,
                key);
            av_packet_unref(packet);
        }
    }

    std::mutex sinkMutex;
    std::weak_ptr<IEncodedPacketSink> sink;
    std::atomic<std::uint64_t> errors{0};
    AVCodecContext* context = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* packet = nullptr;
    std::vector<std::uint8_t> parameterSets;
    std::int64_t lastPts = 0;
};

FFmpegH264VideoEncoder::FFmpegH264VideoEncoder()
    : impl_(std::make_unique<Impl>()) {
}

FFmpegH264VideoEncoder::~FFmpegH264VideoEncoder() = default;

void FFmpegH264VideoEncoder::setSink(
    std::weak_ptr<IEncodedPacketSink> sink) noexcept {
    std::lock_guard<std::mutex> lock(impl_->sinkMutex);
    impl_->sink = std::move(sink);
}

Status FFmpegH264VideoEncoder::configure(
    const VideoEncoderConfig& config) {
    return impl_->configure(config);
}

Status FFmpegH264VideoEncoder::encode(
    const VideoFrame& frame) {
    return impl_->encode(frame);
}

Status FFmpegH264VideoEncoder::flush() {
    return impl_->flush();
}

Status FFmpegH264VideoEncoder::setTargetBitrate(int) {
    return failure("dynamic H.264 bitrate is not implemented");
}

void FFmpegH264VideoEncoder::reset() noexcept {
    std::lock_guard<std::mutex> lock(impl_->sinkMutex);
    impl_->reset();
}

struct FFmpegH264VideoDecoder::Impl {
    struct SinkHolder {
        std::mutex mutex;
        std::weak_ptr<IVideoFrameSink> sink;
    };

    ~Impl() {
        resetCodec();
    }

    std::shared_ptr<SinkHolder> sink =
        std::make_shared<SinkHolder>();
    AVCodecContext* decoder = nullptr;
    SwsContext* converter = nullptr;
    VideoPixelFormat outputFormat = VideoPixelFormat::I420;
    Rational frameRate{0, 1};
    std::atomic<std::uint64_t> errors{0};

    Status configure(const VideoDecoderConfig& config) {
        resetCodec();
        if (config.codecType != CodecType::H264) {
            return failure("video decoder requires H.264");
        }
        const AVCodec* codec =
            avcodec_find_decoder(AV_CODEC_ID_H264);
        if (!codec) {
            return failure("H.264 decoder is unavailable");
        }
        decoder = avcodec_alloc_context3(codec);
        if (!decoder) {
            return failure("failed to allocate H.264 decoder");
        }
        decoder->thread_count = 1;
        const auto result =
            avcodec_open2(decoder, codec, nullptr);
        if (result < 0) {
            resetCodec();
            return failure(
                "failed to open H.264 decoder: " +
                ffmpegError(result));
        }
        outputFormat = config.outputFormat;
        frameRate.numerator = intParameter(
            config.parameters, "frameRateNumerator", 0);
        frameRate.denominator = std::max(
            1,
            intParameter(
                config.parameters,
                "frameRateDenominator",
                1));
        return {};
    }

    Status decode(const EncodedPacket& value) {
        if (!decoder) {
            return failure("H.264 decoder is not configured");
        }
        const auto* data = bufferViewData(value.data);
        const auto size = bufferViewSize(value.data);
        if (!data || size == 0) {
            return failure("empty H.264 packet");
        }
        AVPacket* input = av_packet_alloc();
        AVFrame* decoded = av_frame_alloc();
        if (!input || !decoded) {
            av_packet_free(&input);
            av_frame_free(&decoded);
            return failure(
                "failed to allocate H.264 decoder buffers");
        }
        const auto alloc =
            av_new_packet(input, static_cast<int>(size));
        if (alloc < 0) {
            av_packet_free(&input);
            av_frame_free(&decoded);
            return failure(
                "failed to allocate H.264 input packet");
        }
        std::memcpy(input->data, data, size);
        input->pts = value.ptsMicros;
        auto result = avcodec_send_packet(decoder, input);
        av_packet_free(&input);
        if (result < 0 && result != AVERROR(EAGAIN)) {
            av_frame_free(&decoded);
            errors.fetch_add(1);
            return failure(
                "failed to submit H.264 packet: " +
                ffmpegError(result));
        }
        while ((result =
                    avcodec_receive_frame(decoder, decoded)) >= 0) {
            if (!convertAndEmit(decoded, value.ptsMicros)) {
                errors.fetch_add(1);
            }
            av_frame_unref(decoded);
        }
        av_frame_free(&decoded);
        if (result != AVERROR(EAGAIN) &&
            result != AVERROR_EOF) {
            errors.fetch_add(1);
            return failure(
                "failed to decode H.264 packet: " +
                ffmpegError(result));
        }
        return {};
    }

    bool convertAndEmit(const AVFrame* frame,
                        std::int64_t fallbackPts) {
        const auto destinationFormat =
            pixelFormat(outputFormat);
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
            destinationFormat,
            frame->width,
            frame->height,
            1);
        if (byteCount <= 0) {
            return false;
        }
        auto buffer = allocateMediaBuffer(
            static_cast<std::size_t>(byteCount));
        if (!buffer) {
            return false;
        }
        std::uint8_t* destination[3] = {
            buffer->data(), nullptr, nullptr};
        int strides[3] = {frame->width, 0, 0};
        if (outputFormat == VideoPixelFormat::BGRA) {
            strides[0] = frame->width * 4;
        } else if (outputFormat == VideoPixelFormat::I420) {
            destination[1] = destination[0] +
                static_cast<std::size_t>(frame->width) *
                    frame->height;
            destination[2] = destination[1] +
                static_cast<std::size_t>(frame->width / 2) *
                    (frame->height / 2);
            strides[1] = frame->width / 2;
            strides[2] = frame->width / 2;
        } else {
            destination[1] = destination[0] +
                static_cast<std::size_t>(frame->width) *
                    frame->height;
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
        VideoFrame output;
        output.format.width = frame->width;
        output.format.height = frame->height;
        output.format.frameRate = frameRate;
        output.format.pixelFormat = outputFormat;
        output.pixels = MediaBufferView{
            std::move(buffer),
            0,
            static_cast<std::size_t>(byteCount)};
        output.ptsMicros =
            frame->pts == AV_NOPTS_VALUE
                ? fallbackPts : frame->pts;
        output.key =
            (frame->flags & AV_FRAME_FLAG_KEY) != 0;
        sinkValue->onVideoFrame(output);
        return true;
    }

    void resetCodec() noexcept {
        sws_freeContext(converter);
        converter = nullptr;
        avcodec_free_context(&decoder);
    }
};

FFmpegH264VideoDecoder::FFmpegH264VideoDecoder()
    : impl_(std::make_unique<Impl>()) {
}

FFmpegH264VideoDecoder::~FFmpegH264VideoDecoder() = default;

void FFmpegH264VideoDecoder::setSink(
    std::weak_ptr<IVideoFrameSink> sink) noexcept {
    std::lock_guard<std::mutex> lock(impl_->sink->mutex);
    impl_->sink->sink = std::move(sink);
}

Status FFmpegH264VideoDecoder::configure(
    const VideoDecoderConfig& config) {
    return impl_->configure(config);
}

Status FFmpegH264VideoDecoder::decode(
    const EncodedPacket& packet) {
    return impl_->decode(packet);
}

void FFmpegH264VideoDecoder::reset() noexcept {
    impl_->resetCodec();
}

} // namespace iris::media::v2
