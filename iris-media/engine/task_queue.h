#pragma once

#include <chrono>
#include <functional>
#include <memory>

namespace iris::media {

using Task = std::function<void()>;

class TaskQueue {
public:
    virtual ~TaskQueue() = default;

    // Thread-safe. Accepted tasks execute in FIFO order when ready. Returns
    // false when the queue no longer accepts tasks.
    virtual bool Post(Task task) = 0;

    // Queues the task to run no earlier than the given delay.
    virtual bool Post(Task task, std::chrono::milliseconds delay) = 0;
};

class TaskQueueFactory {
public:
    virtual ~TaskQueueFactory() = default;

    virtual std::unique_ptr<TaskQueue> Create() const = 0;
};

} // namespace iris::media
