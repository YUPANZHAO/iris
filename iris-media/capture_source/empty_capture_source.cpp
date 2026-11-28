#include "capture_source/empty_capture_source.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstring>
#include <exception>
#include <memory>
#include <string_view>
#include <utility>

namespace iris::media::capture {
namespace {

using namespace std::chrono_literals;

std::size_t bytesPerSample(AudioSampleFormat format) {
    return format == AudioSampleFormat::Float32
        ? sizeof(float)
        : sizeof(std::int16_t);
}

std::size_t planeSize(int width, int height) {
    return static_cast<std::size_t>(width) *
           static_cast<std::size_t>(height);
}

std::size_t frameByteCount(const VideoFormat& format) {
    const auto luma = planeSize(format.width, format.height);
    switch (format.pixelFormat) {
    case VideoPixelFormat::I420:
        return luma +
               planeSize((format.width + 1) / 2,
                         (format.height + 1) / 2) *
                   2;
    case VideoPixelFormat::NV12:
        return luma +
               planeSize((format.width + 1) / 2,
                         (format.height + 1) / 2) *
                   2;
    case VideoPixelFormat::BGRA:
        return luma * 4;
    }
    return 0;
}

std::string_view glyph(char character) {
    switch (character) {
    case '0': return "111101101101111";
    case '1': return "010110010010111";
    case '2': return "110001010100111";
    case '3': return "110001010001110";
    case '4': return "101101111001001";
    case '5': return "111100110001110";
    case '6': return "011100111101111";
    case '7': return "111001010010010";
    case '8': return "111101111101111";
    case '9': return "111101111001110";
    case 'A': return "010101111101101";
    case 'B': return "110101110101110";
    case 'C': return "011100100100011";
    case 'D': return "110101101101110";
    case 'E': return "111100110100111";
    case 'F': return "111100110100100";
    case 'G': return "011100101101011";
    case 'H': return "101101111101101";
    case 'I': return "111010010010111";
    case 'J': return "001001001101010";
    case 'K': return "101101110101101";
    case 'L': return "100100100100111";
    case 'M': return "101111111101101";
    case 'N': return "110101101101101";
    case 'O': return "010101101101010";
    case 'P': return "110101110100100";
    case 'Q': return "010101101111011";
    case 'R': return "110101110101101";
    case 'S': return "011100010001110";
    case 'T': return "111010010010010";
    case 'U': return "101101101101111";
    case 'V': return "101101101101010";
    case 'W': return "101101111111101";
    case 'X': return "101101010101101";
    case 'Y': return "101101010010010";
    case 'Z': return "111001010100111";
    case '.': return "000000000000010";
    case ',': return "000000000010100";
    case '-': return "000000111000000";
    case '_': return "000000000000111";
    case ':': return "000010000010000";
    case '@': return "010101111100011";
    case '/': return "001001010100100";
    case '+': return "000010111010000";
    case '=': return "000111000111000";
    case '(': return "001010010010001";
    case ')': return "100010010010100";
    case ' ': return "000000000000000";
    default: return "110001010000010";
    }
}

std::vector<char> displayCharacters(const std::string& text) {
    std::vector<char> result;
    for (std::size_t index = 0; index < text.size();) {
        const auto byte = static_cast<unsigned char>(text[index]);
        if (byte < 0x80) {
            const auto character = static_cast<char>(byte);
            result.push_back(
                character >= 'a' && character <= 'z'
                    ? static_cast<char>(
                          std::toupper(
                              static_cast<unsigned char>(character)))
                    : character);
            ++index;
            continue;
        }
        result.push_back('?');
        do {
            ++index;
        } while (index < text.size() &&
                 (static_cast<unsigned char>(text[index]) & 0xc0) ==
                     0x80);
    }
    return result;
}

void setLuma(std::vector<std::uint8_t>& pixels,
             const VideoFormat& format,
             int x,
             int y,
             std::uint8_t value) {
    if (format.pixelFormat == VideoPixelFormat::BGRA) {
        const auto offset =
            (static_cast<std::size_t>(y) * format.width + x) * 4;
        pixels[offset] = value;
        pixels[offset + 1] = value;
        pixels[offset + 2] = value;
        pixels[offset + 3] = 255;
        return;
    }
    pixels[static_cast<std::size_t>(y) * format.width + x] = value;
}

void renderText(std::vector<std::uint8_t>& pixels,
                const VideoFormat& format,
                const std::string& text) {
    const auto characters = displayCharacters(text);
    if (characters.empty()) {
        return;
    }
    constexpr int glyphWidth = 3;
    constexpr int glyphHeight = 5;
    constexpr int glyphSpacing = 1;
    const auto textWidth =
        static_cast<int>(characters.size()) *
            (glyphWidth + glyphSpacing) -
        glyphSpacing;
    const auto scale = std::max(
        1,
        std::min(
            (format.width * 4 / 5) / std::max(1, textWidth),
            (format.height / 2) / glyphHeight));
    const auto width = textWidth * scale;
    const auto height = glyphHeight * scale;
    const auto originX = (format.width - width) / 2;
    const auto originY = (format.height - height) / 2;
    const auto textValue =
        format.pixelFormat == VideoPixelFormat::BGRA
            ? static_cast<std::uint8_t>(255)
            : static_cast<std::uint8_t>(235);

    for (std::size_t glyphIndex = 0;
         glyphIndex < characters.size();
         ++glyphIndex) {
        const auto rows = glyph(characters[glyphIndex]);
        for (int row = 0; row < glyphHeight; ++row) {
            for (int column = 0; column < glyphWidth; ++column) {
                if (rows[static_cast<std::size_t>(
                        row * glyphWidth + column)] != '1') {
                    continue;
                }
                for (int scaleY = 0; scaleY < scale; ++scaleY) {
                    for (int scaleX = 0; scaleX < scale; ++scaleX) {
                        const auto x =
                            originX +
                            static_cast<int>(glyphIndex) *
                                (glyphWidth + glyphSpacing) *
                                scale +
                            column * scale + scaleX;
                        const auto y =
                            originY + row * scale + scaleY;
                        if (x >= 0 && x < format.width &&
                            y >= 0 && y < format.height) {
                            setLuma(
                                pixels, format, x, y, textValue);
                        }
                    }
                }
            }
        }
    }
}

} // namespace

EmptyAudioCaptureSource::EmptyAudioCaptureSource(std::string text)
    : text_(std::move(text)),
      format_{48000, 2, AudioSampleFormat::Signed16} {
}

EmptyAudioCaptureSource::~EmptyAudioCaptureSource() {
    stop();
}

Status EmptyAudioCaptureSource::configure(
    const AudioCaptureConfig& config) {
    if (running()) {
        return Status::failure(
            "cannot configure a running empty audio source");
    }
    auto format = config.outputFormat;
    if (format.sampleRate <= 0) {
        format.sampleRate = 48000;
    }
    if (format.channels <= 0) {
        format.channels = 2;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    format_ = format;
    configured_ = true;
    error_.clear();
    return {};
}

Status EmptyAudioCaptureSource::start() {
    stop();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!configured_) {
            return Status::failure(
                "empty audio source is not configured");
        }
        running_ = true;
    }
    try {
        worker_ = std::thread([this] {
            run();
        });
    } catch (const std::exception& error) {
        running_ = false;
        std::lock_guard<std::mutex> lock(mutex_);
        error_ = error.what();
        return Status::failure("failed to start empty audio source");
    }
    return {};
}

