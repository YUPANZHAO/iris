# iris-media

`iris-media` contains reusable execution, memory, and codec primitives for the
media pipeline.

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

Set `IRIS_MEDIA_BUILD_CODECS=OFF` for an engine-only build. Codec consumers can
include `codec/codec.h` and link `iris::media::codec`.
