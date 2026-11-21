#include "core/media_session_manager.h"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>

int main(int argc, char** argv) {
    const bool forceNull =
        std::find(argv, argv + argc, std::string("--null-input")) !=
        argv + argc;
    const bool audioOnly =
        std::find(argv, argv + argc, std::string("--audio-only")) !=
        argv + argc;
    const bool videoOnly =
        std::find(argv, argv + argc, std::string("--video-only")) !=
        argv + argc;

    iris::MediaRuntimeConfig config;
    config.audioEnabled = !videoOnly;
    config.videoEnabled = !audioOnly;
    config.renderAudio = false;
    config.renderVideo = false;
    config.audioInput = forceNull ? "null" : "null";
    config.videoInput = "null";
    config.videoWidth = 320;
    config.videoHeight = 180;
    config.videoFps = 15;

    iris::core::MediaSessionManager caller(config);
    iris::core::MediaSessionManager callee(config);
    const iris::CallId callerId{"media-loopback-caller"};
    const iris::CallId calleeId{"media-loopback-callee"};

    const auto offer = caller.prepareOutgoing(
        callerId, "sip:caller@example.com");
    if (!offer.ok) {
        std::cerr << "offer failed: " << offer.error << '\n';
        return 1;
    }
    const auto answer = callee.prepareAnswer(
        calleeId, "sip:callee@example.com", offer.sdp);
    if (!answer.ok) {
        std::cerr << "answer failed: " << answer.error << '\n';
        return 1;
    }
    const auto applied =
        caller.applyRemoteAnswer(callerId, answer.sdp);
    if (!applied.ok) {
        std::cerr << "apply answer failed: " << applied.error << '\n';
        return 1;
    }
    const auto callerStarted = caller.start(callerId);
    const auto calleeStarted = callee.start(calleeId);
    if (!callerStarted.ok || !calleeStarted.ok) {
        std::cerr << "media start failed: "
                  << callerStarted.error << ' '
                  << calleeStarted.error << '\n';
        return 1;
    }

    iris::MediaStats callerStats;
    iris::MediaStats calleeStats;
    for (int retry = 0; retry < 150; ++retry) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        callerStats = caller.stats(callerId);
        calleeStats = callee.stats(calleeId);
        const bool callerReady = audioOnly
            ? callerStats.audioFramesDecoded > 0
            : callerStats.videoFramesDecoded > 0;
        const bool calleeReady = audioOnly
            ? calleeStats.audioFramesDecoded > 0
            : calleeStats.videoFramesDecoded > 0;
        if (callerReady && calleeReady) {
            break;
        }
    }

    caller.stop(callerId);
    callee.stop(calleeId);
    const bool success = audioOnly
        ? callerStats.audioFramesDecoded > 0 &&
              calleeStats.audioFramesDecoded > 0
        : callerStats.videoFramesDecoded > 0 &&
              calleeStats.videoFramesDecoded > 0;
    if (!success) {
        std::cerr << "media loopback did not decode media"
                  << " caller=" << callerStats.videoFramesDecoded
                  << " callee=" << calleeStats.videoFramesDecoded
                  << " audio-caller=" << callerStats.audioFramesDecoded
                  << " audio-callee=" << calleeStats.audioFramesDecoded
                  << '\n';
        return 1;
    }
    std::cout << "media frames decoded\n";
    return 0;
}
