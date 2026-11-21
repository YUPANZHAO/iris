#include "media/codec/ffmpeg_codec_impl.h"

#include "media/engine/media_buffer.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixfmt.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <atomic>
#include <cstring>
#include <functional>
#include <initializer_list>
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

AVSampleFormat sampleFormat(AudioSampleFormat format) {
    return format == AudioSampleFormat::Float32
        ? AV_SAMPLE_FMT_FLT
        : AV_SAMPLE_FMT_S16;
}

std::size_t audioSampleBytes(AudioSampleFormat format) {
    return format == AudioSampleFormat::Float32
        ? sizeof(float)
        : sizeof(std::int16_t);
}

struct NalUnitView {
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
};

void appendH264StartCode(std::vector<std::uint8_t>& output) {
    output.insert(output.end(), {0, 0, 0, 1});
}

std::vector<NalUnitView> splitH264Nals(const std::uint8_t* data,
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

bool containsH264Nal(const std::vector<NalUnitView>& nals,
                     std::uint8_t type) {
    return std::any_of(
        nals.begin(), nals.end(), [type](const NalUnitView& nal) {
            return nal.size > 0 && (nal.data[0] & 0x1f) == type;
        });
}

std::vector<std::uint8_t> buildH264ParameterSets(
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
        appendH264StartCode(result);
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
        appendH264StartCode(result);
        result.insert(result.end(), data + offset, data + offset + length);
        offset += length;
    }
    return result;
}

using PacketOutput = std::function<void(const std::uint8_t*,
                                        std::size_t,
                                        std::int64_t,
                                        bool)>;

class AudioEncodeCore {
public:
    Status configure(const AudioEncoderConfig& config,
                     PacketOutput output) {
        reset();
        if (!output || config.inputFormat.sampleRate <= 0 ||
            config.inputFormat.channels <= 0) {
            return failure("invalid Opus encoder configuration");
        }
        const AVCodec* codec =
            avcodec_find_encoder_by_name("libopus");
        if (!codec) {
            return failure("libopus encoder is unavailable");
        }
        context_ = avcodec_alloc_context3(codec);
        frame_ = av_frame_alloc();
        packet_ = av_packet_alloc();
        if (!context_ || !frame_ || !packet_) {
            reset();
            return failure("failed to allocate Opus encoder");
        }
        context_->sample_fmt = AV_SAMPLE_FMT_S16;
        context_->sample_rate = config.inputFormat.sampleRate;
        av_channel_layout_default(
            &context_->ch_layout, config.inputFormat.channels);
        context_->bit_rate = std::max(16000, config.bitrateKbps * 1000);
        context_->time_base =
            AVRational{1, config.inputFormat.sampleRate};
        context_->frame_size = intParameter(
            config.parameters, "frameSamples",
            config.inputFormat.sampleRate / 50);
        const auto status = avcodec_open2(context_, codec, nullptr);
        if (status < 0) {
            reset();
            return failure(
                "failed to open Opus encoder: " +
                ffmpegError(status));
        }
        output_ = std::move(output);
        return {};
    }

    Status encode(const AudioFrame& input) {
        const auto data = bufferViewData(input.samples);
        const auto size = bufferViewSize(input.samples);
        const auto bytesPerSample =
            static_cast<std::size_t>(input.format.channels) *
            audioSampleBytes(input.format.sampleFormat);
        if (!context_ || !data || bytesPerSample == 0 ||
            size % bytesPerSample != 0 ||
            input.format.sampleFormat != AudioSampleFormat::Signed16) {
            return failure("invalid Opus input frame");
        }
        frame_->format = AV_SAMPLE_FMT_S16;
        frame_->sample_rate = input.format.sampleRate;
        av_channel_layout_default(
            &frame_->ch_layout, input.format.channels);
        frame_->nb_samples = static_cast<int>(size / bytesPerSample);
        frame_->pts = input.ptsMicros;
        frame_->data[0] = const_cast<std::uint8_t*>(data);
        frame_->linesize[0] = static_cast<int>(bytesPerSample);
        lastPts_ = input.ptsMicros;
        const auto status = avcodec_send_frame(context_, frame_);
        return status < 0
            ? failure("failed to submit Opus frame: " +
                      ffmpegError(status))
            : drain();
    }

