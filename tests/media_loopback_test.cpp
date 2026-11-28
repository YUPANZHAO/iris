#include "core/media_session.h"

#include <algorithm>
#include <atomic>
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
    config.audioInput = forceNull ? "null" : "null";
    config.videoInput = "null";
    config.videoWidth = 320;
    config.videoHeight = 180;
    config.videoFps = 15;

    const iris::CallId callerId{"media-loopback-caller"};
    const iris::CallId calleeId{"media-loopback-callee"};
    std::atomic<int> callerAudioFrames{0};
    std::atomic<int> calleeAudioFrames{0};
    iris::core::MediaSession caller(
        callerId,
        "sip:caller@example.com",
        config,
        [&callerAudioFrames] {
            return iris::AudioFrameCallback(
                [&callerAudioFrames](const iris::AudioFrameView&) {
                    callerAudioFrames.fetch_add(1);
                });
        },
        [] { return iris::VideoFrameCallback{}; });
    iris::core::MediaSession callee(
        calleeId,
        "sip:callee@example.com",
        config,
        [&calleeAudioFrames] {
            return iris::AudioFrameCallback(
                [&calleeAudioFrames](const iris::AudioFrameView&) {
                    calleeAudioFrames.fetch_add(1);
                });
        },
        [] { return iris::VideoFrameCallback{}; });

    std::string offerSdp;
    const auto offer = caller.prepareOutgoing(&offerSdp);
    if (!offer.ok) {
        std::cerr << "offer failed: " << offer.message << '\n';
        return 1;
    }
    std::string answerSdp;
    const auto answer =
        callee.prepareAnswer(offerSdp, &answerSdp);
    if (!answer.ok) {
        std::cerr << "answer failed: " << answer.message << '\n';
        return 1;
    }
    const auto applied = caller.applyRemoteAnswer(answerSdp);
    if (!applied.ok) {
        std::cerr << "apply answer failed: " << applied.message << '\n';
        return 1;
    }
    const auto callerStarted = caller.start();
    const auto calleeStarted = callee.start();
    if (!callerStarted.ok || !calleeStarted.ok) {
        std::cerr << "media start failed: "
                  << callerStarted.message << ' '
                  << calleeStarted.message << '\n';
        return 1;
    }

    iris::MediaStats callerStats;
    iris::MediaStats calleeStats;
    for (int retry = 0; retry < 150; ++retry) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        callerStats = caller.stats();
        calleeStats = callee.stats();
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

    caller.stop();
    callee.stop();
    const bool success = audioOnly
        ? callerStats.audioFramesDecoded > 0 &&
              calleeStats.audioFramesDecoded > 0 &&
              callerAudioFrames.load() > 0 &&
              calleeAudioFrames.load() > 0
        : callerStats.videoFramesDecoded > 0 &&
              calleeStats.videoFramesDecoded > 0;
    if (!success) {
        std::cerr << "media loopback did not decode media"
                  << " caller=" << callerStats.videoFramesDecoded
                  << " callee=" << calleeStats.videoFramesDecoded
                  << " audio-caller=" << callerStats.audioFramesDecoded
                  << " audio-callee=" << calleeStats.audioFramesDecoded
                  << " sent-caller=" << callerStats.packetsSent
                  << " sent-callee=" << calleeStats.packetsSent
                  << " recv-caller=" << callerStats.packetsReceived
                  << " recv-callee=" << calleeStats.packetsReceived
                  << '\n';
        return 1;
    }
    std::cout << "media frames decoded\n";
    return 0;
}
