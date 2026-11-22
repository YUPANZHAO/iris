#pragma once

#include "media/control/media_control.h"
#include "media/transport/rtp_transport.h"

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace iris::media::v2 {

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
    Status setRemoteEndpoint(StreamId id, const std::string& endpoint);
    std::string localEndpoint() const;
    std::string localEndpoint(StreamId id) const;
    TransportStats transportStats() const;
    const MediaControlConfig& config() const noexcept { return config_; }

private:
    struct StreamEntry {
        EncodedPacketStreamPtr stream;
        std::shared_ptr<RTPTransport> transport;
        std::string remoteEndpoint;
    };

    Status configureStream(const StreamConfig& config,
                           EncodedPacketStreamPtr stream);
    AttributeMap transportConfig(const StreamConfig& stream,
                                 const std::string& remoteEndpoint,
                                 bool useConfiguredPort) const;

    mutable std::mutex mutex_;
    MediaControlConfig config_;
    std::unordered_map<StreamId, StreamEntry> streams_;
    std::unordered_map<std::string, std::string> reservedPorts_;
    bool initialized_ = false;
};

} // namespace iris::media::v2
