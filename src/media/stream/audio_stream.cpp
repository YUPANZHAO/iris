#include "media/stream/audio_stream.h"

#include "media/codec/ffmpeg_codec_factory.h"

#include <utility>

namespace iris::media::v2 {
namespace {

CodecType codecFromAttributes(const AttributeMap& values) {
    const auto iterator = values.find("codecType");
    if (iterator == values.end()) {
        return CodecType::Unknown;
    }
    if (iterator->second == "opus" || iterator->second == "Opus") {
        return CodecType::Opus;
    }
    if (iterator->second == "h264" || iterator->second == "H264") {
        return CodecType::H264;
    }
    return CodecType::Unknown;
}

} // namespace

class AudioFrameStream::EncodeSink final : public IEncodedPacketSink {
public:
    explicit EncodeSink(std::weak_ptr<AudioFrameStream> stream)
        : stream_(std::move(stream)) {
    }

    void onEncodedPacket(const EncodedPacket& packet) override {
        if (auto stream = stream_.lock()) {
            (void)stream->sendEncodedPacket(packet);
        }
    }

private:
    std::weak_ptr<AudioFrameStream> stream_;
};

class AudioFrameStream::DecodeSink final : public IAudioFrameSink {
public:
    explicit DecodeSink(std::weak_ptr<AudioFrameStream> stream)
        : stream_(std::move(stream)) {
    }

    void onAudioFrame(const AudioFrame& frame) override {
        auto stream = stream_.lock();
        if (!stream) {
            return;
        }
        std::shared_ptr<IAudioFrameSink> output;
        {
            std::lock_guard<std::mutex> lock(stream->mutex_);
            stream->stats_.media.samplesProduced += 1;
            output = stream->outputSink_.lock();
        }
        if (output) {
            output->onAudioFrame(frame);
        }
    }

private:
    std::weak_ptr<AudioFrameStream> stream_;
};

AudioEncodedStream::AudioEncodedStream() {
    config_.kind = MediaKind::Audio;
    config_.direction = MediaDirection::SendRecv;
    info_.state = StreamState::Idle;
}

AudioEncodedStream::~AudioEncodedStream() {
    shutdown();
}

Status AudioEncodedStream::configure(const StreamConfig& value) {
    if (value.kind != MediaKind::Unknown &&
        value.kind != MediaKind::Audio) {
        return {false, 1, "audio stream requires audio media kind"};
    }
    std::lock_guard<std::mutex> lock(mutex_);
    config_ = value;
    config_.kind = MediaKind::Audio;
    info_.state = StreamState::Idle;
    return {};
}

StreamConfig AudioEncodedStream::config() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return config_;
}

Status AudioEncodedStream::start() {
    std::shared_ptr<IEncodedPacketTransport> transport;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!transport_) {
            info_.state = StreamState::Failed;
            return {false, 1, "audio stream has no attached transport"};
        }
        transport = transport_;
    }
    const auto transportInfo = transport->info();
    std::lock_guard<std::mutex> lock(mutex_);
    info_.transport = transportInfo;
    info_.state = StreamState::Active;
    return {};
}

void AudioEncodedStream::shutdown() noexcept {
    detach();
    std::lock_guard<std::mutex> lock(mutex_);
    info_.state = StreamState::Stopped;
}

Status AudioEncodedStream::attach(
    std::shared_ptr<IEncodedPacketTransport> transport) {
    if (!transport) {
        return {false, 1, "audio stream transport is null"};
    }
    const auto self = weak_from_this();
    if (self.expired()) {
        return {false, 1, "audio stream is not owned by a shared pointer"};
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (transport_ && transport_ != transport) {
            return {false, 1, "audio stream already has a transport"};
        }
        transport_ = transport;
        info_.transport = transport->info();
    }
    transport->setSink(std::weak_ptr<IEncodedPacketSink>{self});
    return {};
}

void AudioEncodedStream::detach() noexcept {
    std::shared_ptr<IEncodedPacketTransport> transport;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        transport = std::move(transport_);
        info_.transport = {};
    }
    if (transport) {
        transport->setSink(std::weak_ptr<IEncodedPacketSink>{});
    }
}

Status AudioEncodedStream::sendEncodedPacket(const EncodedPacket& packet) {
    std::shared_ptr<IEncodedPacketTransport> transport;
    StreamId streamId = 0;
    CodecType fallbackCodec = CodecType::Unknown;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (info_.state != StreamState::Active) {
            return {false, 1, "audio stream is not active"};
        }
        transport = transport_;
        streamId = config_.id;
        fallbackCodec = codecFromAttributes(config_.attributes);
    }
    if (!transport) {
        return {false, 1, "audio stream has no transport"};
    }
    auto value = packet;
    value.streamId = streamId;
    value.kind = MediaKind::Audio;
    if (value.codecType == CodecType::Unknown) {
        value.codecType = fallbackCodec;
    }
    const auto result = transport->send(value);
    if (!result.ok) {
        std::lock_guard<std::mutex> lock(mutex_);
        stats_.media.codecErrors += 1;
    }
    return result;
}

void AudioEncodedStream::onEncodedPacket(const EncodedPacket&) {
}

StreamInfo AudioEncodedStream::info() const {
    std::shared_ptr<IEncodedPacketTransport> transport;
    StreamInfo result;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        result = info_;
        transport = transport_;
    }
    if (transport) {
        result.transport = transport->info();
    }
    return result;
}

