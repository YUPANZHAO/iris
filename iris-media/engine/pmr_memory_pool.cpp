#include "engine/pmr_memory_pool.h"

#include <boost/container/pmr/synchronized_pool_resource.hpp>

#include <cstddef>

namespace iris::media {

class PmrMemoryPoolImpl final {
public:
    boost::container::pmr::synchronized_pool_resource resource;
};

PmrMemoryPool::PmrMemoryPool()
    : impl_(std::make_unique<PmrMemoryPoolImpl>()) {
}

PmrMemoryPool& PmrMemoryPool::Instance() {
    static PmrMemoryPool instance;
    return instance;
}

PmrMemoryPool::~PmrMemoryPool() = default;

void* PmrMemoryPool::Allocate(std::size_t capacity) {
    return capacity == 0
        ? nullptr
        : impl_->resource.allocate(capacity, alignof(std::max_align_t));
}

void PmrMemoryPool::Deallocate(void* data,
                               std::size_t capacity) noexcept {
    if (data != nullptr && capacity != 0) {
        impl_->resource.deallocate(
            data, capacity, alignof(std::max_align_t));
    }
}

} // namespace iris::media