    Status flush() {
        if (!context_) {
            return {};
        }
        const auto status = avcodec_send_frame(context_, nullptr);
        if (status < 0 && status != AVERROR_EOF) {
            return failure("failed to flush Opus encoder: " +
                           ffmpegError(status));
        }
        return drain();
    }

    void reset() noexcept {
        output_ = {};
        av_packet_free(&packet_);
        av_frame_free(&frame_);
        avcodec_free_context(&context_);
        lastPts_ = 0;
    }

private:
    Status drain() {
        while (true) {
            const auto status = avcodec_receive_packet(context_, packet_);
            if (status == AVERROR(EAGAIN) || status == AVERROR_EOF) {
                return {};
            }
            if (status < 0) {
                return failure("failed to receive Opus packet: " +
                               ffmpegError(status));
            }
            if (output_) {
                output_(
                    packet_->data,
                    static_cast<std::size_t>(packet_->size),
                    packet_->pts == AV_NOPTS_VALUE
                        ? lastPts_ : packet_->pts,
                    (packet_->flags & AV_PKT_FLAG_KEY) != 0);
            }
            av_packet_unref(packet_);
        }
    }

    AVCodecContext* context_ = nullptr;
    AVFrame* frame_ = nullptr;
    AVPacket* packet_ = nullptr;
    PacketOutput output_;
    std::int64_t lastPts_ = 0;
};

class VideoEncodeCore {
public:
    Status configure(const VideoEncoderConfig& config,
                     PacketOutput output) {
        reset();
        if (!output || config.inputFormat.width <= 0 ||
            config.inputFormat.height <= 0 ||
            config.inputFormat.frameRate.numerator <= 0) {
            return failure("invalid H.264 encoder configuration");
        }
        const AVCodec* codec =
            avcodec_find_encoder_by_name("libopenh264");
        if (!codec) {
            return failure("libopenh264 encoder is unavailable");
        }
        context_ = avcodec_alloc_context3(codec);
        frame_ = av_frame_alloc();
        packet_ = av_packet_alloc();
        if (!context_ || !frame_ || !packet_) {
            reset();
            return failure("failed to allocate H.264 encoder");
        }
        const auto fps = std::max(
            1, config.inputFormat.frameRate.numerator /
                std::max(1, config.inputFormat.frameRate.denominator));
        context_->width = config.inputFormat.width;
        context_->height = config.inputFormat.height;
        context_->pix_fmt = AV_PIX_FMT_YUV420P;
        context_->time_base = AVRational{1, fps};
        context_->framerate = AVRational{fps, 1};
        context_->bit_rate = std::max(100000, config.bitrateKbps * 1000);
        context_->gop_size =
            std::max(1, intParameter(config.parameters, "gopSize", fps));
        context_->max_b_frames = 0;
        context_->thread_count = 1;
        context_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        av_opt_set_int(
            context_->priv_data, "allow_skip_frames", 1, 0);
        const auto status = avcodec_open2(context_, codec, nullptr);
        if (status < 0) {
            reset();
            return failure("failed to open OpenH264 encoder: " +
                           ffmpegError(status));
        }
        parameterSets_ = buildH264ParameterSets(
            context_->extradata,
            context_->extradata_size > 0
                ? static_cast<std::size_t>(context_->extradata_size)
                : 0);
        output_ = std::move(output);
        return {};
    }

