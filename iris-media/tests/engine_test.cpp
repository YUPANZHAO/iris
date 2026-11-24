#include "engine/engine.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;

class TrackingMemoryPool final : public iris::media::MemoryPool {
public:
    void* Allocate(std::size_t capacity) override {
        if (capacity == 0) {
            return nullptr;
        }

        std::uint8_t* data = nullptr;
        {
            std::lock_guard<std::mutex> lock(tracker_->mutex);
            auto entry = tracker_->free_blocks.find(capacity);
            if (entry != tracker_->free_blocks.end() &&
                !entry->second.empty()) {
                data = entry->second.back();
                entry->second.pop_back();
            }
        }
        if (data == nullptr) {
            data = static_cast<std::uint8_t*>(std::malloc(capacity));
        }
        return data;
    }

    void Deallocate(void* data, std::size_t capacity) noexcept override {
        if (data == nullptr) {
            return;
        }

        try {
            std::lock_guard<std::mutex> lock(tracker_->mutex);
            ++tracker_->released_count;
            tracker_->last_released = data;
            tracker_->free_blocks[capacity].push_back(
                static_cast<std::uint8_t*>(data));
        } catch (...) {
            std::free(data);
        }
    }

    int ReleasedCount() const {
        std::lock_guard<std::mutex> lock(tracker_->mutex);
        return tracker_->released_count;
    }

    void* LastReleased() const {
        std::lock_guard<std::mutex> lock(tracker_->mutex);
        return tracker_->last_released;
    }

private:
    struct ReleaseTracker final {
        ~ReleaseTracker() {
            for (auto& entry : free_blocks) {
                for (auto* block : entry.second) {
                    std::free(block);
                }
            }
        }

        int released_count = 0;
        void* last_released = nullptr;
        mutable std::mutex mutex;
        std::map<std::size_t, std::vector<std::uint8_t*>> free_blocks;
    };

    std::shared_ptr<ReleaseTracker> tracker_ =
        std::make_shared<ReleaseTracker>();
};

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void test_simple_task_queue() {
    std::unique_ptr<iris::media::TaskQueueFactory> factory =
        std::make_unique<iris::media::SimpleTaskQueueFactory>();
    auto queue = factory->Create();
    require(queue != nullptr, "factory returns a task queue");

    const auto caller_thread = std::this_thread::get_id();
    std::mutex mutex;
    std::condition_variable completed;
    std::vector<int> order;
    std::vector<std::thread::id> worker_threads;

    for (int task_id = 0; task_id < 5; ++task_id) {
        require(queue->Post([&, task_id] {
            {
                std::lock_guard<std::mutex> lock(mutex);
                order.push_back(task_id);
                worker_threads.push_back(std::this_thread::get_id());
            }
            completed.notify_one();
        }), "running queue accepts a task");
    }

    {
        std::unique_lock<std::mutex> lock(mutex);
        require(completed.wait_for(lock, 5s, [&] {
            return order.size() == 5;
        }), "all tasks execute");

        for (std::size_t index = 0; index < order.size(); ++index) {
            require(order[index] == static_cast<int>(index),
                    "tasks execute in FIFO order");
            require(worker_threads[index] == worker_threads.front(),
                    "tasks execute on one worker thread");
            require(worker_threads[index] != caller_thread,
                    "tasks do not execute on the posting thread");
        }
    }

    require(!queue->Post({}), "empty tasks are rejected");

    queue.reset();
}

void test_task_queue_clears_on_request() {
    iris::media::SimpleTaskQueue queue;
    std::atomic<int> executed{0};

    for (int index = 0; index < 20; ++index) {
        require(queue.Post([&executed] { ++executed; }),
                "queue accepts drain test task");
    }

    queue.Clear();
    require(executed == 20, "Clear drains accepted tasks");
    require(!queue.Post([] {}), "Clear rejects new tasks");
    queue.Clear();
}

void test_task_queue_survives_task_exception() {
    iris::media::SimpleTaskQueue queue;
    std::atomic<bool> continued{false};

    require(queue.Post([] { throw 1; }), "queue accepts throwing task");
    require(queue.Post([&continued] { continued = true; }),
            "queue accepts task after throwing task");

    queue.Clear();
    require(continued.load(), "worker survives a task exception");
}

void test_task_queue_clears_on_destruction() {
    std::atomic<bool> executed{false};
    {
        iris::media::SimpleTaskQueue queue;
        require(queue.Post([&executed] { executed = true; }, 200ms),
                "queue accepts task for destructor cleanup");
    }

    require(executed.load(), "destructor drains accepted tasks");
}

