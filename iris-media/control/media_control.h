#pragma once

#include "pipeline/stream.h"

#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace iris::media::transport {
class RTPEngine;
}

namespace iris::media::control {

using Stream = pipeline::Stream;
using StreamPtr = std::shared_ptr<Stream>;

struct MediaStream {
    StreamId id = 0;
    std::string mid;
    MediaKind kind = MediaKind::Unknown;
    AttributeMap attributes;
    // Local send stream. For sendrecv media this is the CaptureStream.
    StreamPtr stream;
    // Set when the negotiated direction also receives media.
    StreamPtr receiveStream;
    // Internal ownership for codec packet routing and RTP send/receive.
    std::shared_ptr<codec::IEncodedPacketSink> encodedPacketSink;
    std::shared_ptr<transport::RTPEngine> rtpEngine;
    AttributeMap rtpAttachOptions;
};

// Called before a negotiated stream is added to MediaControl. The application
// may configure the default stream or replace it with its own send stream.
class MediaControlSink {
public:
    virtual ~MediaControlSink() = default;
    virtual Status onStreamCreate(MediaStream& mediaStream) = 0;
};

class MediaControl {
public:
    MediaControl() = default;
    virtual ~MediaControl();

    MediaControl(const MediaControl&) = delete;
    MediaControl& operator=(const MediaControl&) = delete;

    void setMediaControlSink(
        std::weak_ptr<MediaControlSink> sink) noexcept;

    StreamPtr stream(StreamId id) const;
    std::vector<MediaStream> streams() const;
    Status startAll();
    void stopAll() noexcept;
    bool started() const noexcept;

protected:
    Status replaceStreams(std::vector<MediaStream> mediaStreams);

private:
    mutable std::mutex mutex_;
    std::weak_ptr<MediaControlSink> sink_;
    std::vector<MediaStream> streams_;
    StreamId nextStreamId_ = 1;
    bool started_ = false;
};

} // namespace iris::media::control