    Status encode(const VideoFrame& input) {
        const auto data = mutableBufferViewData(input.pixels);
        const auto size = bufferViewSize(input.pixels);
        const auto width = input.format.width;
        const auto height = input.format.height;
        const auto expected =
            static_cast<std::size_t>(width) * height * 3 / 2;
        if (!context_ || !data || width <= 0 || height <= 0 ||
            size < expected ||
            input.format.pixelFormat != VideoPixelFormat::I420) {
            return failure("invalid H.264 I420 input frame");
        }
        frame_->format = AV_PIX_FMT_YUV420P;
        frame_->width = width;
        frame_->height = height;
        frame_->pts = input.ptsMicros;
        frame_->data[0] = data;
        frame_->data[1] =
            data + static_cast<std::size_t>(width) * height;
        frame_->data[2] =
            frame_->data[1] +
            static_cast<std::size_t>(width / 2) * (height / 2);
        frame_->linesize[0] = width;
        frame_->linesize[1] = width / 2;
        frame_->linesize[2] = width / 2;
        lastPts_ = input.ptsMicros;
        const auto status = avcodec_send_frame(context_, frame_);
        return status < 0
            ? failure("failed to submit H.264 frame: " +
                      ffmpegError(status))
            : drain();
    }

    Status flush() {
        if (!context_) {
            return {};
        }
        const auto status = avcodec_send_frame(context_, nullptr);
        if (status < 0 && status != AVERROR_EOF) {
            return failure("failed to flush H.264 encoder: " +
                           ffmpegError(status));
        }
        return drain();
    }

    std::vector<std::uint8_t> parameterSets() const {
        return parameterSets_;
    }

    void reset() noexcept {
        output_ = {};
        parameterSets_.clear();
        av_packet_free(&packet_);
        av_frame_free(&frame_);
        avcodec_free_context(&context_);
        lastPts_ = 0;
    }

private:
    Status drain() {
        while (true) {
            const auto status = avcodec_receive_packet(context_, packet_);
            if (status == AVERROR(EAGAIN) || status == AVERROR_EOF) {
                return {};
            }
            if (status < 0) {
                return failure("failed to receive H.264 packet: " +
                               ffmpegError(status));
            }
            const auto nals = splitH264Nals(
                packet_->data, static_cast<std::size_t>(packet_->size));
            const bool key =
                (packet_->flags & AV_PKT_FLAG_KEY) != 0 ||
                containsH264Nal(nals, 5);
            if (output_) {
                output_(
                    packet_->data,
                    static_cast<std::size_t>(packet_->size),
                    packet_->pts == AV_NOPTS_VALUE
                        ? lastPts_ : packet_->pts,
                    key);
            }
            av_packet_unref(packet_);
        }
    }

    AVCodecContext* context_ = nullptr;
    AVFrame* frame_ = nullptr;
    AVPacket* packet_ = nullptr;
    PacketOutput output_;
    std::vector<std::uint8_t> parameterSets_;
    std::int64_t lastPts_ = 0;
};

} // namespace

struct OpusAudioEncoder::Impl {
    std::mutex mutex;
    std::weak_ptr<IEncodedPacketSink> sink;
    std::unique_ptr<AudioEncodeCore> encoder;
    std::atomic<std::uint64_t> errors{0};

    void emit(const std::uint8_t* data,
              std::size_t size,
              std::int64_t ptsMicros,
              bool key) {
        auto sinkValue = [&] {
            std::lock_guard<std::mutex> lock(mutex);
            return sink.lock();
        }();
        if (!sinkValue) {
            return;
        }
        auto buffer = copyToMediaBuffer(data, size);
        if (!buffer.buffer) {
            errors.fetch_add(1);
            return;
        }
        EncodedPacket value;
        value.kind = MediaKind::Audio;
        value.codecType = CodecType::Opus;
        value.data = std::move(buffer);
        value.ptsMicros = ptsMicros;
        value.key = key;
        sinkValue->onEncodedPacket(value);
    }
};

OpusAudioEncoder::OpusAudioEncoder()
    : impl_(std::make_unique<Impl>()) {
}

OpusAudioEncoder::~OpusAudioEncoder() = default;

void OpusAudioEncoder::setSink(
    std::weak_ptr<IEncodedPacketSink> sink) noexcept {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->sink = std::move(sink);
}

