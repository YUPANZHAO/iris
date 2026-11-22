#include "media/capture/ffmpeg_capture.h"

#include "media/engine/media_buffer.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavdevice/avdevice.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace iris::media::v2 {
namespace {

Status failure(std::string message) {
    return Status{false, 1, std::move(message)};
}

std::string ffmpegError(int error) {
    char message[AV_ERROR_MAX_STRING_SIZE]{};
    if (av_strerror(error, message, sizeof(message)) < 0) {
        return "unknown FFmpeg error";
    }
    return message;
}

void ensureDevicesRegistered() {
    static const bool initialized = [] {
        avdevice_register_all();
        return true;
    }();
    (void)initialized;
}

struct SourceSpec {
    std::string format;
    std::string url;
    std::string displayName;
    bool isNull = false;
};

struct SourceResolution {
    SourceSpec source;
    std::string error;
    bool ok() const noexcept { return error.empty(); }
};

AVMediaType mediaType(MediaKind kind) {
    return kind == MediaKind::Video
        ? AVMEDIA_TYPE_VIDEO
        : AVMEDIA_TYPE_AUDIO;
}

bool supports(const AVDeviceInfo* device, AVMediaType type) {
    if (!device) {
        return false;
    }
    for (int index = 0; index < device->nb_media_types; ++index) {
        if (device->media_types[index] == type) {
            return true;
        }
    }
    return false;
}

std::vector<CaptureSourceInfo> enumerateDevices(MediaKind kind,
                                                std::string* error) {
    ensureDevicesRegistered();
    const AVInputFormat* format = av_find_input_format("dshow");
    if (!format) {
        if (error) {
            *error = "FFmpeg DirectShow input is unavailable";
        }
        return {};
    }
    AVDeviceInfoList* list = nullptr;
    const auto status =
        avdevice_list_input_sources(format, nullptr, nullptr, &list);
    if (status < 0 || !list) {
        if (error) {
            *error = "failed to enumerate capture devices: " +
                ffmpegError(status);
        }
        return {};
    }

    std::vector<CaptureSourceInfo> result;
    const auto type = mediaType(kind);
    bool foundDefault = false;
    for (int index = 0; index < list->nb_devices; ++index) {
        const auto* device = list->devices[index];
        if (!supports(device, type)) {
            continue;
        }
        CaptureSourceInfo value;
        value.id = device->device_name ? device->device_name : "";
        value.name = device->device_description
            ? device->device_description
            : value.id;
        if (value.name.empty()) {
            value.name = value.id;
        }
        value.kind = kind;
        value.backend = "ffmpeg/dshow";
        value.isDefault = index == list->default_device;
        foundDefault = foundDefault || value.isDefault;
        if (!value.id.empty()) {
            result.push_back(std::move(value));
        }
    }
    if (!foundDefault && !result.empty()) {
        result.front().isDefault = true;
    }
    avdevice_free_list_devices(&list);
    return result;
}

std::string selectDevice(MediaKind kind,
                         const std::string& selector,
                         std::string& displayName,
                         std::string& error) {
    const auto devices = enumerateDevices(kind, &error);
    if (!error.empty()) {
        return {};
    }
    if (devices.empty()) {
        error = kind == MediaKind::Audio
            ? "no DirectShow microphone was found"
            : "no DirectShow camera was found";
        return {};
    }
    const CaptureSourceInfo* selected = nullptr;
    if (selector.empty() || selector == "auto") {
        selected = &devices.front();
        for (const auto& value : devices) {
            if (value.isDefault) {
                selected = &value;
                break;
            }
        }
    } else {
        int index = -1;
        const auto parsed = std::from_chars(
            selector.data(), selector.data() + selector.size(), index);
        if (parsed.ec == std::errc{} &&
            parsed.ptr == selector.data() + selector.size()) {
            if (index < 0 ||
                index >= static_cast<int>(devices.size())) {
                error = "capture device index is out of range";
                return {};
            }
            selected = &devices[static_cast<std::size_t>(index)];
        } else {
            for (const auto& value : devices) {
                if (value.id == selector || value.name == selector) {
                    selected = &value;
                    break;
                }
            }
        }
    }
    if (!selected) {
        error = "capture device was not found";
        return {};
    }
    displayName = selected->name;
    return selected->id;
}

SourceResolution resolveSource(MediaKind kind,
                               const std::string& configured) {
    SourceResolution result;
    const auto input = configured.empty() ? "auto" : configured;
    if (input == "null" || input == "none" || input == "empty") {
        result.source.format = "null";
        result.source.displayName = "empty source";
        result.source.isNull = true;
        return result;
    }
    if (input.rfind("lavfi:", 0) == 0) {
        result.source.format = "lavfi";
        result.source.url = input.substr(6);
        result.source.displayName = result.source.url;
        return result;
    }
    if (input.rfind("gdigrab:", 0) == 0 &&
        kind == MediaKind::Video) {
        result.source.format = "gdigrab";
        result.source.url = input.substr(8);
        result.source.displayName = result.source.url;
        return result;
    }
    if (input.rfind("dshow:", 0) == 0) {
        auto url = input.substr(6);
        const auto prefix =
            kind == MediaKind::Audio ? "audio=" : "video=";
        if (url.rfind(prefix, 0) != 0) {
            url = prefix + url;
        }
        result.source.format = "dshow";
        result.source.url = std::move(url);
        result.source.displayName = result.source.url;
        return result;
    }

    std::string deviceError;
    const auto id = selectDevice(
        kind, input, result.source.displayName, deviceError);
    if (id.empty()) {
        result.error = std::move(deviceError);
        return result;
    }
    result.source.format = "dshow";
    result.source.url =
        std::string(kind == MediaKind::Audio ? "audio=" : "video=") + id;
    return result;
}

int interruptCallback(void* opaque) {
    return !static_cast<std::atomic<bool>*>(opaque)->load();
}

class CaptureReader {
public:
    Status open(const SourceSpec& source,
                MediaKind kind,
                int width,
                int height,
                int fps) {
        close();
        format_ = source.format;
        kind_ = kind;
        const AVInputFormat* inputFormat =
            av_find_input_format(source.format.c_str());
        AVDictionary* options = nullptr;
        if (source.format == "lavfi") {
            av_dict_set(&options, "realtime", "1", 0);
            if (kind == MediaKind::Video) {
                const auto size =
                    std::to_string(width) + "x" + std::to_string(height);
                av_dict_set(&options, "video_size", size.c_str(), 0);
                av_dict_set(
                    &options, "framerate", std::to_string(fps).c_str(), 0);
            }
        }
        running_.store(true, std::memory_order_release);
        const auto status = avformat_open_input(
            &input_, source.url.c_str(), inputFormat, &options);
        av_dict_free(&options);
        if (status < 0) {
            close();
            return failure(
                "failed to open capture input: " + ffmpegError(status));
        }
        input_->interrupt_callback.callback = &interruptCallback;
        input_->interrupt_callback.opaque = &running_;
        if (avformat_find_stream_info(input_, nullptr) < 0) {
            close();
            return failure("capture input has no stream information");
        }
        streamIndex_ = av_find_best_stream(
            input_, mediaType(kind), -1, -1, nullptr, 0);
        if (streamIndex_ < 0) {
            close();
            return failure("capture input has no requested media stream");
        }
        const auto* stream = input_->streams[streamIndex_];
        const AVCodec* codec =
            avcodec_find_decoder(stream->codecpar->codec_id);
        if (!codec) {
            close();
            return failure("capture input decoder is unavailable");
        }
        decoder_ = avcodec_alloc_context3(codec);
        if (!decoder_ ||
            avcodec_parameters_to_context(decoder_, stream->codecpar) < 0 ||
            avcodec_open2(decoder_, codec, nullptr) < 0) {
            close();
            return failure("failed to open capture decoder");
        }
        packet_ = av_packet_alloc();
        frame_ = av_frame_alloc();
        if (!packet_ || !frame_) {
            close();
            return failure("failed to allocate capture buffers");
        }
        return {};
    }

