#pragma once

#include "task_queue.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace iris::media {

// A task queue with no configured capacity. Tasks are executed by exactly one
// worker thread in readiness order. Equal-deadline tasks retain FIFO order.
class SimpleTaskQueue final : public TaskQueue {
public:
    SimpleTaskQueue();
    ~SimpleTaskQueue() override;

    SimpleTaskQueue(const SimpleTaskQueue&) = delete;
    SimpleTaskQueue& operator=(const SimpleTaskQueue&) = delete;
    SimpleTaskQueue(SimpleTaskQueue&&) = delete;
    SimpleTaskQueue& operator=(SimpleTaskQueue&&) = delete;

    bool Post(Task task) override;
    bool Post(Task task, std::chrono::milliseconds delay) override;

    // Rejects new work, drains accepted work immediately, and joins the worker.
    // It is safe to call more than once.
    void Clear() noexcept;

private:
    struct ScheduledTask final {
        std::chrono::steady_clock::time_point due_time;
        std::uint64_t sequence = 0;
        Task task;
    };

    struct LaterFirst final {
        bool operator()(const ScheduledTask& left,
                        const ScheduledTask& right) const noexcept {
            if (left.due_time != right.due_time) {
                return left.due_time > right.due_time;
            }
            return left.sequence > right.sequence;
        }
    };

    void Run() noexcept;

    std::mutex mutex_;
    std::condition_variable wakeup_;
    std::vector<ScheduledTask> tasks_;
    std::uint64_t next_sequence_ = 0;
    bool stopping_ = false;

    std::mutex join_mutex_;
    std::thread worker_;
};

class SimpleTaskQueueFactory final : public TaskQueueFactory {
public:
    std::unique_ptr<TaskQueue> Create() const override;
};

} // namespace iris::media
