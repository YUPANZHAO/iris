#pragma once

#include <cstddef>

namespace iris::media {

class MemoryPool;

// Move-only owning view over a memory allocation. The allocation is returned to
// its pool when the buffer is destroyed or move-assigned.
class ByteBuffer final {
public:
    ByteBuffer();
    explicit ByteBuffer(std::size_t capacity);
    ByteBuffer(std::size_t capacity, MemoryPool& pool);
    ~ByteBuffer();

    ByteBuffer(const ByteBuffer&) = delete;
    ByteBuffer& operator=(const ByteBuffer&) = delete;
    ByteBuffer(ByteBuffer&& other) noexcept;
    ByteBuffer& operator=(ByteBuffer&& other) noexcept;

    void* data() noexcept {
        return allocation_;
    }

    const void* data() const noexcept {
        return allocation_;
    }

    std::size_t size() const noexcept {
        return size_;
    }

    // Sets the number of valid bytes in the block. Returns false when size is
    // greater than capacity.
    bool setSize(std::size_t size) noexcept {
        if (size > capacity_) {
            return false;
        }
        size_ = size;
        return true;
    }

    std::size_t capacity() const noexcept {
        return capacity_;
    }

    explicit operator bool() const noexcept {
        return allocation_ != nullptr;
    }

private:
    void Reset() noexcept;

    MemoryPool* pool_ = nullptr;
    void* allocation_ = nullptr;
    std::size_t size_ = 0;
    std::size_t capacity_ = 0;
};

class MemoryPool {
public:
    virtual ~MemoryPool() = default;

    virtual void* Allocate(std::size_t capacity) = 0;
    virtual void Deallocate(void* data,
                            std::size_t capacity) noexcept = 0;
};

} // namespace iris::media
