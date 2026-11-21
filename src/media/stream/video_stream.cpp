#include "media/stream/video_stream.h"

#include "media/codec/ffmpeg_codec_impl.h"
#include "media/stream/stream_internal.h"

#include <mutex>
#include <utility>

namespace iris::media::v2 {
namespace {

using stream_internal::EncodedStreamState;
using stream_internal::VideoFrameState;

CodecType codecFromAttributes(const AttributeMap& values) {
    const auto iterator = values.find("codecType");
    if (iterator == values.end()) {
        return CodecType::Unknown;
    }
    if (iterator->second == "h264" || iterator->second == "H264") {
        return CodecType::H264;
    }
    if (iterator->second == "opus" || iterator->second == "Opus") {
        return CodecType::Opus;
    }
    return CodecType::Unknown;
}

class VideoDecodeSink final : public IVideoFrameSink {
public:
    VideoDecodeSink(std::weak_ptr<EncodedStreamState> encoded,
                    std::weak_ptr<VideoFrameState> frame)
        : encoded_(std::move(encoded)),
          frame_(std::move(frame)) {
    }

    void onVideoFrame(const VideoFrame& frameValue) override {
        if (auto encoded = encoded_.lock()) {
            std::lock_guard<std::mutex> lock(encoded->mutex);
            encoded->stats.media.samplesProduced += 1;
        }
        if (auto frame = frame_.lock()) {
            std::shared_ptr<IVideoFrameSink> output;
            {
                std::lock_guard<std::mutex> lock(frame->mutex);
                output = frame->outputSink.lock();
            }
            if (output) {
                output->onVideoFrame(frameValue);
            }
        }
    }

private:
    std::weak_ptr<EncodedStreamState> encoded_;
    std::weak_ptr<VideoFrameState> frame_;
};

} // namespace

VideoEncodedStream::VideoEncodedStream() {
    auto state = std::make_shared<EncodedStreamState>();
    state->config.kind = MediaKind::Video;
    state->config.direction = MediaDirection::SendRecv;
    state->info.state = StreamState::Idle;
    state->send = [this](const EncodedPacket& packet) {
        return sendEncodedPacket(packet);
    };
    stream_internal::registerEncodedStream(this, std::move(state));
}

VideoEncodedStream::~VideoEncodedStream() {
    shutdown();
    if (auto state = stream_internal::encodedStreamState(this)) {
        state->send = {};
        state->receive = {};
    }
    stream_internal::unregisterEncodedStream(this);
}

Status VideoEncodedStream::configure(const StreamConfig& value) {
    auto state = stream_internal::encodedStreamState(this);
    if (!state) {
        return {false, 1, "video stream state is unavailable"};
    }
    if (value.kind != MediaKind::Unknown &&
        value.kind != MediaKind::Video) {
        return {false, 1, "video stream requires video media kind"};
    }
    std::lock_guard<std::mutex> lock(state->mutex);
    state->config = value;
    state->config.kind = MediaKind::Video;
    state->info.state = StreamState::Idle;
    return {};
}

const StreamConfig& VideoEncodedStream::config() const noexcept {
    if (auto state = stream_internal::encodedStreamState(this)) {
        return state->config;
    }
    static const StreamConfig empty{};
    return empty;
}

Status VideoEncodedStream::start() {
    auto state = stream_internal::encodedStreamState(this);
    if (!state) {
        return {false, 1, "video stream state is unavailable"};
    }
    std::lock_guard<std::mutex> lock(state->mutex);
    if (!state->transport) {
        state->info.state = StreamState::Failed;
        return {false, 1, "video stream has no attached transport"};
    }
    state->info.transport = state->transport->info();
    state->info.state = StreamState::Active;
    return {};
}

void VideoEncodedStream::shutdown() noexcept {
    auto state = stream_internal::encodedStreamState(this);
    if (!state) {
        return;
    }
    detach();
    std::lock_guard<std::mutex> lock(state->mutex);
    state->info.state = StreamState::Stopped;
}

Status VideoEncodedStream::attach(
    std::shared_ptr<IEncodedPacketTransport> transport) {
    if (!transport) {
        return {false, 1, "video stream transport is null"};
    }
    auto state = stream_internal::encodedStreamState(this);
    auto bridge = stream_internal::encodedSinkBridge(this);
    if (!state || !bridge) {
        return {false, 1, "video stream state is unavailable"};
    }
    std::shared_ptr<IEncodedPacketTransport> previous;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        previous = state->transport;
        state->transport = transport;
        state->info.transport = transport->info();
    }
    if (previous && previous != transport) {
        previous->setSink(std::weak_ptr<IEncodedPacketSink>{});
    }
    transport->setSink(bridge);
    return {};
}

