#include "media/backend/ffmpeg/ffmpeg_video_encoder.h"

#include "media/pipeline/h264_bitstream.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/opt.h>
#include <libavutil/pixfmt.h>
}

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

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

struct FfmpegVideoEncoder::Impl {
    MediaResult configure(const pipeline::VideoEncoderConfig& config,
                          pipeline::EncodedPacketCallback callback) {
        reset();
        if (config.width <= 0 || config.height <= 0 ||
            config.framesPerSecond <= 0 || !callback) {
            return MediaResult::failure(
                "invalid H.264 encoder configuration");
        }
        const AVCodec* codec =
            avcodec_find_encoder_by_name("libopenh264");
        if (!codec) {
            return MediaResult::failure(
                "libopenh264 encoder is unavailable");
        }
        context = avcodec_alloc_context3(codec);
        if (!context) {
            return MediaResult::failure(
                "failed to allocate H.264 encoder");
        }
        context->width = config.width;
        context->height = config.height;
        context->pix_fmt = AV_PIX_FMT_YUV420P;
        context->time_base = AVRational{1, config.framesPerSecond};
        context->framerate =
            AVRational{config.framesPerSecond, 1};
        context->bit_rate = std::max(100000, config.bitrate);
        context->gop_size = std::max(1, config.gopSize);
        context->max_b_frames = 0;
        context->thread_count = 1;
        context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        av_opt_set_int(
            context->priv_data, "allow_skip_frames", 1, 0);
        const int status = avcodec_open2(context, codec, nullptr);
        if (status < 0) {
            reset();
            return MediaResult::failure(
                "failed to open OpenH264 encoder: " +
                ffmpegError(status));
        }
        inputFrame = av_frame_alloc();
        packet = av_packet_alloc();
        if (!inputFrame || !packet) {
            reset();
            return MediaResult::failure(
                "failed to allocate H.264 encoder buffers");
        }
        onPacket = std::move(callback);
        parameterSets = pipeline::h264ParameterSets({
            context->extradata,
            context->extradata_size > 0
                ? static_cast<std::size_t>(context->extradata_size)
                : 0,
        });
        return MediaResult::success();
    }

    MediaResult encode(const pipeline::VideoFrameView& frame) {
        if (!context || !onPacket) {
            return MediaResult::failure(
                "H.264 encoder is not configured");
        }
        if (!frame.data[0] || !frame.data[1] || !frame.data[2] ||
            frame.width <= 0 || frame.height <= 0) {
            return MediaResult::failure("invalid I420 video frame");
        }

        if (!inputFrame) {
            return MediaResult::failure(
                "H.264 input frame is unavailable");
        }
        inputFrame->format = AV_PIX_FMT_YUV420P;
        inputFrame->width = frame.width;
        inputFrame->height = frame.height;
        inputFrame->pts = frame.pts;
        for (int index = 0; index < 3; ++index) {
            inputFrame->data[index] =
                const_cast<std::uint8_t*>(frame.data[index]);
            inputFrame->linesize[index] = frame.stride[index];
        }
        lastInputPts = frame.pts;

        const int status =
            avcodec_send_frame(context, inputFrame);
        if (status < 0) {
            return MediaResult::failure(
                "failed to submit video frame: " +
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
                "failed to flush video encoder: " +
                ffmpegError(status));
        }
        return drainPackets();
    }

    MediaResult drainPackets() {
        if (!packet) {
            return MediaResult::failure(
                "encoded video packet buffer is unavailable");
        }
        MediaResult result = MediaResult::success();
        while (true) {
            const int status = avcodec_receive_packet(context, packet);
            if (status == AVERROR(EAGAIN) || status == AVERROR_EOF) {
                break;
            }
            if (status < 0) {
                result = MediaResult::failure(
                    "failed to receive H.264 packet: " +
                    ffmpegError(status));
                break;
            }
            onPacket(pipeline::EncodedPacketView{
                packet->data,
                static_cast<std::size_t>(packet->size),
                packet->pts == AV_NOPTS_VALUE
                    ? lastInputPts
                    : packet->pts,
                (packet->flags & AV_PKT_FLAG_KEY) != 0 ||
                    pipeline::detectH264KeyFrame(
                        packet->data,
                        static_cast<std::size_t>(packet->size)),
            });
            av_packet_unref(packet);
        }
        return result;
    }

    void reset() noexcept {
        onPacket = {};
        parameterSets.clear();
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
    std::vector<std::uint8_t> parameterSets;
    std::int64_t lastInputPts = 0;
};

FfmpegVideoEncoder::FfmpegVideoEncoder()
    : impl_(std::make_unique<Impl>()) {
}

FfmpegVideoEncoder::~FfmpegVideoEncoder() = default;

MediaResult FfmpegVideoEncoder::configure(
    const pipeline::VideoEncoderConfig& config,
    pipeline::EncodedPacketCallback callback) {
    return impl_->configure(config, std::move(callback));
}

MediaResult FfmpegVideoEncoder::encode(
    const pipeline::VideoFrameView& frame) {
    return impl_->encode(frame);
}

MediaResult FfmpegVideoEncoder::flush() {
    return impl_->flush();
}

ByteView FfmpegVideoEncoder::parameterSets() const noexcept {
    return {
        impl_->parameterSets.data(),
        impl_->parameterSets.size(),
    };
}

} // namespace iris::media
