#include "media/backend/ffmpeg/ffmpeg_audio_pipeline.h"

#include "media/backend/ffmpeg/capture_sources.h"
#include "media/render/native_renderer.h"
#include "media/transport/rtp.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace iris::media {
namespace {

constexpr int kSampleRate = 48000;
constexpr int kChannels = 2;
constexpr int kFrameSamples = 960;
constexpr std::uint8_t kDefaultOpusPayloadType = 111;

bool equals_opus(const std::string& value) {
    return value.size() == 4 &&
        std::tolower(static_cast<unsigned char>(value[0])) == 'o' &&
        std::tolower(static_cast<unsigned char>(value[1])) == 'p' &&
        std::tolower(static_cast<unsigned char>(value[2])) == 'u' &&
        std::tolower(static_cast<unsigned char>(value[3])) == 's';
}

std::uint8_t opus_payload_type(const MediaSessionConfig& config) {
    for (const auto& preference : config.codecPreferences) {
        if (preference.kind == MediaKind::Audio &&
            equals_opus(preference.encoding) &&
            preference.payloadType >= 0 &&
            preference.payloadType <= 127) {
            return static_cast<std::uint8_t>(preference.payloadType);
        }
    }
    return kDefaultOpusPayloadType;
}

int interrupt_callback(void* opaque) {
    return !static_cast<std::atomic<bool>*>(opaque)->load();
}

std::string ffmpeg_error(int error) {
    char message[AV_ERROR_MAX_STRING_SIZE]{};
    if (av_strerror(error, message, sizeof(message)) < 0) {
        return "unknown FFmpeg error";
    }
    return message;
}

} // namespace

