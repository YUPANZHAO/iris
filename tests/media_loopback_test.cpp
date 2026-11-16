#include "media/backend/ffmpeg/ffmpeg_video_pipeline.h"
#include "media/engine/media_engine.h"
#include "media/transport/pj_ioqueue_service.h"
#include "media/transport/rtp.h"

#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>

namespace {

// Act as a peer that sends SPS and PPS in STAP-A rather than as individual
// RTP packets. The receiver must decode inter frames between periodic IDRs.
class StapALoopback : public iris::media::IMediaPacketSink {
public:
    explicit StapALoopback(iris::media::FfmpegVideoPipeline& pipeline)
        : pipeline_(pipeline) {}

    iris::media::MediaResult sendRtp(iris::MediaKind,
                                    iris::media::ByteView datagram) override {
        iris::media::RtpPacket packet;
        if (!iris::media::parse_rtp(datagram.data, datagram.size, packet) ||
            packet.payloadSize == 0) {
            return iris::media::MediaResult::failure("invalid test RTP packet");
        }
        const auto nal_type = packet.payload[0] & 0x1f;
        if (nal_type == 7 || nal_type == 8) {
            aggregate_.push_back(static_cast<std::uint8_t>(packet.payloadSize >> 8));
            aggregate_.push_back(static_cast<std::uint8_t>(packet.payloadSize));
            aggregate_.insert(aggregate_.end(), packet.payload,
                              packet.payload + packet.payloadSize);
            return iris::media::MediaResult::success();
        }
        if (aggregate_.size() > 1) {
            const auto result = deliver(packet, aggregate_.data(), aggregate_.size(), false);
            aggregate_.resize(1);
            if (!result.ok) {
                return result;
            }
        }
        return deliver(packet, packet.payload, packet.payloadSize, packet.header.marker);
    }

    iris::media::MediaResult sendRtcp(iris::MediaKind,
                                     iris::media::ByteView) override {
        return iris::media::MediaResult::success();
    }

private:
    iris::media::MediaResult deliver(const iris::media::RtpPacket& packet,
                                     const std::uint8_t* payload,
                                     std::size_t size, bool marker) {
        const auto datagram = sender_.create(packet.header.payloadType,
                                             packet.header.timestamp,
                                             payload, size, marker);
        return pipeline_.onRtpPacket({datagram.data(), datagram.size()});
    }