    bool read(const std::function<void(const AVFrame&)>& callback) {
        if (!input_ || !decoder_ || !packet_ || !frame_ ||
            !callback || !running_.load(std::memory_order_acquire)) {
            return false;
        }
        while (running_.load(std::memory_order_acquire) &&
               av_read_frame(input_, packet_) >= 0) {
            bool emitted = false;
            if (packet_->stream_index == streamIndex_ &&
                avcodec_send_packet(decoder_, packet_) >= 0) {
                while (running_.load(std::memory_order_acquire) &&
                       avcodec_receive_frame(decoder_, frame_) >= 0) {
                    callback(*frame_);
                    av_frame_unref(frame_);
                    emitted = true;
                }
            }
            av_packet_unref(packet_);
            if (emitted) {
                return true;
            }
        }
        return false;
    }

    void abort() noexcept {
        running_.store(false, std::memory_order_release);
    }

    void close() noexcept {
        abort();
        av_packet_free(&packet_);
        av_frame_free(&frame_);
        avcodec_free_context(&decoder_);
        if (input_) {
            avformat_close_input(&input_);
        }
        streamIndex_ = -1;
    }

private:
    std::string format_;
    MediaKind kind_ = MediaKind::Unknown;
    AVFormatContext* input_ = nullptr;
    AVCodecContext* decoder_ = nullptr;
    AVPacket* packet_ = nullptr;
    AVFrame* frame_ = nullptr;
    int streamIndex_ = -1;
    std::atomic<bool> running_{false};
};

void notifyState(const std::weak_ptr<ICaptureObserver>& observer,
                 CaptureState state) {
    if (auto value = observer.lock()) {
        value->onCaptureStateChanged(state);
    }
}

void notifyError(const std::weak_ptr<ICaptureObserver>& observer,
                 const Status& status) {
    if (auto value = observer.lock()) {
        value->onCaptureError(status);
    }
}

class AudioCapture final : public IAudioCaptureSource {
public:
    explicit AudioCapture(CaptureConfig config)
        : genericConfig_(std::move(config)) {
        typedConfig_.format = AudioFormat{
            48000, 2, AudioSampleFormat::Signed16};
    }