void test_delayed_task() {
    iris::media::SimpleTaskQueue queue;
    std::mutex mutex;
    std::condition_variable completed;
    std::vector<int> order;
    std::chrono::milliseconds delayed_elapsed{0};
    const auto started = std::chrono::steady_clock::now();

    require(queue.Post([&] {
        std::lock_guard<std::mutex> lock(mutex);
        delayed_elapsed = std::chrono::duration_cast<
            std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started);
        order.push_back(1);
        completed.notify_one();
    }, 80ms), "queue accepts a delayed task");

    require(queue.Post([&] {
        std::lock_guard<std::mutex> lock(mutex);
        order.push_back(2);
        completed.notify_one();
    }), "queue accepts an immediate task behind a delayed task");

    {
        std::unique_lock<std::mutex> lock(mutex);
        require(completed.wait_for(lock, 5s, [&] {
            return order.size() == 2;
        }), "both immediate and delayed tasks execute");

        require(order[0] == 2 && order[1] == 1,
                "delayed task does not block an immediate task");
    }
    require(delayed_elapsed >= 70ms,
            "delayed task respects its requested delay");
}

void test_byte_buffer() {
    static_assert(!std::is_copy_constructible_v<iris::media::ByteBuffer>);
    static_assert(!std::is_copy_assignable_v<iris::media::ByteBuffer>);
    static_assert(std::is_move_constructible_v<iris::media::ByteBuffer>);
    static_assert(std::is_move_assignable_v<iris::media::ByteBuffer>);

    auto& pool = iris::media::PmrMemoryPool::Instance();
    require(&iris::media::PmrMemoryPool::Instance() == &pool,
            "PMR memory pool exposes one instance");

    iris::media::ByteBuffer buffer(128);

    require(static_cast<bool>(buffer), "buffer allocation succeeds");
    require(buffer.data() != nullptr, "buffer exposes writable storage");
    require(buffer.size() == 128, "buffer exposes its logical size");
    require(buffer.capacity() == 128, "buffer reports its capacity");

    auto* bytes = static_cast<std::uint8_t*>(buffer.data());
    for (std::size_t index = 0; index < buffer.size(); ++index) {
        bytes[index] = static_cast<std::uint8_t>(index & 0xff);
    }
    require(bytes[37] == 37, "buffer storage is writable");

    require(buffer.setSize(32), "size can be reduced within capacity");
    require(buffer.size() == 32, "size reports valid data length");
    require(buffer.data() == bytes, "data remains at the block start");
    require(buffer.capacity() == 128, "capacity remains unchanged");
    require(!buffer.setSize(129), "size cannot exceed capacity");
    require(buffer.size() == 32, "invalid size update is ignored");

    iris::media::ByteBuffer moved(std::move(buffer));
    require(moved.data() == bytes, "move preserves the allocation");
    require(moved.size() == 32, "move preserves the valid data size");
    require(buffer.data() == nullptr, "moved-from buffer is empty");
    require(buffer.size() == 0 && buffer.capacity() == 0,
            "move transfers size and capacity");

    iris::media::ByteBuffer empty(0);
    require(!empty && empty.data() == nullptr,
            "zero-sized allocation returns an empty buffer");
    require(empty.size() == 0 && empty.capacity() == 0,
            "empty buffer has zero dimensions");
}

void test_byte_buffer_returns_memory() {
    TrackingMemoryPool pool;
    {
        iris::media::ByteBuffer buffer(32, pool);
        require(static_cast<bool>(buffer),
                "tracked buffer allocation succeeds");

        iris::media::ByteBuffer assigned(16, pool);
        const auto first_allocation = buffer.data();
        buffer = std::move(assigned);

        require(pool.ReleasedCount() == 1,
                "move assignment releases the replaced allocation");
        require(pool.LastReleased() == first_allocation,
                "move assignment releases the correct allocation");
    }
    require(pool.ReleasedCount() == 2,
            "buffer destruction returns its allocation to the pool");
}

void test_custom_pool_recycles_blocks() {
    TrackingMemoryPool pool;
    const auto* original_block = static_cast<const std::uint8_t*>(nullptr);
    {
        iris::media::ByteBuffer buffer(48, pool);
        original_block = static_cast<const std::uint8_t*>(buffer.data());
    }

    iris::media::ByteBuffer reused(48, pool);
    require(reused.data() == original_block,
            "derived pool can recycle released blocks");
}

} // namespace

int main() {
    test_simple_task_queue();
    test_task_queue_clears_on_request();
    test_task_queue_survives_task_exception();
    test_task_queue_clears_on_destruction();
    test_delayed_task();
    test_byte_buffer();
    test_byte_buffer_returns_memory();
    test_custom_pool_recycles_blocks();
    std::cout << "iris-media engine tests passed\n";
    return EXIT_SUCCESS;
}