Status OpusAudioEncoder::configure(const AudioEncoderConfig& config) {
    if (config.codecType != CodecType::Opus) {
        return failure("audio encoder requires Opus");
    }
    if (config.inputFormat.sampleRate <= 0 ||
        config.inputFormat.channels <= 0 ||
        config.inputFormat.sampleFormat != AudioSampleFormat::Signed16) {
        return failure(
            "Opus encoder requires positive signed-16 audio format");
    }
    auto encoder = std::make_unique<AudioEncodeCore>();
    auto result = encoder->configure(
        config,
        [this](const std::uint8_t* data,
               std::size_t size,
               std::int64_t ptsMicros,
               bool key) {
            impl_->emit(data, size, ptsMicros, key);
        });
    if (!result.ok) {
        return result;
    }
    impl_->encoder = std::move(encoder);
    return {};
}

Status OpusAudioEncoder::encode(const AudioFrame& frame) {
    if (!impl_->encoder) {
        return failure("Opus encoder is not configured");
    }
    if (!frame.samples.buffer || frame.format.channels <= 0 ||
        frame.format.sampleFormat != AudioSampleFormat::Signed16) {
        return failure("invalid Opus input frame");
    }
    const auto sampleBytes = audioSampleBytes(frame.format.sampleFormat);
    const auto bytesPerSample =
        static_cast<std::size_t>(frame.format.channels) * sampleBytes;
    const auto size = bufferViewSize(frame.samples);
    if (bytesPerSample == 0 || size % bytesPerSample != 0) {
        return failure("invalid Opus sample buffer size");
    }
    const auto result = impl_->encoder->encode(frame);
    if (!result.ok) {
        impl_->errors.fetch_add(1);
    }
    return result;
}

Status OpusAudioEncoder::flush() {
    if (!impl_->encoder) {
        return {};
    }
    return impl_->encoder->flush();
}

void OpusAudioEncoder::reset() noexcept {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->encoder.reset();
}

struct OpusAudioDecoder::Impl {
    struct SinkHolder {
        std::mutex mutex;
        std::weak_ptr<IAudioFrameSink> sink;
    };

    std::shared_ptr<SinkHolder> sink = std::make_shared<SinkHolder>();
    AVCodecContext* decoder = nullptr;
    SwrContext* resampler = nullptr;
    AudioFormat output;
    std::atomic<std::uint64_t> errors{0};