struct FfmpegAudioPipeline::Impl {
    MediaResult start(const MediaRuntimeConfig& runtime,
                      const MediaSessionConfig& session,
                      IMediaPacketSink& sink) {
        stop();
        sink_ = &sink;
        muted = false;
        running = true;
        payload_type = opus_payload_type(session);
        next_audio_pts = 0;
        pcm.clear();

        auto resolution =
            resolveCaptureSource(MediaKind::Audio, runtime.audioInput);
        if (!resolution.ok()) {
            running = false;
            return MediaResult::failure(resolution.error);
        }
        source = std::move(resolution.source);
        av_log(
            nullptr, AV_LOG_INFO,
            "Iris audio capture source: %s\n",
            source.isNull ? "empty (silence)" : source.displayName.c_str());

        if (!source.isNull) {
            const AVInputFormat* input_format =
                av_find_input_format(source.format.c_str());
            AVDictionary* options = nullptr;
            if (source.format == "lavfi") {
                av_dict_set(&options, "realtime", "1", 0);
            }

            const int open_result = avformat_open_input(
                &input, source.url.c_str(), input_format, &options);
            av_dict_free(&options);
            if (open_result < 0) {
                running = false;
                return MediaResult::failure(
                    "failed to open audio input " + source.url +
                    ": " + ffmpeg_error(open_result));
            }
            input->interrupt_callback.callback = &interrupt_callback;
            input->interrupt_callback.opaque = &running;

            audio_stream = av_find_best_stream(
                input, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
            if (audio_stream < 0 ||
                avformat_find_stream_info(input, nullptr) < 0) {
                stop();
                return MediaResult::failure(
                    "audio input has no audio stream");
            }

            auto* stream = input->streams[audio_stream];
            const AVCodec* decoder = avcodec_find_decoder(
                stream->codecpar->codec_id);
            if (!decoder) {
                stop();
                return MediaResult::failure("no audio decoder");
            }
            capture_decoder = avcodec_alloc_context3(decoder);
            if (!capture_decoder ||
                avcodec_parameters_to_context(
                    capture_decoder, stream->codecpar) < 0 ||
                avcodec_open2(capture_decoder, decoder, nullptr) < 0) {
                stop();
                return MediaResult::failure(
                    "failed to open audio decoder");
            }
        }

        const AVCodec* encoder = avcodec_find_encoder_by_name("libopus");
        if (!encoder) {
            stop();
            return MediaResult::failure("libopus encoder is unavailable");
        }
        encoder_context = avcodec_alloc_context3(encoder);
        encoder_context->sample_fmt = AV_SAMPLE_FMT_S16;
        encoder_context->sample_rate = kSampleRate;
        av_channel_layout_default(&encoder_context->ch_layout, kChannels);
        encoder_context->bit_rate =
            std::max(16000, runtime.audioBitrateKbps * 1000);
        encoder_context->time_base = AVRational{1, kSampleRate};
        encoder_context->frame_size = kFrameSamples;
        if (avcodec_open2(encoder_context, encoder, nullptr) < 0) {
            stop();
            return MediaResult::failure("failed to open Opus encoder");
        }

        const AVCodec* opus_decoder =
            avcodec_find_decoder(AV_CODEC_ID_OPUS);
        if (!opus_decoder) {
            stop();
            return MediaResult::failure("Opus decoder is unavailable");
        }
        receive_decoder = avcodec_alloc_context3(opus_decoder);
        receive_decoder->sample_rate = kSampleRate;
        av_channel_layout_default(&receive_decoder->ch_layout, kChannels);
        if (avcodec_open2(receive_decoder, opus_decoder, nullptr) < 0) {
            stop();
            return MediaResult::failure("failed to open Opus decoder");
        }

        if (runtime.renderAudio) {
            audio_renderer = std::make_unique<NativeAudioRenderer>();
            if (!audio_renderer->start(kSampleRate, kChannels)) {
                stop();
                return MediaResult::failure(
                    "failed to open native audio renderer");
            }
        }

        capture_thread = std::thread([this] {
            if (source.isNull) {
                null_capture_loop();
            } else {
                capture_loop();
            }
        });
        decode_thread = std::thread([this] { decode_loop(); });
        return MediaResult::success();
    }

    void stop() noexcept {
        running = false;
        queue_condition.notify_all();
        if (capture_thread.joinable() &&
            capture_thread.get_id() != std::this_thread::get_id()) {
            capture_thread.join();
        }
        if (decode_thread.joinable() &&
            decode_thread.get_id() != std::this_thread::get_id()) {
            decode_thread.join();
        }
        if (audio_renderer) {
            audio_renderer->stop();
            audio_renderer.reset();
        }
        {
            std::lock_guard<std::mutex> lock(queue_mutex);
            packet_queue.clear();
        }
        swr_free(&capture_swr);
        swr_free(&receive_swr);
        avcodec_free_context(&capture_decoder);
        avcodec_free_context(&encoder_context);
        avcodec_free_context(&receive_decoder);
        if (input) {
            avformat_close_input(&input);
        }
        pcm.clear();
        sink_ = nullptr;
        audio_stream = -1;
    }

    MediaResult on_rtp_packet(ByteView packet) {
        if (!running || !receive_decoder) {
            return MediaResult::failure("audio pipeline is not running");
        }
        RtpPacket rtp;
        if (!parse_rtp(packet.data, packet.size, rtp) ||
            rtp.payloadSize == 0 ||
            rtp.header.payloadType != payload_type) {
            return MediaResult::failure("invalid audio RTP packet");
        }
        {
            std::lock_guard<std::mutex> lock(queue_mutex);
            if (packet_queue.size() >= 100) {
                packet_queue.pop_front();
                ++packets_lost;
            }
            packet_queue.emplace_back(
                rtp.payload, rtp.payload + rtp.payloadSize);
        }
        queue_condition.notify_one();
        return MediaResult::success();
    }

    void set_muted(bool value) {
        muted = value;
    }

    void add_stats(MediaStats& result) {
        result.packetsSent += packets_sent.load();
        result.sendErrors += send_errors.load();
        result.packetsReceived += packets_received.load();
        result.packetsLost += packets_lost.load();
        result.bytesSent += bytes_sent.load();
        result.bytesReceived += bytes_received.load();
        result.audioFramesDecoded += frames_decoded.load();
    }

    void null_capture_loop() {
        constexpr auto frame_duration =
            std::chrono::microseconds(
                kFrameSamples * 1000000LL / kSampleRate);
        auto next_frame = std::chrono::steady_clock::now();
        while (running) {
            pcm.assign(
                static_cast<std::size_t>(
                    kFrameSamples * kChannels),
                0);
            encode_audio_frame();
            pcm.clear();

            next_frame += frame_duration;
            const auto now = std::chrono::steady_clock::now();
            if (next_frame > now) {
                std::this_thread::sleep_until(next_frame);
            } else {
                next_frame = now + frame_duration;
            }
        }
        flush_encoder();
    }

    void capture_loop() {
        AVPacket* packet = av_packet_alloc();
        AVFrame* frame = av_frame_alloc();
        while (running && av_read_frame(input, packet) >= 0) {
            if (packet->stream_index == audio_stream) {
                if (avcodec_send_packet(capture_decoder, packet) >= 0) {
                    while (avcodec_receive_frame(
                               capture_decoder, frame) >= 0) {
                        append_audio_frame(frame);
                    }
                }
            }
            av_packet_unref(packet);
            if (source.format == "lavfi") {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }
        flush_encoder();
        av_frame_free(&frame);
        av_packet_free(&packet);
    }

    bool ensure_capture_swr(const AVFrame* frame) {
        if (capture_swr) {
            return true;
        }
        AVChannelLayout output_layout;
        av_channel_layout_default(&output_layout, kChannels);
        AVChannelLayout input_layout;
        if (frame->ch_layout.nb_channels > 0) {
            av_channel_layout_copy(&input_layout, &frame->ch_layout);
        } else {
            av_channel_layout_default(&input_layout, kChannels);
        }
        const int status = swr_alloc_set_opts2(
            &capture_swr,
            &output_layout, AV_SAMPLE_FMT_S16, kSampleRate,
            &input_layout,
            static_cast<AVSampleFormat>(frame->format),
            frame->sample_rate, 0, nullptr);
        av_channel_layout_uninit(&input_layout);
        av_channel_layout_uninit(&output_layout);
        return status >= 0 && swr_init(capture_swr) >= 0;
    }

    void append_audio_frame(const AVFrame* frame) {
        if (!ensure_capture_swr(frame)) {
            return;
        }
        const auto output_capacity = av_rescale_rnd(
            swr_get_delay(capture_swr, frame->sample_rate) +
                frame->nb_samples,
            kSampleRate, frame->sample_rate, AV_ROUND_UP);
        std::vector<std::int16_t> converted(
            static_cast<std::size_t>(output_capacity) * kChannels);
        std::uint8_t* output[] = {
            reinterpret_cast<std::uint8_t*>(converted.data())
        };
        const auto input = const_cast<const std::uint8_t**>(
            frame->extended_data);
        const auto samples = swr_convert(
            capture_swr, output, output_capacity,
            input, frame->nb_samples);
        if (samples <= 0) {
            return;
        }
        converted.resize(static_cast<std::size_t>(samples) * kChannels);
        pcm.insert(pcm.end(), converted.begin(), converted.end());
        while (pcm.size() >=
               static_cast<std::size_t>(kFrameSamples * kChannels)) {
            encode_audio_frame();
            pcm.erase(
                pcm.begin(),
                pcm.begin() + kFrameSamples * kChannels);
        }
    }

    void encode_audio_frame() {
        AVFrame* frame = av_frame_alloc();
        frame->format = AV_SAMPLE_FMT_S16;
        frame->sample_rate = kSampleRate;
        av_channel_layout_default(&frame->ch_layout, kChannels);
        frame->nb_samples = kFrameSamples;
        frame->pts = next_audio_pts;
        if (av_frame_get_buffer(frame, 0) < 0) {
            av_frame_free(&frame);
            return;
        }
        const auto byte_count =
            static_cast<std::size_t>(kFrameSamples * kChannels) *
            sizeof(std::int16_t);
        if (muted) {
            std::memset(frame->data[0], 0, byte_count);
        } else {
            std::memcpy(
                frame->data[0], pcm.data(), byte_count);
        }
        next_audio_pts += kFrameSamples;

        if (avcodec_send_frame(encoder_context, frame) >= 0) {
            AVPacket* packet = av_packet_alloc();
            while (avcodec_receive_packet(encoder_context, packet) >= 0) {
                auto datagram = rtp.create(
                    payload_type,
                    static_cast<std::uint32_t>(frame->pts),
                    packet->data, packet->size, true);
                auto send_result = sink_
                    ? sink_->sendRtp(
                          MediaKind::Audio,
                          ByteView{datagram.data(), datagram.size()})
                    : MediaResult::failure(
                          "audio packet sink is unavailable");
                if (send_result.ok) {
                    packets_sent.fetch_add(1);
                    bytes_sent.fetch_add(datagram.size());
                } else {
                    send_errors.fetch_add(1);
                }
                av_packet_unref(packet);
            }
            av_packet_free(&packet);
        }
        av_frame_free(&frame);
    }

    void flush_encoder() {
        avcodec_send_frame(encoder_context, nullptr);
        AVPacket* packet = av_packet_alloc();
        while (avcodec_receive_packet(encoder_context, packet) >= 0) {
            av_packet_unref(packet);
        }
        av_packet_free(&packet);
    }

    void decode_loop() {
        AVFrame* frame = av_frame_alloc();
        AVPacket* packet = av_packet_alloc();
        while (running) {
            std::vector<std::uint8_t> data;
            {
                std::unique_lock<std::mutex> lock(queue_mutex);
                queue_condition.wait(lock, [this] {
                    return !running || !packet_queue.empty();
                });
                if (!running) {
                    break;
                }
                data = std::move(packet_queue.front());
                packet_queue.pop_front();
            }
            packets_received.fetch_add(1);
            bytes_received.fetch_add(data.size());
            av_packet_unref(packet);
            av_new_packet(packet, static_cast<int>(data.size()));
            std::memcpy(packet->data, data.data(), data.size());
            if (avcodec_send_packet(receive_decoder, packet) < 0) {
                continue;
            }
            while (avcodec_receive_frame(receive_decoder, frame) >= 0) {
                render_audio_frame(frame);
                av_frame_unref(frame);
            }
        }
        av_packet_free(&packet);
        av_frame_free(&frame);
    }

    void render_audio_frame(const AVFrame* frame) {
        frames_decoded.fetch_add(1);
        if (!audio_renderer) {
            return;
        }
        if (!receive_swr) {
            AVChannelLayout output_layout;
            av_channel_layout_default(&output_layout, kChannels);
            AVChannelLayout input_layout;
            if (frame->ch_layout.nb_channels > 0) {
                av_channel_layout_copy(&input_layout, &frame->ch_layout);
            } else {
                av_channel_layout_default(&input_layout, kChannels);
            }
            if (swr_alloc_set_opts2(
                    &receive_swr,
                    &output_layout, AV_SAMPLE_FMT_S16, kSampleRate,
                    &input_layout,
                    static_cast<AVSampleFormat>(frame->format),
                    frame->sample_rate, 0, nullptr) < 0 ||
                swr_init(receive_swr) < 0) {
                av_channel_layout_uninit(&input_layout);
                av_channel_layout_uninit(&output_layout);
                return;
            }
            av_channel_layout_uninit(&input_layout);
            av_channel_layout_uninit(&output_layout);
        }
        const auto capacity = av_rescale_rnd(
            swr_get_delay(receive_swr, frame->sample_rate) +
                frame->nb_samples,
            kSampleRate, frame->sample_rate, AV_ROUND_UP);
        std::vector<std::int16_t> samples(
            static_cast<std::size_t>(capacity) * kChannels);
        std::uint8_t* output[] = {
            reinterpret_cast<std::uint8_t*>(samples.data())
        };
        const auto input = const_cast<const std::uint8_t**>(
            frame->extended_data);
        const auto converted = swr_convert(
            receive_swr, output, capacity, input, frame->nb_samples);
        if (converted > 0) {
            samples.resize(static_cast<std::size_t>(converted) * kChannels);
            audio_renderer->submit(std::move(samples));
        }
    }

    CaptureSourceSpec source;
    AVFormatContext* input = nullptr;
    int audio_stream = -1;
    AVCodecContext* capture_decoder = nullptr;
    AVCodecContext* encoder_context = nullptr;
    AVCodecContext* receive_decoder = nullptr;
    SwrContext* capture_swr = nullptr;
    SwrContext* receive_swr = nullptr;
    IMediaPacketSink* sink_ = nullptr;
    RtpSender rtp;
    std::uint8_t payload_type = kDefaultOpusPayloadType;
    std::unique_ptr<NativeAudioRenderer> audio_renderer;
    std::vector<std::int16_t> pcm;
    std::int64_t next_audio_pts = 0;
    std::thread capture_thread;
    std::thread decode_thread;
    std::atomic<bool> running{false};
    std::atomic<bool> muted{false};
    std::atomic<std::uint64_t> packets_sent{0};
    std::atomic<std::uint64_t> send_errors{0};
    std::atomic<std::uint64_t> packets_received{0};
    std::atomic<std::uint64_t> bytes_sent{0};
    std::atomic<std::uint64_t> bytes_received{0};
    std::atomic<std::uint64_t> frames_decoded{0};
    std::atomic<std::uint64_t> packets_lost{0};
    std::mutex queue_mutex;
    std::condition_variable queue_condition;
    std::deque<std::vector<std::uint8_t>> packet_queue;
};

FfmpegAudioPipeline::FfmpegAudioPipeline()
    : impl_(std::make_unique<Impl>()) {
}

FfmpegAudioPipeline::~FfmpegAudioPipeline() {
    stop();
}

MediaResult FfmpegAudioPipeline::start(
    const MediaRuntimeConfig& runtime,
    const MediaSessionConfig& session,
    IMediaPacketSink& sink) {
    return impl_->start(runtime, session, sink);
}

void FfmpegAudioPipeline::stop() noexcept {
    impl_->stop();
}

MediaResult FfmpegAudioPipeline::onRtpPacket(ByteView packet) {
    return impl_->on_rtp_packet(packet);
}

void FfmpegAudioPipeline::setMuted(bool muted) {
    impl_->set_muted(muted);
}

void FfmpegAudioPipeline::addStats(MediaStats& stats) const {
    impl_->add_stats(stats);
}

} // namespace iris::media