void VideoEncodedStream::detach() noexcept {
    auto state = stream_internal::encodedStreamState(this);
    if (!state) {
        return;
    }
    std::shared_ptr<IEncodedPacketTransport> transport;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        transport = std::move(state->transport);
        state->info.transport = {};
    }
    if (transport) {
        transport->setSink(std::weak_ptr<IEncodedPacketSink>{});
    }
}

Status VideoEncodedStream::sendEncodedPacket(const EncodedPacket& packet) {
    auto state = stream_internal::encodedStreamState(this);
    if (!state) {
        return {false, 1, "video stream state is unavailable"};
    }
    std::shared_ptr<IEncodedPacketTransport> transport;
    StreamId streamId = 0;
    CodecType fallbackCodec = CodecType::Unknown;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        transport = state->transport;
        streamId = state->config.id;
        fallbackCodec = codecFromAttributes(state->config.attributes);
        if (state->info.state != StreamState::Active) {
            return {false, 1, "video stream is not active"};
        }
    }
    if (!transport) {
        return {false, 1, "video stream has no transport"};
    }
    auto value = packet;
    value.streamId = streamId;
    value.kind = MediaKind::Video;
    if (value.codecType == CodecType::Unknown) {
        value.codecType = fallbackCodec;
    }
    auto result = transport->send(value);
    if (!result.ok) {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->stats.media.codecErrors += 1;
    }
    return result;
}

void VideoEncodedStream::onEncodedPacket(const EncodedPacket& packet) {
    auto state = stream_internal::encodedStreamState(this);
    if (!state) {
        return;
    }
    std::function<void(const EncodedPacket&)> receive;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        receive = state->receive;
    }
    if (receive) {
        receive(packet);
    }
}

StreamInfo VideoEncodedStream::info() const {
    if (auto state = stream_internal::encodedStreamState(this)) {
        std::lock_guard<std::mutex> lock(state->mutex);
        auto result = state->info;
        if (state->transport) {
            result.transport = state->transport->info();
        }
        return result;
    }
    return {};
}

StreamStats VideoEncodedStream::stats() const {
    if (auto state = stream_internal::encodedStreamState(this)) {
        std::lock_guard<std::mutex> lock(state->mutex);
        auto result = state->stats;
        if (state->transport) {
            result.transport = state->transport->stats();
        }
        return result;
    }
    return {};
}

VideoFrameStream::VideoFrameStream() {
    auto state = std::make_shared<VideoFrameState>();
    auto encoded = stream_internal::encodedStreamState(this);
    state->decodeSink = std::make_shared<VideoDecodeSink>(encoded, state);
    stream_internal::registerVideoFrameState(this, state);
    if (encoded) {
        std::lock_guard<std::mutex> lock(encoded->mutex);
        encoded->receive = [this](const EncodedPacket& packet) {
            if (auto frame = stream_internal::videoFrameState(this)) {
                std::shared_ptr<IVideoDecoder> decoder;
                {
                    std::lock_guard<std::mutex> frameLock(frame->mutex);
                    decoder = frame->decoder;
                }
                if (decoder) {
                    (void)decoder->decode(packet);
                }
            }
        };
    }
}

VideoFrameStream::~VideoFrameStream() {
    if (auto encoded = stream_internal::encodedStreamState(this)) {
        std::lock_guard<std::mutex> lock(encoded->mutex);
        encoded->receive = {};
    }
    auto state = stream_internal::videoFrameState(this);
    if (state) {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->encoder.reset();
        state->decoder.reset();
        state->decodeSink.reset();
    }
    stream_internal::unregisterVideoFrameState(this);
}

