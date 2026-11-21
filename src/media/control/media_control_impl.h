#pragma once

#include "media/control/media_control.h"
#include "media/transport/rtp_transport.h"

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace iris::media::v2 {

class StreamMuxSink final : public IEncodedPacketSink {
public:
    void setStream(StreamId id,
                   std::weak_ptr<IEncodedPacketStream> stream);
    void removeStream(StreamId id);
    void onEncodedPacket(const EncodedPacket& packet) override;

private:
    std::mutex mutex_;
    std::unordered_map<StreamId, std::weak_ptr<IEncodedPacketStream>>
        streams_;
};

class MediaControl : public IMediaControl {
public:
    MediaControl();
    ~MediaControl() override;

    Status initialize(const MediaControlConfig& config) override;
    Status createStream(const StreamConfig& config,
                        EncodedPacketStreamPtr* stream) override;
    bool destroyStream(StreamId id) override;

    EncodedPacketStreamPtr stream(StreamId id) const override;
    std::vector<StreamId> streams() const override;

    Status startAll();
    void stopAll() noexcept;
    MediaStats stats() const override;

    Status setRemoteEndpoint(const std::string& endpoint);
    std::string localEndpoint() const;
    TransportStats transportStats() const;
    const MediaControlConfig& config() const noexcept { return config_; }

private:
    Status configureStream(const StreamConfig& config,
                           EncodedPacketStreamPtr stream);
    void updateMux();
    TransportConfig transportConfig() const;

    mutable std::mutex mutex_;
    MediaControlConfig config_;
    std::shared_ptr<RTPTransport> transport_;
    std::shared_ptr<StreamMuxSink> mux_;
    std::unordered_map<StreamId, EncodedPacketStreamPtr> streams_;
    bool initialized_ = false;
};

} // namespace iris::media::v2
