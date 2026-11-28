#pragma once

#include "iris/config.h"
#include "iris/events.h"
#include "iris/types.h"

#include <memory>
#include <string>
#include <vector>

namespace iris {

class RuntimeImpl;

class Runtime {
public:
    explicit Runtime(const RuntimeConfig& config = {});
    ~Runtime();

    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;
    Runtime(Runtime&&) noexcept;
    Runtime& operator=(Runtime&&) noexcept;

    std::string version() const;
    std::string pjprojectVersion() const;
    unsigned sipPort() const;

    AccountId addAccount(const AccountConfig& config);
    void registerAccount(const AccountId& id);
    void unregisterAccount(const AccountId& id);
    void removeAccount(const AccountId& id);

    CallId startCall(const AccountId& accountId, const std::string& destinationUri);
    void answerCall(const CallId& id);
    void rejectCall(const CallId& id);
    void hangupCall(const CallId& id);
    void holdCall(const CallId& id);
    void resumeCall(const CallId& id);
    void setAudioMuted(const CallId& id, bool muted);
    void setVideoMuted(const CallId& id, bool muted);
    void setMediaInput(MediaKind kind, const std::string& source);
    std::string mediaInput(MediaKind kind) const;
    void setAudioFrameCallback(AudioFrameCallback callback);
    void setVideoFrameCallback(VideoFrameCallback callback);

    std::vector<AccountSnapshot> accounts() const;
    std::vector<CallSnapshot> calls() const;
    MediaStats mediaStats(const CallId& id) const;

    // Returns events accumulated since the previous call.
    std::vector<Event> pollEvents();

private:
    std::unique_ptr<RuntimeImpl> impl_;
};

} // namespace iris

