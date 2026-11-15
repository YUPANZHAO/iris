#pragma once

#include <pjlib.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

namespace iris::media {

class PjIoqueueService final {
public:
    PjIoqueueService();
    ~PjIoqueueService();

    PjIoqueueService(const PjIoqueueService&) = delete;
    PjIoqueueService& operator=(const PjIoqueueService&) = delete;

    bool ok() const noexcept { return error_.empty(); }
    const std::string& error() const noexcept { return error_; }
    pj_ioqueue_t* ioqueue() const noexcept { return ioqueue_; }
    pj_pool_factory* pool_factory() const noexcept { return pool_factory_; }
    bool uses_pjsua_ioqueue() const noexcept { return !owns_ioqueue_; }

private:
    static int worker_main(void* user_data);

    std::string error_;
    pj_pool_factory* pool_factory_ = nullptr;
    pj_ioqueue_t* ioqueue_ = nullptr;
    pj_caching_pool caching_pool_{};
    pj_pool_t* service_pool_ = nullptr;
    pj_thread_desc worker_thread_desc_{};
    std::thread worker_;
    std::mutex worker_mutex_;
    std::condition_variable worker_condition_;
    std::atomic<bool> running_{false};
    bool worker_ready_ = false;
    bool worker_started_ = false;
    bool pj_initialized_ = false;
    bool caching_pool_initialized_ = false;
    bool owns_ioqueue_ = false;
};

} // namespace iris::media
