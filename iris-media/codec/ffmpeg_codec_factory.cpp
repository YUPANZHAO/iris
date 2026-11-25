#include "codec/ffmpeg_codec_factory.h"

#include "codec/ffmpeg_h264_decoder.h"
#include "codec/ffmpeg_h264_encoder.h"
#include "codec/ffmpeg_opus_decoder.h"
#include "codec/ffmpeg_opus_encoder.h"

namespace iris::media::codec {

std::unique_ptr<AudioEncoder>
FFmpegCodecFactory::createAudioEncoder(CodecType type) const {
    if (type == CodecType::Opus) {
        return std::make_unique<FFmpegOpusEncoder>();
    }
    return {};
}

std::unique_ptr<VideoEncoder>
FFmpegCodecFactory::createVideoEncoder(CodecType type) const {
    switch (type) {
    case CodecType::H264:
        return std::make_unique<FFmpegH264Encoder>();
    case CodecType::Opus:
    case CodecType::Unknown:
        return {};
    }
    return {};
}

std::unique_ptr<AudioDecoder>
FFmpegCodecFactory::createAudioDecoder(CodecType type) const {
    if (type == CodecType::Opus) {
        return std::make_unique<FFmpegOpusDecoder>();
    }
    return {};
}

std::unique_ptr<VideoDecoder>
FFmpegCodecFactory::createVideoDecoder(CodecType type) const {
    switch (type) {
    case CodecType::H264:
        return std::make_unique<FFmpegH264Decoder>();
    case CodecType::Opus:
    case CodecType::Unknown:
        return {};
    }
    return {};
}

} // namespace iris::media::codec
