#include "media/backend/ffmpeg/ffmpeg_video_pipeline.h"

#include "media/backend/ffmpeg/ffmpeg_video_capture_source.h"
#include "media/backend/ffmpeg/ffmpeg_video_encoder.h"
#include "media/pipeline/h264_bitstream.h"
#include "media/pipeline/h264_rtp_packetizer.h"
#include "media/render/native_renderer.h"
#include "media/transport/rtp.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace iris::media {
namespace {

constexpr std::uint8_t kDefaultH264PayloadType = 96;
constexpr std::size_t kRtpPayloadMtu = 1200;

std::uint64_t now_ms() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

bool equals_h264(const std::string& value) {
    return value.size() == 4 &&
        std::tolower(static_cast<unsigned char>(value[0])) == 'h' &&
        std::tolower(static_cast<unsigned char>(value[1])) == '2' &&
        std::tolower(static_cast<unsigned char>(value[2])) == '6' &&
        std::tolower(static_cast<unsigned char>(value[3])) == '4';
}

std::uint8_t h264_payload_type(const MediaSessionConfig& config) {
    for (const auto& preference : config.codecPreferences) {
        if (preference.kind == MediaKind::Video &&
            equals_h264(preference.encoding) &&
            preference.payloadType >= 0 &&
            preference.payloadType <= 127) {
            return static_cast<std::uint8_t>(preference.payloadType);
        }
    }
    return kDefaultH264PayloadType;
}

} // namespace

struct FfmpegVideoPipeline::Impl {
    struct ReceivedPacket {
        std::uint16_t sequence = 0;
        std::uint32_t timestamp = 0;
        bool marker = false;
        std::vector<std::uint8_t> payload;
    };

