# Media implementation

`src/media` is now the only media implementation used by the project. The
`media_new/` directory is kept as a reference and is not included by the build.

## Components

| Component | Files |
| --- | --- |
| shared types | `media_types.h`, `media.h` |
| engine | optional service interfaces; `engine/media_buffer.{h,cpp}` |
| codec | `codec/codec_types.h`, `codec/ffmpeg_codec_factory.{h,cpp}`, `codec/ffmpeg_{opus,h264}_codec.{h,cpp}` |
| capture | `capture/capture.h`, `capture/ffmpeg_capture.{h,cpp}`, `capture/device_query.cpp` |
| transport | `transport/transport.h`, `transport/rtp_transport.h`, `transport/rtp_transport.cpp` |
| stream | `stream/stream*.h`, `stream/{audio,video}_stream.*` |
| SDP | `sdp/sdp_*.h`, `sdp/sdp_components.{h,cpp}` |
| control | interfaces `control/media_control.h`; implementations `media_control_impl.h`, `sdp_media_control.h`, and matching `.cpp` files |

## Runtime flow

```text
core::MediaSessionManager
  -> SDPMediaControl
  -> VideoFrameStream / AudioFrameStream
  -> RTPTransport (EncodedPacket)
  -> UDP socket

FFmpegCaptureProvider
  -> capture source
  -> FrameStream
```

Capture and transport use ordinary `std::thread` workers. Task queue, thread
pool, and memory pool interfaces are present in the copied architecture but are
not used by the current implementation.

## RTP transport boundary

`RTPTransport` accepts and emits `EncodedPacket` values. It adds or removes the
generic RTP header and manages sequence numbers, SSRCs, timestamps and RTCP
classification internally. Payload bytes are opaque: the transport does not
branch on `MediaKind`, `CodecType` or codec-specific payload structure. Stream
configuration only supplies the RTP payload type and clock rate.

Transport configuration is split into two `AttributeMap` stages:

- `configure()` binds stable local listener settings such as `bindAddress` and
  `port`.
- `open()` applies runtime settings such as `remoteEndpoint` and
  `stream.<id>.payloadType` / `stream.<id>.clockRate`.

After configuration, consumers use only the encoded send and sink interface.
Each stream owns one transport directly, and the stream itself is registered as
that transport's packet sink. There is no shared stream mux or global stream
registry.
