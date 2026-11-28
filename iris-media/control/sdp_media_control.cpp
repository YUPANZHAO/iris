#include "control/sdp_media_control.h"

#include "capture_source/empty_capture_source.h"
#include "codec/ffmpeg_codec_factory.h"
#include "pipeline/capture_stream.h"
#include "pipeline/rtp_media_stream.h"
#include "transport/rtp_engine.h"

#include <algorithm>
#include <utility>
#include <vector>

namespace iris::media::control {
namespace {

bool sends(sdp::SdpDirection direction) noexcept {
    return direction == sdp::SdpDirection::SendOnly ||
           direction == sdp::SdpDirection::SendRecv;
}

const char* directionText(sdp::SdpDirection direction) noexcept {
    switch (direction) {
    case sdp::SdpDirection::Inactive:
        return "inactive";
    case sdp::SdpDirection::SendOnly:
        return "sendonly";
    case sdp::SdpDirection::RecvOnly:
        return "recvonly";
    case sdp::SdpDirection::SendRecv:
        return "sendrecv";
    }
    return "inactive";
}

std::string codecName(codec::CodecType type) {
    switch (type) {
    case codec::CodecType::Opus:
        return "opus";
    case codec::CodecType::H264:
        return "h264";
    case codec::CodecType::Unknown:
        return {};
    }
    return {};
}

sdp::SdpNegotiationInput toNegotiationInput(
    const sdp::SdpSessionDescription& description) {
    sdp::SdpNegotiationInput result;
    result.origin = description.origin;
    result.connection = description.connection;
    result.sessionName = description.sessionName;
    result.startTime = description.startTime;
    result.stopTime = description.stopTime;
    result.attributes = description.attributes;
    for (const auto& media : description.media) {
        sdp::SdpMediaCapability capability;
        capability.kind = media.kind;
        capability.mid = media.mid;
        capability.direction = media.direction;
        capability.transport = media.transport;
        capability.headerExtensions = media.headerExtensions;
        capability.attributes = media.attributes;
        for (const auto& codec : media.codecs) {
            sdp::SdpCodecPreference preference;
            preference.codecType = codec.codecType;
            preference.encodingName = codec.encodingName;
            preference.payloadType = codec.payloadType;
            preference.fmtp = codec.fmtp;
            preference.attributes = codec.attributes;
            capability.codecs.push_back(std::move(preference));
        }
        result.media.push_back(std::move(capability));
    }
    return result;
}

const sdp::SdpMediaCapability* findLocalCapability(
    const sdp::SdpNegotiationInput& local,
    const sdp::SdpNegotiatedMedia& media) {
    if (!media.mid.empty()) {
        const auto byMid = std::find_if(
            local.media.begin(),
            local.media.end(),
            [&](const sdp::SdpMediaCapability& candidate) {
                return candidate.mid == media.mid;
            });
        if (byMid != local.media.end()) {
            return &*byMid;
        }
    }
    const auto byKind = std::find_if(
        local.media.begin(),
        local.media.end(),
        [&](const sdp::SdpMediaCapability& candidate) {
            return candidate.kind == media.kind;
        });
    return byKind == local.media.end() ? nullptr : &*byKind;
}

int integer(const std::string& text, int fallback) {
    try {
        return std::stoi(text);
    } catch (...) {
        return fallback;
    }
}

int integer(
    const AttributeMap& attributes,
    const std::string& name,
    int fallback) {
    const auto value = attributes.find(name);
    return value == attributes.end()
        ? fallback
        : integer(value->second, fallback);
}

const sdp::SdpMediaDescription* findMedia(
    const sdp::SdpSessionDescription& description,
    const sdp::SdpNegotiatedMedia& media) {
    const auto byMid = std::find_if(
        description.media.begin(),
        description.media.end(),
        [&](const sdp::SdpMediaDescription& candidate) {
            return !media.mid.empty() &&
                   candidate.mid == media.mid;
        });
    if (byMid != description.media.end()) {
        return &*byMid;
    }
    const auto byKind = std::find_if(
        description.media.begin(),
        description.media.end(),
        [&](const sdp::SdpMediaDescription& candidate) {
            return candidate.kind == media.kind;
        });
    return byKind == description.media.end() ? nullptr : &*byKind;
}

std::string connectionAddress(
    const sdp::SdpSessionDescription& description,
    const sdp::SdpMediaDescription& media) {
    const auto address =
        media.transport.attributes.find("connectionAddress");
    return address != media.transport.attributes.end() &&
                   !address->second.empty()
        ? address->second
        : description.connection.address;
}

AttributeMap rtpOptions(
    const sdp::SdpSessionDescription& local,
    const sdp::SdpSessionDescription* remote,
    const sdp::SdpNegotiatedMedia& media,
    StreamId id) {
    AttributeMap options;
    const auto* localMedia = findMedia(local, media);
    if (!localMedia) {
        return options;
    }
    options["localAddress"] =
        connectionAddress(local, *localMedia);
    options["localPort"] =
        std::to_string(localMedia->transport.dataPort);
    options["streamId"] = std::to_string(id);
    options["payloadType"] =
        std::to_string(media.codec.payloadType);
    const auto clock = media.codec.attributes.find("clockRate");
    options["clockRate"] = clock == media.codec.attributes.end()
        ? (media.kind == MediaKind::Video ? "90000" : "48000")
        : clock->second;

    if (remote) {
        const auto* remoteMedia = findMedia(*remote, media);
        if (remoteMedia && !remoteMedia->rejected) {
            options["remoteAddress"] =
                connectionAddress(*remote, *remoteMedia);
            options["remotePort"] =
                std::to_string(
                    remoteMedia->transport.dataPort);
        }
    }
    return options;
}

int clockRate(
    const sdp::SdpNegotiatedMedia& media,
    int fallback) {
    const auto value =
        media.codec.attributes.find("clockRate");
    if (value == media.codec.attributes.end()) {
        return fallback;
    }
    return integer(value->second, fallback);
}

Status makeAudioCapture(
    const sdp::SdpNegotiatedMedia& media,
    const sdp::SdpMediaCapability& capability,
    const std::string& text,
    StreamPtr* output) {
    if (!output) {
        return Status::failure("audio stream output is null");
    }
    codec::AudioFormat format;
    format.sampleRate = std::max(
        1,
        integer(
            capability.attributes,
            "sampleRate",
            clockRate(media, 48000)));
    format.channels = std::max(
        1,
        integer(capability.attributes, "channels", 2));
    format.sampleFormat = codec::AudioSampleFormat::Signed16;

    auto capture =
        std::make_shared<capture::EmptyAudioCaptureSource>(text);
    capture::AudioCaptureConfig captureConfig;
    captureConfig.outputFormat = format;
    auto status = capture->configure(captureConfig);
    if (!status.ok) {
        return status;
    }

    codec::FFmpegCodecFactory factory;
    auto encoder = factory.createAudioEncoder(media.codec.codecType);
    if (!encoder) {
        return Status::failure(
            "negotiated audio codec has no encoder");
    }
    codec::AudioEncoderConfig encoderConfig;
    encoderConfig.codecType = media.codec.codecType;
    encoderConfig.inputFormat = format;
    encoderConfig.bitrateKbps = integer(
        capability.attributes, "bitrateKbps", 64);
    status = encoder->configure(encoderConfig);
    if (!status.ok) {
        return status;
    }

    auto stream =
        std::make_shared<pipeline::CaptureAudioStream>();
    status = stream->setCapture(std::move(capture));
    if (status.ok) {
        status = stream->setEncoder(std::move(encoder));
    }
    if (!status.ok) {
        return status;
    }
    *output = std::move(stream);
    return {};
}

Status makeVideoCapture(
    const sdp::SdpNegotiatedMedia& media,
    const sdp::SdpMediaCapability& capability,
    const std::string& text,
    StreamPtr* output) {
    if (!output) {
        return Status::failure("video stream output is null");
    }
    codec::VideoFormat format;
    format.width = std::max(
        1, integer(capability.attributes, "width", 640));
    format.height = std::max(
        1, integer(capability.attributes, "height", 360));
    const auto resolution =
        capability.attributes.find("resolution");
    if (resolution != capability.attributes.end()) {
        const auto separator = resolution->second.find('x');
        if (separator != std::string::npos) {
            format.width = std::max(
                1, integer(
                       resolution->second.substr(0, separator),
                       format.width));
            format.height = std::max(
                1, integer(
                       resolution->second.substr(separator + 1),
                       format.height));
        }
    }
    format.frameRate = codec::Rational{
        std::max(
            1,
            integer(
                capability.attributes,
                "frameRateNumerator",
                15)),
        std::max(
            1,
            integer(
                capability.attributes,
                "frameRateDenominator",
                1)),
    };
    format.pixelFormat = codec::VideoPixelFormat::I420;

    auto capture =
        std::make_shared<capture::EmptyVideoCaptureSource>(text);
    capture::VideoCaptureConfig captureConfig;
    captureConfig.outputFormat = format;
    auto status = capture->configure(captureConfig);
    if (!status.ok) {
        return status;
    }

    codec::FFmpegCodecFactory factory;
    auto encoder = factory.createVideoEncoder(media.codec.codecType);
    if (!encoder) {
        return Status::failure(
            "negotiated video codec has no encoder");
    }
    codec::VideoEncoderConfig encoderConfig;
    encoderConfig.codecType = media.codec.codecType;
    encoderConfig.inputFormat = capture->outputFormat();
    encoderConfig.bitrateKbps = integer(
        capability.attributes, "bitrateKbps", 800);
    status = encoder->configure(encoderConfig);
    if (!status.ok) {
        return status;
    }

    auto stream =
        std::make_shared<pipeline::CaptureVideoStream>();
    status = stream->setCapture(std::move(capture));
    if (status.ok) {
        status = stream->setEncoder(std::move(encoder));
    }
    if (!status.ok) {
        return status;
    }
    *output = std::move(stream);
    return {};
}

class RtpPacketSender final : public codec::IEncodedPacketSink {
public:
    RtpPacketSender(
        std::shared_ptr<transport::RTPEngine> engine,
        StreamId streamId,
        MediaKind kind,
        codec::CodecType codecType)
        : engine_(std::move(engine)),
          streamId_(streamId),
          kind_(kind),
          codecType_(codecType) {
    }

