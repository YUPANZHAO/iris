#pragma once

#include "engine/memory_pool.h"

#include <memory>

namespace iris::media {

class PmrMemoryPoolImpl;

class PmrMemoryPool final : public MemoryPool {
public:
    static PmrMemoryPool& Instance();

    ~PmrMemoryPool() override;

    PmrMemoryPool(const PmrMemoryPool&) = delete;
    PmrMemoryPool& operator=(const PmrMemoryPool&) = delete;
    PmrMemoryPool(PmrMemoryPool&&) = delete;
    PmrMemoryPool& operator=(PmrMemoryPool&&) = delete;

    void* Allocate(std::size_t capacity) override;
    void Deallocate(void* data,
                    std::size_t capacity) noexcept override;

private:
    PmrMemoryPool();

    std::unique_ptr<PmrMemoryPoolImpl> impl_;
};

} // namespace iris::media
