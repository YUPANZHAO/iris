#include "engine/memory_pool.h"

#include "engine/pmr_memory_pool.h"

#include <cassert>
#include <cstring>
#include <utility>

namespace iris::media {

ByteBuffer::ByteBuffer()
    : ByteBuffer(nullptr, 0, 0, PmrMemoryPool::Instance()) {
}

ByteBuffer::ByteBuffer(std::size_t capacity)
    : ByteBuffer(
        nullptr, capacity, capacity, PmrMemoryPool::Instance()) {
}

ByteBuffer::ByteBuffer(std::size_t capacity, MemoryPool& pool)
    : ByteBuffer(nullptr, capacity, capacity, pool) {
}

ByteBuffer::ByteBuffer(const void* data, std::size_t size)
    : ByteBuffer(data, size, PmrMemoryPool::Instance()) {
}

ByteBuffer::ByteBuffer(const void* data,
                       std::size_t size,
                       MemoryPool& pool)
    : ByteBuffer(data, size, size, pool) {
}

ByteBuffer::ByteBuffer(const void* data,
                       std::size_t size,
                       std::size_t capacity,
                       MemoryPool& pool)
    : pool_(&pool), size_(size) {
    assert(size <= capacity);
    if (capacity == 0) {
        return;
    }

    allocation_ = pool.Allocate(capacity);
    if (allocation_ == nullptr) {
        pool_ = nullptr;
        return;
    }

    capacity_ = capacity;
    if (data != nullptr && size <= capacity) {
        std::memcpy(allocation_, data, size);
    }
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