    Status configure(const AudioDecoderConfig& config) {
        resetCodec();
        if (config.codecType != CodecType::Opus) {
            return failure("audio decoder requires Opus");
        }
        if (config.outputFormat.sampleRate <= 0 ||
            config.outputFormat.channels <= 0) {
            return failure("invalid Opus output format");
        }
        const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_OPUS);
        if (!codec) {
            return failure("Opus decoder is unavailable");
        }
        decoder = avcodec_alloc_context3(codec);
        if (!decoder) {
            return failure("failed to allocate Opus decoder");
        }
        decoder->sample_rate = config.outputFormat.sampleRate;
        av_channel_layout_default(
            &decoder->ch_layout, config.outputFormat.channels);
        const auto result =
            avcodec_open2(decoder, codec, nullptr);
        if (result < 0) {
            resetCodec();
            return failure(
                "failed to open Opus decoder: " +
                ffmpegError(result));
        }
        output = config.outputFormat;
        return {};
    }

    Status decode(const EncodedPacket& value) {
        if (!decoder) {
            return failure("Opus decoder is not configured");
        }
        const auto* data = bufferViewData(value.data);
        const auto size = bufferViewSize(value.data);
        if (!data || size == 0) {
            return failure("empty Opus packet");
        }
        AVPacket* packet = av_packet_alloc();
        AVFrame* frame = av_frame_alloc();
        if (!packet || !frame) {
            av_packet_free(&packet);
            av_frame_free(&frame);
            return failure("failed to allocate Opus decoder buffers");
        }
        const auto alloc = av_new_packet(packet, static_cast<int>(size));
        if (alloc < 0) {
            av_packet_free(&packet);
            av_frame_free(&frame);
            return failure("failed to allocate Opus input packet");
        }
        std::memcpy(packet->data, data, size);
        packet->pts = value.ptsMicros;
        auto result = avcodec_send_packet(decoder, packet);
        av_packet_free(&packet);
        if (result < 0 && result != AVERROR(EAGAIN)) {
            av_frame_free(&frame);
            errors.fetch_add(1);
            return failure(
                "failed to submit Opus packet: " +
                ffmpegError(result));
        }

        while ((result = avcodec_receive_frame(decoder, frame)) >= 0) {
            if (!convertAndEmit(frame, value.ptsMicros)) {
                errors.fetch_add(1);
            }
            av_frame_unref(frame);
        }
        av_frame_free(&frame);
        if (result != AVERROR(EAGAIN) && result != AVERROR_EOF) {
            errors.fetch_add(1);
            return failure(
                "failed to decode Opus packet: " +
                ffmpegError(result));
        }
        return {};
    }

    bool convertAndEmit(const AVFrame* frame, std::int64_t fallbackPts) {
        if (!resampler) {
            AVChannelLayout inputLayout;
            AVChannelLayout outputLayout;
            if (frame->ch_layout.nb_channels > 0) {
                av_channel_layout_copy(&inputLayout, &frame->ch_layout);
            } else {
                av_channel_layout_default(&inputLayout, output.channels);
            }
            av_channel_layout_default(&outputLayout, output.channels);
            const auto created = swr_alloc_set_opts2(
                &resampler,
                &outputLayout, sampleFormat(output.sampleFormat),
                output.sampleRate,
                &inputLayout, static_cast<AVSampleFormat>(frame->format),
                frame->sample_rate, 0, nullptr);
            av_channel_layout_uninit(&inputLayout);
            av_channel_layout_uninit(&outputLayout);
            if (created < 0 || swr_init(resampler) < 0) {
                swr_free(&resampler);
                return false;
            }
        }
        const auto capacity = av_rescale_rnd(
            swr_get_delay(resampler, frame->sample_rate) +
                frame->nb_samples,
            output.sampleRate, frame->sample_rate, AV_ROUND_UP);
        if (capacity <= 0) {
            return false;
        }
        const auto sampleBytes = audioSampleBytes(output.sampleFormat);
        const auto byteCount = static_cast<std::size_t>(capacity) *
            static_cast<std::size_t>(output.channels) * sampleBytes;
        auto buffer = allocateMediaBuffer(byteCount);
        if (!buffer) {
            return false;
        }
        std::uint8_t* target[] = {buffer->data()};
        const auto input = const_cast<const std::uint8_t**>(
            frame->extended_data);
        const auto converted = swr_convert(
            resampler, target, capacity, input, frame->nb_samples);
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
        AudioFrame outputFrame;
        outputFrame.format = output;
        outputFrame.samples = MediaBufferView{
            std::move(buffer), 0,
            static_cast<std::size_t>(converted) *
                static_cast<std::size_t>(output.channels) * sampleBytes};
        outputFrame.ptsMicros =
            frame->pts == AV_NOPTS_VALUE ? fallbackPts : frame->pts;
        sinkValue->onAudioFrame(outputFrame);
        return true;
    }

    void resetCodec() noexcept {
        swr_free(&resampler);
        avcodec_free_context(&decoder);
        output = {};
    }
};

OpusAudioDecoder::OpusAudioDecoder()
    : impl_(std::make_unique<Impl>()) {
}

OpusAudioDecoder::~OpusAudioDecoder() {
    reset();
}

void OpusAudioDecoder::setSink(
    std::weak_ptr<IAudioFrameSink> sink) noexcept {
    std::lock_guard<std::mutex> lock(impl_->sink->mutex);
    impl_->sink->sink = std::move(sink);
}

Status OpusAudioDecoder::configure(const AudioDecoderConfig& config) {
    return impl_->configure(config);
}

Status OpusAudioDecoder::decode(const EncodedPacket& packet) {
    return impl_->decode(packet);
}

void OpusAudioDecoder::reset() noexcept {
    impl_->resetCodec();
}

struct H264VideoEncoder::Impl {
    std::mutex sinkMutex;
    std::weak_ptr<IEncodedPacketSink> sink;
    std::unique_ptr<VideoEncodeCore> encoder;
    std::atomic<std::uint64_t> errors{0};

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

        const auto parameterSets = encoder->parameterSets();
        const auto nals = splitH264Nals(data, size);
        const bool hasSps = containsH264Nal(nals, 7);
        const bool hasPps = containsH264Nal(nals, 8);
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
                std::move(buffer), 0,
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
};

