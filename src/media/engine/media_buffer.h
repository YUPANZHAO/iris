#pragma once

#include "media/media_types.h"

#include <cstddef>
#include <cstdint>

namespace iris::media::v2 {

MediaBuffer allocateMediaBuffer(std::size_t size);

MediaBufferView copyToMediaBuffer(const std::uint8_t* data,
                                  std::size_t size);

std::size_t bufferViewSize(const MediaBufferView& view) noexcept;
const std::uint8_t* bufferViewData(const MediaBufferView& view) noexcept;
std::uint8_t* mutableBufferViewData(const MediaBufferView& view) noexcept;

} // namespace iris::media::v2
