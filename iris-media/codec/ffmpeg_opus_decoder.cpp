#include "codec/ffmpeg_opus_decoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

#include <algorithm>
#include <atomic>
#include <climits>
#include <cstdint>
#include <cstring>
#include <memory>
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

struct FFmpegOpusDecoder::Impl {
    struct SinkHolder {
        std::mutex mutex;
        std::weak_ptr<IAudioFrameSink> sink;
    };

    ~Impl() {
        resetCodec();
    }

    std::shared_ptr<SinkHolder> sink = std::make_shared<SinkHolder>();
    AVCodecContext* decoder = nullptr;
    SwrContext* resampler = nullptr;
    AudioFormat output;
    std::atomic<std::uint64_t> errors{0};
    StreamId streamId = 0;

    Status configure(const AudioDecoderConfig& config) {
        resetCodec();
        if (config.codecType != CodecType::Opus) {
            return Status::failure("Opus decoder requires Opus configuration");
        }
        if (config.outputFormat.sampleRate <= 0 ||
            config.outputFormat.channels <= 0) {
            return Status::failure("invalid Opus output format");
        }
        const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_OPUS);
        if (!codec) {
            return Status::failure("Opus decoder is unavailable");
        }
        decoder = avcodec_alloc_context3(codec);
        if (!decoder) {
            return Status::failure("failed to allocate Opus decoder");
        }
        decoder->sample_rate = config.outputFormat.sampleRate;
        av_channel_layout_default(
            &decoder->ch_layout, config.outputFormat.channels);
        const auto result = avcodec_open2(decoder, codec, nullptr);
        if (result < 0) {
            resetCodec();
            return Status::failure(
                "failed to open Opus decoder: " +
                ffmpegError(result));
        }
        output = config.outputFormat;
        return {};
    }

    Status decode(const EncodedPacket& value) {
        if (!decoder) {
            return Status::failure("Opus decoder is not configured");
        }
        if (value.kind != MediaKind::Audio ||
            value.codecType != CodecType::Opus) {
            return Status::failure("Opus decoder requires an Opus audio packet");
        }
        if (!value.data) {
            return Status::failure("Opus decoder received an empty buffer");
        }
        const auto* data =
            static_cast<const std::uint8_t*>(value.data->data());
        const auto size = value.data->size();
        if (!data || size == 0 || size > INT_MAX) {
            return Status::failure("invalid Opus packet");
        }
        streamId = value.streamId;

        AVPacket* input = av_packet_alloc();
        if (!input) {
            return Status::failure("failed to allocate Opus input packet");
        }
        const auto alloc = av_new_packet(input, static_cast<int>(size));
        if (alloc < 0) {
            av_packet_free(&input);
            return Status::failure("failed to allocate Opus packet payload");
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
                return Status::failure("failed to allocate Opus retry packet");
            }
            const auto retryAlloc =
                av_new_packet(retry, static_cast<int>(size));
            if (retryAlloc < 0) {
                av_packet_free(&retry);
                return Status::failure("failed to allocate Opus retry payload");
            }
            std::memcpy(retry->data, data, size);
            retry->pts = value.ptsMicros;
            result = avcodec_send_packet(decoder, retry);
            av_packet_free(&retry);
        }
        if (result < 0 && result != AVERROR_EOF) {
            errors.fetch_add(1);
            return Status::failure(
                "failed to submit Opus packet: " +
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
                "failed to flush Opus decoder: " +
                ffmpegError(result));
        }
        return drain(0);
    }

    Status drain(std::int64_t fallbackPts) {
        AVFrame* decoded = av_frame_alloc();
        if (!decoded) {
            return Status::failure("failed to allocate Opus output frame");
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
                    "failed to decode Opus packet: " +
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
        if (!resampler) {
            AVChannelLayout inputLayout;
            AVChannelLayout outputLayout;
            std::memset(&inputLayout, 0, sizeof(inputLayout));
            std::memset(&outputLayout, 0, sizeof(outputLayout));
            if (frame->ch_layout.nb_channels > 0) {
                av_channel_layout_copy(&inputLayout, &frame->ch_layout);
            } else {
                av_channel_layout_default(&inputLayout, output.channels);
            }
            av_channel_layout_default(&outputLayout, output.channels);
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
        const auto capacity64 = av_rescale_rnd(
            swr_get_delay(resampler, frame->sample_rate) +
                frame->nb_samples,
            output.sampleRate,
            frame->sample_rate,
            AV_ROUND_UP);
        if (capacity64 <= 0 || capacity64 > INT_MAX) {
            return false;
        }
        const auto capacity = static_cast<int>(capacity64);
        const auto sampleBytes = audioSampleBytes(output.sampleFormat);
        const auto byteCount =
            static_cast<std::size_t>(capacity) *
            static_cast<std::size_t>(output.channels) * sampleBytes;
        auto buffer = std::make_shared<ByteBuffer>(byteCount);
        if (!buffer || !*buffer) {
            return false;
        }
        std::uint8_t* target[] = {
            static_cast<std::uint8_t*>(buffer->data())};
        const auto input =
            const_cast<const std::uint8_t**>(frame->extended_data);
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

        AudioRawFrame outputFrame;
        outputFrame.streamId = streamId;
        outputFrame.format = output;
        outputFrame.samples = std::move(buffer);
        if (!outputFrame.samples->setSize(
                static_cast<std::size_t>(converted) *
                static_cast<std::size_t>(output.channels) * sampleBytes)) {
            return false;
        }
        outputFrame.ptsMicros =
            frame->pts == AV_NOPTS_VALUE ? fallbackPts : frame->pts;
        sinkValue->onAudioFrame(outputFrame);
        return true;
    }

    void resetCodec() noexcept {
        swr_free(&resampler);
        avcodec_free_context(&decoder);
        output = {};
        streamId = 0;
    }
};

FFmpegOpusDecoder::FFmpegOpusDecoder()
    : impl_(std::make_unique<Impl>()) {
}

FFmpegOpusDecoder::~FFmpegOpusDecoder() = default;

FFmpegOpusDecoder::FFmpegOpusDecoder(FFmpegOpusDecoder&&) noexcept = default;

FFmpegOpusDecoder& FFmpegOpusDecoder::operator=(
    FFmpegOpusDecoder&&) noexcept = default;

Status FFmpegOpusDecoder::configure(const AudioDecoderConfig& config) {
    return impl_->configure(config);
}

Status FFmpegOpusDecoder::decode(const EncodedPacket& packet) {
    return impl_->decode(packet);
}

Status FFmpegOpusDecoder::flush() {
    return impl_->flush();
}

void FFmpegOpusDecoder::setRawFrameSink(
    std::weak_ptr<IAudioFrameSink> sink) noexcept {
    std::lock_guard<std::mutex> lock(impl_->sink->mutex);
    impl_->sink->sink = std::move(sink);
}

void FFmpegOpusDecoder::reset() noexcept {
    std::lock_guard<std::mutex> lock(impl_->sink->mutex);
    impl_->resetCodec();
}

} // namespace iris::media::codec
