#include "capture_source/ffmpeg_audio_capture_source.h"

#include "capture_source/ffmpeg_capture_utils.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace iris::media::capture {
namespace {

std::string ffmpegError(int error) {
    char message[AV_ERROR_MAX_STRING_SIZE]{};
    if (av_strerror(error, message, sizeof(message)) < 0) {
        return "unknown FFmpeg error";
    }
    return message;
}

AVSampleFormat sampleFormat(AudioSampleFormat format) {
    switch (format) {
    case AudioSampleFormat::Signed16:
        return AV_SAMPLE_FMT_S16;
    case AudioSampleFormat::Float32:
        return AV_SAMPLE_FMT_FLT;
    }
    return AV_SAMPLE_FMT_NONE;
}

std::int64_t timestampMicros(std::int64_t timestamp,
                             AVRational timeBase) {
    return av_rescale_q(timestamp, timeBase, AVRational{1, 1000000});
}

} // namespace

struct FFmpegAudioCaptureSource::Impl
    : public std::enable_shared_from_this<
          FFmpegAudioCaptureSource::Impl> {
    ~Impl() {
        stop();
        reset();
    }

    Status configure(const AudioCaptureConfig& value) {
        std::lock_guard<std::mutex> lock(lifecycleMutex);
        if (running.load()) {
            return Status::failure("cannot configure a running audio source");
        }
        if (value.name.empty()) {
            return Status::failure("audio source input URL must not be empty");
        }
        if (sampleFormat(value.outputFormat.sampleFormat) ==
            AV_SAMPLE_FMT_NONE) {
            return Status::failure("unsupported audio output sample format");
        }
        {
            std::lock_guard<std::mutex> configLock(configMutex);
            config = value;
            configured = true;
        }
        setOutputFormat(value.outputFormat);
        clearError();
        return {};
    }

    Status start() {
        std::lock_guard<std::mutex> lock(lifecycleMutex);
        if (running.load()) {
            return Status::failure("audio source is already running");
        }
        if (worker.joinable()) {
            if (worker.get_id() == std::this_thread::get_id()) {
                return Status::failure(
                    "cannot restart an audio source from its callback");
            }
            worker.join();
        }

        AudioCaptureConfig value;
        {
            std::lock_guard<std::mutex> configLock(configMutex);
            if (!configured) {
                return Status::failure("audio source is not configured");
            }
            value = config;
        }

        clearError();
        interrupted.store(false);
        const auto prepared = prepare(value);
        if (!prepared.ok) {
            reset();
            return prepared;
        }

        running.store(true);
        const auto self = shared_from_this();
        try {
            worker = std::thread([self] {
                self->run();
            });
        } catch (const std::exception& error) {
            running.store(false);
            reset();
            return Status::failure(
                "failed to start audio capture thread: " +
                std::string(error.what()));
        }
        return {};
    }

    void stop() noexcept {
        std::unique_lock<std::mutex> lock(lifecycleMutex);
        running.store(false);
        interrupted.store(true);
        if (!worker.joinable()) {
            return;
        }
        if (worker.get_id() == std::this_thread::get_id()) {
            return;
        }
        try {
            worker.join();
        } catch (...) {
        }
    }

    Status prepare(const AudioCaptureConfig& value) {
        reset();
        detail::ensureDevicesRegistered();

        const AVInputFormat* inputFormat = nullptr;
        if (!value.backend.empty()) {
            inputFormat =
                av_find_input_format(value.backend.c_str());
            if (!inputFormat) {
                return Status::failure(
                    "unknown FFmpeg input format: " +
                    value.backend);
            }
        }

        AVDictionary* inputOptions = nullptr;
        for (const auto& option : value.options) {
            const auto status = av_dict_set(
                &inputOptions, option.first.c_str(),
                option.second.c_str(), 0);
            if (status < 0) {
                av_dict_free(&inputOptions);
                return Status::failure(
                    "failed to set audio input option '" +
                    option.first + "': " + ffmpegError(status));
            }
        }

        formatContext = avformat_alloc_context();
        if (!formatContext) {
            av_dict_free(&inputOptions);
            return Status::failure("failed to allocate FFmpeg input context");
        }
        formatContext->interrupt_callback.callback =
            &detail::interruptCapture;
        formatContext->interrupt_callback.opaque =
            &interrupted;

        auto status = avformat_open_input(
            &formatContext, value.name.c_str(), inputFormat,
            &inputOptions);
        av_dict_free(&inputOptions);
        if (status < 0) {
            reset();
            return Status::failure(
                "failed to open audio input: " + ffmpegError(status));
        }

        status = avformat_find_stream_info(formatContext, nullptr);
        if (status < 0) {
            reset();
            return Status::failure(
                "failed to inspect audio input: " +
                ffmpegError(status));
        }

        const AVCodec* decoder = nullptr;
        streamIndex = av_find_best_stream(
            formatContext, AVMEDIA_TYPE_AUDIO, -1, -1, &decoder, 0);
        if (streamIndex < 0 || !decoder) {
            reset();
            return Status::failure(
                "audio input does not contain a decodable audio stream");
        }

        const auto* stream = formatContext->streams[streamIndex];
        decoderContext = avcodec_alloc_context3(decoder);
        packet = av_packet_alloc();
        decodedFrame = av_frame_alloc();
        if (!decoderContext || !packet || !decodedFrame) {
            reset();
            return Status::failure("failed to allocate audio capture resources");
        }

        status = avcodec_parameters_to_context(
            decoderContext, stream->codecpar);
        if (status < 0) {
            reset();
            return Status::failure(
                "failed to initialize audio decoder: " +
                ffmpegError(status));
        }
        decoderContext->pkt_timebase = stream->time_base;
        status = avcodec_open2(decoderContext, decoder, nullptr);
        if (status < 0) {
            reset();
            return Status::failure(
                "failed to open audio decoder: " +
                ffmpegError(status));
        }
        if (decoderContext->sample_rate <= 0 ||
            decoderContext->ch_layout.nb_channels <= 0 ||
            decoderContext->sample_fmt == AV_SAMPLE_FMT_NONE) {
            reset();
            return Status::failure("audio decoder reported an invalid format");
        }

        streamTimeBase =
            stream->time_base.num > 0 && stream->time_base.den > 0
                ? stream->time_base
                : AVRational{1, 1000000};

        auto concreteFormat = value.outputFormat;
        if (concreteFormat.sampleRate <= 0) {
            concreteFormat.sampleRate =
                decoderContext->sample_rate;
        }
        if (concreteFormat.channels <= 0) {
            concreteFormat.channels =
                decoderContext->ch_layout.nb_channels;
        }
        if (sampleFormat(concreteFormat.sampleFormat) ==
                AV_SAMPLE_FMT_NONE ||
            concreteFormat.sampleRate <= 0 ||
            concreteFormat.channels <= 0) {
            reset();
            return Status::failure("invalid audio output format");
        }

        av_channel_layout_default(
            &outputLayout, concreteFormat.channels);
        AVChannelLayout inputLayout{};
        if (decoderContext->ch_layout.order ==
                AV_CHANNEL_ORDER_UNSPEC ||
            decoderContext->ch_layout.nb_channels <= 0) {
            av_channel_layout_default(
                &inputLayout,
                std::max(1,
                         decoderContext->ch_layout.nb_channels));
        } else {
            status = av_channel_layout_copy(
                &inputLayout, &decoderContext->ch_layout);
            if (status < 0) {
                reset();
                return Status::failure(
                    "failed to copy audio channel layout: " +
                    ffmpegError(status));
            }
        }

        status = swr_alloc_set_opts2(
            &resampler, &outputLayout,
            sampleFormat(concreteFormat.sampleFormat),
            concreteFormat.sampleRate, &inputLayout,
            decoderContext->sample_fmt,
            decoderContext->sample_rate, 0, nullptr);
        av_channel_layout_uninit(&inputLayout);
        if (status < 0 || !resampler) {
            reset();
            return Status::failure(
                "failed to configure audio conversion: " +
                (status < 0 ? ffmpegError(status)
                            : std::string("no resampler")));
        }
        status = swr_init(resampler);
        if (status < 0) {
            reset();
            return Status::failure(
                "failed to initialize audio conversion: " +
                ffmpegError(status));
        }

        setOutputFormat(concreteFormat);
        return {};
    }

    void run() noexcept {
        bool reachedEnd = false;
        try {
            while (running.load()) {
                av_packet_unref(packet);
                const auto readStatus = av_read_frame(
                    formatContext, packet);
                if (readStatus == AVERROR(EAGAIN)) {
                    continue;
                }
                if (readStatus == AVERROR_EXIT || !running.load()) {
                    break;
                }
                if (readStatus == AVERROR_EOF) {
                    reachedEnd = true;
                    break;
                }
                if (readStatus < 0) {
                    setError(
                        "failed to read audio input: " +
                        ffmpegError(readStatus));
                    break;
                }
                if (packet->stream_index == streamIndex &&
                    !decodePacket()) {
                    break;
                }
            }

            if (reachedEnd && running.load()) {
                const auto status =
                    avcodec_send_packet(decoderContext, nullptr);
                if (status < 0 && status != AVERROR_EOF) {
                    setError(
                        "failed to drain audio decoder: " +
                        ffmpegError(status));
                } else if (!receiveFrames()) {
                    // receiveFrames() records the concrete error.
                }
            }
        } catch (const std::exception& error) {
            setError(
                "audio frame callback failed: " +
                std::string(error.what()));
        } catch (...) {
            setError("audio frame callback failed");
        }

        running.store(false);
        reset();
    }

    bool decodePacket() {
        auto status =
            avcodec_send_packet(decoderContext, packet);
        if (status == AVERROR(EAGAIN)) {
            if (!receiveFrames()) {
                return false;
            }
            status =
                avcodec_send_packet(decoderContext, packet);
        }
        if (status < 0) {
            setError(
                "failed to decode audio packet: " +
                ffmpegError(status));
            return false;
        }
        return receiveFrames();
    }

    bool receiveFrames() {
        while (true) {
            const auto status =
                avcodec_receive_frame(decoderContext, decodedFrame);
            if (status == AVERROR(EAGAIN) ||
                status == AVERROR_EOF) {
                return true;
            }
            if (status < 0) {
                setError(
                    "failed to receive audio frame: " +
                    ffmpegError(status));
                return false;
            }
            const auto delivered = deliverFrame(*decodedFrame);
            av_frame_unref(decodedFrame);
            if (!delivered.ok) {
                setError(delivered.message);
                return false;
            }
        }
    }

    Status deliverFrame(const AVFrame& source) {
        if (!running.load() || source.nb_samples <= 0) {
            return {};
        }
        const auto targetFormat = getOutputFormat();
        const auto targetSampleFormat =
            sampleFormat(targetFormat.sampleFormat);
        if (targetSampleFormat == AV_SAMPLE_FMT_NONE) {
            return Status::failure("unsupported audio output sample format");
        }

        const auto maxSamples = swr_get_out_samples(
            resampler, source.nb_samples);
        if (maxSamples <= 0) {
            return Status::failure("failed to calculate audio output size");
        }
        const auto maxBytes = av_samples_get_buffer_size(
            nullptr, targetFormat.channels, maxSamples,
            targetSampleFormat, 0);
        if (maxBytes <= 0) {
            return Status::failure("invalid converted audio buffer size");
        }
        auto samples = std::make_shared<ByteBuffer>(
            static_cast<std::size_t>(maxBytes));
        if (!samples || !*samples) {
            return Status::failure("failed to allocate converted audio frame");
        }

        const auto channelCount = std::max(
            1, source.ch_layout.nb_channels);
        std::vector<const std::uint8_t*> inputData(
            static_cast<std::size_t>(channelCount), nullptr);
        for (int channel = 0; channel < channelCount; ++channel) {
            inputData[static_cast<std::size_t>(channel)] =
                source.extended_data[channel];
        }
        std::uint8_t* outputData[1] = {
            static_cast<std::uint8_t*>(samples->data())};
        const auto convertedSamples = swr_convert(
            resampler, outputData, maxSamples, inputData.data(),
            source.nb_samples);
        if (convertedSamples < 0) {
            return Status::failure(
                "failed to convert audio frame: " +
                ffmpegError(convertedSamples));
        }
        if (convertedSamples == 0) {
            return {};
        }
        const auto byteCount = av_samples_get_buffer_size(
            nullptr, targetFormat.channels, convertedSamples,
            targetSampleFormat, 0);
        if (byteCount < 0 || !samples->setSize(
                                 static_cast<std::size_t>(byteCount))) {
            return Status::failure("invalid converted audio byte count");
        }

        std::int64_t pts = lastPts;
        auto timestamp = source.best_effort_timestamp;
        if (timestamp == AV_NOPTS_VALUE) {
            timestamp = source.pts;
        }
        if (timestamp != AV_NOPTS_VALUE) {
            const auto converted =
                timestampMicros(timestamp, streamTimeBase);
            if (!hasTimestampOrigin) {
                timestampOrigin = converted;
                hasTimestampOrigin = true;
            }
            pts = std::max<std::int64_t>(
                0, converted - timestampOrigin);
        } else {
            pts += (static_cast<std::int64_t>(convertedSamples) *
                    1000000) /
                   targetFormat.sampleRate;
        }
        lastPts = pts;

        auto sinkValue = [&] {
            std::lock_guard<std::mutex> lock(sinkMutex);
            return sink.lock();
        }();
        if (!sinkValue) {
            return {};
        }

        AudioFrame frame;
        frame.format = targetFormat;
        frame.samples = std::move(samples);
        frame.ptsMicros = pts;
        sinkValue->onAudioFrame(frame);
        return {};
    }

    void reset() noexcept {
        av_frame_free(&decodedFrame);
        av_packet_free(&packet);
        avcodec_free_context(&decoderContext);
        if (formatContext) {
            avformat_close_input(&formatContext);
        }
        swr_free(&resampler);
        av_channel_layout_uninit(&outputLayout);
        streamIndex = -1;
        streamTimeBase = {};
        lastPts = 0;
        timestampOrigin = 0;
        hasTimestampOrigin = false;
    }

    void setError(std::string value) {
        std::lock_guard<std::mutex> lock(errorMutex);
        error = std::move(value);
    }

    void clearError() {
        std::lock_guard<std::mutex> lock(errorMutex);
        error.clear();
    }

    std::string getError() const {
        std::lock_guard<std::mutex> lock(errorMutex);
        return error;
    }

    void setOutputFormat(const AudioFormat& value) {
        std::lock_guard<std::mutex> lock(formatMutex);
        output = value;
    }

    AudioFormat getOutputFormat() const {
        std::lock_guard<std::mutex> lock(formatMutex);
        return output;
    }

    std::mutex lifecycleMutex;
    mutable std::mutex configMutex;
    mutable std::mutex sinkMutex;
    mutable std::mutex errorMutex;
    mutable std::mutex formatMutex;

    AudioCaptureConfig config;
    bool configured = false;
    AudioFormat output;
    std::weak_ptr<AudioFrameSink> sink;
    std::string error;

    std::atomic<bool> running{false};
    std::atomic<bool> interrupted{false};
    std::thread worker;

    AVFormatContext* formatContext = nullptr;
    AVCodecContext* decoderContext = nullptr;
    AVPacket* packet = nullptr;
    AVFrame* decodedFrame = nullptr;
    SwrContext* resampler = nullptr;
    AVChannelLayout outputLayout{};
    int streamIndex = -1;
    AVRational streamTimeBase{};
    std::int64_t lastPts = 0;
    std::int64_t timestampOrigin = 0;
    bool hasTimestampOrigin = false;
};

