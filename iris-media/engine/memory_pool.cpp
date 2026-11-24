#include "engine/memory_pool.h"

#include "engine/pmr_memory_pool.h"

#include <utility>

namespace iris::media {

ByteBuffer::ByteBuffer()
    : ByteBuffer(0, PmrMemoryPool::Instance()) {
}

ByteBuffer::ByteBuffer(std::size_t capacity)
    : ByteBuffer(capacity, PmrMemoryPool::Instance()) {
}

ByteBuffer::ByteBuffer(std::size_t capacity, MemoryPool& pool)
    : pool_(&pool) {
    if (capacity == 0) {
        return;
    }

    allocation_ = pool.Allocate(capacity);
    if (allocation_ == nullptr) {
        pool_ = nullptr;
        return;
    }

    capacity_ = capacity;
    size_ = capacity;
}

ByteBuffer::~ByteBuffer() {
    Reset();
}

ByteBuffer::ByteBuffer(ByteBuffer&& other) noexcept
    : pool_(std::exchange(other.pool_, nullptr)),
      allocation_(std::exchange(other.allocation_, nullptr)),
      size_(std::exchange(other.size_, 0)),
      capacity_(std::exchange(other.capacity_, 0)) {
}

ByteBuffer& ByteBuffer::operator=(ByteBuffer&& other) noexcept {
    if (this != &other) {
        Reset();
        pool_ = std::exchange(other.pool_, nullptr);
        allocation_ = std::exchange(other.allocation_, nullptr);
        size_ = std::exchange(other.size_, 0);
        capacity_ = std::exchange(other.capacity_, 0);
    }
    return *this;
}

void ByteBuffer::Reset() noexcept {
    if (allocation_ != nullptr && pool_ != nullptr) {
        pool_->Deallocate(allocation_, capacity_);
    }
    pool_ = nullptr;
    allocation_ = nullptr;
    size_ = 0;
    capacity_ = 0;
}

} // namespace iris::media
