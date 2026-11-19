#include "iris/media.h"
#include "media/engine/media_interfaces.h"
#include "media/pipeline/capture_source.h"
#include "media/pipeline/encoder.h"

#include <memory>
#include <type_traits>
#include <utility>

static_assert(std::is_abstract_v<iris::media::IMediaTransport>);
static_assert(std::is_abstract_v<iris::media::IMediaBackend>);
static_assert(std::is_abstract_v<iris::media::ISdpController>);
static_assert(std::is_abstract_v<iris::media::IMediaEngine>);
static_assert(std::is_abstract_v<
              iris::media::pipeline::ICaptureSource>);
static_assert(std::is_abstract_v<
              iris::media::pipeline::IAudioCaptureSource>);
static_assert(std::is_abstract_v<
              iris::media::pipeline::IVideoCaptureSource>);
static_assert(std::is_abstract_v<
              iris::media::pipeline::IH264VideoEncoder>);

namespace {

class MockVideoSource final
    : public iris::media::pipeline::IVideoCaptureSource {
public:
    iris::media::MediaResult start() override {
        running_ = true;
        return iris::media::MediaResult::success();
    }

    void stop() noexcept override {
        running_ = false;
    }

    bool running() const noexcept override {
        return running_;
    }

    void setFrameSink(FrameSink sink) override {
        sink_ = std::move(sink);
    }

    void setMuted(bool muted) override {
        muted_ = muted;
    }

    void emit(const iris::media::pipeline::VideoFrameView& frame) {
        if (sink_) {
            sink_(frame);
        }
    }

private:
    FrameSink sink_;
    bool running_ = false;
    bool muted_ = false;
};

// Processing sources use the same lifecycle contract as physical capture
// sources and own an upstream source internally.
class PassthroughVideoSource final
    : public iris::media::pipeline::IVideoCaptureSource {
public:
    explicit PassthroughVideoSource(
        std::unique_ptr<IVideoCaptureSource> upstream)
        : upstream_(std::move(upstream)) {
    }

    iris::media::MediaResult start() override {
        upstream_->setFrameSink(
            [this](const iris::media::pipeline::VideoFrameView& frame) {
                if (sink_) {
                    sink_(frame);
                }
            });
        return upstream_->start();
    }

    void stop() noexcept override {
        upstream_->stop();
    }

    bool running() const noexcept override {
        return upstream_->running();
    }

    void setFrameSink(FrameSink sink) override {
        sink_ = std::move(sink);
    }

    void setMuted(bool muted) override {
        upstream_->setMuted(muted);
    }

private:
    std::unique_ptr<IVideoCaptureSource> upstream_;
    FrameSink sink_;
};

} // namespace

int main() {
    iris::MediaSessionConfig config;
    config.audioEnabled = true;
    config.videoEnabled = true;
    config.audioDirection = iris::MediaDirection::SendRecv;
    config.videoDirection = iris::MediaDirection::SendRecv;

    auto upstream = std::make_unique<MockVideoSource>();
    auto* mock = upstream.get();
    PassthroughVideoSource source(std::move(upstream));
    int frames = 0;
    source.setFrameSink(
        [&frames](const iris::media::pipeline::VideoFrameView&) {
            ++frames;
        });
    if (!source.start().ok) {
        return 1;
    }
    mock->emit({});
    source.stop();
    if (frames != 1 || source.running()) {
        return 1;
    }
    return 0;
}

