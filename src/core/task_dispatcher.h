#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>

namespace iris::core {

class TaskDispatcher {
public:
    TaskDispatcher() = default;
    TaskDispatcher(const TaskDispatcher&) = delete;
    TaskDispatcher& operator=(const TaskDispatcher&) = delete;
    ~TaskDispatcher();

    void start();
    bool post(std::function<void()> task);

    template <typename Function>
    auto invoke(Function&& function) -> std::future<std::invoke_result_t<Function>> {
        using Result = std::invoke_result_t<Function>;
        auto task = std::make_shared<std::packaged_task<Result()>>(
            std::forward<Function>(function));
        auto result = task->get_future();
        if (!post([task] { (*task)(); })) {
            throw std::runtime_error("iris runtime is shutting down");
        }
        return result;
    }

    void close_queue();
    void stop();
    bool is_worker_thread() const noexcept {
        return std::this_thread::get_id() == worker_.get_id();
    }

private:
    void run();

    std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<std::function<void()>> tasks_;
    std::thread worker_;
    bool accepting_ = true;
    bool stopping_ = false;
};

} // namespace iris::core

