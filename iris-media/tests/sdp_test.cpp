#include "sdp/sdp.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <type_traits>

namespace {

using namespace iris::media;
using namespace iris::media::sdp;

static_assert(!std::is_abstract_v<SdpBuilder>);
static_assert(!std::is_abstract_v<SdpParser>);
static_assert(!std::is_abstract_v<SdpNegotiator>);

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

bool contains(const std::string& text, const char* value) {
    return text.find(value) != std::string::npos;
}

SdpNegotiationInput makeLocalInput() {
    SdpNegotiationInput input;
    input.origin.username = "iris";
    input.origin.sessionId = 123456;
    input.origin.sessionVersion = 1;
    input.origin.unicastAddress = "127.0.0.1";
    input.connection.address = "127.0.0.1";
    input.sessionName = "iris-test";
    input.attributes["group"] = "BUNDLE audio video";

    SdpMediaCapability audio;
    audio.kind = MediaKind::Audio;
    audio.mid = "audio";
    audio.direction = SdpDirection::SendRecv;
    audio.transport.dataPort = 5000;
    audio.transport.attributes["rtcpMux"] = "true";

    SdpCodecPreference opus;
    opus.codecType = CodecType::Opus;
    opus.encodingName = "opus";
    opus.payloadType = 111;
    opus.attributes["clockRate"] = "48000";
    opus.attributes["channels"] = "2";
    audio.codecs.push_back(opus);
    input.media.push_back(audio);

    SdpMediaCapability video;
    video.kind = MediaKind::Video;
    video.mid = "video";
    video.direction = SdpDirection::SendRecv;
    video.transport.dataPort = 5002;
    video.transport.attributes["rtcpMux"] = "1";

    SdpCodecPreference h264;
    h264.codecType = CodecType::H264;
    h264.encodingName = "H264";
    h264.payloadType = 96;
    h264.attributes["clockRate"] = "90000";
    h264.fmtp = "packetization-mode=1;profile-level-id=42e01f";
    video.codecs.push_back(h264);

    SdpHeaderExtension extension;
    extension.id = 1;
    extension.uri = "urn:ietf:params:rtp-hdrext:sdes:mid";
    video.headerExtensions.push_back(extension);
    input.media.push_back(video);
    return input;
}

void testBuildAndParse() {
    const auto input = makeLocalInput();
    SdpNegotiator negotiator;
    const auto offer = negotiator.createOffer(input);
    require(offer.status.ok, offer.status.message.c_str());
    require(offer.media.size() == 2,
            "offer contains audio and video media");

    SdpBuilder builder;
    std::string text;
    const auto built = builder.build(offer.description, &text);
    require(built.ok, built.message.c_str());
    require(contains(text, "m=audio 5000 RTP/AVP 111"),
            "builder emits the audio media line");
    require(contains(text, "a=mid:audio"),
            "builder emits the audio mid");
    require(contains(text, "a=rtpmap:111 opus/48000/2"),
            "builder emits the Opus rtpmap");
    require(contains(text, "a=fmtp:96 packetization-mode=1"),
            "builder emits H.264 fmtp");
    require(contains(text, "a=rtcp-mux"),
            "builder emits RTCP mux");
    require(contains(text, "a=extmap:1 "),
            "builder emits header extensions");

    SdpSessionDescription parsed;
    SdpParser parser;
    const auto parseResult = parser.parse(text, &parsed);
    require(parseResult.ok, parseResult.message.c_str());
    require(parsed.origin.sessionId == 123456 &&
                parsed.connection.address == "127.0.0.1",
            "parser restores session identity and connection");
    require(parsed.media.size() == 2,
            "parser restores both media sections");
    require(parsed.media[0].kind == MediaKind::Audio &&
                parsed.media[0].mid == "audio" &&
                parsed.media[0].transport.dataPort == 5000,
            "parser restores the audio section");
    require(parsed.media[0].codecs.size() == 1 &&
                parsed.media[0].codecs[0].codecType == CodecType::Opus &&
                parsed.media[0].codecs[0].attributes["clockRate"] ==
                    "48000" &&
                parsed.media[0].codecs[0].attributes["channels"] == "2",
            "parser restores the Opus codec");
    require(parsed.media[1].kind == MediaKind::Video &&
                parsed.media[1].codecs[0].codecType == CodecType::H264 &&
                parsed.media[1].codecs[0].payloadType == 96 &&
                parsed.media[1].headerExtensions.size() == 1,
            "parser restores the video codec and extension");
}

void testNegotiation() {
    const auto input = makeLocalInput();
    SdpNegotiator negotiator;
    const auto offer = negotiator.createOffer(input);
    require(offer.status.ok, offer.status.message.c_str());

    SdpNegotiationInput answerInput;
    answerInput.origin.username = "answer";
    answerInput.origin.sessionId = 654321;
    answerInput.origin.unicastAddress = "192.0.2.10";
    answerInput.connection.address = "192.0.2.10";

    SdpMediaCapability audio;
    audio.kind = MediaKind::Audio;
    audio.mid = "audio";
    audio.direction = SdpDirection::SendOnly;
    audio.transport.dataPort = 6000;
    SdpCodecPreference opus;
    opus.codecType = CodecType::Opus;
    opus.encodingName = "opus";
    opus.payloadType = 109;
    opus.attributes["clockRate"] = "48000";
    opus.attributes["channels"] = "2";
    audio.codecs.push_back(opus);
    answerInput.media.push_back(audio);

    const auto answer =
        negotiator.createAnswer(answerInput, offer.description);
    require(answer.status.ok, answer.status.message.c_str());
    require(answer.media.size() == 2,
            "answer mirrors every remote media section");
    require(!answer.media[0].rejected &&
                answer.media[0].direction == SdpDirection::SendOnly &&
                answer.media[0].codec.payloadType == 111,
            "answer selects the remote payload and negotiated direction");
    require(answer.media[1].rejected &&
                answer.media[1].kind == MediaKind::Video,
            "answer rejects unsupported remote media");

    const auto applied =
        negotiator.applyRemoteAnswer(input, answer.description);
    require(applied.status.ok, applied.status.message.c_str());
    require(applied.media[0].direction == SdpDirection::RecvOnly,
            "answer direction is applied from the offerer perspective");
    require(applied.media[1].rejected,
            "remote answer rejects unsupported video");

    SdpBuilder builder;
    std::string text;
    require(builder.build(answer.description, &text).ok,
            "negotiated rejected media can be built");
    require(contains(text, "m=video 0 RTP/AVP\r\n"),
            "rejected media uses a zero port");

    SdpSessionDescription parsed;
    SdpParser parser;
    const auto parseResult = parser.parse(text, &parsed);
    require(parseResult.ok, parseResult.message.c_str());
    require(parsed.media.size() == 2 &&
                parsed.media[1].rejected,
            "rejected media can be parsed again");
}

void testValidation() {
    SdpSessionDescription invalid;
    SdpParser parser;
    require(!parser.validate(invalid).ok,
            "empty SDP description is rejected");

    auto description = SdpSessionDescription{};
    description.connection.address = "127.0.0.1";
    SdpMediaDescription audio;
    audio.kind = MediaKind::Audio;
    audio.transport.dataPort = 4000;
    audio.codecs.push_back(SdpCodecDescription{});
    description.media.push_back(audio);

    SdpBuilder builder;
    std::string text;
    require(!builder.build(description, &text).ok,
            "builder rejects an invalid payload type");

    SdpSessionDescription parsed;
    require(!parser.parse("s=missing-version\r\n", &parsed).ok,
            "parser rejects SDP without a version");
}

} // namespace

int main() {
    testBuildAndParse();
    testNegotiation();
    testValidation();
    std::cout << "SDP tests passed\n";
    return EXIT_SUCCESS;
}
