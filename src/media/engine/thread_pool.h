#pragma once

#include "task_queue.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace iris::media::v2 {

struct ThreadPoolConfig {
    std::size_t workerCount = 1;
    std::string threadNamePrefix = "media";
};

struct ThreadPoolStats {
    std::size_t activeWorkers = 0;
    std::size_t pendingTasks = 0;
    std::uint64_t completedTasks = 0;
};

class IThreadPool {
public:
    virtual ~IThreadPool() = default;

    virtual Status start(const ThreadPoolConfig& config) = 0;
    virtual void shutdown() noexcept = 0;

    virtual Status submit(Task task,
                          TaskPriority priority,
                          std::string debugName) = 0;
    virtual ThreadPoolStats stats() const = 0;
};

} // namespace iris::media::v2
