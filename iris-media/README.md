# iris-media

`iris-media` contains reusable execution, memory, codec, transport, and capture
primitives for the media pipeline.

The `engine` library currently provides:

- `TaskQueue`, the abstract task posting interface.
- `SimpleTaskQueue`, an unbounded task queue backed by one worker thread.
- `TaskQueueFactory`, the abstract queue factory interface.
- `SimpleTaskQueueFactory`, the factory for `SimpleTaskQueue`.
- `MemoryPool`, the abstract allocation interface.
- `PmrMemoryPool`, an allocator backed by
  `boost::container::pmr::synchronized_pool_resource`.
- `ByteBuffer`, a move-only owning buffer with `data()`, `size()`,
  `setSize()`, and `capacity()` accessors.

Build and test the subproject directly:

```text
cmake -S iris-media -B build/iris-media
cmake --build build/iris-media
ctest --test-dir build/iris-media --output-on-failure
```

Applications can replace the queue implementation by selecting a different
`TaskQueueFactory` implementation. `MemoryPool` has the same property through
its virtual allocation and deallocation methods.

`ByteBuffer(capacity, pool)` acquires its allocation from the supplied pool and
returns the original block when destroyed or move-assigned. The pool must
outlive all buffers created from it. `PmrMemoryPool` uses a synchronized Boost
PMR pool, so released blocks are cached and reused by the resource.

`ByteBuffer(data, size)` copies `size` bytes into a default-pool allocation.
The overloads `ByteBuffer(data, size, pool)` and
`ByteBuffer(data, size, capacity, pool)` select a custom pool and optionally
reserve additional capacity.

All constructors delegate to `(data, size, capacity, pool)`. The constructor
asserts that `size <= capacity`, records the logical size, skips pool allocation
when `capacity == 0`, and copies bytes only when `data` is non-null.

`PmrMemoryPool::Instance()` is a process-wide singleton. `ByteBuffer(capacity)`
and the empty `ByteBuffer()` constructor use that singleton by default; use the
`(capacity, pool)` overload to bind a different `MemoryPool`.

`data()` returns the start of the allocated block. `size()` reports the current
valid byte count, which can be updated with `setSize()` up to `capacity()`.

`TaskQueue::Post` supports immediate tasks and tasks delayed by a number of
milliseconds. `SimpleTaskQueue` cleans up in its destructor, so callers do not
need to expose or invoke queue shutdown through the abstract interface.

The optional `iris-media-codec` target provides a codec-neutral API and FFmpeg
implementations:

- `AudioEncoder`/`VideoEncoder` and `AudioDecoder`/`VideoDecoder` are the
  typed abstract codec interfaces in `codec/codec_types.h`.
- `AudioRawFrame` and `VideoRawFrame` are separate concrete frame types.
- `AudioEncoderConfig`, `VideoEncoderConfig`, `AudioDecoderConfig`, and
  `VideoDecoderConfig` are separate configuration types.
- Packet and frame payloads use `MediaBuffer`, an alias for
  `std::shared_ptr<ByteBuffer>`, so storage is provided by the engine memory
  pool and can outlive a synchronous callback safely.
- `AudioEncoder::encode()` and `VideoEncoder::encode()` consume their matching
  concrete raw frame; encoded output is delivered through
  `setEncodedPacketSink()`.
- `AudioDecoder::decode()` and `VideoDecoder::decode()` consume an
  `EncodedPacket`; decoded output is delivered through `setRawFrameSink()` to
  the matching typed frame sink.
- `FFmpegH264Encoder`, `FFmpegH264Decoder`, `FFmpegOpusEncoder`, and
  `FFmpegOpusDecoder` provide the current implementations.

Set both `IRIS_MEDIA_BUILD_CODECS=OFF` and
`IRIS_MEDIA_BUILD_CAPTURE_SOURCES=OFF` for an engine-only build. Codec consumers
can include `codec/codec.h` and link `iris::media::codec`.

The optional capture-source module is compiled directly into the `iris-media`
target when `IRIS_MEDIA_BUILD_CAPTURE_SOURCES` is enabled:

- `CaptureSource` owns the common `start()`, `stop()`, `running()`, and error
  contract.
- `AudioCaptureSource` and `VideoCaptureSource` add typed configuration, output
  formats, and frame sinks.
- `StreamId`, `AttributeMap`, `MediaKind`, `Status`, and `MediaBuffer` are
  shared through `media_types.h`.
- `FFmpegAudioCaptureSource` and `FFmpegVideoCaptureSource` open FFmpeg input
  devices in a background thread. `backend` selects a backend such as
  `dshow`, `v4l2`, `alsa`, or `lavfi`, while `options` forwards device options.
- Audio capture converts with libswresample to the requested rate, channel
  count, and signed-16 or float sample format.
- Video capture converts with libswscale to the requested dimensions, frame
  rate, and I420, NV12, or BGRA pixel format.
- Frame callbacks are asynchronous. Keeping the source object alive is the
  caller's responsibility through `configure()`, `start()`, and `stop()`.

Include the typed header (or the `capture_source/capture.h` umbrella) and link
`iris::media`:

```cpp
#include "capture_source/ffmpeg_video_capture_source.h"

auto source =
    std::make_shared<iris::media::capture::FFmpegVideoCaptureSource>();
source->setFrameSink(frameSink);

iris::media::capture::VideoCaptureConfig config;
config.backend = "dshow";
config.name = "video=Integrated Camera";
source->configure(config);
source->start();
// ...
source->stop();
```

Set `IRIS_MEDIA_BUILD_CAPTURE_SOURCES=OFF` to omit this target.

The optional `iris-media-transport` target provides the RTP payload
transport boundary and a small RTP implementation:

- `Transport` is the message-oriented base interface with
  `send(MediaBuffer)`, `setSink()`, `configure()`, `attach()`, and `detach()`.
- `configure()` binds local settings such as `localAddress`/`bindAddress` and
  `localPort`/`port`. `attach()` sets `remoteAddress`/`remotePort` or
  `remoteEndpoint` and starts receiving. `detach()` closes the socket.
- `ITransportSink::onTransportState()` reports configuring, configured,
  attaching, connected, detaching, disconnected, and failed states.
- `UdpTransport` implements the interface on top of a cross-platform UDP
  socket with an asynchronous receive thread.
- `RTPTransport` inherits `Transport` and sends
  `send(RtpParams, EncodedPacket)`.
- Stream ID to payload type and clock rate bindings are supplied through
  `configure()` or `attach()` using `stream.<id>.payloadType` and
  `stream.<id>.clockRate`.
- `RtpParams` carries the RTP marker. RTPEngine manages SSRC, sequence numbers,
  timestamps, and fragmentation.
- `RTPTransportSink::onRtpRecv()` receives `RtpParams` and `EncodedPacket`.
  `onRtcpRecv()` receives `RtcpParams` values split from compound RTCP.
- `RTPEngine` derives from `RTPTransport`, creates a `UdpTransport` internally,
  and manages per-stream RTP sequence numbers, timestamps, SSRC values,
  timestamp origins, and payload-type routing.
- Inbound media PTS values are zero-based on the first RTP timestamp observed
  for each stream.

Encoded packets larger than the configured `mtu` are split into consecutive
RTP datagrams with the same timestamp. The MTU is the maximum RTP datagram
size including the 12-byte RTP header. Include `transport/rtp.h` and link
`iris::media::transport`.