    ~AudioCapture() override {
        close();
    }

    Status configure(const AudioCaptureConfig& config) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ != CaptureState::Closed) {
            return failure("audio capture must be closed before configuration");
        }
        typedConfig_ = config;
        return {};
    }

    Status open(const CaptureConfig& config) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ != CaptureState::Closed) {
            return failure("audio capture is already open");
        }
        genericConfig_ = config;
        genericConfig_.kind = MediaKind::Audio;
        auto resolution = resolveSource(
            MediaKind::Audio, genericConfig_.source);
        if (!resolution.ok()) {
            return failure(resolution.error);
        }
        source_ = std::move(resolution.source);
        state_ = CaptureState::Closed;
        return {};
    }

    Status start() override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (source_.format.empty()) {
            return failure("audio capture is not open");
        }
        stopThread();
        nextPtsMicros_ = 0;
        if (!source_.isNull) {
            auto status = reader_.open(
                source_, MediaKind::Audio, 0, 0, 0);
            if (!status.ok) {
                return status;
            }
        }
        running_.store(true, std::memory_order_release);
        thread_ = std::thread([this] { captureLoop(); });
        state_ = CaptureState::Capturing;
        notifyState(observer_, state_);
        return {};
    }

    Status pause() override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ != CaptureState::Capturing) {
            return failure("audio capture is not running");
        }
        muted_.store(true, std::memory_order_relaxed);
        state_ = CaptureState::Paused;
        notifyState(observer_, state_);
        return {};
    }

    Status resume() override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ != CaptureState::Paused) {
            return failure("audio capture is not paused");
        }
        muted_.store(false, std::memory_order_relaxed);
        state_ = CaptureState::Capturing;
        notifyState(observer_, state_);
        return {};
    }

    void stop() noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        stopThread();
        state_ = CaptureState::Closed;
    }

    void close() noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        stopThread();
        source_ = {};
        reader_.close();
        state_ = CaptureState::Closed;
    }

    void setAudioSink(
        std::weak_ptr<IAudioFrameSink> sink) override {
        std::lock_guard<std::mutex> lock(sinkMutex_);
        sink_ = std::move(sink);
    }

    void setObserver(
        std::weak_ptr<ICaptureObserver> observer) override {
        std::lock_guard<std::mutex> lock(mutex_);
        observer_ = std::move(observer);
    }

    CaptureSourceInfo info() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return CaptureSourceInfo{
            "ffmpeg-audio",
            source_.displayName.empty()
                ? genericConfig_.source : source_.displayName,
            MediaKind::Audio,
            "ffmpeg",
            genericConfig_.source == "auto",
            genericConfig_.options,
        };
    }

    CaptureState state() const noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        return state_;
    }

