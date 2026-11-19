#include "media/backend/ffmpeg/ffmpeg_capture_reader.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
}

#include <atomic>
#include <string>
#include <utility>

namespace iris::media {
namespace {

int interruptCallback(void* opaque) {
    return !static_cast<std::atomic<bool>*>(opaque)->load();
}

std::string ffmpegError(int error) {
    char message[AV_ERROR_MAX_STRING_SIZE]{};
    if (av_strerror(error, message, sizeof(message)) < 0) {
        return "unknown FFmpeg error";
    }
    return message;
}

} // namespace

struct FfmpegCaptureReader::Impl {
    MediaResult open(const CaptureSourceSpec& value,
                     const CaptureReaderConfig& settings) {
        close();
        source = value;
        config = settings;
        if (source.isNull) {
            return MediaResult::failure(
                "null capture sources do not use a reader");
        }
        const AVInputFormat* inputFormat =
            av_find_input_format(source.format.c_str());
        AVDictionary* options = nullptr;
        if (source.format == "lavfi") {
            av_dict_set(&options, "realtime", "1", 0);
            if (config.kind == MediaKind::Video) {
                av_dict_set(
                    &options,
                    "video_size",
                    (std::to_string(config.videoWidth) + "x" +
                     std::to_string(config.videoHeight)).c_str(),
                    0);
                av_dict_set(
                    &options,
                    "framerate",
                    std::to_string(config.videoFps).c_str(),
                    0);
            }
        }
        running = true;
        const int status = avformat_open_input(
            &input, source.url.c_str(), inputFormat, &options);
        av_dict_free(&options);
        if (status < 0) {
            close();
            return MediaResult::failure(
                "failed to open media input " + source.url +
                ": " + ffmpegError(status));
        }
        input->interrupt_callback.callback = &interruptCallback;
        input->interrupt_callback.opaque = &running;

        const auto mediaType = config.kind == MediaKind::Audio
            ? AVMEDIA_TYPE_AUDIO
            : AVMEDIA_TYPE_VIDEO;
        streamIndex = av_find_best_stream(
            input, mediaType, -1, -1, nullptr, 0);
        if (streamIndex < 0 ||
            avformat_find_stream_info(input, nullptr) < 0) {
            close();
            return MediaResult::failure(
                config.kind == MediaKind::Audio
                    ? "audio input has no audio stream"
                    : "video input has no video stream");
        }
        auto* stream = input->streams[streamIndex];
        const AVCodec* codec =
            avcodec_find_decoder(stream->codecpar->codec_id);
        if (!codec) {
            close();
            return MediaResult::failure(
                "capture input has no decoder");
        }
        decoder = avcodec_alloc_context3(codec);
        if (!decoder ||
            avcodec_parameters_to_context(
                decoder, stream->codecpar) < 0 ||
            avcodec_open2(decoder, codec, nullptr) < 0) {
            close();
            return MediaResult::failure(
                "failed to open capture decoder");
        }
        packet = av_packet_alloc();
        frame = av_frame_alloc();
        if (!packet || !frame) {
            close();
            return MediaResult::failure(
                "failed to allocate capture buffers");
        }
        return MediaResult::success();
    }

    bool read(const FrameCallback& callback) {
        if (!input || !decoder || !packet || !frame ||
            !running || !callback) {
            return false;
        }
        while (running && av_read_frame(input, packet) >= 0) {
            bool processed = false;
            if (packet->stream_index == streamIndex) {
                if (avcodec_send_packet(decoder, packet) >= 0) {
                    while (running &&
                           avcodec_receive_frame(decoder, frame) >= 0) {
                        callback(*frame);
                        av_frame_unref(frame);
                        processed = true;
                    }
                }
            }
            av_packet_unref(packet);
            if (processed) {
                return true;
            }
        }
        return false;
    }

    void abortInput() noexcept {
        running.store(false, std::memory_order_release);
    }

    void close() noexcept {
        running.store(false, std::memory_order_release);
        av_packet_free(&packet);
        av_frame_free(&frame);
        avcodec_free_context(&decoder);
        if (input) {
            avformat_close_input(&input);
        }
        streamIndex = -1;
    }

    CaptureSourceSpec source;
    CaptureReaderConfig config;
    AVFormatContext* input = nullptr;
    AVCodecContext* decoder = nullptr;
    AVPacket* packet = nullptr;
    AVFrame* frame = nullptr;
    int streamIndex = -1;
    std::atomic<bool> running{false};
};

FfmpegCaptureReader::FfmpegCaptureReader()
    : impl_(std::make_unique<Impl>()) {
}

FfmpegCaptureReader::~FfmpegCaptureReader() {
    close();
}

MediaResult FfmpegCaptureReader::open(
    const CaptureSourceSpec& source,
    const CaptureReaderConfig& config) {
    return impl_->open(source, config);
}

bool FfmpegCaptureReader::read(const FrameCallback& callback) {
    return impl_->read(callback);
}

void FfmpegCaptureReader::abortInput() noexcept {
    impl_->abortInput();
}

void FfmpegCaptureReader::close() noexcept {
    impl_->close();
}

} // namespace iris::media
