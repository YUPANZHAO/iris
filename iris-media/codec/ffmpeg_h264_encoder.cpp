#include "codec/ffmpeg_h264_encoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/opt.h>
#include <libavutil/pixfmt.h>
}

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace iris::media::codec {
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
            return nal.size > 0 && (nal.data[0] & 0x1f) == type;
        });
}

std::vector<std::uint8_t> buildParameterSets(
    const std::uint8_t* data,
    std::size_t size) {
    if (!data || size == 0) {
        return {};
    }
    const bool annexB =
        (size >= 3 && data[0] == 0 && data[1] == 0 && data[2] == 1) ||
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
        result.insert(result.end(), data + offset, data + offset + length);
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
        result.insert(result.end(), data + offset, data + offset + length);
        offset += length;
    }
    return result;
}

} // namespace

struct FFmpegH264Encoder::Impl {
    ~Impl() {
        reset();
    }

    Status configure(const VideoEncoderConfig& config) {
        reset();
        if (config.codecType != CodecType::H264) {
            return failure("H.264 encoder requires H.264 configuration");
        }
        const auto& format = config.inputFormat;
        if (format.width <= 0 || format.height <= 0 ||
            format.width % 2 != 0 || format.height % 2 != 0 ||
            format.frameRate.numerator <= 0 ||
            format.frameRate.denominator <= 0 ||
            format.pixelFormat != VideoPixelFormat::I420) {
            return failure(
                "H.264 encoder requires even I420 video dimensions");
        }
        const AVCodec* codec = avcodec_find_encoder_by_name("libopenh264");
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
            format.frameRate.numerator /
                std::max(1, format.frameRate.denominator));
        inputWidth = format.width;
        inputHeight = format.height;
        inputFrameRate = format.frameRate;
        context->width = format.width;
        context->height = format.height;
        context->pix_fmt = AV_PIX_FMT_YUV420P;
        context->time_base = AVRational{1, 1000000};
        context->framerate = AVRational{
            format.frameRate.numerator, format.frameRate.denominator};
        context->bit_rate =
            std::max(100000, config.bitrateKbps * 1000);
        context->gop_size = std::max(
            1, intParameter(config.parameters, "gopSize", fps));
        context->max_b_frames = 0;
        context->thread_count = 1;
        context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        av_opt_set_int(context->priv_data, "allow_skip_frames", 1, 0);
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

    Status encode(const VideoRawFrame& video) {
        if (!context) {
            return failure("H.264 encoder is not configured");
        }
        if (!video.pixels) {
            return failure("H.264 encoder received an empty buffer");
        }
        auto* data = const_cast<std::uint8_t*>(
            static_cast<const std::uint8_t*>(video.pixels->data()));
        const auto size = video.pixels->size();
        const auto width = video.format.width;
        const auto height = video.format.height;
        if (!data || width <= 0 || height <= 0 ||
            video.format.pixelFormat != VideoPixelFormat::I420 ||
            width != inputWidth || height != inputHeight ||
            video.format.frameRate.numerator !=
                inputFrameRate.numerator ||
            video.format.frameRate.denominator !=
                inputFrameRate.denominator) {
            return failure("invalid H.264 I420 input frame");
        }
        const auto expected =
            static_cast<std::size_t>(width) * height * 3 / 2;
        if (size < expected) {
            return failure("truncated H.264 I420 input frame");
        }
        frame->format = AV_PIX_FMT_YUV420P;
        frame->width = width;
        frame->height = height;
        frame->pts = video.ptsMicros;
        frame->data[0] = data;
        frame->data[1] =
            data + static_cast<std::size_t>(width) * height;
        frame->data[2] =
            frame->data[1] +
            static_cast<std::size_t>(width / 2) * (height / 2);
        frame->linesize[0] = width;
        frame->linesize[1] = width / 2;
        frame->linesize[2] = width / 2;
        lastPts = video.ptsMicros;
        lastStreamId = video.streamId;
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
        lastStreamId = 0;
        inputWidth = 0;
        inputHeight = 0;
        inputFrameRate = {};
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
        const auto prefixSize =
            key && (!hasSps || !hasPps)
                ? parameterSets.size()
                : 0;
        std::shared_ptr<ByteBuffer> payload;
        if (prefixSize == 0) {
            payload = std::make_shared<ByteBuffer>(data, size);
        } else {
            payload =
                std::make_shared<ByteBuffer>(prefixSize + size);
            if (payload && *payload) {
                std::memcpy(
                    payload->data(), parameterSets.data(), prefixSize);
                std::memcpy(
                    static_cast<std::uint8_t*>(payload->data()) +
                        prefixSize,
                    data,
                    size);
            }
        }
        if (!payload || !*payload) {
            errors.fetch_add(1);
            return;
        }
        EncodedPacket value;
        value.streamId = lastStreamId;
        value.kind = MediaKind::Video;
        value.codecType = CodecType::H264;
        value.data = std::move(payload);
        value.ptsMicros = ptsMicros;
        value.key = key;
        sinkValue->onEncodedPacket(value);
    }

    Status drain() {
        while (true) {
            const auto status = avcodec_receive_packet(context, packet);
            if (status == AVERROR(EAGAIN) || status == AVERROR_EOF) {
                return {};
            }
            if (status < 0) {
                errors.fetch_add(1);
                return failure(
                    "failed to receive H.264 packet: " +
                    ffmpegError(status));
            }
            const auto nals = splitNals(
                packet->data, static_cast<std::size_t>(packet->size));
            const bool key =
                (packet->flags & AV_PKT_FLAG_KEY) != 0 ||
                containsNal(nals, 5);
            emit(
                packet->data,
                static_cast<std::size_t>(packet->size),
                packet->pts == AV_NOPTS_VALUE ? lastPts : packet->pts,
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
    StreamId lastStreamId = 0;
    int inputWidth = 0;
    int inputHeight = 0;
    Rational inputFrameRate;
};

FFmpegH264Encoder::FFmpegH264Encoder()
    : impl_(std::make_unique<Impl>()) {
}

FFmpegH264Encoder::~FFmpegH264Encoder() = default;

FFmpegH264Encoder::FFmpegH264Encoder(FFmpegH264Encoder&&) noexcept = default;

FFmpegH264Encoder& FFmpegH264Encoder::operator=(
    FFmpegH264Encoder&&) noexcept = default;

Status FFmpegH264Encoder::configure(const VideoEncoderConfig& config) {
    return impl_->configure(config);
}

Status FFmpegH264Encoder::encode(const VideoRawFrame& frame) {
    return impl_->encode(frame);
}

Status FFmpegH264Encoder::flush() {
    return impl_->flush();
}

void FFmpegH264Encoder::setEncodedPacketSink(
    std::weak_ptr<IEncodedPacketSink> sink) noexcept {
    std::lock_guard<std::mutex> lock(impl_->sinkMutex);
    impl_->sink = std::move(sink);
}

void FFmpegH264Encoder::reset() noexcept {
    std::lock_guard<std::mutex> lock(impl_->sinkMutex);
    impl_->reset();
}

} // namespace iris::media::codec