private:
    void stopThread() noexcept {
        running_.store(false, std::memory_order_release);
        reader_.abort();
        if (thread_.joinable() &&
            thread_.get_id() != std::this_thread::get_id()) {
            thread_.join();
        }
        reader_.close();
        swr_free(&resampler_);
    }

    void captureLoop() {
        if (source_.isNull) {
            nullLoop();
        } else {
            while (running_.load(std::memory_order_acquire) &&
                   reader_.read([this](const AVFrame& frame) {
                       appendAudio(frame);
                   })) {
            }
        }
        running_.store(false, std::memory_order_release);
    }

    void nullLoop() {
        const auto samples = std::max(
            1, typedConfig_.format.sampleRate / 50);
        const auto duration = std::chrono::microseconds(
            samples * 1000000LL / typedConfig_.format.sampleRate);
        auto next = std::chrono::steady_clock::now();
        std::vector<std::int16_t> silence(
            static_cast<std::size_t>(samples) *
            typedConfig_.format.channels);
        while (running_.load(std::memory_order_acquire)) {
            emitPcm(
                reinterpret_cast<const std::uint8_t*>(silence.data()),
                silence.size() * sizeof(std::int16_t));
            next += duration;
            const auto now = std::chrono::steady_clock::now();
            if (next > now) {
                std::this_thread::sleep_until(next);
            } else {
                next = now + duration;
            }
        }
    }

    bool ensureResampler(const AVFrame& frame) {
        if (resampler_) {
            return true;
        }
        AVChannelLayout inputLayout;
        AVChannelLayout outputLayout;
        if (frame.ch_layout.nb_channels > 0) {
            av_channel_layout_copy(&inputLayout, &frame.ch_layout);
        } else {
            av_channel_layout_default(
                &inputLayout, typedConfig_.format.channels);
        }
        av_channel_layout_default(
            &outputLayout, typedConfig_.format.channels);
        const auto status = swr_alloc_set_opts2(
            &resampler_,
            &outputLayout, AV_SAMPLE_FMT_S16,
            typedConfig_.format.sampleRate,
            &inputLayout, static_cast<AVSampleFormat>(frame.format),
            frame.sample_rate, 0, nullptr);
        av_channel_layout_uninit(&inputLayout);
        av_channel_layout_uninit(&outputLayout);
        return status >= 0 && swr_init(resampler_) >= 0;
    }

    void appendAudio(const AVFrame& frame) {
        if (!ensureResampler(frame)) {
            return;
        }
        const auto capacity = av_rescale_rnd(
            swr_get_delay(resampler_, frame.sample_rate) +
                frame.nb_samples,
            typedConfig_.format.sampleRate,
            frame.sample_rate, AV_ROUND_UP);
        if (capacity <= 0) {
            return;
        }
        std::vector<std::int16_t> converted(
            static_cast<std::size_t>(capacity) *
            typedConfig_.format.channels);
        std::uint8_t* output[] = {
            reinterpret_cast<std::uint8_t*>(converted.data())
        };
        const auto input =
            const_cast<const std::uint8_t**>(frame.extended_data);
        const auto samples = swr_convert(
            resampler_, output, capacity,
            input, frame.nb_samples);
        if (samples <= 0) {
            return;
        }
        const auto bytes =
            static_cast<std::size_t>(samples) *
            typedConfig_.format.channels * sizeof(std::int16_t);
        if (muted_.load(std::memory_order_relaxed)) {
            std::fill(
                converted.begin(),
                converted.begin() + samples * typedConfig_.format.channels,
                0);
        }
        emitPcm(
            reinterpret_cast<const std::uint8_t*>(converted.data()),
            bytes);
    }

    void emitPcm(const std::uint8_t* data, std::size_t size) {
        auto sink = [&] {
            std::lock_guard<std::mutex> lock(sinkMutex_);
            return sink_.lock();
        }();
        if (!sink || !data || size == 0) {
            return;
        }
        auto buffer = copyToMediaBuffer(data, size);
        if (!buffer.buffer) {
            return;
        }
        AudioFrame frame;
        frame.format = typedConfig_.format;
        frame.samples = std::move(buffer);
        frame.ptsMicros = nextPtsMicros_;
        nextPtsMicros_ += static_cast<std::int64_t>(
            size / (typedConfig_.format.channels * sizeof(std::int16_t)) *
            1000000LL / typedConfig_.format.sampleRate);
        sink->onAudioFrame(frame);
    }

    mutable std::mutex mutex_;
    std::mutex sinkMutex_;
    CaptureConfig genericConfig_;
    AudioCaptureConfig typedConfig_;
    SourceSpec source_;
    CaptureReader reader_;
    SwrContext* resampler_ = nullptr;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> muted_{false};
    std::weak_ptr<IAudioFrameSink> sink_;
    std::weak_ptr<ICaptureObserver> observer_;
    CaptureState state_ = CaptureState::Closed;
    std::int64_t nextPtsMicros_ = 0;
};

