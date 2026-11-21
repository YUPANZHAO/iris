#pragma once

#include "memory_pool.h"
#include "task_queue.h"
#include "thread_pool.h"

#include <cstddef>
#include <string>

namespace iris::media::v2 {

enum class RuntimeState {
    Created,
    Starting,
    Running,
    Stopping,
    Stopped,
    Failed,
};

struct MediaRuntimeConfig {
    std::string runtimeName = "media";
    std::size_t workerThreads = 1;
};

struct MediaRuntimeStats {
    RuntimeState state = RuntimeState::Created;
    ThreadPoolStats workerPool;
    TaskQueueStats taskQueue;
};

// Runtime owns execution services, not media semantics. It has no audio/video
// lanes or fixed capacities. Components schedule generic tasks through it.
class IMediaRuntime {
public:
    virtual ~IMediaRuntime() = default;

    virtual Status initialize(const MediaRuntimeConfig& config) = 0;
    virtual Status start() = 0;
    virtual void shutdown() noexcept = 0;

    virtual ITaskQueue& taskQueue() = 0;
    virtual IThreadPool& threadPool() = 0;
    virtual IMemoryPool& memoryPool() = 0;

    virtual RuntimeState state() const noexcept = 0;
    virtual MediaRuntimeStats stats() const = 0;
};

} // namespace iris::media::v2
