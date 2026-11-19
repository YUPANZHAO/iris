#include "media/backend/ffmpeg/ffmpeg_audio_encoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/samplefmt.h>
}

#include <algorithm>
#include <string>
#include <utility>

namespace iris::media {
namespace {

std::string ffmpegError(int error) {
    char message[AV_ERROR_MAX_STRING_SIZE]{};
    if (av_strerror(error, message, sizeof(message)) < 0) {
        return "unknown FFmpeg error";
    }
    return message;
}

} // namespace

struct FfmpegAudioEncoder::Impl {
    MediaResult configure(const pipeline::AudioEncoderConfig& config,
                          pipeline::EncodedPacketCallback callback) {
        reset();
        if (config.sampleRate <= 0 || config.channels <= 0 ||
            config.frameSamples <= 0 || !callback) {
            return MediaResult::failure(
                "invalid Opus encoder configuration");
        }
        const AVCodec* codec = avcodec_find_encoder_by_name("libopus");
        if (!codec) {
            return MediaResult::failure(
                "libopus encoder is unavailable");
        }
        context = avcodec_alloc_context3(codec);
        if (!context) {
            return MediaResult::failure(
                "failed to allocate Opus encoder");
        }
        context->sample_fmt = AV_SAMPLE_FMT_S16;
        context->sample_rate = config.sampleRate;
        av_channel_layout_default(
            &context->ch_layout, config.channels);
        context->bit_rate = std::max(16000, config.bitrate);
        context->time_base = AVRational{1, config.sampleRate};
        context->frame_size = config.frameSamples;
        const int status = avcodec_open2(context, codec, nullptr);
        if (status < 0) {
            reset();
            return MediaResult::failure(
                "failed to open Opus encoder: " + ffmpegError(status));
        }
        inputFrame = av_frame_alloc();
        packet = av_packet_alloc();
        if (!inputFrame || !packet) {
            reset();
            return MediaResult::failure(
                "failed to allocate Opus encoder buffers");
        }
        onPacket = std::move(callback);
        return MediaResult::success();
    }

    MediaResult encode(const pipeline::AudioFrameView& frame) {
        if (!context || !onPacket) {
            return MediaResult::failure("Opus encoder is not configured");
        }
        const auto expected =
            static_cast<std::size_t>(frame.samples) *
            static_cast<std::size_t>(frame.channels) *
            sizeof(std::int16_t);
        if (!frame.data || frame.size < expected || frame.samples <= 0 ||
            frame.channels <= 0 || frame.sampleRate <= 0 ||
            frame.format != pipeline::AudioSampleFormat::Signed16) {
            return MediaResult::failure("invalid signed-16 audio frame");
        }

        if (!inputFrame) {
            return MediaResult::failure(
                "Opus input frame is unavailable");
        }
        inputFrame->format = AV_SAMPLE_FMT_S16;
        inputFrame->sample_rate = frame.sampleRate;
        av_channel_layout_default(
            &inputFrame->ch_layout, frame.channels);
        inputFrame->nb_samples = frame.samples;
        inputFrame->pts = frame.pts;
        inputFrame->data[0] =
            const_cast<std::uint8_t*>(frame.data);
        inputFrame->linesize[0] = static_cast<int>(
            frame.channels * sizeof(std::int16_t));
        lastInputPts = frame.pts;

        const int status =
            avcodec_send_frame(context, inputFrame);
        if (status < 0) {
            return MediaResult::failure(
                "failed to submit audio frame: " +
                ffmpegError(status));
        }
        return drainPackets();
    }

    MediaResult flush() {
        if (!context || !onPacket) {
            return MediaResult::success();
        }
        const int status = avcodec_send_frame(context, nullptr);
        if (status < 0 && status != AVERROR_EOF) {
            return MediaResult::failure(
                "failed to flush audio encoder: " +
                ffmpegError(status));
        }
        return drainPackets();
    }

    MediaResult drainPackets() {
        if (!packet) {
            return MediaResult::failure(
                "audio packet buffer is unavailable");
        }
        MediaResult result = MediaResult::success();
        while (true) {
            const int status = avcodec_receive_packet(context, packet);
            if (status == AVERROR(EAGAIN) || status == AVERROR_EOF) {
                break;
            }
            if (status < 0) {
                result = MediaResult::failure(
                    "failed to receive Opus packet: " +
                    ffmpegError(status));
                break;
            }
            onPacket(pipeline::EncodedPacketView{
                packet->data,
                static_cast<std::size_t>(packet->size),
                packet->pts == AV_NOPTS_VALUE
                    ? lastInputPts
                    : packet->pts,
                (packet->flags & AV_PKT_FLAG_KEY) != 0,
            });
            av_packet_unref(packet);
        }
        return result;
    }

    void reset() noexcept {
        onPacket = {};
        lastInputPts = 0;
        av_packet_free(&packet);
        av_frame_free(&inputFrame);
        avcodec_free_context(&context);
    }

    ~Impl() noexcept {
        reset();
    }

    AVCodecContext* context = nullptr;
    AVFrame* inputFrame = nullptr;
    AVPacket* packet = nullptr;
    pipeline::EncodedPacketCallback onPacket;
    std::int64_t lastInputPts = 0;
};

FfmpegAudioEncoder::FfmpegAudioEncoder()
    : impl_(std::make_unique<Impl>()) {
}

FfmpegAudioEncoder::~FfmpegAudioEncoder() = default;

MediaResult FfmpegAudioEncoder::configure(
    const pipeline::AudioEncoderConfig& config,
    pipeline::EncodedPacketCallback callback) {
    return impl_->configure(config, std::move(callback));
}

MediaResult FfmpegAudioEncoder::encode(
    const pipeline::AudioFrameView& frame) {
    return impl_->encode(frame);
}

MediaResult FfmpegAudioEncoder::flush() {
    return impl_->flush();
}

} // namespace iris::media
