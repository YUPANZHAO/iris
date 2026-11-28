#pragma once

#include "iris/media.h"

#include <memory>

class MediaRenderer {
public:
    explicit MediaRenderer(bool enabled = false);
    ~MediaRenderer();

    MediaRenderer(const MediaRenderer&) = delete;
    MediaRenderer& operator=(const MediaRenderer&) = delete;

    void start();
    void stop();
    bool enabled() const noexcept;
    void set_enabled(bool enabled);

    void onAudioFrame(const iris::AudioFrameView& frame);
    void onVideoFrame(const iris::VideoFrameView& frame);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
