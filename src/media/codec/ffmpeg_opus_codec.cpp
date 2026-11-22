#include "media/codec/ffmpeg_opus_codec.h"

#include "media/engine/media_buffer.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>

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

} // namespace

struct FFmpegOpusAudioEncoder::Impl {
    ~Impl() {
        reset();
    }

    Status configure(const AudioEncoderConfig& config) {
        reset();
        if (config.codecType != CodecType::Opus) {
            return failure("audio encoder requires Opus");
        }
        if (config.inputFormat.sampleRate <= 0 ||
            config.inputFormat.channels <= 0 ||
            config.inputFormat.sampleFormat !=
                AudioSampleFormat::Signed16) {
            return failure(
                "Opus encoder requires positive signed-16 audio format");
        }
        const AVCodec* codec =
            avcodec_find_encoder_by_name("libopus");
        if (!codec) {
            return failure("libopus encoder is unavailable");
        }
        context = avcodec_alloc_context3(codec);
        frame = av_frame_alloc();
        packet = av_packet_alloc();
        if (!context || !frame || !packet) {
            reset();
            return failure("failed to allocate Opus encoder");
        }
        context->sample_fmt = AV_SAMPLE_FMT_S16;
        context->sample_rate = config.inputFormat.sampleRate;
        av_channel_layout_default(
            &context->ch_layout, config.inputFormat.channels);
        context->bit_rate =
            std::max(16000, config.bitrateKbps * 1000);
        context->time_base =
            AVRational{1, config.inputFormat.sampleRate};
        context->frame_size = intParameter(
            config.parameters, "frameSamples",
            config.inputFormat.sampleRate / 50);
        const auto status = avcodec_open2(context, codec, nullptr);
        if (status < 0) {
            reset();
            return failure(
                "failed to open Opus encoder: " +
                ffmpegError(status));
        }
        return {};
    }

    Status encode(const AudioFrame& input) {
        if (!context) {
            return failure("Opus encoder is not configured");
        }
        const auto data = bufferViewData(input.samples);
        const auto size = bufferViewSize(input.samples);
        const auto bytesPerSample =
            static_cast<std::size_t>(input.format.channels) *
            audioSampleBytes(input.format.sampleFormat);
        if (!data || bytesPerSample == 0 ||
            size % bytesPerSample != 0 ||
            input.format.sampleFormat != AudioSampleFormat::Signed16) {
            return failure("invalid Opus input frame");
        }
        frame->format = AV_SAMPLE_FMT_S16;
        frame->sample_rate = input.format.sampleRate;
        av_channel_layout_default(
            &frame->ch_layout, input.format.channels);
        frame->nb_samples =
            static_cast<int>(size / bytesPerSample);
        frame->pts = input.ptsMicros;
        frame->data[0] = const_cast<std::uint8_t*>(data);
        frame->linesize[0] = static_cast<int>(bytesPerSample);
        lastPts = input.ptsMicros;
        const auto status = avcodec_send_frame(context, frame);
        if (status < 0) {
            errors.fetch_add(1);
            return failure(
                "failed to submit Opus frame: " +
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
                "failed to flush Opus encoder: " +
                ffmpegError(status));
        }
        return drain();
    }

    void reset() noexcept {
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
                    "failed to receive Opus packet: " +
                    ffmpegError(status));
            }
            emit(
                packet->data,
                static_cast<std::size_t>(packet->size),
                packet->pts == AV_NOPTS_VALUE
                    ? lastPts : packet->pts,
                (packet->flags & AV_PKT_FLAG_KEY) != 0);
            av_packet_unref(packet);
        }
    }

    std::mutex sinkMutex;
    std::weak_ptr<IEncodedPacketSink> sink;
    std::atomic<std::uint64_t> errors{0};
    AVCodecContext* context = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* packet = nullptr;
    std::int64_t lastPts = 0;
};

FFmpegOpusAudioEncoder::FFmpegOpusAudioEncoder()
    : impl_(std::make_unique<Impl>()) {
}

FFmpegOpusAudioEncoder::~FFmpegOpusAudioEncoder() = default;

void FFmpegOpusAudioEncoder::setSink(
    std::weak_ptr<IEncodedPacketSink> sink) noexcept {
    std::lock_guard<std::mutex> lock(impl_->sinkMutex);
    impl_->sink = std::move(sink);
}

Status FFmpegOpusAudioEncoder::configure(
    const AudioEncoderConfig& config) {
    return impl_->configure(config);
}

Status FFmpegOpusAudioEncoder::encode(const AudioFrame& frame) {
    return impl_->encode(frame);
}