    void onEncodedPacket(
        const codec::EncodedPacket& packet) override {
        auto output = packet;
        output.streamId = streamId_;
        output.kind = kind_;
        output.codecType = codecType_;
        if (kind_ == MediaKind::Video &&
            codecType_ == codec::CodecType::H264) {
            sendH264(output);
            return;
        }
        (void)engine_->send(
            transport::RtpParams{kind_ == MediaKind::Video},
            output);
    }

private:
    static bool startCode(
        const std::uint8_t* data,
        std::size_t size,
        std::size_t offset,
        std::size_t* length) {
        if (!data || !length || offset + 3 > size) {
            return false;
        }
        if (offset + 4 <= size &&
            data[offset] == 0 && data[offset + 1] == 0 &&
            data[offset + 2] == 0 && data[offset + 3] == 1) {
            *length = 4;
            return true;
        }
        if (data[offset] == 0 && data[offset + 1] == 0 &&
            data[offset + 2] == 1) {
            *length = 3;
            return true;
        }
        return false;
    }

    void sendPayload(
        const std::uint8_t* data,
        std::size_t size,
        std::int64_t ptsMicros,
        bool marker) const {
        auto payload = std::make_shared<ByteBuffer>(data, size);
        if (!payload || !*payload) {
            return;
        }
        codec::EncodedPacket value;
        value.streamId = streamId_;
        value.kind = MediaKind::Video;
        value.codecType = codec::CodecType::H264;
        value.data = std::move(payload);
        value.ptsMicros = ptsMicros;
        (void)engine_->send(
            transport::RtpParams{marker}, value);
    }

