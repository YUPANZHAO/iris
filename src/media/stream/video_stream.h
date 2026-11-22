#pragma once

#include "stream.h"

#include "../codec/codec_types.h"

#include <mutex>

namespace iris::media::v2 {

// Concrete encoded-video stream.
class VideoEncodedStream : public IEncodedPacketStream {
public:
    VideoEncodedStream();
    ~VideoEncodedStream() override;

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
    void setOutputSink(std::weak_ptr<IVideoFrameSink> sink);

    void onEncodedPacket(const EncodedPacket& packet) override;
    void onVideoFrame(const VideoFrame& frame) override;

private:
    class DecodeSink;
    class EncodeSink;

    VideoFormat inputFormat_;
    VideoPixelFormat outputFormat_ = VideoPixelFormat::I420;
    CodecType encoderType_ = CodecType::Unknown;
    CodecType decoderType_ = CodecType::Unknown;
    int bitrateKbps_ = 0;
    AttributeMap parameters_;
    std::shared_ptr<IVideoEncoder> encoder_;
    std::shared_ptr<IVideoDecoder> decoder_;
    std::shared_ptr<DecodeSink> decodeSink_;
    std::shared_ptr<EncodeSink> encodeSink_;
    std::weak_ptr<IVideoFrameSink> outputSink_;
    bool muted_ = false;
};

} // namespace iris::media::v2
