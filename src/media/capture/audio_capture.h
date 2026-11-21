#pragma once

#include "capture.h"

#include "../codec/codec_types.h"

namespace iris::media::v2 {

struct AudioCaptureConfig {
    CaptureConfig capture;
    AudioFormat format;
};

// Audio output is derived from the generic capture lifecycle.
class IAudioCaptureSource : public ICaptureSource {
public:
    ~IAudioCaptureSource() override = default;

    virtual Status configure(const AudioCaptureConfig& config) = 0;
    virtual void setAudioSink(
        std::weak_ptr<IAudioFrameSink> sink) = 0;
};

} // namespace iris::media::v2