void EmptyAudioCaptureSource::stop() noexcept {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        running_ = false;
    }
    condition_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
}

bool EmptyAudioCaptureSource::running() const noexcept {
    return running_.load();
}

std::string EmptyAudioCaptureSource::lastError() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return error_;
}

void EmptyAudioCaptureSource::setFrameSink(
    std::weak_ptr<AudioFrameSink> sink) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    sink_ = std::move(sink);
}

AudioFormat EmptyAudioCaptureSource::outputFormat() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return format_;
}

void EmptyAudioCaptureSource::setText(std::string text) {
    std::lock_guard<std::mutex> lock(mutex_);
    text_ = std::move(text);
}

std::string EmptyAudioCaptureSource::text() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return text_;
}

void EmptyAudioCaptureSource::run() noexcept {
    constexpr auto interval = 20ms;
    std::int64_t pts = 0;
    try {
        while (running_.load()) {
            AudioFormat format;
            std::weak_ptr<AudioFrameSink> sink;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                format = format_;
                sink = sink_;
            }
            const auto sampleCount =
                std::max(
                    1,
                    format.sampleRate *
                        static_cast<int>(interval.count()) /
                        1000);
            const auto byteCount =
                static_cast<std::size_t>(sampleCount) *
                static_cast<std::size_t>(format.channels) *
                bytesPerSample(format.sampleFormat);
            auto samples = std::make_shared<ByteBuffer>(byteCount);
            if (!samples || !*samples) {
                std::lock_guard<std::mutex> lock(mutex_);
                error_ = "failed to allocate empty audio frame";
                break;
            }
            std::memset(
                samples->data(), 0, samples->size());
            if (auto target = sink.lock()) {
                AudioFrame frame;
                frame.format = format;
                frame.samples = std::move(samples);
                frame.ptsMicros = pts;
                target->onAudioFrame(frame);
            }
            pts += 20000;
            std::unique_lock<std::mutex> lock(mutex_);
            condition_.wait_for(
                lock, interval, [&] {
                    return !running_.load();
                });
        }
    } catch (const std::exception& error) {
        std::lock_guard<std::mutex> lock(mutex_);
        error_ = error.what();
    }
    running_ = false;
}

