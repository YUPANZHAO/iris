#pragma once

#include "media/codec/codec_types.h"

#include <memory>

namespace iris::media::v2 {

class FFmpegCodecFactory final : public ICodecFactory {
public:
    std::unique_ptr<IAudioEncoder> createAudioEncoder(
        CodecType type) override;
    std::unique_ptr<IAudioDecoder> createAudioDecoder(
        CodecType type) override;
    std::unique_ptr<IVideoEncoder> createVideoEncoder(
        CodecType type) override;
    std::unique_ptr<IVideoDecoder> createVideoDecoder(
        CodecType type) override;
};

} // namespace iris::media::v2
