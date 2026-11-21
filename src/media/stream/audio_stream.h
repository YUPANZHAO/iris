#pragma once

#include "stream.h"

#include "../codec/codec_types.h"

namespace iris::media::v2 {

// Concrete encoded-audio stream. Its transport is always an encoded-packet
// transport; RTP framing is hidden behind IEncodedPacketTransport.
class AudioEncodedStream : public IEncodedPacketStream {
public:
    AudioEncodedStream();
    ~AudioEncodedStream() override;

    Status configure(const StreamConfig& config) override;
    const StreamConfig& config() const noexcept;
    Status start() override;
    void shutdown() noexcept override;

    Status attach(
        std::shared_ptr<IEncodedPacketTransport> transport) override;
    void detach() noexcept override;
    Status sendEncodedPacket(const EncodedPacket& packet) override;
    void onEncodedPacket(const EncodedPacket& packet) override;

    StreamInfo info() const override;
    StreamStats stats() const override;
};

struct AudioFrameStreamConfig {
    StreamConfig stream;
    AudioFormat inputFormat;
    AudioFormat outputFormat;
    CodecType encoderType = CodecType::Unknown;
    CodecType decoderType = CodecType::Unknown;
    int bitrateKbps = 0;
    AttributeMap codecParameters;
};

// Raw-frame audio layer assembled above AudioEncodedStream.
class AudioFrameStream final : public AudioEncodedStream,
                               public IAudioFrameSink {
public:
    AudioFrameStream();
    ~AudioFrameStream() override;

    Status configureAudio(const AudioFrameStreamConfig& config);
    Status setEncoder(std::unique_ptr<IAudioEncoder> encoder);
    Status setDecoder(std::unique_ptr<IAudioDecoder> decoder);
    Status setMuted(bool muted);

    void onAudioFrame(const AudioFrame& frame) override;
};

} // namespace iris::media::v2
