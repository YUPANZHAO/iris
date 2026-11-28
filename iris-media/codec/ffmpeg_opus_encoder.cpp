#include "codec/ffmpeg_opus_encoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/samplefmt.h>
}

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

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

std::size_t audioSampleBytes(AudioSampleFormat format) {
    return format == AudioSampleFormat::Float32
        ? sizeof(float)
        : sizeof(std::int16_t);
}

} // namespace

struct FFmpegOpusEncoder::Impl {
    ~Impl() {
        reset();
    }

    Status configure(const AudioEncoderConfig& config) {
        reset();
        if (config.codecType != CodecType::Opus) {
            return Status::failure("Opus encoder requires Opus configuration");
        }
        const auto& format = config.inputFormat;
        if (format.sampleRate <= 0 || format.channels <= 0 ||
            format.sampleFormat != AudioSampleFormat::Signed16) {
            return Status::failure(
                "Opus encoder requires positive signed-16 audio format");
        }
        const AVCodec* codec = avcodec_find_encoder_by_name("libopus");
        if (!codec) {
            return Status::failure("libopus encoder is unavailable");
        }
        context = avcodec_alloc_context3(codec);
        frame = av_frame_alloc();
        packet = av_packet_alloc();
        if (!context || !frame || !packet) {
            reset();
            return Status::failure("failed to allocate Opus encoder");
        }
        context->sample_fmt = AV_SAMPLE_FMT_S16;
        context->sample_rate = format.sampleRate;
        av_channel_layout_default(&context->ch_layout, format.channels);
        context->bit_rate =
            std::max(16000, config.bitrateKbps * 1000);
        context->time_base = AVRational{1, 1000000};
        context->frame_size = std::max(
            1,
            intParameter(
                config.parameters, "frameSamples", format.sampleRate / 50));
        const auto status = avcodec_open2(context, codec, nullptr);
        if (status < 0) {
            reset();
            return Status::failure(
                "failed to open Opus encoder: " +
                ffmpegError(status));
        }
        inputFormat = format;
        return {};
    }

    Status encode(const AudioRawFrame& audio) {
        if (!context) {
            return Status::failure("Opus encoder is not configured");
        }
        if (!audio.samples) {
            return Status::failure("Opus encoder received an empty buffer");
        }
        const auto* data =
            static_cast<const std::uint8_t*>(audio.samples->data());
        const auto size = audio.samples->size();
        const auto& format = audio.format;
        const auto bytesPerSample =
            static_cast<std::size_t>(format.channels) *
            audioSampleBytes(format.sampleFormat);
        if (!data || bytesPerSample == 0 ||
            size % bytesPerSample != 0 ||
            format.sampleFormat != AudioSampleFormat::Signed16 ||
            format.sampleRate != inputFormat.sampleRate ||
            format.channels != inputFormat.channels) {
            return Status::failure("invalid Opus signed-16 input frame");
        }
        lastPts = audio.ptsMicros;
        lastStreamId = audio.streamId;
        if (pending.empty()) {
            pendingPtsMicros = audio.ptsMicros;
        }
        pending.insert(pending.end(), data, data + size);

        const auto frameSamples = std::max(1, context->frame_size);
        const auto frameBytes =
            static_cast<std::size_t>(frameSamples) * bytesPerSample;
        std::size_t offset = 0;
        while (pending.size() - offset >= frameBytes) {
            auto status = sendFrame(
                pending.data() + offset,
                frameSamples,
                bytesPerSample,
                pendingPtsMicros);
            if (!status.ok) {
                return status;
            }
            offset += frameBytes;
            pendingPtsMicros +=
                static_cast<std::int64_t>(frameSamples) * 1000000 /
                format.sampleRate;
        }
        if (offset != 0) {
            pending.erase(
                pending.begin(),
                pending.begin() + static_cast<std::ptrdiff_t>(offset));
        }
        return {};
    }

