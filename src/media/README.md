# Media implementation

`src/media` is now the only media implementation used by the project. The
`media_new/` directory is kept as a reference and is not included by the build.

## Components

| Component | Files |
| --- | --- |
| shared types | `media_types.h`, `media.h` |
| engine | optional service interfaces; `engine/media_buffer.{h,cpp}` |
| codec | `codec/codec_types.h`, `codec/ffmpeg_codec_impl.{h,cpp}` |
| capture | `capture/capture.h`, `capture/ffmpeg_capture.{h,cpp}`, `capture/device_query.cpp` |
| transport | `transport/transport.h`, `transport/rtp_transport.h`, `transport/rtp_transport.cpp` |
| stream | `stream/stream*.h`, `stream/{audio,video}_stream.*`, internal `stream_internal.*` |
| SDP | `sdp/sdp_*.h`, `sdp/sdp_components.{h,cpp}` |
| control | interfaces `control/media_control.h`; implementations `media_control_impl.h`, `sdp_media_control.h`, and matching `.cpp` files |

## Runtime flow

```text
core::MediaSessionManager
  -> SDPMediaControl
  -> VideoFrameStream / AudioFrameStream
  -> RTPTransport
  -> UDP socket

FFmpegCaptureProvider
  -> capture source
  -> FrameStream
```

Capture and transport use ordinary `std::thread` workers. Task queue, thread
pool, and memory pool interfaces are present in the copied architecture but are
not used by the current implementation.
