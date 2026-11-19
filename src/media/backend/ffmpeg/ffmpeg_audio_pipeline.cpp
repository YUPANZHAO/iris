#include "media/backend/ffmpeg/ffmpeg_audio_pipeline.h"

#include "media/backend/ffmpeg/ffmpeg_audio_capture_source.h"
#include "media/backend/ffmpeg/ffmpeg_audio_encoder.h"
#include "media/pipeline/opus_rtp_packetizer.h"
#include "media/render/native_renderer.h"
#include "media/transport/rtp.h"

extern "C" {
#include <libavcodec/avcodec.h>
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

        encoder = std::make_unique<FfmpegAudioEncoder>();
        const auto encoder_result = encoder->configure(
            pipeline::AudioEncoderConfig{
                kSampleRate,
                kChannels,
                runtime.audioBitrateKbps * 1000,
                kFrameSamples,
            },
            [this](const pipeline::EncodedPacketView& packet) {
                send_encoded_audio(packet);
            });
        if (!encoder_result.ok) {
            stop();
            return encoder_result;
        }
        rtp_parameters = pipeline::RtpParameters{
            payload_type,
            static_cast<std::uint32_t>(kSampleRate),
            1200,
        };

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

        auto concreteSource =
            std::make_unique<FFmpegAudioCaptureSource>();
        const auto captureResult = concreteSource->configure(
            runtime, session);
        if (!captureResult.ok) {
            stop();
            return captureResult;
        }
        concreteSource->setFrameSink(
            [this](const pipeline::AudioFrameView& frame) {
                encode_audio_frame(frame);
            });
        concreteSource->setMuted(false);
        capture_source = std::move(concreteSource);

        decode_thread = std::thread([this] { decode_loop(); });
        const auto sourceResult = capture_source->start();
        if (!sourceResult.ok) {
            stop();
            return sourceResult;
        }
        return MediaResult::success();
    }

    void stop() noexcept {
        if (capture_source) {
            capture_source->stop();
        }
        flush_encoder();
        running = false;
        queue_condition.notify_all();
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
        swr_free(&receive_swr);
        avcodec_free_context(&receive_decoder);
        capture_source.reset();
        encoder.reset();
        sink_ = nullptr;
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
        if (capture_source) {
            capture_source->setMuted(value);
        }
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

    void encode_audio_frame(
        const pipeline::AudioFrameView& frame) {
        if (encoder) {
            (void)encoder->encode(frame);
        }
    }

    void send_encoded_audio(
        const pipeline::EncodedPacketView& packet) {
        auto send_result = packetizer.packetize(
            packet,
            static_cast<std::uint32_t>(packet.pts),
            rtp_parameters,
            [this](ByteView datagram) {
                auto result = sink_
                    ? sink_->sendRtp(MediaKind::Audio, datagram)
                    : MediaResult::failure(
                          "audio packet sink is unavailable");
                if (result.ok) {
                    packets_sent.fetch_add(1);
                    bytes_sent.fetch_add(datagram.size);
                } else {
                    send_errors.fetch_add(1);
                }
                return result;
            });
        (void)send_result;
    }

    void flush_encoder() {
        if (encoder) {
            (void)encoder->flush();
        }
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

    AVCodecContext* receive_decoder = nullptr;
    SwrContext* receive_swr = nullptr;
    IMediaPacketSink* sink_ = nullptr;
    std::unique_ptr<pipeline::IAudioCaptureSource> capture_source;
    std::unique_ptr<FfmpegAudioEncoder> encoder;
    pipeline::OpusRtpPacketizer packetizer;
    pipeline::RtpParameters rtp_parameters;
    std::uint8_t payload_type = kDefaultOpusPayloadType;
    std::unique_ptr<NativeAudioRenderer> audio_renderer;
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

