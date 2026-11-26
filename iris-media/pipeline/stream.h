#pragma once

#include "codec/codec_types.h"

#include <memory>
#include <mutex>

namespace iris::media::pipeline {

// Common lifecycle and encoded-packet boundary for capture-to-encoder streams.
// Implementations decide how capture and codec resources are opened and closed.
class Stream : public codec::IEncodedPacketSink {
public:
    ~Stream() override = default;

    virtual Status open() = 0;
    virtual void close() noexcept = 0;

    void setEncodedPacketSink(
        std::weak_ptr<codec::IEncodedPacketSink> sink) noexcept;

    void onEncodedPacket(const codec::EncodedPacket& packet) final;

private:
    mutable std::mutex sinkMutex_;
    std::weak_ptr<codec::IEncodedPacketSink> encodedPacketSink_;
};

} // namespace iris::media::pipeline
