#pragma once

#include "pipeline/stream.h"

#include "capture_source/capture_source.h"

#include <memory>
#include <type_traits>
#include <utility>

namespace iris::media::pipeline {

// Video capture-to-encoder pipeline. The capture source and encoder are
// configured by the caller; open() connects and starts them as one stream.
class ffmpeg_video_stream final
    : public Stream,
      public capture::VideoFrameSink,
      public std::enable_shared_from_this<ffmpeg_video_stream> {
public:
    ffmpeg_video_stream() = default;
    ~ffmpeg_video_stream() override;

    ffmpeg_video_stream(const ffmpeg_video_stream&) = delete;
    ffmpeg_video_stream& operator=(const ffmpeg_video_stream&) = delete;

    Status open() override;
    void close() noexcept override;

    Status setCapture(
        std::shared_ptr<capture::VideoCaptureSource> capture);
    template <
        typename Capture,
        std::enable_if_t<
            std::is_base_of_v<capture::VideoCaptureSource, Capture>,
            int> = 0>
    Status setCapture(std::unique_ptr<Capture> capture) {
        return setCapture(
            std::shared_ptr<capture::VideoCaptureSource>(std::move(capture)));
    }

    Status setEncoder(
        std::shared_ptr<codec::VideoEncoder> encoder);
    template <
        typename Encoder,
        std::enable_if_t<
            std::is_base_of_v<codec::VideoEncoder, Encoder>,
            int> = 0>
    Status setEncoder(std::unique_ptr<Encoder> encoder) {
        return setEncoder(
            std::shared_ptr<codec::VideoEncoder>(std::move(encoder)));
    }

    void onVideoFrame(const codec::VideoRawFrame& frame) override;

private:
    std::shared_ptr<capture::VideoCaptureSource> capture_;
    std::shared_ptr<codec::VideoEncoder> encoder_;
};

} // namespace iris::media::pipeline
