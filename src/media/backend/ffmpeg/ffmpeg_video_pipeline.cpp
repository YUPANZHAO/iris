#include "media/backend/ffmpeg/ffmpeg_video_pipeline.h"

#include "media/backend/ffmpeg/capture_sources.h"
#include "media/backend/ffmpeg/null_video_image.h"
#include "media/render/native_renderer.h"
#include "media/transport/rtp.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <map>
#include <mutex>
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

struct NalUnit {
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
};

std::vector<NalUnit> split_annex_b(const std::uint8_t* data,
                                   std::size_t size) {
    std::vector<NalUnit> result;
    std::vector<std::size_t> starts;
    for (std::size_t index = 0; index + 3 <= size; ++index) {
        if (index + 4 <= size && data[index] == 0 &&
            data[index + 1] == 0 && data[index + 2] == 0 &&
            data[index + 3] == 1) {
            starts.push_back(index + 4);
            index += 3;
        } else if (data[index] == 0 && data[index + 1] == 0 &&
                   data[index + 2] == 1) {
            starts.push_back(index + 3);
            index += 2;
        }
    }
    if (starts.empty()) {
        if (size > 0) {
            result.push_back({data, size});
        }
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
        width = std::max(160, runtime.videoWidth);
        height = std::max(90, runtime.videoHeight);
        fps = std::max(1, runtime.videoFps);
        muted = false;
        running = true;
        payload_type = h264_payload_type(session);
        cached_sps.clear();
        cached_pps.clear();
        parameter_sets.clear();
        waiting_for_keyframe = true;
        next_video_pts = 0;

        auto resolution =
            resolveCaptureSource(MediaKind::Video, runtime.videoInput);
        if (!resolution.ok()) {
            running = false;
            return MediaResult::failure(resolution.error);
        }
        source = std::move(resolution.source);
        if (source.isNull) {
            null_image = renderNullVideoImage(
                width, height, session.localUri);
        }
        av_log(
            nullptr, AV_LOG_INFO,
            "Iris video capture source: %s\n",
            source.isNull ? "empty (account URI card)" :
                            source.displayName.c_str());

        if (!source.isNull) {
            const AVInputFormat* input_format =
                av_find_input_format(source.format.c_str());
            AVDictionary* options = nullptr;
            if (source.format == "lavfi") {
                av_dict_set(
                    &options, "video_size",
                    (std::to_string(width) + "x" +
                     std::to_string(height)).c_str(), 0);
                av_dict_set(
                    &options, "framerate",
                    std::to_string(fps).c_str(), 0);
                av_dict_set(&options, "realtime", "1", 0);
            }
            const int open_result = avformat_open_input(
                &input, source.url.c_str(), input_format, &options);
            av_dict_free(&options);
            if (open_result < 0) {
                running = false;
                return MediaResult::failure(
                    "failed to open video input " + source.url +
                    ": " + ffmpeg_error(open_result));
            }
            input->interrupt_callback.callback = &interrupt_callback;
            input->interrupt_callback.opaque = &running;

            if (avformat_find_stream_info(input, nullptr) < 0) {
                stop();
                return MediaResult::failure(
                    "failed to inspect video input");
            }
            video_stream = av_find_best_stream(
                input, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
            if (video_stream < 0) {
                stop();
                return MediaResult::failure(
                    "video input has no video stream");
            }
            auto* stream = input->streams[video_stream];

            const AVCodec* capture_codec =
                avcodec_find_decoder(stream->codecpar->codec_id);
            if (!capture_codec) {
                stop();
                return MediaResult::failure("no video decoder");
            }
            capture_decoder = avcodec_alloc_context3(capture_codec);
            if (!capture_decoder ||
                avcodec_parameters_to_context(
                    capture_decoder, stream->codecpar) < 0 ||
                avcodec_open2(
                    capture_decoder, capture_codec, nullptr) < 0) {
                stop();
                return MediaResult::failure(
                    "failed to open video decoder");
            }
        }

        const AVCodec* encoder =
            avcodec_find_encoder_by_name("libopenh264");
        if (!encoder) {
            stop();
            return MediaResult::failure(
                "libopenh264 encoder is unavailable");
        }
        encoder_context = avcodec_alloc_context3(encoder);
        encoder_context->width = width;
        encoder_context->height = height;
        encoder_context->pix_fmt = AV_PIX_FMT_YUV420P;
        encoder_context->time_base = AVRational{1, fps};
        encoder_context->framerate = AVRational{fps, 1};
        encoder_context->bit_rate =
            std::max(100000, runtime.videoBitrateKbps * 1000);
        encoder_context->gop_size = fps;
        encoder_context->max_b_frames = 0;
        encoder_context->thread_count = 1;
        encoder_context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        av_opt_set_int(
            encoder_context->priv_data, "allow_skip_frames", 1, 0);
        if (avcodec_open2(encoder_context, encoder, nullptr) < 0) {
            stop();
            return MediaResult::failure(
                "failed to open OpenH264 encoder");
        }
        build_parameter_sets();

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
        decode_condition.notify_all();
        if (capture_thread.joinable() &&
            capture_thread.get_id() != std::this_thread::get_id()) {
            capture_thread.join();
        }
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
        sws_freeContext(capture_sws);
        capture_sws = nullptr;
        sws_freeContext(render_sws);
        render_sws = nullptr;
        sws_freeContext(preview_sws);
        preview_sws = nullptr;
        avcodec_free_context(&capture_decoder);
        avcodec_free_context(&encoder_context);
        avcodec_free_context(&receive_decoder);
        if (input) {
            avformat_close_input(&input);
        }
        sink_ = nullptr;
        video_stream = -1;
        assembling.clear();
        receiving_fragment = false;
        waiting_for_keyframe = true;
        null_image = {};
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

    bool ensure_capture_sws(AVFrame* frame) {
        if (capture_sws) {
            return true;
        }
        capture_sws = sws_getContext(
            frame->width, frame->height,
            static_cast<AVPixelFormat>(frame->format),
            width, height, AV_PIX_FMT_YUV420P,
            SWS_BILINEAR, nullptr, nullptr, nullptr);
        return capture_sws != nullptr;
    }

    void null_capture_loop() {
        const auto frame_duration =
            std::chrono::microseconds(1000000 / fps);
        auto next_frame = std::chrono::steady_clock::now();
        while (running) {
            AVFrame* source_frame = av_frame_alloc();
            if (!source_frame) {
                break;
            }
            source_frame->format = AV_PIX_FMT_BGRA;
            source_frame->width = width;
            source_frame->height = height;
            if (av_frame_get_buffer(source_frame, 32) < 0) {
                av_frame_free(&source_frame);
                break;
            }
            const std::size_t source_stride =
                static_cast<std::size_t>(width) * 4;
            for (int y = 0; y < height; ++y) {
                std::memcpy(
                    source_frame->data[0] +
                        static_cast<std::ptrdiff_t>(y) *
                            source_frame->linesize[0],
                    null_image.bgra.data() +
                        static_cast<std::size_t>(y) * source_stride,
                    source_stride);
            }
            encode_frame(source_frame);
            av_frame_free(&source_frame);

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
        AVFrame* decoded = av_frame_alloc();
        auto next_frame_time = std::chrono::steady_clock::now();
        const auto frame_interval =
            std::chrono::microseconds(1000000 / fps);
        while (running && av_read_frame(input, packet) >= 0) {
            if (packet->stream_index == video_stream) {
                if (avcodec_send_packet(capture_decoder, packet) >= 0) {
                    while (avcodec_receive_frame(
                               capture_decoder, decoded) >= 0) {
                        const auto now = std::chrono::steady_clock::now();
                        if (now >= next_frame_time) {
                            encode_frame(decoded);
                            next_frame_time = now + frame_interval;
                        }
                        av_frame_unref(decoded);
                    }
                }
            }
            av_packet_unref(packet);
        }
        flush_encoder();
        av_frame_free(&decoded);
        av_packet_free(&packet);
    }

    void encode_frame(AVFrame* source_frame) {
        if (!ensure_capture_sws(source_frame)) {
            return;
        }
        emit_local_preview(source_frame);
        AVFrame* frame = av_frame_alloc();
        frame->format = AV_PIX_FMT_YUV420P;
        frame->width = width;
        frame->height = height;
        frame->pts = next_video_pts++;
        if (av_frame_get_buffer(frame, 32) < 0) {
            av_frame_free(&frame);
            return;
        }
        if (muted) {
            av_frame_make_writable(frame);
            std::memset(frame->data[0], 16, frame->linesize[0] * height);
            std::memset(frame->data[1], 128, frame->linesize[1] * height / 2);
            std::memset(frame->data[2], 128, frame->linesize[2] * height / 2);
        } else {
            sws_scale(
                capture_sws, source_frame->data, source_frame->linesize,
                0, source_frame->height, frame->data, frame->linesize);
        }

        if (avcodec_send_frame(encoder_context, frame) >= 0) {
            AVPacket* encoded = av_packet_alloc();
            while (avcodec_receive_packet(encoder_context, encoded) >= 0) {
                send_h264(
                    encoded->data,
                    static_cast<std::size_t>(encoded->size),
                    static_cast<std::uint32_t>(
                        frame->pts * 90000 / fps));
                av_packet_unref(encoded);
            }
            av_packet_free(&encoded);
        }
        av_frame_free(&frame);
    }

    void emit_local_preview(AVFrame* source_frame) {
        if (!frame_sink || !frame_sink->active()) {
            return;
        }
        preview_sws = sws_getCachedContext(
            preview_sws,
            source_frame->width, source_frame->height,
            static_cast<AVPixelFormat>(source_frame->format),
            source_frame->width, source_frame->height,
            AV_PIX_FMT_BGRA, SWS_BILINEAR,
            nullptr, nullptr, nullptr);
        if (!preview_sws) {
            return;
        }
        const auto byte_count = av_image_get_buffer_size(
            AV_PIX_FMT_BGRA, source_frame->width,
            source_frame->height, 1);
        if (byte_count <= 0) {
            return;
        }
        std::vector<std::uint8_t> bgra(
            static_cast<std::size_t>(byte_count));
        std::uint8_t* destination[] = {bgra.data()};
        const int lines[] = {source_frame->width * 4};
        sws_scale(
            preview_sws, source_frame->data,
            source_frame->linesize, 0, source_frame->height,
            destination, lines);
        frame_sink->onVideoFrame(VideoFrameView{
            call_id,
            VideoFrameKind::LocalPreview,
            bgra.data(),
            bgra.size(),
            source_frame->width,
            source_frame->height,
            source_frame->width * 4,
            now_ms(),
        });
    }

    void send_h264(const std::uint8_t* data,
                   std::size_t size,
                   std::uint32_t timestamp) {
        const auto nals = split_annex_b(data, size);
        const bool has_idr = std::any_of(
            nals.begin(), nals.end(), [](const NalUnit& nal) {
                return nal.size > 0 && (nal.data[0] & 0x1f) == 5;
            });
        const bool has_sps = std::any_of(
            nals.begin(), nals.end(), [](const NalUnit& nal) {
                return nal.size > 0 && (nal.data[0] & 0x1f) == 7;
            });
        if (has_idr && !has_sps && !parameter_sets.empty()) {
            const auto parameter_nals =
                split_annex_b(parameter_sets.data(), parameter_sets.size());
            for (const auto& parameter_nal : parameter_nals) {
                send_payload(
                    parameter_nal.data, parameter_nal.size,
                    timestamp, false);
            }
        }
        for (std::size_t nal_index = 0;
             nal_index < nals.size();
             ++nal_index) {
            const auto nal = nals[nal_index];
            if (nal.size == 0) {
                continue;
            }
            const bool final_nal = nal_index + 1 == nals.size();
            if (nal.size <= kRtpPayloadMtu) {
                send_payload(
                    nal.data, nal.size, timestamp,
                    final_nal);
                continue;
            }

            const std::uint8_t indicator =
                static_cast<std::uint8_t>((nal.data[0] & 0xe0) | 28);
            std::size_t offset = 1;
            bool first = true;
            while (offset < nal.size) {
                const auto fragment_size =
                    std::min(kRtpPayloadMtu - 2, nal.size - offset);
                std::vector<std::uint8_t> payload(fragment_size + 2);
                payload[0] = indicator;
                payload[1] = static_cast<std::uint8_t>(
                    (first ? 0x80 : 0) |
                    (offset + fragment_size >= nal.size ? 0x40 : 0) |
                    (nal.data[0] & 0x1f));
                std::memcpy(
                    payload.data() + 2, nal.data + offset, fragment_size);
                const bool last =
                    offset + fragment_size >= nal.size;
                send_payload(
                    payload.data(), payload.size(), timestamp,
                    final_nal && last);
                offset += fragment_size;
                first = false;
            }
        }
    }

    void send_payload(const std::uint8_t* data,
                      std::size_t size,
                      std::uint32_t timestamp,
                      bool marker) {
        auto packet = rtp.create(
            payload_type, timestamp, data, size, marker);
        auto send_result = sink_
            ? sink_->sendRtp(
                  MediaKind::Video,
                  ByteView{packet.data(), packet.size()})
            : MediaResult::failure("video packet sink is unavailable");
        if (send_result.ok) {
            packets_sent.fetch_add(1);
            bytes_sent.fetch_add(packet.size());
        } else {
            send_errors.fetch_add(1);
        }
    }

    void flush_encoder() {
        avcodec_send_frame(encoder_context, nullptr);
        AVPacket* encoded = av_packet_alloc();
        while (avcodec_receive_packet(encoder_context, encoded) >= 0) {
            send_h264(
                encoded->data, static_cast<std::size_t>(encoded->size),
                static_cast<std::uint32_t>(
                    (next_video_pts - 1) * 90000 / fps));
            av_packet_unref(encoded);
        }
        av_packet_free(&encoded);
    }

    void append_start_code(std::vector<std::uint8_t>& output) {
        output.insert(
            output.end(), std::initializer_list<std::uint8_t>{0, 0, 0, 1});
    }

    void build_parameter_sets() {
        if (!encoder_context ||
            !encoder_context->extradata ||
            encoder_context->extradata_size <= 0) {
            return;
        }
        const auto* data = encoder_context->extradata;
        const auto size =
            static_cast<std::size_t>(encoder_context->extradata_size);
        if ((size >= 4 && data[0] == 0 && data[1] == 0 &&
             data[2] == 0 && data[3] == 1) ||
            (size >= 3 && data[0] == 0 && data[1] == 0 &&
             data[2] == 1)) {
            parameter_sets.assign(data, data + size);
            return;
        }
        if (size < 7 || data[0] != 1) {
            return;
        }
        std::size_t offset = 5;
        const int sps_count = data[offset++] & 0x1f;
        for (int index = 0; index < sps_count; ++index) {
            if (offset + 2 > size) {
                break;
            }
            const auto length =
                static_cast<std::size_t>((data[offset] << 8) |
                                         data[offset + 1]);
            offset += 2;
            if (offset + length > size) {
                break;
            }
            append_start_code(parameter_sets);
            parameter_sets.insert(
                parameter_sets.end(), data + offset, data + offset + length);
            offset += length;
        }
        if (offset >= size) {
            return;
        }
        const int pps_count = data[offset++];
        for (int index = 0; index < pps_count; ++index) {
            if (offset + 2 > size) {
                break;
            }
            const auto length =
                static_cast<std::size_t>((data[offset] << 8) |
                                         data[offset + 1]);
            offset += 2;
            if (offset + length > size) {
                break;
            }
            append_start_code(parameter_sets);
            parameter_sets.insert(
                parameter_sets.end(), data + offset, data + offset + length);
            offset += length;
        }
    }

    void decode_loop() {
        AVFrame* frame = av_frame_alloc();
        AVPacket* packet = av_packet_alloc();
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

            if (!have_expected) {
                expected_sequence = received.sequence;
                have_expected = true;
            }
            if (received.sequence != expected_sequence) {
                packets_lost.fetch_add(1);
                assembling.clear();
                receiving_fragment = false;
                expected_sequence = received.sequence;
            }
            expected_sequence =
                static_cast<std::uint16_t>(expected_sequence + 1);

            packets_received.fetch_add(1);
            bytes_received.fetch_add(received.payload.size());
            consume_h264_payload(received);
        }
        av_packet_free(&packet);
        av_frame_free(&frame);
    }

    void consume_h264_payload(const ReceivedPacket& received) {
        if (received.payload.empty()) {
            return;
        }
        const auto nal_type = received.payload[0] & 0x1f;
        bool parameter_set = nal_type == 7 || nal_type == 8;
        bool keyframe = nal_type == 5;
        if (nal_type == 28 && received.payload.size() >= 3) {
            keyframe = (received.payload[1] & 0x1f) == 5;
        } else if (nal_type == 24) {
            std::size_t offset = 1;
            while (offset + 2 <= received.payload.size()) {
                const auto nal_size =
                    static_cast<std::size_t>(
                        received.payload[offset] << 8 |
                        received.payload[offset + 1]);
                offset += 2;
                if (offset + nal_size > received.payload.size()) {
                    break;
                }
                const auto contained_type =
                    received.payload[offset] & 0x1f;
                parameter_set = parameter_set ||
                    contained_type == 7 || contained_type == 8;
                keyframe = keyframe || contained_type == 5;
                offset += nal_size;
            }
        }
        if (waiting_for_keyframe && !parameter_set && !keyframe) {
            if (received.marker) {
                assembling.clear();
            }
            return;
        }
        if (nal_type == 7) {
            cached_sps.assign(
                received.payload.begin(), received.payload.end());
        } else if (nal_type == 8) {
            cached_pps.assign(
                received.payload.begin(), received.payload.end());
        }
        if (nal_type == 28 && received.payload.size() >= 3) {
            const auto fu_header = received.payload[1];
            const bool start = (fu_header & 0x80) != 0;
            const bool end = (fu_header & 0x40) != 0;
            if (start) {
                assembling.clear();
                assembling.push_back(0);
                assembling.push_back(0);
                assembling.push_back(0);
                assembling.push_back(1);
                assembling.push_back(static_cast<std::uint8_t>(
                    (received.payload[0] & 0xe0) |
                    (fu_header & 0x1f)));
                receiving_fragment = true;
            }
            if (receiving_fragment) {
                assembling.insert(
                    assembling.end(),
                    received.payload.begin() + 2, received.payload.end());
            }
            if (end && receiving_fragment) {
                if (keyframe && !cached_sps.empty() &&
                    !cached_pps.empty()) {
                    waiting_for_keyframe = false;
                }
                decode_access_unit();
                assembling.clear();
                receiving_fragment = false;
            }
        } else if (nal_type == 24) {
            std::size_t offset = 1;
            while (offset + 2 <= received.payload.size()) {
                const auto nal_size =
                    static_cast<std::size_t>(
                        received.payload[offset] << 8 |
                        received.payload[offset + 1]);
                offset += 2;
                if (offset + nal_size > received.payload.size()) {
                    break;
                }
                assembling.insert(
                    assembling.end(), std::initializer_list<std::uint8_t>{
                        0, 0, 0, 1});
                assembling.insert(
                    assembling.end(),
                    received.payload.begin() + offset,
                    received.payload.begin() + offset + nal_size);
                offset += nal_size;
            }
            if (received.marker) {
                if (keyframe && !cached_sps.empty() &&
                    !cached_pps.empty()) {
                    waiting_for_keyframe = false;
                }
                decode_access_unit();
                assembling.clear();
            }
        } else {
            assembling.insert(
                assembling.end(), std::initializer_list<std::uint8_t>{
                    0, 0, 0, 1});
            assembling.insert(
                assembling.end(),
                received.payload.begin(), received.payload.end());
            if (received.marker) {
                if (keyframe && !cached_sps.empty() &&
                    !cached_pps.empty()) {
                    waiting_for_keyframe = false;
                }
                decode_access_unit();
                assembling.clear();
            }
        }
    }

    void decode_access_unit() {
        if (assembling.empty()) {
            return;
        }
        std::vector<std::uint8_t> with_parameters;
        if (!cached_sps.empty() && !cached_pps.empty()) {
            const auto parameter_nals =
                split_annex_b(assembling.data(), assembling.size());
            const bool has_sps = std::any_of(
                parameter_nals.begin(), parameter_nals.end(),
                [](const NalUnit& nal) {
                    return nal.size > 0 && (nal.data[0] & 0x1f) == 7;
                });
            if (!has_sps) {
                append_start_code(with_parameters);
                with_parameters.insert(
                    with_parameters.end(),
                    cached_sps.begin(), cached_sps.end());
                append_start_code(with_parameters);
                with_parameters.insert(
                    with_parameters.end(),
                    cached_pps.begin(), cached_pps.end());
                with_parameters.insert(
                    with_parameters.end(),
                    assembling.begin(), assembling.end());
            }
        }
        const auto& access_unit =
            with_parameters.empty() ? assembling : with_parameters;
        AVPacket* packet = av_packet_alloc();
        av_new_packet(packet, static_cast<int>(access_unit.size()));
        std::memcpy(packet->data, access_unit.data(), access_unit.size());
        if (avcodec_send_packet(receive_decoder, packet) >= 0) {
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

    CaptureSourceSpec source;
    AVFormatContext* input = nullptr;
    int video_stream = -1;
    AVCodecContext* capture_decoder = nullptr;
    AVCodecContext* encoder_context = nullptr;
    AVCodecContext* receive_decoder = nullptr;
    SwsContext* capture_sws = nullptr;
    SwsContext* render_sws = nullptr;
    SwsContext* preview_sws = nullptr;
    IMediaPacketSink* sink_ = nullptr;
    IVideoFrameSink* frame_sink = nullptr;
    CallId call_id;
    RtpSender rtp;
    std::uint8_t payload_type = kDefaultH264PayloadType;
    std::unique_ptr<NativeVideoRenderer> renderer;
    int width = 640;
    int height = 360;
    int fps = 15;
    std::int64_t next_video_pts = 0;
    std::thread capture_thread;
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
    std::vector<std::uint8_t> cached_sps;
    std::vector<std::uint8_t> cached_pps;
    bool receiving_fragment = false;
    bool waiting_for_keyframe = true;
    std::vector<std::uint8_t> parameter_sets;
    NullVideoImage null_image;
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

