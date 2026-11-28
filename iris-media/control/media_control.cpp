#include "control/media_control.h"

#include "transport/rtp_engine.h"

#include <algorithm>
#include <set>
#include <utility>

namespace iris::media::control {
namespace {

Status failure(std::string message) {
    return Status::failure(std::move(message));
}

} // namespace

MediaControl::~MediaControl() {
    stopAll();
}

void MediaControl::setMediaControlSink(
    std::weak_ptr<MediaControlSink> sink) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    sink_ = std::move(sink);
}

StreamPtr MediaControl::stream(StreamId id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto iterator = std::find_if(
        streams_.begin(), streams_.end(),
        [id](const MediaStream& mediaStream) {
            return mediaStream.id == id;
        });
    return iterator == streams_.end() ? StreamPtr{} : iterator->stream;
}

std::vector<MediaStream> MediaControl::streams() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return streams_;
}

Status MediaControl::startAll() {
    std::vector<MediaStream> mediaStreams;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (streams_.empty()) {
            return failure("media control has no streams");
        }
        if (started_) {
            return {};
        }
        started_ = true;
        mediaStreams = streams_;
    }

    std::vector<StreamPtr> opened;
    const auto wasOpened = [&](const StreamPtr& stream) {
        return std::any_of(
            opened.begin(),
            opened.end(),
            [&](const StreamPtr& candidate) {
                return candidate == stream;
            });
    };
    const auto rollback = [&] {
        for (auto iterator = opened.rbegin();
             iterator != opened.rend();
             ++iterator) {
            (*iterator)->close();
        }
        for (const auto& mediaStream : mediaStreams) {
            if (mediaStream.rtpEngine) {
                mediaStream.rtpEngine->detach();
            }
        }
        std::lock_guard<std::mutex> lock(mutex_);
        started_ = false;
    };

    // Register receive sinks before an attached transport can deliver packets.
    for (const auto& mediaStream : mediaStreams) {
        if (!mediaStream.receiveStream ||
            wasOpened(mediaStream.receiveStream)) {
            continue;
        }
        const auto status = mediaStream.receiveStream->open();
        if (!status.ok) {
            rollback();
            return status;
        }
        opened.push_back(mediaStream.receiveStream);
    }

    // Attach only after the receive sink and stream bindings are ready.
    for (const auto& mediaStream : mediaStreams) {
        if (!mediaStream.rtpEngine) {
            continue;
        }
        auto status =
            mediaStream.rtpEngine->configure(
                mediaStream.rtpAttachOptions);
        if (status.ok) {
            status = mediaStream.rtpEngine->attach(
                mediaStream.rtpAttachOptions);
        }
        if (!status.ok) {
            rollback();
            return status;
        }
    }

    // Start capture last so its first encoded packet has an attached transport.
    for (const auto& mediaStream : mediaStreams) {
        if (!mediaStream.stream ||
            wasOpened(mediaStream.stream)) {
            continue;
        }
        if (mediaStream.encodedPacketSink) {
            mediaStream.stream->setEncodedPacketSink(
                mediaStream.encodedPacketSink);
        }
        const auto status = mediaStream.stream->open();
        if (!status.ok) {
            rollback();
            return status;
        }
        opened.push_back(mediaStream.stream);
    }
    return {};
}

void MediaControl::stopAll() noexcept {
    std::vector<MediaStream> mediaStreams;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        mediaStreams = streams_;
        started_ = false;
    }

    std::vector<StreamPtr> closed;
    const auto close = [&](const StreamPtr& stream) {
        if (!stream ||
            std::any_of(
                closed.begin(),
                closed.end(),
                [&](const StreamPtr& candidate) {
                    return candidate == stream;
                })) {
            return;
        }
        stream->close();
        closed.push_back(stream);
    };
    for (const auto& mediaStream : mediaStreams) {
        close(mediaStream.stream);
        close(mediaStream.receiveStream);
    }
    for (const auto& mediaStream : mediaStreams) {
        if (mediaStream.rtpEngine) {
            mediaStream.rtpEngine->detach();
        }
    }
}

bool MediaControl::started() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return started_;
}

Status MediaControl::replaceStreams(
    std::vector<MediaStream> mediaStreams) {
    std::shared_ptr<MediaControlSink> sink;
    StreamId nextId = 1;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (started_) {
            return failure(
                "cannot replace streams while media control is started");
        }
        sink = sink_.lock();
        nextId = nextStreamId_;
    }

    std::set<StreamId> ids;
    for (auto& mediaStream : mediaStreams) {
        if (mediaStream.id == 0) {
            mediaStream.id = nextId++;
        }
        const auto assignedId = mediaStream.id;
        if (sink) {
            const auto status = sink->onStreamCreate(mediaStream);
            if (!status.ok) {
                return status;
            }
        }
        mediaStream.id = assignedId;
        if (mediaStream.id == 0 ||
            mediaStream.kind == MediaKind::Unknown ||
            !mediaStream.stream) {
            return failure("media control sink produced an incomplete stream");
        }
        if (!ids.insert(mediaStream.id).second) {
            return failure("negotiated stream id is duplicated");
        }
        nextId = std::max(nextId, mediaStream.id + 1);
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (started_) {
        return failure(
            "cannot replace streams while media control is started");
    }
    nextStreamId_ = nextId;
    streams_.swap(mediaStreams);
    return {};
}

} // namespace iris::media::control
