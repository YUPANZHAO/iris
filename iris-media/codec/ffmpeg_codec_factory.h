#pragma once

#include "codec/codec_types.h"

#include <memory>

namespace iris::media::codec {

class FFmpegCodecFactory final {
public:
    std::unique_ptr<AudioEncoder> createAudioEncoder(CodecType type) const;
    std::unique_ptr<VideoEncoder> createVideoEncoder(CodecType type) const;
    std::unique_ptr<AudioDecoder> createAudioDecoder(CodecType type) const;
    std::unique_ptr<VideoDecoder> createVideoDecoder(CodecType type) const;
};

} // namespace iris::media::codec
