# iris-media

`iris-media` contains reusable execution and memory primitives for the media
pipeline.

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

`PmrMemoryPool::Instance()` is a process-wide singleton. `ByteBuffer(capacity)`
and the empty `ByteBuffer()` constructor use that singleton by default; use the
`(capacity, pool)` overload to bind a different `MemoryPool`.

`data()` returns the start of the allocated block. `size()` reports the current
valid byte count, which can be updated with `setSize()` up to `capacity()`.

`TaskQueue::Post` supports immediate tasks and tasks delayed by a number of
milliseconds. `SimpleTaskQueue` cleans up in its destructor, so callers do not
need to expose or invoke queue shutdown through the abstract interface.