StreamStats AudioEncodedStream::stats() const {
    std::shared_ptr<IEncodedPacketTransport> transport;
    StreamStats result;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        result = stats_;
        transport = transport_;
    }
    if (transport) {
        result.transport = transport->stats();
    }
    return result;
}

AudioFrameStream::AudioFrameStream() = default;

AudioFrameStream::~AudioFrameStream() {
    shutdown();
    std::lock_guard<std::mutex> lock(mutex_);
    encoder_.reset();
    decoder_.reset();
    decodeSink_.reset();
    encodeSink_.reset();
}

Status AudioFrameStream::configureAudio(
    const AudioFrameStreamConfig& configValue) {
    auto result = AudioEncodedStream::configure(configValue.stream);
    if (!result.ok) {
        return result;
    }
    CodecType encoderType = CodecType::Unknown;
    CodecType decoderType = CodecType::Unknown;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        inputFormat_ = configValue.inputFormat;
        outputFormat_ = configValue.outputFormat;
        encoderType_ = configValue.encoderType;
        decoderType_ = configValue.decoderType;
        bitrateKbps_ = configValue.bitrateKbps;
        parameters_ = configValue.codecParameters;
        encoderType = encoderType_;
        decoderType = decoderType_;
    }
    FFmpegCodecFactory codecFactory;
    if (encoderType != CodecType::Unknown) {
        auto encoder = codecFactory.createAudioEncoder(encoderType);
        if (!encoder) {
            return {false, 1, "requested audio encoder is unavailable"};
        }
        result = setEncoder(std::move(encoder));
        if (!result.ok) {
            return result;
        }
    }
    if (decoderType != CodecType::Unknown) {
        auto decoder = codecFactory.createAudioDecoder(decoderType);
        if (!decoder) {
            return {false, 1, "requested audio decoder is unavailable"};
        }
        result = setDecoder(std::move(decoder));
        if (!result.ok) {
            return result;
        }
    }
    return {};
}

Status AudioFrameStream::setEncoder(
    std::unique_ptr<IAudioEncoder> encoder) {
    if (!encoder) {
        return {false, 1, "invalid audio encoder"};
    }
    auto owner = shared_from_this();
    if (!owner) {
        return {false, 1, "audio stream is not owned by a shared pointer"};
    }
    const std::weak_ptr<AudioFrameStream> self =
        std::static_pointer_cast<AudioFrameStream>(std::move(owner));
    auto sharedEncoder =
        std::shared_ptr<IAudioEncoder>(std::move(encoder));
    std::shared_ptr<EncodeSink> encodeSink;
    AudioEncoderConfig config;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!encodeSink_) {
            encodeSink_ = std::make_shared<EncodeSink>(self);
        }
        encodeSink = encodeSink_;
        config.codecType = encoderType_;
        config.inputFormat = inputFormat_;
        config.bitrateKbps = bitrateKbps_;
        config.parameters = parameters_;
    }
    sharedEncoder->setSink(encodeSink);
    const auto result = sharedEncoder->configure(config);
    if (!result.ok) {
        return result;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    encoder_ = std::move(sharedEncoder);
    return {};
}

Status AudioFrameStream::setDecoder(
    std::unique_ptr<IAudioDecoder> decoder) {
    if (!decoder) {
        return {false, 1, "invalid audio decoder"};
    }
    auto owner = shared_from_this();
    if (!owner) {
        return {false, 1, "audio stream is not owned by a shared pointer"};
    }
    const std::weak_ptr<AudioFrameStream> self =
        std::static_pointer_cast<AudioFrameStream>(std::move(owner));
    auto sharedDecoder =
        std::shared_ptr<IAudioDecoder>(std::move(decoder));
    std::shared_ptr<DecodeSink> decodeSink;
    AudioDecoderConfig config;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!decodeSink_) {
            decodeSink_ = std::make_shared<DecodeSink>(self);
        }
        decodeSink = decodeSink_;
        config.codecType = decoderType_;
        config.outputFormat = outputFormat_;
        config.parameters = parameters_;
    }
    sharedDecoder->setSink(decodeSink);
    const auto result = sharedDecoder->configure(config);
    if (!result.ok) {
        return result;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    decoder_ = std::move(sharedDecoder);
    return {};
}

Status AudioFrameStream::setMuted(bool muted) {
    std::lock_guard<std::mutex> lock(mutex_);
    muted_ = muted;
    return {};
}

void AudioFrameStream::setOutputSink(
    std::weak_ptr<IAudioFrameSink> sink) {
    std::lock_guard<std::mutex> lock(mutex_);
    outputSink_ = std::move(sink);
}

void AudioFrameStream::onEncodedPacket(const EncodedPacket& packet) {
    std::shared_ptr<IAudioDecoder> decoder;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        decoder = decoder_;
    }
    if (decoder) {
        (void)decoder->decode(packet);
    }
}

void AudioFrameStream::onAudioFrame(const AudioFrame& frame) {
    std::shared_ptr<IAudioEncoder> encoder;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (muted_) {
            return;
        }
        if (!encoder_) {
            stats_.media.codecErrors += 1;
            return;
        }
        stats_.media.samplesConsumed += 1;
        encoder = encoder_;
    }
    const auto result = encoder->encode(frame);
    if (!result.ok) {
        std::lock_guard<std::mutex> lock(mutex_);
        stats_.media.codecErrors += 1;
    }
}

} // namespace iris::media::v2
