#pragma once

#include "../media_types.h"

#include <cstddef>
#include <cstdint>

namespace iris::media::v2 {

struct MemoryPoolConfig {
    std::size_t alignment = alignof(std::max_align_t);
    std::size_t growthThresholdBytes = 0;
};

struct MemoryPoolStats {
    std::size_t liveAllocations = 0;
    std::size_t peakLiveAllocations = 0;
    std::uint64_t totalAllocatedBytes = 0;
    std::uint64_t totalReleasedBytes = 0;
    std::uint64_t allocationFailures = 0;
};

struct BufferAcquireResult {
    Status status;
    MediaBuffer buffer;
};

// The pool accepts allocations of any requested size. Implementations may
// internally use slab classes or growth policies, but there is no fixed block
// size, fixed block count, or media-specific pool role in the API.
class IMemoryPool {
public:
    virtual ~IMemoryPool() = default;

    virtual Status initialize(const MemoryPoolConfig& config) = 0;
    virtual void shutdown() noexcept = 0;
    virtual BufferAcquireResult allocate(std::size_t requestedSize) = 0;
    virtual MemoryPoolStats stats() const = 0;
};

// The active pool is configured once by the media owner. Components consume it
// through these macros instead of carrying IMemoryPool through every API.
IMemoryPool& globalMemoryPool() noexcept;
Status setGlobalMemoryPool(IMemoryPool& pool);
void resetGlobalMemoryPool() noexcept;

} // namespace iris::media::v2

#define IRIS_MEDIA_MEMORY_POOL (::iris::media::v2::globalMemoryPool())
#define IRIS_MEDIA_ALLOCATE(size) \
    (IRIS_MEDIA_MEMORY_POOL.allocate((size)))