class VideoCapture final : public IVideoCaptureSource {
public:
    explicit VideoCapture(CaptureConfig config)
        : genericConfig_(std::move(config)) {
        typedConfig_.format.width = 640;
        typedConfig_.format.height = 360;
        typedConfig_.format.frameRate = Rational{15, 1};
        typedConfig_.format.pixelFormat = VideoPixelFormat::I420;
    }

    ~VideoCapture() override {
        close();
    }

    Status configure(const VideoCaptureConfig& config) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ != CaptureState::Closed) {
            return failure("video capture must be closed before configuration");
        }
        typedConfig_ = config;
        return {};
    }

    Status open(const CaptureConfig& config) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ != CaptureState::Closed) {
            return failure("video capture is already open");
        }
        genericConfig_ = config;
        genericConfig_.kind = MediaKind::Video;
        auto resolution = resolveSource(
            MediaKind::Video, genericConfig_.source);
        if (!resolution.ok()) {
            return failure(resolution.error);
        }
        source_ = std::move(resolution.source);
        state_ = CaptureState::Closed;
        return {};
    }

    Status start() override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (source_.format.empty()) {
            return failure("video capture is not open");
        }
        stopThread();
        const auto fps = std::max(
            1, typedConfig_.format.frameRate.numerator /
                   std::max(1, typedConfig_.format.frameRate.denominator));
        if (!source_.isNull) {
            auto status = reader_.open(
                source_, MediaKind::Video,
                typedConfig_.format.width,
                typedConfig_.format.height,
                fps);
            if (!status.ok) {
                return status;
            }
        }
        running_.store(true, std::memory_order_release);
        thread_ = std::thread([this] { captureLoop(); });
        state_ = CaptureState::Capturing;
        notifyState(observer_, state_);
        return {};
    }

    Status pause() override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ != CaptureState::Capturing) {
            return failure("video capture is not running");
        }
        muted_.store(true, std::memory_order_relaxed);
        state_ = CaptureState::Paused;
        notifyState(observer_, state_);
        return {};
    }

    Status resume() override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ != CaptureState::Paused) {
            return failure("video capture is not paused");
        }
        muted_.store(false, std::memory_order_relaxed);
        state_ = CaptureState::Capturing;
        notifyState(observer_, state_);
        return {};
    }

    void stop() noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        stopThread();
        state_ = CaptureState::Closed;
    }

    void close() noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        stopThread();
        source_ = {};
        reader_.close();
        state_ = CaptureState::Closed;
    }

    void setVideoSink(
        std::weak_ptr<IVideoFrameSink> sink) override {
        std::lock_guard<std::mutex> lock(sinkMutex_);
        sink_ = std::move(sink);
    }

    void setObserver(
        std::weak_ptr<ICaptureObserver> observer) override {
        std::lock_guard<std::mutex> lock(mutex_);
        observer_ = std::move(observer);
    }

    CaptureSourceInfo info() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return CaptureSourceInfo{
            "ffmpeg-video",
            source_.displayName.empty()
                ? genericConfig_.source : source_.displayName,
            MediaKind::Video,
            "ffmpeg",
            genericConfig_.source == "auto",
            genericConfig_.options,
        };
    }

    CaptureState state() const noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        return state_;
    }