    void sendH264(const codec::EncodedPacket& packet) const {
        if (!packet.data || !packet.data->data() ||
            packet.data->size() == 0) {
            return;
        }
        constexpr std::size_t maxNalUnit = 1188;
        constexpr std::size_t maxFuPayload = maxNalUnit - 2;
        const auto* bytes = static_cast<const std::uint8_t*>(
            packet.data->data());
        const auto size = packet.data->size();

        std::vector<std::pair<const std::uint8_t*, std::size_t>> nals;
        std::size_t current = std::string::npos;
        for (std::size_t offset = 0; offset < size;) {
            std::size_t prefix = 0;
            if (startCode(bytes, size, offset, &prefix)) {
                if (current != std::string::npos &&
                    offset > current) {
                    nals.push_back({bytes + current, offset - current});
                }
                offset += prefix;
                current = offset;
            } else {
                ++offset;
            }
        }
        if (current != std::string::npos && current < size) {
            nals.push_back({bytes + current, size - current});
        }
        if (nals.empty()) {
            nals.push_back({bytes, size});
        }
        const bool hasVcl = std::any_of(
            nals.begin(),
            nals.end(),
            [](const auto& nal) {
                if (!nal.first || nal.second == 0) {
                    return false;
                }
                const auto type = nal.first[0] & 0x1f;
                return type >= 1 && type <= 5;
            });

        for (std::size_t nalIndex = 0;
             nalIndex < nals.size();
             ++nalIndex) {
            const auto* nal = nals[nalIndex].first;
            const auto nalSize = nals[nalIndex].second;
            const bool lastNal = nalIndex + 1 == nals.size();
            if (nalSize <= maxNalUnit) {
                sendPayload(
                    nal,
                    nalSize,
                    packet.ptsMicros,
                    lastNal && hasVcl);
                continue;
            }
            const auto fragmentCount =
                (nalSize - 1 + maxFuPayload - 1) /
                maxFuPayload;
            std::size_t offset = 1;
            for (std::size_t index = 0;
                 index < fragmentCount;
                 ++index) {
                const auto chunkSize =
                    std::min(maxFuPayload, nalSize - offset);
                std::vector<std::uint8_t> payload(2 + chunkSize);
                payload[0] = static_cast<std::uint8_t>(
                    (nal[0] & 0xe0) | 28);
                payload[1] = static_cast<std::uint8_t>(
                    (index == 0 ? 0x80 : 0x00) |
                    (index + 1 == fragmentCount ? 0x40 : 0x00) |
                    (nal[0] & 0x1f));
                std::copy(
                    nal + offset,
                    nal + offset + chunkSize,
                    payload.begin() + 2);
                sendPayload(
                    payload.data(),
                    payload.size(),
                    packet.ptsMicros,
                    lastNal && hasVcl &&
                        index + 1 == fragmentCount);
                offset += chunkSize;
            }
        }
    }

