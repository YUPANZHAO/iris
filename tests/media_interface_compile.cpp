#include "media/capture/ffmpeg_capture.h"
#include "media/codec/ffmpeg_codec_factory.h"
#include "media/codec/ffmpeg_h264_codec.h"
#include "media/codec/ffmpeg_opus_codec.h"
#include "media/control/media_control_impl.h"
#include "media/media.h"
#include "media/sdp/sdp_components.h"
#include "media/transport/rtp_transport.h"

#include <type_traits>

static_assert(std::is_abstract_v<iris::media::v2::IStream>);
static_assert(std::is_abstract_v<
              iris::media::v2::IEncodedPacketStream>);
static_assert(std::is_abstract_v<
              iris::media::v2::IEncodedPacketTransport>);
static_assert(std::is_abstract_v<iris::media::v2::ICaptureSource>);
static_assert(std::is_abstract_v<iris::media::v2::IAudioEncoder>);
static_assert(std::is_abstract_v<iris::media::v2::IVideoEncoder>);
static_assert(std::is_abstract_v<iris::media::v2::ICodecFactory>);
static_assert(std::is_abstract_v<iris::media::v2::IMediaControl>);
static_assert(!std::is_abstract_v<
              iris::media::v2::FFmpegCodecFactory>);
static_assert(!std::is_abstract_v<iris::media::v2::RTPTransport>);
static_assert(!std::is_abstract_v<
              iris::media::v2::AudioEncodedStream>);
static_assert(!std::is_abstract_v<
              iris::media::v2::VideoEncodedStream>);
static_assert(!std::is_abstract_v<iris::media::v2::MediaControl>);
static_assert(!std::is_abstract_v<
              iris::media::v2::SDPMediaControl>);
static_assert(!std::is_abstract_v<iris::media::v2::SdpBuilder>);
static_assert(!std::is_abstract_v<iris::media::v2::SdpParser>);
static_assert(!std::is_abstract_v<iris::media::v2::SdpNegotiator>);

int main() {
    return 0;
}