private:
    void stopThread() noexcept {
        running_.store(false, std::memory_order_release);
        reader_.abort();
        if (thread_.joinable() &&
            thread_.get_id() != std::this_thread::get_id()) {
            thread_.join();
        }
        reader_.close();
        sws_freeContext(scaler_);
        scaler_ = nullptr;
    }

    void captureLoop() {
        if (source_.isNull) {
            const auto fps = std::max(
                1, typedConfig_.format.frameRate.numerator /
                       std::max(1, typedConfig_.format.frameRate.denominator));
            const auto duration =
                std::chrono::microseconds(1000000 / fps);
            auto next = std::chrono::steady_clock::now();
            while (running_.load(std::memory_order_acquire)) {
                emitBlackFrame();
                next += duration;
                const auto now = std::chrono::steady_clock::now();
                if (next > now) {
                    std::this_thread::sleep_until(next);
                } else {
                    next = now + duration;
                }
            }
        } else {
            while (running_.load(std::memory_order_acquire) &&
                   reader_.read([this](const AVFrame& frame) {
                       emitDecodedFrame(frame);
                   })) {
            }
        }
        running_.store(false, std::memory_order_release);
    }

    void emitBlackFrame() {
        const auto width = typedConfig_.format.width;
        const auto height = typedConfig_.format.height;
        const auto ySize = static_cast<std::size_t>(width) * height;
        const auto uvSize =
            static_cast<std::size_t>(width / 2) * (height / 2);
        auto buffer = allocateMediaBuffer(ySize + uvSize * 2);
        if (!buffer) {
            return;
        }
        std::memset(buffer->data(), 16, ySize);
        std::memset(buffer->data() + ySize, 128, uvSize * 2);
        emitFrame(std::move(buffer), ySize + uvSize * 2);
    }

    void emitDecodedFrame(const AVFrame& frame) {
        if (muted_.load(std::memory_order_relaxed)) {
            emitBlackFrame();
            return;
        }
        const auto width = typedConfig_.format.width;
        const auto height = typedConfig_.format.height;
        scaler_ = sws_getCachedContext(
            scaler_,
            frame.width, frame.height,
            static_cast<AVPixelFormat>(frame.format),
            width, height, AV_PIX_FMT_YUV420P,
            SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!scaler_) {
            return;
        }
        AVFrame* converted = av_frame_alloc();
        if (!converted) {
            return;
        }
        converted->format = AV_PIX_FMT_YUV420P;
        converted->width = width;
        converted->height = height;
        if (av_frame_get_buffer(converted, 32) < 0) {
            av_frame_free(&converted);
            return;
        }
        sws_scale(
            scaler_,
            frame.data, frame.linesize, 0, frame.height,
            converted->data, converted->linesize);
        const auto ySize = static_cast<std::size_t>(width) * height;
        const auto uvSize =
            static_cast<std::size_t>(width / 2) * (height / 2);
        auto buffer = allocateMediaBuffer(ySize + uvSize * 2);
        if (!buffer) {
            av_frame_free(&converted);
            return;
        }
        std::uint8_t* destination = buffer->data();
        for (int row = 0; row < height; ++row) {
            std::memcpy(
                destination + static_cast<std::size_t>(row) * width,
                converted->data[0] +
                    static_cast<std::size_t>(row) * converted->linesize[0],
                width);
        }
        destination += ySize;
        for (int row = 0; row < height / 2; ++row) {
            std::memcpy(
                destination + static_cast<std::size_t>(row) * (width / 2),
                converted->data[1] +
                    static_cast<std::size_t>(row) * converted->linesize[1],
                width / 2);
        }
        destination += uvSize;
        for (int row = 0; row < height / 2; ++row) {
            std::memcpy(
                destination + static_cast<std::size_t>(row) * (width / 2),
                converted->data[2] +
                    static_cast<std::size_t>(row) * converted->linesize[2],
                width / 2);
        }
        av_frame_free(&converted);
        emitFrame(std::move(buffer), ySize + uvSize * 2);
    }

    void emitFrame(MediaBuffer buffer,
                   std::size_t size) {
        auto sink = [&] {
            std::lock_guard<std::mutex> lock(sinkMutex_);
            return sink_.lock();
        }();
        if (!sink || !buffer) {
            return;
        }
        VideoFrame frame;
        frame.format = typedConfig_.format;
        frame.pixels = MediaBufferView{std::move(buffer), 0, size};
        frame.ptsMicros = nextPtsMicros_;
        nextPtsMicros_ += static_cast<std::int64_t>(
            1000000LL * typedConfig_.format.frameRate.denominator /
            std::max(1, typedConfig_.format.frameRate.numerator));
        sink->onVideoFrame(frame);
    }

    mutable std::mutex mutex_;
    std::mutex sinkMutex_;
    CaptureConfig genericConfig_;
    VideoCaptureConfig typedConfig_;
    SourceSpec source_;
    CaptureReader reader_;
    SwsContext* scaler_ = nullptr;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> muted_{false};
    std::weak_ptr<IVideoFrameSink> sink_;
    std::weak_ptr<ICaptureObserver> observer_;
    CaptureState state_ = CaptureState::Closed;
    std::int64_t nextPtsMicros_ = 0;
};

} // namespace

FFmpegCaptureProvider::FFmpegCaptureProvider() = default;
FFmpegCaptureProvider::~FFmpegCaptureProvider() = default;

std::vector<CaptureSourceInfo> FFmpegCaptureProvider::list(
    MediaKind kind) const {
    if (kind == MediaKind::Unknown) {
        return {};
    }
    std::string error;
    return enumerateDevices(kind, &error);
}

std::unique_ptr<ICaptureSource> FFmpegCaptureProvider::create(
    const CaptureConfig& config,
    Status* status) {
    if (config.kind == MediaKind::Audio) {
        if (status) {
            *status = {};
        }
        return std::make_unique<AudioCapture>(config);
    }
    if (config.kind == MediaKind::Video) {
        if (status) {
            *status = {};
        }
        return std::make_unique<VideoCapture>(config);
    }
    if (status) {
        *status = failure("unsupported capture media kind");
    }
    return {};
}

} // namespace iris::media::v2
