#include "media/codec/ffmpeg_codec_factory.h"

#include "media/codec/ffmpeg_h264_codec.h"
#include "media/codec/ffmpeg_opus_codec.h"

namespace iris::media::v2 {

std::unique_ptr<IAudioEncoder>
FFmpegCodecFactory::createAudioEncoder(CodecType type) {
    if (type == CodecType::Opus) {
        return std::make_unique<FFmpegOpusAudioEncoder>();
    }
    return {};
}

std::unique_ptr<IAudioDecoder>
FFmpegCodecFactory::createAudioDecoder(CodecType type) {
    if (type == CodecType::Opus) {
        return std::make_unique<FFmpegOpusAudioDecoder>();
    }
    return {};
}

std::unique_ptr<IVideoEncoder>
FFmpegCodecFactory::createVideoEncoder(CodecType type) {
    if (type == CodecType::H264) {
        return std::make_unique<FFmpegH264VideoEncoder>();
    }
    return {};
}

std::unique_ptr<IVideoDecoder>
FFmpegCodecFactory::createVideoDecoder(CodecType type) {
    if (type == CodecType::H264) {
        return std::make_unique<FFmpegH264VideoDecoder>();
    }
    return {};
}

} // namespace iris::media::v2
