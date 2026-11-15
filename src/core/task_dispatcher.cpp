#include "core/task_dispatcher.h"

#include <memory>

namespace iris::core {

TaskDispatcher::~TaskDispatcher() {
    stop();
}

void TaskDispatcher::start() {
    worker_ = std::thread([this] { run(); });
}

bool TaskDispatcher::post(std::function<void()> task) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!accepting_) {
            return false;
        }
        tasks_.push_back(std::move(task));
    }
    condition_.notify_one();
    return true;
}

void TaskDispatcher::close_queue() {
    std::lock_guard<std::mutex> lock(mutex_);
    accepting_ = false;
    tasks_.clear();
}

void TaskDispatcher::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        accepting_ = false;
        stopping_ = true;
        tasks_.clear();
    }
    condition_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
}

void TaskDispatcher::run() {
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            condition_.wait(lock, [this] {
                return stopping_ || !tasks_.empty();
            });
            if (tasks_.empty()) {
                if (stopping_) {
                    return;
                }
                continue;
            }
            task = std::move(tasks_.front());
            tasks_.pop_front();
        }
        task();
    }
}

} // namespace iris::core

