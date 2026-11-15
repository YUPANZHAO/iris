#include "media/transport/pj_ioqueue_service.h"

#include <pjsua-lib/pjsua.h>

#include <atomic>
#include <mutex>
#include <thread>

namespace iris::media {
namespace {

std::string pj_error(pj_status_t status) {
    char message[PJ_ERR_MSG_SIZE]{};
    pj_strerror(status, message, sizeof(message));
    return message;
}

} // namespace

PjIoqueueService::PjIoqueueService() {
    const auto init_status = pj_init();
    if (init_status != PJ_SUCCESS) {
        error_ = "failed to initialize PJLIB: " + pj_error(init_status);
        return;
    }
    pj_initialized_ = true;

    if (auto* endpoint = pjsua_get_pjmedia_endpt()) {
        ioqueue_ = pjmedia_endpt_get_ioqueue(endpoint);
        pool_factory_ = pjsua_get_pool_factory();
        owns_ioqueue_ = false;
        if (!ioqueue_ || !pool_factory_) {
            error_ = "PJSUA media endpoint has no ioqueue";
        }
        return;
    }

    owns_ioqueue_ = true;
    pj_caching_pool_init(&caching_pool_, nullptr, 16 * 1024 * 1024);
    caching_pool_initialized_ = true;
    pool_factory_ = &caching_pool_.factory;
    service_pool_ = pj_pool_create(
        pool_factory_, "iris-ioqueue", 4096, 4096, nullptr);
    if (!service_pool_) {
        error_ = "failed to create PJLIB ioqueue pool";
        return;
    }

    const auto queue_status =
        pj_ioqueue_create(service_pool_, 64, &ioqueue_);
    if (queue_status != PJ_SUCCESS) {
        error_ = "failed to create PJLIB ioqueue: " +
            pj_error(queue_status);
        return;
    }

    running_.store(true, std::memory_order_release);
    try {
        worker_ = std::thread([this] { worker_main(this); });
    } catch (...) {
        running_.store(false, std::memory_order_release);
        error_ = "failed to create PJLIB ioqueue worker";
        return;
    }

    std::unique_lock<std::mutex> lock(worker_mutex_);
    worker_condition_.wait(lock, [this] { return worker_ready_; });
    if (!worker_started_) {
        error_ = "failed to register PJLIB ioqueue worker";
    }
}

PjIoqueueService::~PjIoqueueService() {
    if (owns_ioqueue_) {
        running_.store(false, std::memory_order_release);
        if (worker_.joinable()) {
            worker_.join();
        }
        if (ioqueue_) {
            pj_ioqueue_destroy(ioqueue_);
            ioqueue_ = nullptr;
        }
        if (service_pool_) {
            pj_pool_release(service_pool_);
            service_pool_ = nullptr;
        }
        if (caching_pool_initialized_) {
            pj_caching_pool_destroy(&caching_pool_);
            caching_pool_initialized_ = false;
        }
    }
    if (pj_initialized_) {
        pj_shutdown();
    }
}

int PjIoqueueService::worker_main(void* user_data) {
    auto* service = static_cast<PjIoqueueService*>(user_data);
    pj_thread_t* thread = nullptr;
    if (pj_thread_register(
            "iris-media-io", service->worker_thread_desc_, &thread) !=
        PJ_SUCCESS) {
        {
            std::lock_guard<std::mutex> lock(service->worker_mutex_);
            service->worker_ready_ = true;
            service->worker_started_ = false;
        }
        service->worker_condition_.notify_all();
        return 1;
    }

    {
        std::lock_guard<std::mutex> lock(service->worker_mutex_);
        service->worker_ready_ = true;
        service->worker_started_ = true;
    }
    service->worker_condition_.notify_all();

    pj_time_val timeout{};
    timeout.msec = 100;
    while (service->running_.load(std::memory_order_acquire)) {
        pj_ioqueue_poll(service->ioqueue_, &timeout);
    }
    pj_thread_unregister();
    return 0;
}

} // namespace iris::media