    std::shared_ptr<transport::RTPEngine> engine_;
    StreamId streamId_ = 0;
    MediaKind kind_ = MediaKind::Unknown;
    codec::CodecType codecType_ = codec::CodecType::Unknown;
};

} // namespace

SdpMediaControl::SdpMediaControl(sdp::SdpNegotiationInput local)
    : local_(std::move(local)) {
}

SdpResult SdpMediaControl::createOffer() {
    SdpResult output;
    hasLocalDescription_ = false;
    hasRemoteDescription_ = false;
    localDescription_ = {};
    remoteDescription_ = {};

    const auto result = negotiator_.createOffer(local_);
    if (!result.status.ok) {
        output.status = result.status;
        return output;
    }
    output.status = builder_.build(result.description, &output.sdp);
    return output;
}

SdpResult SdpMediaControl::createAnswer() {
    SdpResult output;
    if (!hasRemoteDescription_) {
        output.status = Status::failure(
            "setRemoteDescription must be called before createAnswer");
        return output;
    }

    auto result = negotiator_.createAnswer(local_, remoteDescription_);
    if (!result.status.ok) {
        output.status = result.status;
        return output;
    }
    output.status = builder_.build(result.description, &output.sdp);
    if (!output.status.ok) {
        return output;
    }
    output.status = buildStreams(result, &remoteDescription_);
    return output;
}

Status SdpMediaControl::setLocalDescription(const std::string& sdp) {
    sdp::SdpSessionDescription description;
    auto status = parser_.parse(sdp, &description);
    if (!status.ok) {
        return status;
    }
    localDescription_ = std::move(description);
    hasLocalDescription_ = true;
    return {};
}

Status SdpMediaControl::setRemoteDescription(const std::string& sdp) {
    sdp::SdpSessionDescription description;
    auto status = parser_.parse(sdp, &description);
    if (!status.ok) {
        return status;
    }

    if (hasLocalDescription_) {
        const auto local = toNegotiationInput(localDescription_);
        auto result = negotiator_.applyRemoteAnswer(local, description);
        if (!result.status.ok) {
            return result.status;
        }
        status = buildStreams(result, &description);
        if (!status.ok) {
            return status;
        }
    }

    remoteDescription_ = std::move(description);
    hasRemoteDescription_ = true;
    return {};
}

sdp::SdpSessionDescription SdpMediaControl::localDescription() const {
    return localDescription_;
}

sdp::SdpSessionDescription SdpMediaControl::remoteDescription() const {
    return remoteDescription_;
}

Status SdpMediaControl::buildStreams(
    const sdp::SdpNegotiationResult& result,
    const sdp::SdpSessionDescription* remote) {
    std::vector<MediaStream> streams;
    streams.reserve(result.media.size());
    for (const auto& media : result.media) {
        if (media.rejected ||
            media.direction == sdp::SdpDirection::Inactive) {
            continue;
        }
        MediaStream mediaStream;
        mediaStream.id = static_cast<StreamId>(streams.size() + 1);
        const auto status = makeStream(
            media,
            result.description,
            remote,
            mediaStream.id,
            &mediaStream);
        if (!status.ok) {
            return status;
        }
        streams.push_back(std::move(mediaStream));
    }
    if (streams.empty()) {
        return Status::failure(
            "negotiation produced no active media stream");
    }
    return replaceStreams(std::move(streams));
}

Status SdpMediaControl::makeStream(
    const sdp::SdpNegotiatedMedia& media,
    const sdp::SdpSessionDescription& local,
    const sdp::SdpSessionDescription* remote,
    StreamId id,
    MediaStream* mediaStream) const {
    if (!mediaStream) {
        return Status::failure("media stream output is null");
    }
    const auto* capability = findLocalCapability(local_, media);
    if (!capability) {
        return Status::failure(
            "negotiated media has no local capability");
    }

    MediaStream result;
    result.mid = media.mid;
    result.kind = media.kind;
    result.attributes = capability->attributes;
    result.attributes["mid"] = media.mid;
    result.attributes["direction"] = directionText(media.direction);
    const auto codecNameValue = codecName(media.codec.codecType);
    if (!codecNameValue.empty()) {
        result.attributes["codecType"] = codecNameValue;
    }
    if (!media.codec.encodingName.empty()) {
        result.attributes["encodingName"] =
            media.codec.encodingName;
    }
    if (!media.codec.fmtp.empty()) {
        result.attributes["fmtp"] = media.codec.fmtp;
    }
    result.attributes["payloadType"] =
        std::to_string(media.codec.payloadType);
    const auto clockRate =
        media.codec.attributes.find("clockRate");
    if (clockRate != media.codec.attributes.end()) {
        result.attributes["clockRate"] = clockRate->second;
    }
    const auto channels =
        media.codec.attributes.find("channels");
    if (channels != media.codec.attributes.end()) {
        result.attributes["channels"] = channels->second;
    }

    const bool transmit = sends(media.direction);
    const bool receive =
        media.direction == sdp::SdpDirection::RecvOnly ||
        media.direction == sdp::SdpDirection::SendRecv;
    if (!transmit && !receive) {
        return Status::failure("negotiated media direction is inactive");
    }
    if (media.kind != MediaKind::Audio &&
        media.kind != MediaKind::Video) {
        return Status::failure(
            "negotiated media kind is unsupported");
    }
    if (!remote) {
        return Status::failure(
            "remote SDP is required to create a media stream");
    }

    auto engine = std::make_shared<transport::RTPEngine>();
    result.rtpEngine = engine;
    result.rtpAttachOptions =
        rtpOptions(local, remote, media, id);
    const auto text = capability->attributes.find("displayText");
    const std::string displayText =
        text != capability->attributes.end()
            ? text->second
            : (local_.sessionName.empty()
                   ? "Iris"
                   : local_.sessionName);

    if (transmit) {
        StreamPtr captureStream;
        const auto status = media.kind == MediaKind::Audio
            ? makeAudioCapture(
                  media, *capability, displayText, &captureStream)
            : makeVideoCapture(
                  media, *capability, displayText, &captureStream);
        if (!status.ok) {
            return status;
        }
        result.stream = std::move(captureStream);
        result.encodedPacketSink =
            std::make_shared<RtpPacketSender>(
                engine, id, media.kind, media.codec.codecType);
    }

    if (receive) {
        auto receiveStream =
            std::make_shared<pipeline::RtpMediaStream>(media.kind);
        const auto status =
            receiveStream->setRTPTransport(engine);
        if (!status.ok) {
            return status;
        }
        result.receiveStream = receiveStream;
        if (!result.stream) {
            result.stream = receiveStream;
        }
    }

    if (!result.stream) {
        return Status::failure(
            "negotiated media kind is unsupported");
    }
    *mediaStream = std::move(result);
    return {};
}

} // namespace iris::media::control