    MediaResult start(const MediaRuntimeConfig& runtime,
                      const MediaSessionConfig& session,
                      IMediaPacketSink& sink,
                      IVideoFrameSink* frameSink) {
        stop();
        sink_ = &sink;
        frame_sink = frameSink;
        call_id = session.callId;
        const int width = std::max(160, runtime.videoWidth);
        const int height = std::max(90, runtime.videoHeight);
        fps = std::max(1, runtime.videoFps);
        muted = false;
        running = true;
        payload_type = h264_payload_type(session);
        packetizer.setParameterSets({});
        cached_sps.clear();
        cached_pps.clear();
        waiting_for_keyframe = true;

        auto concreteEncoder =
            std::make_unique<FfmpegVideoEncoder>();
        const auto encoder_result = concreteEncoder->configure(
            pipeline::VideoEncoderConfig{
                width,
                height,
                fps,
                runtime.videoBitrateKbps * 1000,
                fps,
            },
            [this](const pipeline::EncodedPacketView& packet) {
                send_encoded_video(packet);
            });
        if (!encoder_result.ok) {
            stop();
            return encoder_result;
        }
        video_encoder = std::move(concreteEncoder);
        packetizer.setParameterSets(
            video_encoder->parameterSets());
        rtp_parameters = pipeline::RtpParameters{
            payload_type,
            90000,
            kRtpPayloadMtu,
        };

        const AVCodec* decoder =
            avcodec_find_decoder(AV_CODEC_ID_H264);
        if (!decoder) {
            stop();
            return MediaResult::failure("H.264 decoder is unavailable");
        }
        receive_decoder = avcodec_alloc_context3(decoder);
        receive_decoder->thread_count = 1;
        if (avcodec_open2(receive_decoder, decoder, nullptr) < 0) {
            stop();
            return MediaResult::failure("failed to open H.264 decoder");
        }

        if (runtime.renderVideo) {
            renderer = std::make_unique<NativeVideoRenderer>();
            if (!renderer->start("Iris - peer video")) {
                stop();
                return MediaResult::failure(
                    "failed to open native video renderer");
            }
        }

        auto concreteSource =
            std::make_unique<FFmpegVideoCaptureSource>();
        const auto captureResult = concreteSource->configure(
            runtime, session, frameSink);
        if (!captureResult.ok) {
            stop();
            return captureResult;
        }
        concreteSource->setFrameSink(
            [this](const pipeline::VideoFrameView& frame) {
                if (video_encoder) {
                    (void)video_encoder->encode(frame);
                }
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
        decode_condition.notify_all();
        if (decode_thread.joinable() &&
            decode_thread.get_id() != std::this_thread::get_id()) {
            decode_thread.join();
        }
        if (renderer) {
            renderer->stop();
            renderer.reset();
        }
        {
            std::lock_guard<std::mutex> lock(decode_mutex);
            packets.clear();
        }
        sws_freeContext(render_sws);
        render_sws = nullptr;
        avcodec_free_context(&receive_decoder);
        capture_source.reset();
        video_encoder.reset();
        sink_ = nullptr;
        reset_access_unit();
        access_unit_timestamp.reset();
        waiting_for_keyframe = true;
    }

    MediaResult on_rtp_packet(ByteView packet) {
        if (!running || !receive_decoder) {
            return MediaResult::failure("video pipeline is not running");
        }
        RtpPacket rtp;
        if (!parse_rtp(packet.data, packet.size, rtp) ||
            rtp.payloadSize == 0 ||
            rtp.header.payloadType != payload_type) {
            return MediaResult::failure("invalid video RTP packet");
        }
        std::vector<std::uint8_t> payload(
            rtp.payload, rtp.payload + rtp.payloadSize);
        {
            std::lock_guard<std::mutex> lock(decode_mutex);
            if (packets.size() >= 128) {
                packets.erase(packets.begin());
            }
            packets.emplace(rtp.header.sequence, ReceivedPacket{
                rtp.header.sequence,
                rtp.header.timestamp,
                rtp.header.marker,
                std::move(payload)
            });
        }
        decode_condition.notify_one();
        return MediaResult::success();
    }

    void set_muted(bool value) {
        muted = value;
        if (capture_source) {
            capture_source->setMuted(value);
        }
    }

    MediaResult request_keyframe() {
        // OpenH264 does not expose a synchronized force-IDR call through the
        // FFmpeg encoder API. The next periodic GOP remains the keyframe path.
        return MediaResult::failure("keyframe request is not implemented");
    }

    void add_stats(MediaStats& result) {
        result.packetsSent += packets_sent.load();
        result.sendErrors += send_errors.load();
        result.packetsReceived += packets_received.load();
        result.packetsLost += packets_lost.load();
        result.bytesSent += bytes_sent.load();
        result.bytesReceived += bytes_received.load();
        result.videoFramesDecoded += frames_decoded.load();
    }

    void send_encoded_video(
        const pipeline::EncodedPacketView& packet) {
        const auto timestamp = packet.pts > 0
            ? static_cast<std::uint32_t>(
                  packet.pts * 90000 / fps)
            : 0;
        (void)packetizer.packetize(
            packet,
            timestamp,
            rtp_parameters,
            [this](ByteView datagram) {
                auto result = sink_
                    ? sink_->sendRtp(MediaKind::Video, datagram)
                    : MediaResult::failure(
                          "video packet sink is unavailable");
                if (result.ok) {
                    packets_sent.fetch_add(1);
                    bytes_sent.fetch_add(datagram.size);
                } else {
                    send_errors.fetch_add(1);
                }
                return result;
            });
    }

    void flush_encoder() {
        if (video_encoder) {
            (void)video_encoder->flush();
        }
    }

    void decode_loop() {
        bool have_expected = false;
        std::uint16_t expected_sequence = 0;

        while (running) {
            ReceivedPacket received;
            {
                std::unique_lock<std::mutex> lock(decode_mutex);
                decode_condition.wait(lock, [this] {
                    return !running || !packets.empty();
                });
                if (!running) {
                    break;
                }
                auto iterator = packets.begin();
                received = std::move(iterator->second);
                packets.erase(iterator);
            }

            const bool sequence_gap =
                have_expected && received.sequence != expected_sequence;
            if (sequence_gap) {
                packets_lost.fetch_add(1);
            }
            have_expected = true;
            expected_sequence =
                static_cast<std::uint16_t>(received.sequence + 1);

            packets_received.fetch_add(1);
            bytes_received.fetch_add(received.payload.size());
            consume_h264_payload(received, sequence_gap);
        }
    }

    void reset_access_unit() {
        assembling.clear();
        receiving_fragment = false;
        access_unit_damaged = false;
    }

    void finish_access_unit() {
        if (!access_unit_damaged && !receiving_fragment && !assembling.empty()) {
            decode_access_unit();
        }
        reset_access_unit();
    }

    void consume_h264_payload(const ReceivedPacket& received, bool sequence_gap) {
        if (access_unit_timestamp != received.timestamp) {
            // A timestamp change also closes a picture when a peer omits M.
            if (sequence_gap) {
                reset_access_unit();
            } else {
                finish_access_unit();
            }
            access_unit_timestamp = received.timestamp;
        } else if (sequence_gap) {
            access_unit_damaged = true;
        }

        if (!access_unit_damaged) {
            const auto& payload = received.payload;
            const auto nal_type = payload[0] & 0x1f;
            if (nal_type == 28) {
                access_unit_damaged = !append_fragment(payload);
            } else if (receiving_fragment) {
                access_unit_damaged = true;
            } else if (nal_type == 24) {
                access_unit_damaged = !append_aggregation(payload);
            } else if (nal_type >= 1 && nal_type <= 23) {
                pipeline::appendH264StartCode(assembling);
                assembling.insert(assembling.end(), payload.begin(), payload.end());
            } else {
                access_unit_damaged = true;
            }
        }
        if (received.marker) {
            finish_access_unit();
        }
    }

    bool append_aggregation(const std::vector<std::uint8_t>& payload) {
        std::size_t offset = 1;
        while (offset + 2 <= payload.size()) {
            const auto length = static_cast<std::size_t>(
                (payload[offset] << 8) | payload[offset + 1]);
            offset += 2;
            if (length == 0 || length > payload.size() - offset) {
                return false;
            }
            const auto type = payload[offset] & 0x1f;
            if (type == 0 || type > 23) {
                return false;
            }
            pipeline::appendH264StartCode(assembling);
            assembling.insert(assembling.end(), payload.begin() + offset,
                              payload.begin() + offset + length);
            offset += length;
        }
        return offset > 1 && offset == payload.size();
    }

    bool append_fragment(const std::vector<std::uint8_t>& payload) {
        if (payload.size() < 2) {
            return false;
        }
        const auto header = payload[1];
        const auto type = header & 0x1f;
        const bool start = (header & 0x80) != 0;
        const bool end = (header & 0x40) != 0;
        if (type == 0 || type > 23) {
            return false;
        }
        if (start) {
            if (end || receiving_fragment) {
                return false;
            }
            pipeline::appendH264StartCode(assembling);
            assembling.push_back(static_cast<std::uint8_t>(
                (payload[0] & 0xe0) | type));
        } else if (!receiving_fragment) {
            return false;
        }
        assembling.insert(assembling.end(), payload.begin() + 2, payload.end());
        receiving_fragment = !end;
        return true;
    }

    void decode_access_unit() {
        bool has_picture = false;
        bool keyframe = false;
        bool has_sps = false;
        bool has_pps = false;
        for (const auto& nal :
             pipeline::splitAnnexB(assembling.data(), assembling.size())) {
            const auto type = nal.data[0] & 0x1f;
            has_picture = has_picture || (type >= 1 && type <= 5);
            keyframe = keyframe || type == 5;
            if (type == 7) {
                cached_sps.assign(nal.data, nal.data + nal.size);
                has_sps = true;
            } else if (type == 8) {
                cached_pps.assign(nal.data, nal.data + nal.size);
                has_pps = true;
            }
        }
        const bool have_parameters = !cached_sps.empty() && !cached_pps.empty();
        if (!has_picture || (waiting_for_keyframe && !(keyframe && have_parameters))) {
            return;
        }
        std::vector<std::uint8_t> with_parameters;
        if (have_parameters && (!has_sps || !has_pps)) {
            pipeline::appendH264StartCode(with_parameters);
            with_parameters.insert(with_parameters.end(),
                                   cached_sps.begin(), cached_sps.end());
            pipeline::appendH264StartCode(with_parameters);
            with_parameters.insert(with_parameters.end(),
                                   cached_pps.begin(), cached_pps.end());
            with_parameters.insert(with_parameters.end(),
                                   assembling.begin(), assembling.end());
        }
        const auto& access_unit =
            with_parameters.empty() ? assembling : with_parameters;
        AVPacket* packet = av_packet_alloc();
        av_new_packet(packet, static_cast<int>(access_unit.size()));
        std::memcpy(packet->data, access_unit.data(), access_unit.size());
        if (avcodec_send_packet(receive_decoder, packet) >= 0) {
            waiting_for_keyframe = false;
            AVFrame* frame = av_frame_alloc();
            while (avcodec_receive_frame(receive_decoder, frame) >= 0) {
                render_frame(frame);
                av_frame_unref(frame);
            }
            av_frame_free(&frame);
        }
        av_packet_free(&packet);
    }

    void render_frame(AVFrame* frame) {
        frames_decoded.fetch_add(1);
        if (!renderer &&
            !(frame_sink && frame_sink->active())) {
            return;
        }
        render_sws = sws_getCachedContext(
            render_sws,
            frame->width, frame->height,
            static_cast<AVPixelFormat>(frame->format),
            frame->width, frame->height, AV_PIX_FMT_BGRA,
            SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!render_sws) {
            return;
        }
        const auto byte_count = av_image_get_buffer_size(
            AV_PIX_FMT_BGRA, frame->width, frame->height, 1);
        std::vector<std::uint8_t> bgra(
            static_cast<std::size_t>(byte_count));
        std::uint8_t* destination[] = {bgra.data()};
        const int lines[] = {frame->width * 4};
        sws_scale(
            render_sws, frame->data, frame->linesize,
            0, frame->height, destination, lines);
        if (frame_sink && frame_sink->active()) {
            frame_sink->onVideoFrame(VideoFrameView{
                call_id,
                VideoFrameKind::Remote,
                bgra.data(),
                bgra.size(),
                frame->width,
                frame->height,
                frame->width * 4,
                now_ms(),
            });
        }
        if (renderer) {
            renderer->display(
                std::move(bgra), frame->width, frame->height);
        }
    }

    AVCodecContext* receive_decoder = nullptr;
    SwsContext* render_sws = nullptr;
    IMediaPacketSink* sink_ = nullptr;
    IVideoFrameSink* frame_sink = nullptr;
    CallId call_id;
    std::unique_ptr<pipeline::IVideoCaptureSource> capture_source;
    std::unique_ptr<pipeline::IH264VideoEncoder> video_encoder;
    pipeline::H264RtpPacketizer packetizer;
    pipeline::RtpParameters rtp_parameters;
    std::uint8_t payload_type = kDefaultH264PayloadType;
    std::unique_ptr<NativeVideoRenderer> renderer;
    int fps = 15;
    std::thread decode_thread;
    std::atomic<bool> running{false};
    std::atomic<bool> muted{false};
    std::atomic<std::uint64_t> packets_sent{0};
    std::atomic<std::uint64_t> send_errors{0};
    std::atomic<std::uint64_t> packets_received{0};
    std::atomic<std::uint64_t> packets_lost{0};
    std::atomic<std::uint64_t> bytes_sent{0};
    std::atomic<std::uint64_t> bytes_received{0};
    std::atomic<std::uint64_t> frames_decoded{0};
    std::mutex decode_mutex;
    std::condition_variable decode_condition;
    std::map<std::uint16_t, ReceivedPacket> packets;
    std::vector<std::uint8_t> assembling;
    std::optional<std::uint32_t> access_unit_timestamp;
    std::vector<std::uint8_t> cached_sps;
    std::vector<std::uint8_t> cached_pps;
    bool receiving_fragment = false;
    bool access_unit_damaged = false;
    bool waiting_for_keyframe = true;
};

FfmpegVideoPipeline::FfmpegVideoPipeline()
    : impl_(std::make_unique<Impl>()) {
}

FfmpegVideoPipeline::~FfmpegVideoPipeline() {
    stop();
}

MediaResult FfmpegVideoPipeline::start(
    const MediaRuntimeConfig& runtime,
    const MediaSessionConfig& session,
    IMediaPacketSink& sink,
    IVideoFrameSink* frameSink) {
    return impl_->start(runtime, session, sink, frameSink);
}

void FfmpegVideoPipeline::stop() noexcept {
    impl_->stop();
}

MediaResult FfmpegVideoPipeline::onRtpPacket(ByteView packet) {
    return impl_->on_rtp_packet(packet);
}

void FfmpegVideoPipeline::setMuted(bool muted) {
    impl_->set_muted(muted);
}

MediaResult FfmpegVideoPipeline::requestKeyFrame() {
    return impl_->request_keyframe();
}

void FfmpegVideoPipeline::addStats(MediaStats& stats) const {
    impl_->add_stats(stats);
}

} // namespace iris::media

