#include "engine/simple_task_queue.h"

#include <algorithm>
#include <exception>
#include <utility>

namespace iris::media {

SimpleTaskQueue::SimpleTaskQueue()
    : worker_(&SimpleTaskQueue::Run, this) {
}

SimpleTaskQueue::~SimpleTaskQueue() {
    Clear();
}

bool SimpleTaskQueue::Post(Task task) {
    return Post(std::move(task), std::chrono::milliseconds::zero());
}

bool SimpleTaskQueue::Post(Task task,
                           std::chrono::milliseconds delay) {
    if (!task) {
        return false;
    }

    delay = std::max(delay, std::chrono::milliseconds::zero());
    const auto due_time = std::chrono::steady_clock::now() + delay;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) {
            return false;
        }
        tasks_.push_back(ScheduledTask{
            due_time,
            next_sequence_++,
            std::move(task),
        });
        std::push_heap(tasks_.begin(), tasks_.end(), LaterFirst{});
    }
    wakeup_.notify_one();
    return true;
}

void SimpleTaskQueue::Clear() noexcept {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    wakeup_.notify_one();

    std::lock_guard<std::mutex> join_lock(join_mutex_);
    if (worker_.joinable() &&
        worker_.get_id() != std::this_thread::get_id()) {
        worker_.join();
    }
}

void SimpleTaskQueue::Run() noexcept {
    for (;;) {
        Task task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            for (;;) {
                if (tasks_.empty()) {
                    if (stopping_) {
                        return;
                    }
                    wakeup_.wait(lock);
                    continue;
                }

                const auto due_time = tasks_.front().due_time;
                if (stopping_ ||
                    due_time <= std::chrono::steady_clock::now()) {
                    break;
                }
                wakeup_.wait_until(lock, due_time);
            }

            task = std::move(tasks_.front().task);
            std::pop_heap(tasks_.begin(), tasks_.end(), LaterFirst{});
            tasks_.pop_back();
        }

        try {
            task();
        } catch (...) {
            // A failed task must not terminate the queue worker. There is no
            // error channel on the task_queue abstraction yet.
        }
    }
}

std::unique_ptr<TaskQueue> SimpleTaskQueueFactory::Create() const {
    return std::make_unique<SimpleTaskQueue>();
}

} // namespace iris::media