H264VideoEncoder::H264VideoEncoder()
    : impl_(std::make_unique<Impl>()) {
}

H264VideoEncoder::~H264VideoEncoder() = default;

void H264VideoEncoder::setSink(
    std::weak_ptr<IEncodedPacketSink> sink) noexcept {
    std::lock_guard<std::mutex> lock(impl_->sinkMutex);
    impl_->sink = std::move(sink);
}

Status H264VideoEncoder::configure(const VideoEncoderConfig& config) {
    if (config.codecType != CodecType::H264) {
        return failure("video encoder requires H.264");
    }
    if (config.inputFormat.width <= 0 ||
        config.inputFormat.height <= 0 ||
        config.inputFormat.frameRate.numerator <= 0 ||
        config.inputFormat.frameRate.denominator <= 0 ||
        config.inputFormat.pixelFormat != VideoPixelFormat::I420) {
        return failure(
            "H.264 encoder requires positive I420 frame format");
    }
    auto encoder = std::make_unique<VideoEncodeCore>();
    auto result = encoder->configure(
        config,
        [this](const std::uint8_t* data,
               std::size_t size,
               std::int64_t ptsMicros,
               bool key) {
            impl_->emit(data, size, ptsMicros, key);
        });
    if (!result.ok) {
        return result;
    }
    impl_->encoder = std::move(encoder);
    return {};
}

Status H264VideoEncoder::encode(const VideoFrame& frame) {
    if (!impl_->encoder) {
        return failure("H.264 encoder is not configured");
    }
    const auto size = bufferViewSize(frame.pixels);
    const auto width = frame.format.width;
    const auto height = frame.format.height;
    if (!bufferViewData(frame.pixels) || width <= 0 || height <= 0 ||
        frame.format.pixelFormat != VideoPixelFormat::I420) {
        return failure("invalid H.264 I420 input frame");
    }
    const auto expected = static_cast<std::size_t>(width) * height * 3 / 2;
    if (size < expected) {
        return failure("truncated H.264 I420 input frame");
    }
    const auto result = impl_->encoder->encode(frame);
    if (!result.ok) {
        impl_->errors.fetch_add(1);
    }
    return result;
}

Status H264VideoEncoder::flush() {
    if (!impl_->encoder) {
        return {};
    }
    return impl_->encoder->flush();
}

Status H264VideoEncoder::setTargetBitrate(int) {
    return failure("dynamic H.264 bitrate is not implemented");
}

void H264VideoEncoder::reset() noexcept {
    std::lock_guard<std::mutex> lock(impl_->sinkMutex);
    impl_->encoder.reset();
}

struct H264SinkHolder {
    std::mutex mutex;
    std::weak_ptr<IVideoFrameSink> sink;
};

struct H264VideoDecoder::Impl {
    std::shared_ptr<H264SinkHolder> sink;
    AVCodecContext* decoder = nullptr;
    SwsContext* converter = nullptr;
    VideoPixelFormat outputFormat = VideoPixelFormat::I420;
    Rational frameRate{0, 1};
    std::atomic<std::uint64_t> errors{0};

    H264VideoDecoder::Impl();

