#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace iris::media {

class NativeVideoRenderer {
public:
    NativeVideoRenderer();
    ~NativeVideoRenderer();

    NativeVideoRenderer(const NativeVideoRenderer&) = delete;
    NativeVideoRenderer& operator=(const NativeVideoRenderer&) = delete;

    bool start(const std::string& title);
    void display(std::vector<std::uint8_t> bgra, int width, int height);
    void stop() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class NativeAudioRenderer {
public:
    NativeAudioRenderer();
    ~NativeAudioRenderer();

    NativeAudioRenderer(const NativeAudioRenderer&) = delete;
    NativeAudioRenderer& operator=(const NativeAudioRenderer&) = delete;

    bool start(int sampleRate, int channels);
    void submit(std::vector<std::int16_t> samples);
    void stop() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace iris::media

