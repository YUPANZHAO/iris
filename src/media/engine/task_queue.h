#pragma once

#include "../media_types.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace iris::media::v2 {

using TaskId = std::uint64_t;
using Task = std::function<void()>;

enum class TaskPriority {
    Realtime,
    High,
    Normal,
    Low,
};

struct TaskOptions {
    TaskPriority priority = TaskPriority::Normal;
    std::int64_t delayMicros = 0;
    std::string debugName;
};

struct TaskHandle {
    TaskId id = 0;
};

struct TaskQueueConfig {
    std::string name = "media";
};

struct TaskQueueStats {
    std::size_t pending = 0;
    std::size_t peakPending = 0;
    std::uint64_t posted = 0;
    std::uint64_t executed = 0;
    std::uint64_t cancelled = 0;
};

// TaskQueue is intentionally unbounded and media-type agnostic. The
// implementation applies lifecycle and ordering policy, not a fixed lane set.
class ITaskQueue {
public:
    virtual ~ITaskQueue() = default;

    virtual Status start(const TaskQueueConfig& config) = 0;
    virtual void shutdown() noexcept = 0;

    virtual Status post(Task task,
                        const TaskOptions& options,
                        TaskHandle* handle) = 0;
    virtual bool cancel(const TaskHandle& handle) = 0;
    virtual TaskQueueStats stats() const = 0;
};

} // namespace iris::media::v2