EmptyVideoCaptureSource::EmptyVideoCaptureSource(std::string text)
    : text_(std::move(text)),
      format_{640, 360, Rational{15, 1}, VideoPixelFormat::I420} {
}

EmptyVideoCaptureSource::~EmptyVideoCaptureSource() {
    stop();
}

Status EmptyVideoCaptureSource::configure(
    const VideoCaptureConfig& config) {
    if (running()) {
        return Status::failure(
            "cannot configure a running empty video source");
    }
    auto format = config.outputFormat;
    if (format.width <= 0) {
        format.width = 640;
    }
    if (format.height <= 0) {
        format.height = 360;
    }
    if (format.frameRate.numerator <= 0 ||
        format.frameRate.denominator <= 0) {
        format.frameRate = Rational{15, 1};
    }
    if (format.pixelFormat == VideoPixelFormat::I420 ||
        format.pixelFormat == VideoPixelFormat::NV12) {
        format.width &= ~1;
        format.height &= ~1;
    }
    if (format.width <= 0 || format.height <= 0) {
        return Status::failure("empty video format is invalid");
    }
    std::lock_guard<std::mutex> lock(mutex_);
    format_ = format;
    configured_ = true;
    error_.clear();
    return renderCanvasImpl();
}

Status EmptyVideoCaptureSource::start() {
    stop();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!configured_) {
            return Status::failure(
                "empty video source is not configured");
        }
        running_ = true;
    }
    try {
        worker_ = std::thread([this] {
            run();
        });
    } catch (const std::exception& error) {
        running_ = false;
        std::lock_guard<std::mutex> lock(mutex_);
        error_ = error.what();
        return Status::failure("failed to start empty video source");
    }
    return {};
}

void EmptyVideoCaptureSource::stop() noexcept {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        running_ = false;
    }
    condition_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
}

bool EmptyVideoCaptureSource::running() const noexcept {
    return running_.load();
}

std::string EmptyVideoCaptureSource::lastError() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return error_;
}

void EmptyVideoCaptureSource::setFrameSink(
    std::weak_ptr<VideoFrameSink> sink) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    sink_ = std::move(sink);
}

VideoFormat EmptyVideoCaptureSource::outputFormat() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return format_;
}

void EmptyVideoCaptureSource::setText(std::string text) {
    std::lock_guard<std::mutex> lock(mutex_);
    text_ = std::move(text);
    if (configured_) {
        (void)renderCanvasImpl();
    }
}

std::string EmptyVideoCaptureSource::text() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return text_;
}

Status EmptyVideoCaptureSource::renderCanvasImpl() {
    const auto size = frameByteCount(format_);
    if (size == 0) {
        return Status::failure("empty video format is unsupported");
    }
    canvas_.resize(size);
    const auto luma = planeSize(format_.width, format_.height);
    if (format_.pixelFormat == VideoPixelFormat::BGRA) {
        for (std::size_t offset = 0;
             offset < canvas_.size();
             offset += 4) {
            canvas_[offset] = 40;
            canvas_[offset + 1] = 40;
            canvas_[offset + 2] = 40;
            canvas_[offset + 3] = 255;
        }
    } else {
        std::fill(
            canvas_.begin(),
            canvas_.begin() + static_cast<std::ptrdiff_t>(luma),
            static_cast<std::uint8_t>(40));
        std::fill(
            canvas_.begin() + static_cast<std::ptrdiff_t>(luma),
            canvas_.end(),
            static_cast<std::uint8_t>(128));
    }
    renderText(canvas_, format_, text_);
    return {};
}

void EmptyVideoCaptureSource::run() noexcept {
    std::int64_t pts = 0;
    try {
        while (running_.load()) {
            VideoFormat format;
            std::vector<std::uint8_t> canvas;
            std::weak_ptr<VideoFrameSink> sink;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                format = format_;
                canvas = canvas_;
                sink = sink_;
            }
            if (!canvas.empty()) {
                auto pixels = std::make_shared<ByteBuffer>(
                    canvas.data(), canvas.size());
                if (!pixels || !*pixels) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    error_ = "failed to allocate empty video frame";
                    break;
                }
                if (auto target = sink.lock()) {
                    VideoFrame frame;
                    frame.format = format;
                    frame.pixels = std::move(pixels);
                    frame.ptsMicros = pts;
                    frame.key = true;
                    target->onVideoFrame(frame);
                }
            }
            const auto interval = std::chrono::microseconds(
                1000000LL * format.frameRate.denominator /
                std::max(1, format.frameRate.numerator));
            pts += interval.count();
            std::unique_lock<std::mutex> lock(mutex_);
            condition_.wait_for(
                lock, interval, [&] {
                    return !running_.load();
                });
        }
    } catch (const std::exception& error) {
        std::lock_guard<std::mutex> lock(mutex_);
        error_ = error.what();
    }
    running_ = false;
}

} // namespace iris::media::capture