Status VideoFrameStream::configureVideo(
    const VideoFrameStreamConfig& configValue) {
    auto result = VideoEncodedStream::configure(configValue.stream);
    if (!result.ok) {
        return result;
    }
    auto state = stream_internal::videoFrameState(this);
    if (!state) {
        return {false, 1, "video frame stream state is unavailable"};
    }
    CodecType encoderType = CodecType::Unknown;
    CodecType decoderType = CodecType::Unknown;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->inputFormat = configValue.inputFormat;
        state->outputFormat = configValue.outputFormat;
        state->encoderType = configValue.encoderType;
        state->decoderType = configValue.decoderType;
        state->bitrateKbps = configValue.bitrateKbps;
        state->parameters = configValue.codecParameters;
        encoderType = state->encoderType;
        decoderType = state->decoderType;
    }
    if (encoderType != CodecType::Unknown) {
        auto encoder = createVideoEncoder(encoderType);
        if (!encoder) {
            return {false, 1, "requested video encoder is unavailable"};
        }
        result = setEncoder(std::move(encoder));
        if (!result.ok) {
            return result;
        }
    }
    if (decoderType != CodecType::Unknown) {
        auto decoder = createVideoDecoder(decoderType);
        if (!decoder) {
            return {false, 1, "requested video decoder is unavailable"};
        }
        result = setDecoder(std::move(decoder));
        if (!result.ok) {
            return result;
        }
    }
    return {};
}

Status VideoFrameStream::setEncoder(
    std::unique_ptr<IVideoEncoder> encoder) {
    auto frame = stream_internal::videoFrameState(this);
    if (!frame || !encoder) {
        return {false, 1, "invalid video encoder"};
    }
    encoder->setSink(stream_internal::encodedSendBridge(this));
    VideoEncoderConfig config;
    {
        std::lock_guard<std::mutex> lock(frame->mutex);
        config.codecType = frame->encoderType;
        config.inputFormat = frame->inputFormat;
        config.bitrateKbps = frame->bitrateKbps;
        config.parameters = frame->parameters;
        frame->encoder = std::move(encoder);
    }
    return frame->encoder->configure(config);
}

Status VideoFrameStream::setDecoder(
    std::unique_ptr<IVideoDecoder> decoder) {
    auto frame = stream_internal::videoFrameState(this);
    if (!frame || !decoder) {
        return {false, 1, "invalid video decoder"};
    }
    decoder->setSink(frame->decodeSink);
    VideoDecoderConfig config;
    {
        std::lock_guard<std::mutex> lock(frame->mutex);
        config.codecType = frame->decoderType;
        config.outputFormat = frame->outputFormat;
        config.parameters = frame->parameters;
        frame->decoder = std::shared_ptr<IVideoDecoder>(
            std::move(decoder));
    }
    return frame->decoder->configure(config);
}

Status VideoFrameStream::setMuted(bool muted) {
    auto state = stream_internal::videoFrameState(this);
    if (!state) {
        return {false, 1, "video frame stream state is unavailable"};
    }
    std::lock_guard<std::mutex> lock(state->mutex);
    state->muted = muted;
    return {};
}

void VideoFrameStream::onVideoFrame(const VideoFrame& frameValue) {
    auto state = stream_internal::videoFrameState(this);
    if (!state) {
        return;
    }
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->muted) {
        return;
    }
    auto encoded = stream_internal::encodedStreamState(this);
    if (!state->encoder) {
        if (encoded) {
            std::lock_guard<std::mutex> encodedLock(encoded->mutex);
            encoded->stats.media.codecErrors += 1;
        }
        return;
    }
    if (encoded) {
        std::lock_guard<std::mutex> encodedLock(encoded->mutex);
        encoded->stats.media.samplesConsumed += 1;
    }
    const auto result = state->encoder->encode(frameValue);
    if (!result.ok && encoded) {
        std::lock_guard<std::mutex> encodedLock(encoded->mutex);
        encoded->stats.media.codecErrors += 1;
    }
}

} // namespace iris::media::v2
