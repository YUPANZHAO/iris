#pragma once

#include "stream.h"

#include "../codec/codec_types.h"

#include <mutex>

namespace iris::media::v2 {

// Concrete encoded-audio stream. Its transport is always an encoded-packet
// transport; RTP framing is hidden behind IEncodedPacketTransport.
class AudioEncodedStream : public IEncodedPacketStream {
public:
    AudioEncodedStream();
    ~AudioEncodedStream() override;

    Status configure(const StreamConfig& config) override;
    StreamConfig config() const;
    Status start() override;
    void shutdown() noexcept override;

    Status attach(
        std::shared_ptr<IEncodedPacketTransport> transport) override;
    void detach() noexcept override;
    Status sendEncodedPacket(const EncodedPacket& packet) override;
    void onEncodedPacket(const EncodedPacket& packet) override;

    StreamInfo info() const override;
    StreamStats stats() const override;

protected:
    mutable std::mutex mutex_;
    StreamConfig config_;
    StreamInfo info_;
    StreamStats stats_;
    std::shared_ptr<IEncodedPacketTransport> transport_;
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
    void setOutputSink(std::weak_ptr<IAudioFrameSink> sink);

    void onEncodedPacket(const EncodedPacket& packet) override;
    void onAudioFrame(const AudioFrame& frame) override;

private:
    class DecodeSink;
    class EncodeSink;

    AudioFormat inputFormat_;
    AudioFormat outputFormat_;
    CodecType encoderType_ = CodecType::Unknown;
    CodecType decoderType_ = CodecType::Unknown;
    int bitrateKbps_ = 0;
    AttributeMap parameters_;
    std::shared_ptr<IAudioEncoder> encoder_;
    std::shared_ptr<IAudioDecoder> decoder_;
    std::shared_ptr<DecodeSink> decodeSink_;
    std::shared_ptr<EncodeSink> encodeSink_;
    std::weak_ptr<IAudioFrameSink> outputSink_;
    bool muted_ = false;
};

} // namespace iris::media::v2