FFmpegAudioCaptureSource::FFmpegAudioCaptureSource()
    : impl_(std::make_shared<Impl>()) {
}

FFmpegAudioCaptureSource::~FFmpegAudioCaptureSource() = default;

FFmpegAudioCaptureSource::FFmpegAudioCaptureSource(
    FFmpegAudioCaptureSource&&) noexcept = default;

FFmpegAudioCaptureSource& FFmpegAudioCaptureSource::operator=(
    FFmpegAudioCaptureSource&&) noexcept = default;

Status FFmpegAudioCaptureSource::configure(
    const AudioCaptureConfig& config) {
    return impl_->configure(config);
}

Status FFmpegAudioCaptureSource::start() {
    return impl_->start();
}

void FFmpegAudioCaptureSource::stop() noexcept {
    impl_->stop();
}

bool FFmpegAudioCaptureSource::running() const noexcept {
    return impl_->running.load();
}

std::string FFmpegAudioCaptureSource::lastError() const {
    return impl_->getError();
}

void FFmpegAudioCaptureSource::setFrameSink(
    std::weak_ptr<AudioFrameSink> sink) noexcept {
    std::lock_guard<std::mutex> lock(impl_->sinkMutex);
    impl_->sink = std::move(sink);
}

AudioFormat FFmpegAudioCaptureSource::outputFormat() const noexcept {
    return impl_->getOutputFormat();
}

} // namespace iris::media::capture