    Status configure(const VideoDecoderConfig& config) {
        resetCodec();
        if (config.codecType != CodecType::H264) {
            return failure("video decoder requires H.264");
        }
        const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
        if (!codec) {
            return failure("H.264 decoder is unavailable");
        }
        decoder = avcodec_alloc_context3(codec);
        if (!decoder) {
            return failure("failed to allocate H.264 decoder");
        }
        decoder->thread_count = 1;
        const auto result = avcodec_open2(decoder, codec, nullptr);
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
            1, intParameter(
                config.parameters, "frameRateDenominator", 1));
        return {};
    }

    Status decode(const EncodedPacket& packet) {
        if (!decoder) {
            return failure("H.264 decoder is not configured");
        }
        const auto* data = bufferViewData(packet.data);
        const auto size = bufferViewSize(packet.data);
        if (!data || size == 0) {
            return failure("empty H.264 packet");
        }
        AVPacket* avPacket = av_packet_alloc();
        AVFrame* frame = av_frame_alloc();
        if (!avPacket || !frame) {
            av_packet_free(&avPacket);
            av_frame_free(&frame);
            return failure("failed to allocate H.264 decoder buffers");
        }
        const auto alloc =
            av_new_packet(avPacket, static_cast<int>(size));
        if (alloc < 0) {
            av_packet_free(&avPacket);
            av_frame_free(&frame);
            return failure("failed to allocate H.264 input packet");
        }
        std::memcpy(avPacket->data, data, size);
        avPacket->pts = packet.ptsMicros;
        auto result = avcodec_send_packet(decoder, avPacket);
        av_packet_free(&avPacket);
        if (result < 0 && result != AVERROR(EAGAIN)) {
            av_frame_free(&frame);
            errors.fetch_add(1);
            return failure(
                "failed to submit H.264 packet: " +
                ffmpegError(result));
        }
        while ((result = avcodec_receive_frame(decoder, frame)) >= 0) {
            if (!convertAndEmit(frame, packet.ptsMicros)) {
                errors.fetch_add(1);
            }
            av_frame_unref(frame);
        }
        av_frame_free(&frame);
        if (result != AVERROR(EAGAIN) && result != AVERROR_EOF) {
            errors.fetch_add(1);
            return failure(
                "failed to decode H.264 packet: " +
                ffmpegError(result));
        }
        return {};
    }

    bool convertAndEmit(const AVFrame* frame, std::int64_t fallbackPts) {
        const auto destinationFormat = pixelFormat(outputFormat);
        converter = sws_getCachedContext(
            converter,
            frame->width, frame->height,
            static_cast<AVPixelFormat>(frame->format),
            frame->width, frame->height, destinationFormat,
            SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!converter) {
            return false;
        }
        const auto byteCount = av_image_get_buffer_size(
            destinationFormat, frame->width, frame->height, 1);
        if (byteCount <= 0) {
            return false;
        }
        auto buffer = allocateMediaBuffer(
            static_cast<std::size_t>(byteCount));
        if (!buffer) {
            return false;
        }
        std::uint8_t* destination[3] = {buffer->data(), nullptr, nullptr};
        int strides[3] = {frame->width, 0, 0};
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
            converter, frame->data, frame->linesize,
            0, frame->height, destination, strides);
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
            std::move(buffer), 0,
            static_cast<std::size_t>(byteCount)};
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
    }
};

H264VideoDecoder::Impl::Impl()
    : sink(std::make_shared<H264SinkHolder>()) {
}

H264VideoDecoder::H264VideoDecoder()
    : impl_(std::make_unique<Impl>()) {
}

H264VideoDecoder::~H264VideoDecoder() {
    reset();
}

void H264VideoDecoder::setSink(
    std::weak_ptr<IVideoFrameSink> sink) noexcept {
    std::lock_guard<std::mutex> lock(impl_->sink->mutex);
    impl_->sink->sink = std::move(sink);
}

Status H264VideoDecoder::configure(const VideoDecoderConfig& config) {
    return impl_->configure(config);
}

Status H264VideoDecoder::decode(const EncodedPacket& packet) {
    return impl_->decode(packet);
}

void H264VideoDecoder::reset() noexcept {
    impl_->resetCodec();
}

std::unique_ptr<IAudioEncoder> createAudioEncoder(CodecType type) {
    if (type == CodecType::Opus) {
        return std::make_unique<OpusAudioEncoder>();
    }
    return {};
}

std::unique_ptr<IAudioDecoder> createAudioDecoder(CodecType type) {
    if (type == CodecType::Opus) {
        return std::make_unique<OpusAudioDecoder>();
    }
    return {};
}

std::unique_ptr<IVideoEncoder> createVideoEncoder(CodecType type) {
    if (type == CodecType::H264) {
        return std::make_unique<H264VideoEncoder>();
    }
    return {};
}

std::unique_ptr<IVideoDecoder> createVideoDecoder(CodecType type) {
    if (type == CodecType::H264) {
        return std::make_unique<H264VideoDecoder>();
    }
    return {};
}

} // namespace iris::media::v2
