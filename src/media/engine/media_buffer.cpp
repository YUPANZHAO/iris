#include "media/engine/media_buffer.h"

#include <algorithm>
#include <cstring>
#include <memory>

namespace iris::media::v2 {
namespace {

class HeapBufferLease final : public IBufferLease {
public:
    explicit HeapBufferLease(std::size_t size)
        : data_(std::make_unique<std::uint8_t[]>(size)),
          size_(size) {
    }

    std::uint8_t* data() noexcept override {
        return data_.get();
    }

    const std::uint8_t* data() const noexcept override {
        return data_.get();
    }

    std::size_t size() const noexcept override {
        return size_;
    }

    std::size_t capacity() const noexcept override {
        return size_;
    }

private:
    std::unique_ptr<std::uint8_t[]> data_;
    std::size_t size_ = 0;
};

} // namespace

MediaBuffer allocateMediaBuffer(std::size_t size) {
    return size == 0
        ? MediaBuffer{}
        : std::make_shared<HeapBufferLease>(size);
}

MediaBufferView copyToMediaBuffer(const std::uint8_t* data,
                                  std::size_t size) {
    if (!data || size == 0) {
        return {};
    }
    auto buffer = allocateMediaBuffer(size);
    if (!buffer) {
        return {};
    }
    std::memcpy(buffer->data(), data, size);
    return MediaBufferView{std::move(buffer), 0, size};
}

std::size_t bufferViewSize(const MediaBufferView& view) noexcept {
    if (!view.buffer || view.offset > view.buffer->size()) {
        return 0;
    }
    return std::min(view.size, view.buffer->size() - view.offset);
}

const std::uint8_t* bufferViewData(const MediaBufferView& view) noexcept {
    if (!view.buffer || !bufferViewSize(view)) {
        return nullptr;
    }
    return view.buffer->data() + view.offset;
}

std::uint8_t* mutableBufferViewData(const MediaBufferView& view) noexcept {
    if (!view.buffer || !bufferViewSize(view)) {
        return nullptr;
    }
    return view.buffer->data() + view.offset;
}

} // namespace iris::media::v2
