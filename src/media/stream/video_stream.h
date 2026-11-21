#pragma once

#include "stream.h"

#include "../codec/codec_types.h"

namespace iris::media::v2 {

// Concrete encoded-video stream.
class VideoEncodedStream : public IEncodedPacketStream {
public:
    VideoEncodedStream();
    ~VideoEncodedStream() override;

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

struct VideoFrameStreamConfig {
    StreamConfig stream;
    VideoFormat inputFormat;
    VideoPixelFormat outputFormat = VideoPixelFormat::I420;
    CodecType encoderType = CodecType::Unknown;
    CodecType decoderType = CodecType::Unknown;
    int bitrateKbps = 0;
    AttributeMap codecParameters;
};

// Raw-frame video layer assembled above VideoEncodedStream.
class VideoFrameStream final : public VideoEncodedStream,
                               public IVideoFrameSink {
public:
    VideoFrameStream();
    ~VideoFrameStream() override;

    Status configureVideo(const VideoFrameStreamConfig& config);
    Status setEncoder(std::unique_ptr<IVideoEncoder> encoder);
    Status setDecoder(std::unique_ptr<IVideoDecoder> decoder);
    Status setMuted(bool muted);

    void onVideoFrame(const VideoFrame& frame) override;
};

} // namespace iris::media::v2