    Status flush() {
        if (!context) {
            return {};
        }
        if (!pending.empty()) {
            const auto bytesPerSample =
                static_cast<std::size_t>(inputFormat.channels) *
                sizeof(std::int16_t);
            const auto frameSamples =
                std::max(1, context->frame_size);
            const auto frameBytes =
                static_cast<std::size_t>(frameSamples) * bytesPerSample;
            pending.resize(frameBytes, 0);
            const auto submitted = sendFrame(
                pending.data(),
                frameSamples,
                bytesPerSample,
                pendingPtsMicros);
            pending.clear();
            if (!submitted.ok) {
                return submitted;
            }
        }
        const auto status = avcodec_send_frame(context, nullptr);
        if (status < 0 && status != AVERROR_EOF) {
            return Status::failure(
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
        lastStreamId = 0;
        pending.clear();
        pendingPtsMicros = 0;
        inputFormat = {};
    }

    Status sendFrame(
        const std::uint8_t* data,
        int samples,
        std::size_t bytesPerSample,
        std::int64_t ptsMicros) {
        frame->format = AV_SAMPLE_FMT_S16;
        frame->sample_rate = inputFormat.sampleRate;
        av_channel_layout_default(
            &frame->ch_layout, inputFormat.channels);
        frame->nb_samples = samples;
        frame->pts = ptsMicros;
        frame->data[0] = const_cast<std::uint8_t*>(data);
        frame->linesize[0] = static_cast<int>(bytesPerSample);
        auto status = avcodec_send_frame(context, frame);
        if (status == AVERROR(EAGAIN)) {
            const auto drained = drain();
            if (!drained.ok) {
                return drained;
            }
            status = avcodec_send_frame(context, frame);
        }
        if (status < 0) {
            errors.fetch_add(1);
            return Status::failure(
                "failed to submit Opus frame: " +
                ffmpegError(status));
        }
        return drain();
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
        auto buffer = std::make_shared<ByteBuffer>(data, size);
        if (!buffer || !*buffer) {
            errors.fetch_add(1);
            return;
        }
        EncodedPacket value;
        value.streamId = lastStreamId;
        value.kind = MediaKind::Audio;
        value.codecType = CodecType::Opus;
        value.data = std::move(buffer);
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
                return Status::failure(
                    "failed to receive Opus packet: " +
                    ffmpegError(status));
            }
            emit(
                packet->data,
                static_cast<std::size_t>(packet->size),
                packet->pts == AV_NOPTS_VALUE ? lastPts : packet->pts,
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
    StreamId lastStreamId = 0;
    std::vector<std::uint8_t> pending;
    std::int64_t pendingPtsMicros = 0;
    AudioFormat inputFormat;
};

FFmpegOpusEncoder::FFmpegOpusEncoder()
    : impl_(std::make_unique<Impl>()) {
}

FFmpegOpusEncoder::~FFmpegOpusEncoder() = default;

FFmpegOpusEncoder::FFmpegOpusEncoder(FFmpegOpusEncoder&&) noexcept = default;

FFmpegOpusEncoder& FFmpegOpusEncoder::operator=(
    FFmpegOpusEncoder&&) noexcept = default;

Status FFmpegOpusEncoder::configure(const AudioEncoderConfig& config) {
    return impl_->configure(config);
}

Status FFmpegOpusEncoder::encode(const AudioRawFrame& frame) {
    return impl_->encode(frame);
}

Status FFmpegOpusEncoder::flush() {
    return impl_->flush();
}

void FFmpegOpusEncoder::setEncodedPacketSink(
    std::weak_ptr<IEncodedPacketSink> sink) noexcept {
    std::lock_guard<std::mutex> lock(impl_->sinkMutex);
    impl_->sink = std::move(sink);
}

void FFmpegOpusEncoder::reset() noexcept {
    std::lock_guard<std::mutex> lock(impl_->sinkMutex);
    impl_->reset();
}

} // namespace iris::media::codec
