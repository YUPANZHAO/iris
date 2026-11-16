#include "media/sdp/sdp_controller.h"
#include "media/transport/pj_ioqueue_service.h"

#include <iostream>
#include <string>

namespace {

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "SDP test failed: " << message << '\n';
    }
    return condition;
}

bool contains(const std::string& text, const std::string& value) {
    return text.find(value) != std::string::npos;
}

} // namespace

int main() {
    using namespace iris::media;
    PjIoqueueService io_service;
    if (!io_service.ok()) {
        std::cerr << "ioqueue setup failed\n";
        return 1;
    }

    iris::MediaSessionConfig video_only;
    video_only.audioEnabled = false;
    video_only.videoEnabled = true;
    video_only.codecPreferences = {
        iris::MediaCodecPreference{
            iris::MediaKind::Video, "h264", 90000, 0, 96},
    };
    LocalSdpPorts video_ports;
    video_ports.address = "192.0.2.10";
    video_ports.videoPort = 5004;

    const auto video_offer =
        create_sdp_offer(
            video_only, video_ports, CodecRegistry(),
            io_service.pool_factory());
    if (!expect(video_offer.ok, "video-only offer was rejected") ||
        !expect(contains(video_offer.sdp, "c=IN IP4 192.0.2.10"),
                "offer omitted its advertised media address") ||
        !expect(!contains(video_offer.sdp, "m=audio"),
                "video-only offer unexpectedly included audio")) {
        return 1;
    }

    const SdpOrigin reinvite_origin{"24680", 7};
    const auto reinvite_offer = create_sdp_offer(
        video_only, video_ports, CodecRegistry(),
        io_service.pool_factory(), &reinvite_origin);
    if (!expect(
            contains(
                reinvite_offer.sdp,
                "o=iris 24680 7 IN IP4 192.0.2.10"),
            "re-INVITE offer did not use its session version")) {
        return 1;
    }

    const std::string remote_offer =
        "v=0\r\n"
        "o=peer 1 1 IN IP4 192.0.2.20\r\n"
        "s=Peer\r\n"
        "c=IN IP4 192.0.2.20\r\n"
        "t=0 0\r\n"
        "m=audio 5000 RTP/AVP 112\r\n"
        "a=rtpmap:112 opus/48000/2\r\n"
        "a=sendrecv\r\n"
        "m=video 5002 RTP/AVP 97\r\n"
        "a=rtpmap:97 H264/90000\r\n"
        "a=fmtp:97 packetization-mode=1;profile-level-id=42e01f\r\n"
        "a=sendrecv\r\n";

    iris::MediaSessionConfig both;
    both.codecPreferences = {
        iris::MediaCodecPreference{
            iris::MediaKind::Audio, "opus", 48000, 2, 111},
        iris::MediaCodecPreference{
            iris::MediaKind::Video, "h264", 90000, 0, 96},
    };
    LocalSdpPorts local_ports;
    local_ports.address = "192.0.2.10";
    local_ports.audioPort = 6000;
    local_ports.videoPort = 6002;

    const auto answer =
        create_sdp_answer(
            both, local_ports, remote_offer, CodecRegistry(),
            io_service.pool_factory());
    if (!expect(answer.ok, "answer generation failed") ||
        !expect(contains(answer.sdp, "m=audio 6000 RTP/AVP 112"),
                "answer did not preserve the offered audio PT") ||
        !expect(contains(answer.sdp,
                         "a=fmtp:112 minptime=10;useinbandfec=1;maxaveragebitrate=48000"),
                "answer omitted the local Opus receive bitrate") ||
        !expect(contains(answer.sdp, "m=video 6002 RTP/AVP 97"),
                "answer did not preserve the offered video PT") ||
        !expect(!contains(answer.sdp, "192.0.2.0"),
                "answer contains an invalid media address")) {
        return 1;
    }

    const std::string send_only_offer =
        "v=0\r\n"
        "o=peer 2 1 IN IP4 192.0.2.20\r\n"
        "s=Peer\r\n"
        "c=IN IP4 192.0.2.20\r\n"
        "t=0 0\r\n"
        "m=video 5002 RTP/AVP 97\r\n"
        "a=rtpmap:97 H264/90000\r\n"
        "a=fmtp:97 packetization-mode=1;profile-level-id=42e01f\r\n"
        "a=sendonly\r\n";
    const auto send_only_answer = create_sdp_answer(
        both, local_ports, send_only_offer, CodecRegistry(),
        io_service.pool_factory());
    if (!expect(send_only_answer.ok,
                "send-only offer was rejected") ||
        !expect(contains(send_only_answer.sdp, "a=recvonly"),
                "answer did not reverse the offered direction")) {
        return 1;
    }

    const auto itc_offer = remote_offer.substr(
        0, remote_offer.find("m=video")) +
        "m=video 5014 RTP/AVP 113 112 110\r\n"
        "a=rtpmap:113 H265/90000\r\n"
        "a=rtpmap:112 H264/90000\r\n"
        "a=fmtp:112 profile-level-id=64001f;packetization-mode=1\r\n"
        "a=rtpmap:110 H264/90000\r\n"
        "a=fmtp:110 profile-level-id=42801f;packetization-mode=1;max-br=8192\r\n"
        "m=application 5016 UDP/BFCP *\r\n";
    const auto itc_answer = create_sdp_answer(
        both, local_ports, itc_offer, CodecRegistry(),
        io_service.pool_factory());
    if (!expect(itc_answer.ok &&
                    contains(itc_answer.sdp, "m=video 6002 RTP/AVP 110\r\n") &&
                    contains(itc_answer.sdp,
                             "a=fmtp:110 packetization-mode=1;profile-level-id=42801f\r\n"),
                "answer did not accept the offered H.264 Baseline format") ||
        !expect(contains(itc_answer.sdp, "m=application 0 UDP/BFCP *\r\n"),
                "answer did not reject the unsupported BFCP section")) {
        return 1;
    }

    const std::string mixed_offer =
        "v=0\r\n"
        "o=peer 1 1 IN IP4 192.0.2.20\r\n"
        "s=Peer\r\n"
        "c=IN IP4 192.0.2.20\r\n"
        "t=0 0\r\n"
        "m=text 5000 RTP/AVP 98\r\n"
        "a=rtpmap:98 t140/1000\r\n"
        "m=video 5002 RTP/AVP 97\r\n"
        "a=rtpmap:97 H264/90000\r\n";
    const auto parsed_mixed =
        parse_sdp(mixed_offer, io_service.pool_factory());
    if (!expect(parsed_mixed.media.size() == 2,
                "unsupported media section was not preserved") ||
        !expect(!parsed_mixed.media[0].supported,
                "text media was incorrectly marked as supported") ||
        !expect(parsed_mixed.media[1].kind ==
                    iris::MediaKind::Video,
                "valid media after an unsupported section was lost")) {
        std::cerr << "parsed m-line count: "
                  << parsed_mixed.media.size() << '\n';
        return 1;
    }

    const std::string mixed_answer_offer =
        "v=0\r\n"
        "o=peer 1 1 IN IP4 192.0.2.20\r\n"
        "s=Peer\r\n"
        "c=IN IP4 192.0.2.20\r\n"
        "t=0 0\r\n"
        "m=text 5000 RTP/AVP 98\r\n"
        "a=rtpmap:98 t140/1000\r\n"
        "m=video 5002 RTP/AVP 97\r\n"
        "a=rtpmap:97 H264/90000\r\n"
        "a=fmtp:97 packetization-mode=1;profile-level-id=42e01f\r\n";
    const auto mixed_answer =
        create_sdp_answer(
            both, local_ports, mixed_answer_offer, CodecRegistry(),
            io_service.pool_factory());
    if (!expect(mixed_answer.ok,
                "answer with an unsupported media section failed") ||
        !expect(contains(
                    mixed_answer.sdp,
                    "m=text 0 RTP/AVP 98\r\n"),
                "answer did not preserve the unsupported m-line") ||
        !expect(contains(
                    mixed_answer.sdp,
                    "m=video 6002 RTP/AVP 97\r\n"),
                "answer did not preserve m-line ordering")) {
        return 1;
    }

    CodecRegistry prioritized({
        iris::MediaCodecPreference{
            iris::MediaKind::Video, "H264", 90000, 0, 97,
            "h264-primary",
            "packetization-mode=1;profile-level-id=42e01f"},
        iris::MediaCodecPreference{
            iris::MediaKind::Video, "H264", 90000, 0, 98,
            "h264-fallback",
            "packetization-mode=1;profile-level-id=42e01e"},
    });
    const std::vector<RemoteCodecFormat> remote_formats{
        RemoteCodecFormat{
            98, "H264", 90000, 0,
            "packetization-mode=1;profile-level-id=42e01e"},
        RemoteCodecFormat{
            97, "H264", 90000, 0,
            "packetization-mode=1;profile-level-id=42e01f"},
    };
    const auto prioritized_selection =
        prioritized.select(iris::MediaKind::Video, remote_formats);
    if (!expect(
            prioritized_selection.payloadType == 97 &&
                prioritized_selection.local &&
                prioritized_selection.local->id == "h264-primary",
            "codec selection did not honor local priority")) {
        return 1;
    }

    const CodecRegistry defaults;
    const auto select_h264 = [&defaults](const char* fmtp,
                                         SdpPeer peer = SdpPeer::Offer) {
        return defaults.select(iris::MediaKind::Video,
                               {{105, "H264", 90000, 0, fmtp}}, peer);
    };
    if (!expect(contains(select_h264(
                    "packetization-mode=1;profile-level-id=4D801E").localFmtp,
                    "profile-level-id=4d801e"),
                "equivalent constrained profile at a lower level was rejected") ||
        !expect(contains(select_h264(
                    "packetization-mode=1;profile-level-id=58a020").localFmtp,
                    "profile-level-id=58a01f"),
                "Baseline offer did not negotiate down to the local level") ||
        !expect(contains(select_h264(
                    "packetization-mode=1;profile-level-id=42f00b").localFmtp,
                    "profile-level-id=42f00b"),
                "Level 1b was not preserved") ||
        !expect(contains(select_h264(
                    "packetization-mode=1;profile-level-id=42e01e;"
                    "level-asymmetry-allowed=1").localFmtp,
                    "level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e01f"),
                "mutual level asymmetry did not preserve the local level") ||
        !expect(!select_h264("profile-level-id=42e01f").local,
                "mismatched packetization modes were accepted") ||
        !expect(!select_h264("packetization-mode=1;profile-level-id=64001f").local,
                "unsupported High profile was accepted") ||
        !expect(!select_h264("packetization-mode=1;profile-level-id=42801f",
                             SdpPeer::Answer).local,
                "an answer changed the offered profile") ||
        !expect(!select_h264("packetization-mode=1;profile-level-id=42e020",
                             SdpPeer::Answer).local,
                "an answer raised the level without mutual asymmetry")) {
        return 1;
    }

    std::cout << "SDP offer/answer negotiation checks passed\n";
    return 0;
}
