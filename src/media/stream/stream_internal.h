#pragma once

#include "media/codec/codec_types.h"
#include "media/stream/stream.h"

#include <functional>
#include <memory>
#include <mutex>

namespace iris::media::v2 {

class AudioEncodedStream;
class VideoEncodedStream;

namespace stream_internal {

struct EncodedStreamState {
    mutable std::mutex mutex;
    StreamConfig config;
    StreamInfo info;
    StreamStats stats;
    std::shared_ptr<IEncodedPacketTransport> transport;
    std::function<Status(const EncodedPacket&)> send;
    std::function<void(const EncodedPacket&)> receive;
};

class EncodedSinkBridge final : public IEncodedPacketSink {
public:
    EncodedSinkBridge(std::weak_ptr<EncodedStreamState> state,
                      bool outgoing)
        : state_(std::move(state)),
          outgoing_(outgoing) {
    }

    void onEncodedPacket(const EncodedPacket& packet) override;

private:
    std::weak_ptr<EncodedStreamState> state_;
    bool outgoing_ = false;
};

struct AudioFrameState {
    std::mutex mutex;
    AudioFormat inputFormat;
    AudioFormat outputFormat;
    CodecType encoderType = CodecType::Unknown;
    CodecType decoderType = CodecType::Unknown;
    int bitrateKbps = 0;
    AttributeMap parameters;
    std::unique_ptr<IAudioEncoder> encoder;
    std::shared_ptr<IAudioDecoder> decoder;
    std::shared_ptr<IAudioFrameSink> decodeSink;
    std::weak_ptr<IAudioFrameSink> outputSink;
    bool muted = false;
};

struct VideoFrameState {
    std::mutex mutex;
    VideoFormat inputFormat;
    VideoPixelFormat outputFormat = VideoPixelFormat::I420;
    CodecType encoderType = CodecType::Unknown;
    CodecType decoderType = CodecType::Unknown;
    int bitrateKbps = 0;
    AttributeMap parameters;
    std::unique_ptr<IVideoEncoder> encoder;
    std::shared_ptr<IVideoDecoder> decoder;
    std::shared_ptr<IVideoFrameSink> decodeSink;
    std::weak_ptr<IVideoFrameSink> outputSink;
    bool muted = false;
};

void registerEncodedStream(
    const IEncodedPacketStream* stream,
    std::shared_ptr<EncodedStreamState> state);
void unregisterEncodedStream(const IEncodedPacketStream* stream);
std::shared_ptr<EncodedStreamState> encodedStreamState(
    const IEncodedPacketStream* stream);
std::shared_ptr<EncodedSinkBridge> encodedSinkBridge(
    const IEncodedPacketStream* stream);
std::shared_ptr<EncodedSinkBridge> encodedSendBridge(
    const IEncodedPacketStream* stream);

void registerAudioFrameState(
    const AudioEncodedStream* stream,
    std::shared_ptr<AudioFrameState> state);
void unregisterAudioFrameState(const AudioEncodedStream* stream);
std::shared_ptr<AudioFrameState> audioFrameState(
    const AudioEncodedStream* stream);
void setAudioOutputSink(
    const AudioEncodedStream* stream,
    std::weak_ptr<IAudioFrameSink> sink);

void registerVideoFrameState(
    const VideoEncodedStream* stream,
    std::shared_ptr<VideoFrameState> state);
void unregisterVideoFrameState(const VideoEncodedStream* stream);
std::shared_ptr<VideoFrameState> videoFrameState(
    const VideoEncodedStream* stream);
void setVideoOutputSink(
    const VideoEncodedStream* stream,
    std::weak_ptr<IVideoFrameSink> sink);

} // namespace stream_internal
} // namespace iris::media::v2
