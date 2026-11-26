#include "pipeline/stream.h"

#include <utility>

namespace iris::media::pipeline {

void Stream::setEncodedPacketSink(
    std::weak_ptr<codec::IEncodedPacketSink> sink) noexcept {
    std::lock_guard<std::mutex> lock(sinkMutex_);
    encodedPacketSink_ = std::move(sink);
}

void Stream::onEncodedPacket(const codec::EncodedPacket& packet) {
    std::shared_ptr<codec::IEncodedPacketSink> sink;
    {
        std::lock_guard<std::mutex> lock(sinkMutex_);
        sink = encodedPacketSink_.lock();
    }
    if (sink) {
        sink->onEncodedPacket(packet);
    }
}

} // namespace iris::media::pipeline