Status FFmpegOpusAudioEncoder::flush() {
    return impl_->flush();
}

void FFmpegOpusAudioEncoder::reset() noexcept {
    std::lock_guard<std::mutex> lock(impl_->sinkMutex);
    impl_->reset();
}

struct FFmpegOpusAudioDecoder::Impl {
    struct SinkHolder {
        std::mutex mutex;
        std::weak_ptr<IAudioFrameSink> sink;
    };

    ~Impl() {
        resetCodec();
    }

    std::shared_ptr<SinkHolder> sink =
        std::make_shared<SinkHolder>();
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
        const AVCodec* codec =
            avcodec_find_decoder(AV_CODEC_ID_OPUS);
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
        AVPacket* input = av_packet_alloc();
        AVFrame* decoded = av_frame_alloc();
        if (!input || !decoded) {
            av_packet_free(&input);
            av_frame_free(&decoded);
            return failure(
                "failed to allocate Opus decoder buffers");
        }
        const auto alloc =
            av_new_packet(input, static_cast<int>(size));
        if (alloc < 0) {
            av_packet_free(&input);
            av_frame_free(&decoded);
            return failure(
                "failed to allocate Opus input packet");
        }
        std::memcpy(input->data, data, size);
        input->pts = value.ptsMicros;
        auto result = avcodec_send_packet(decoder, input);
        av_packet_free(&input);
        if (result < 0 && result != AVERROR(EAGAIN)) {
            av_frame_free(&decoded);
            errors.fetch_add(1);
            return failure(
                "failed to submit Opus packet: " +
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
                "failed to decode Opus packet: " +
                ffmpegError(result));
        }
        return {};
    }

    bool convertAndEmit(const AVFrame* frame,
                        std::int64_t fallbackPts) {
        if (!resampler) {
            AVChannelLayout inputLayout;
            AVChannelLayout outputLayout;
            if (frame->ch_layout.nb_channels > 0) {
                av_channel_layout_copy(
                    &inputLayout, &frame->ch_layout);
            } else {
                av_channel_layout_default(
                    &inputLayout, output.channels);
            }
            av_channel_layout_default(
                &outputLayout, output.channels);
            const auto created = swr_alloc_set_opts2(
                &resampler,
                &outputLayout,
                sampleFormat(output.sampleFormat),
                output.sampleRate,
                &inputLayout,
                static_cast<AVSampleFormat>(frame->format),
                frame->sample_rate,
                0,
                nullptr);
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
            output.sampleRate,
            frame->sample_rate,
            AV_ROUND_UP);
        if (capacity <= 0) {
            return false;
        }
        const auto sampleBytes =
            audioSampleBytes(output.sampleFormat);
        const auto byteCount =
            static_cast<std::size_t>(capacity) *
            static_cast<std::size_t>(output.channels) *
            sampleBytes;
        auto buffer = allocateMediaBuffer(byteCount);
        if (!buffer) {
            return false;
        }
        std::uint8_t* target[] = {buffer->data()};
        const auto input = const_cast<const std::uint8_t**>(
            frame->extended_data);
        const auto converted = swr_convert(
            resampler,
            target,
            capacity,
            input,
            frame->nb_samples);
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
            std::move(buffer),
            0,
            static_cast<std::size_t>(converted) *
                static_cast<std::size_t>(output.channels) *
                sampleBytes};
        outputFrame.ptsMicros =
            frame->pts == AV_NOPTS_VALUE
                ? fallbackPts : frame->pts;
        sinkValue->onAudioFrame(outputFrame);
        return true;
    }

    void resetCodec() noexcept {
        swr_free(&resampler);
        avcodec_free_context(&decoder);
        output = {};
    }
};

FFmpegOpusAudioDecoder::FFmpegOpusAudioDecoder()
    : impl_(std::make_unique<Impl>()) {
}

FFmpegOpusAudioDecoder::~FFmpegOpusAudioDecoder() = default;

void FFmpegOpusAudioDecoder::setSink(
    std::weak_ptr<IAudioFrameSink> sink) noexcept {
    std::lock_guard<std::mutex> lock(impl_->sink->mutex);
    impl_->sink->sink = std::move(sink);
}

Status FFmpegOpusAudioDecoder::configure(
    const AudioDecoderConfig& config) {
    return impl_->configure(config);
}

Status FFmpegOpusAudioDecoder::decode(
    const EncodedPacket& packet) {
    return impl_->decode(packet);
}

void FFmpegOpusAudioDecoder::reset() noexcept {
    impl_->resetCodec();
}

} // namespace iris::media::v2
