#include "media/stream/stream_internal.h"

#include "media/stream/audio_stream.h"
#include "media/stream/video_stream.h"

#include <unordered_map>

namespace iris::media::v2 {
namespace stream_internal {
namespace {

std::mutex gRegistryMutex;
std::unordered_map<const IEncodedPacketStream*,
                   std::shared_ptr<EncodedStreamState>> gEncodedStates;
std::unordered_map<const IEncodedPacketStream*,
                   std::shared_ptr<EncodedSinkBridge>> gBridges;
std::unordered_map<const IEncodedPacketStream*,
                   std::shared_ptr<EncodedSinkBridge>> gSendBridges;
std::unordered_map<const AudioEncodedStream*,
                   std::shared_ptr<AudioFrameState>> gAudioStates;
std::unordered_map<const VideoEncodedStream*,
                   std::shared_ptr<VideoFrameState>> gVideoStates;

} // namespace

void EncodedSinkBridge::onEncodedPacket(const EncodedPacket& packet) {
    if (auto state = state_.lock()) {
        if (outgoing_) {
            if (state->send) {
                (void)state->send(packet);
            }
        } else if (state->receive) {
            state->receive(packet);
        }
    }
}

void registerEncodedStream(
    const IEncodedPacketStream* stream,
    std::shared_ptr<EncodedStreamState> state) {
    auto bridge = std::make_shared<EncodedSinkBridge>(state, false);
    auto sendBridge = std::make_shared<EncodedSinkBridge>(state, true);
    std::lock_guard<std::mutex> lock(gRegistryMutex);
    gEncodedStates[stream] = std::move(state);
    gBridges[stream] = std::move(bridge);
    gSendBridges[stream] = std::move(sendBridge);
}

void unregisterEncodedStream(const IEncodedPacketStream* stream) {
    std::lock_guard<std::mutex> lock(gRegistryMutex);
    gBridges.erase(stream);
    gSendBridges.erase(stream);
    gEncodedStates.erase(stream);
}

std::shared_ptr<EncodedStreamState> encodedStreamState(
    const IEncodedPacketStream* stream) {
    std::lock_guard<std::mutex> lock(gRegistryMutex);
    const auto iterator = gEncodedStates.find(stream);
    return iterator == gEncodedStates.end() ? nullptr : iterator->second;
}

std::shared_ptr<EncodedSinkBridge> encodedSinkBridge(
    const IEncodedPacketStream* stream) {
    std::lock_guard<std::mutex> lock(gRegistryMutex);
    const auto iterator = gBridges.find(stream);
    return iterator == gBridges.end() ? nullptr : iterator->second;
}

std::shared_ptr<EncodedSinkBridge> encodedSendBridge(
    const IEncodedPacketStream* stream) {
    std::lock_guard<std::mutex> lock(gRegistryMutex);
    const auto iterator = gSendBridges.find(stream);
    return iterator == gSendBridges.end() ? nullptr : iterator->second;
}

void registerAudioFrameState(
    const AudioEncodedStream* stream,
    std::shared_ptr<AudioFrameState> state) {
    std::lock_guard<std::mutex> lock(gRegistryMutex);
    gAudioStates[stream] = std::move(state);
}

void unregisterAudioFrameState(const AudioEncodedStream* stream) {
    std::lock_guard<std::mutex> lock(gRegistryMutex);
    gAudioStates.erase(stream);
}

std::shared_ptr<AudioFrameState> audioFrameState(
    const AudioEncodedStream* stream) {
    std::lock_guard<std::mutex> lock(gRegistryMutex);
    const auto iterator = gAudioStates.find(stream);
    return iterator == gAudioStates.end() ? nullptr : iterator->second;
}

void setAudioOutputSink(
    const AudioEncodedStream* stream,
    std::weak_ptr<IAudioFrameSink> sink) {
    if (auto state = audioFrameState(stream)) {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->outputSink = std::move(sink);
    }
}

void registerVideoFrameState(
    const VideoEncodedStream* stream,
    std::shared_ptr<VideoFrameState> state) {
    std::lock_guard<std::mutex> lock(gRegistryMutex);
    gVideoStates[stream] = std::move(state);
}

void unregisterVideoFrameState(const VideoEncodedStream* stream) {
    std::lock_guard<std::mutex> lock(gRegistryMutex);
    gVideoStates.erase(stream);
}

std::shared_ptr<VideoFrameState> videoFrameState(
    const VideoEncodedStream* stream) {
    std::lock_guard<std::mutex> lock(gRegistryMutex);
    const auto iterator = gVideoStates.find(stream);
    return iterator == gVideoStates.end() ? nullptr : iterator->second;
}

void setVideoOutputSink(
    const VideoEncodedStream* stream,
    std::weak_ptr<IVideoFrameSink> sink) {
    if (auto state = videoFrameState(stream)) {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->outputSink = std::move(sink);
    }
}

} // namespace stream_internal
} // namespace iris::media::v2
