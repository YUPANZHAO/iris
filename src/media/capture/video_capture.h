#pragma once

#include "capture.h"

#include "../codec/codec_types.h"

namespace iris::media::v2 {

struct VideoCaptureConfig {
    VideoFormat format;
};

class IVideoCaptureSource : public ICaptureSource {
public:
    ~IVideoCaptureSource() override = default;

    virtual Status configure(const VideoCaptureConfig& config) = 0;
    virtual void setVideoSink(
        std::weak_ptr<IVideoFrameSink> sink) = 0;
};

} // namespace iris::media::v2
