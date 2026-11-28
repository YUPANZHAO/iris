#include "capture_source/capture.h"
#include "codec/codec.h"
#include "control/control.h"
#include "pipeline/pipeline.h"
#include "transport/rtp.h"

#include <type_traits>

static_assert(std::is_abstract_v<iris::media::pipeline::Stream>);
static_assert(std::is_abstract_v<iris::media::capture::CaptureSource>);
static_assert(std::is_abstract_v<iris::media::capture::AudioCaptureSource>);
static_assert(std::is_abstract_v<iris::media::capture::VideoCaptureSource>);
static_assert(std::is_abstract_v<iris::media::codec::AudioEncoder>);
static_assert(std::is_abstract_v<iris::media::codec::VideoEncoder>);
static_assert(std::is_abstract_v<iris::media::codec::AudioDecoder>);
static_assert(std::is_abstract_v<iris::media::codec::VideoDecoder>);
static_assert(std::is_abstract_v<iris::media::transport::Transport>);
static_assert(std::is_abstract_v<iris::media::transport::RTPTransport>);
static_assert(!std::is_abstract_v<
              iris::media::pipeline::CaptureAudioStream>);
static_assert(!std::is_abstract_v<
              iris::media::pipeline::CaptureVideoStream>);
static_assert(!std::is_abstract_v<
              iris::media::pipeline::RtpMediaStream>);
static_assert(!std::is_abstract_v<
              iris::media::capture::EmptyAudioCaptureSource>);
static_assert(!std::is_abstract_v<
              iris::media::capture::EmptyVideoCaptureSource>);
static_assert(!std::is_abstract_v<
              iris::media::capture::FFmpegAudioCaptureSource>);
static_assert(!std::is_abstract_v<
              iris::media::capture::FFmpegVideoCaptureSource>);
static_assert(!std::is_abstract_v<
              iris::media::transport::RTPEngine>);
static_assert(!std::is_abstract_v<iris::media::control::MediaControl>);
static_assert(!std::is_abstract_v<
              iris::media::control::SdpMediaControl>);

int main() {
    return 0;
}