    iris::media::FfmpegVideoPipeline& pipeline_;
    iris::media::RtpSender sender_;
    std::vector<std::uint8_t> aggregate_{0x78};
};

int test_stap_a() {
    iris::MediaRuntimeConfig runtime;
    runtime.videoInput = "null";
    runtime.videoWidth = 320;
    runtime.videoHeight = 180;
    runtime.videoFps = 15;
    runtime.renderVideo = false;
    iris::MediaSessionConfig session;
    session.localUri = "sip:stap-a@iris.local";
    iris::media::FfmpegVideoPipeline pipeline;
    StapALoopback loopback(pipeline);
    const auto start = pipeline.start(runtime, session, loopback);
    if (!start.ok) {
        std::cerr << "STAP-A loopback failed to start: " << start.error << '\n';
        return 1;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    iris::MediaStats stats;
    do {
        stats = {};
        pipeline.addStats(stats);
        if (stats.videoFramesDecoded >= 8) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    } while (std::chrono::steady_clock::now() < deadline);
    pipeline.stop();
    std::cout << "STAP-A loopback decoded " << stats.videoFramesDecoded << " frames\n";
    return stats.videoFramesDecoded >= 8 ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "";
    if (mode == "--stap-a") {
        return test_stap_a();
    }
    const bool capture_real_devices = mode == "--capture-real";
    const bool use_null_sources = mode == "--null-input";
    iris::MediaRuntimeConfig config;
    config.audioEnabled = true;
    config.videoEnabled = true;
    config.renderAudio = false;
    config.renderVideo = false;
    config.mediaAddress = "127.0.0.1";
    config.audioInput =
        "lavfi:sine=frequency=440:sample_rate=48000";
    config.videoInput =
        "lavfi:testsrc=size=320x180:rate=15";
    config.videoWidth = 320;
    config.videoHeight = 180;
    config.videoFps = 15;
    if (use_null_sources) {
        config.audioInput = "null";
        config.videoInput = "null";
    }

    iris::MediaRuntimeConfig caller_config = config;
    if (capture_real_devices) {
        caller_config.audioInput = "auto";
        caller_config.videoInput = "auto";
    }

    iris::media::PjIoqueueService io_service;
    if (!io_service.ok()) {
        std::cerr << "ioqueue failed: " << io_service.error() << '\n';
        return 1;
    }
    iris::media::MediaEngine caller(caller_config, io_service);
    iris::media::MediaEngine callee(config, io_service);

    iris::CallId callerId{"media-caller"};
    iris::CallId calleeId{"media-callee"};
    const auto offer = caller.prepareOutgoing(
        callerId, "sip:caller@iris.local");
    if (!offer.ok) {
        std::cerr << "offer failed: " << offer.error << '\n';
        return 1;
    }
    const auto answer = callee.prepareAnswer(
        calleeId, "sip:callee@iris.local", offer.sdp);
    if (!answer.ok) {
        std::cerr << "answer failed: " << answer.error << '\n';
        std::cerr << "offer SDP:\n" << offer.sdp << '\n';
        return 1;
    }
    auto callerAnswer = caller.applyRemoteAnswer(callerId, answer.sdp);
    if (!callerAnswer.ok) {
        std::cerr << "caller answer failed: " << callerAnswer.error << '\n';
        return 1;
    }

    auto callerStart = caller.start(callerId);
    if (!callerStart.ok) {
        std::cerr << "caller media start failed: "
                  << callerStart.error << '\n';
        return 1;
    }
    // Model a real SIP race: the caller may become confirmed before the
    // callee has finished starting its media backend. The first keyframe can
    // therefore be dropped, and the next IDR must repeat SPS/PPS.
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    auto calleeStart = callee.start(calleeId);
    if (!calleeStart.ok) {
        std::cerr << "callee media start failed: "
                  << calleeStart.error << '\n';
        return 1;
    }

    std::this_thread::sleep_for(std::chrono::seconds(3));
    const auto callerStats = caller.stats(callerId);
    const auto calleeStats = callee.stats(calleeId);
    caller.stop(callerId);
    callee.stop(calleeId);

    if (callerStats.packetsSent == 0 ||
        calleeStats.packetsSent == 0 ||
        callerStats.packetsReceived == 0 ||
        calleeStats.packetsReceived == 0 ||
        callerStats.audioFramesDecoded == 0 ||
        calleeStats.audioFramesDecoded == 0 ||
        callerStats.videoFramesDecoded == 0 ||
        calleeStats.videoFramesDecoded == 0) {
        std::cerr
            << "media loopback did not exchange packets\n"
            << "caller sent=" << callerStats.packetsSent
            << " received=" << callerStats.packetsReceived
            << " audioFrames=" << callerStats.audioFramesDecoded
            << " videoFrames=" << callerStats.videoFramesDecoded << '\n'
            << "callee sent=" << calleeStats.packetsSent
            << " received=" << calleeStats.packetsReceived
            << " audioFrames=" << calleeStats.audioFramesDecoded
            << " videoFrames=" << calleeStats.videoFramesDecoded << '\n';
        return 1;
    }

    std::cout
              << (capture_real_devices ? "real-device caller" :
                  use_null_sources ? "null-source caller" : "caller")
              << " sent=" << callerStats.packetsSent
              << " received=" << callerStats.packetsReceived
              << " audioFrames=" << callerStats.audioFramesDecoded
              << " videoFrames=" << callerStats.videoFramesDecoded << '\n'
              << "callee sent=" << calleeStats.packetsSent
              << " received=" << calleeStats.packetsReceived
              << " audioFrames=" << calleeStats.audioFramesDecoded
              << " videoFrames=" << calleeStats.videoFramesDecoded << '\n';
    return 0;
}

