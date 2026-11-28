#include "renderer.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmsystem.h>
#endif

struct MediaRenderer::Impl {
    explicit Impl(bool initial)
        : shouldStart(initial) {
    }

    ~Impl() {
        stop();
    }

    void start();
    void stop();

#ifdef _WIN32
    struct AudioSlot {
        std::vector<std::uint8_t> bytes;
        WAVEHDR header{};
        bool prepared = false;
    };

    static constexpr UINT kVideoMessage = WM_APP + 1;

    static LRESULT CALLBACK windowProc(
        HWND window,
        UINT message,
        WPARAM wparam,
        LPARAM lparam);

    void windowLoop();
    void paint(HDC device);
    void openAudio(const iris::AudioFrameView& frame);
    void closeAudio();

    std::thread windowThread;
    std::atomic<DWORD> windowThreadId{0};
    std::atomic<HWND> window{nullptr};
    std::atomic<bool> windowRunning{false};
    std::mutex frameMutex;
    std::vector<std::uint8_t> latestFrame;
    int frameWidth = 0;
    int frameHeight = 0;

    std::mutex audioMutex;
    HWAVEOUT audio = nullptr;
    iris::AudioFormat audioFormat;
    std::vector<std::unique_ptr<AudioSlot>> audioSlots;
#endif

    std::atomic<bool> enabled{false};
    bool shouldStart = false;
};

#ifdef _WIN32
LRESULT CALLBACK MediaRenderer::Impl::windowProc(
    HWND window,
    UINT message,
    WPARAM wparam,
    LPARAM lparam) {
    auto* self = reinterpret_cast<Impl*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create =
            reinterpret_cast<const CREATESTRUCTW*>(lparam);
        self = static_cast<Impl*>(create->lpCreateParams);
        SetWindowLongPtrW(
            window, GWLP_USERDATA,
            reinterpret_cast<LONG_PTR>(self));
    }
    if (!self) {
        return DefWindowProcW(window, message, wparam, lparam);
    }
    if (message == kVideoMessage) {
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    }
    if (message == WM_PAINT) {
        PAINTSTRUCT paintState{};
        const auto device = BeginPaint(window, &paintState);
        self->paint(device);
        EndPaint(window, &paintState);
        return 0;
    }
    if (message == WM_ERASEBKGND) {
        return 1;
    }
    if (message == WM_CLOSE) {
        DestroyWindow(window);
        return 0;
    }
    if (message == WM_DESTROY) {
        self->window.store(nullptr);
        self->windowRunning.store(false);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

void MediaRenderer::Impl::windowLoop() {
    constexpr wchar_t className[] = L"IrisCliMediaWindow";
    const auto instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = &Impl::windowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor =
        LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    windowClass.hbrBackground =
        reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    windowClass.lpszClassName = className;
    RegisterClassExW(&windowClass);

    windowThreadId.store(GetCurrentThreadId());
    const auto handle = CreateWindowExW(
        0,
        className,
        L"Iris CLI Media",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        960,
        540,
        nullptr,
        nullptr,
        instance,
        this);
    if (!handle) {
        enabled.store(false);
        windowRunning.store(false);
        return;
    }
    window.store(handle);
    windowRunning.store(true);
    ShowWindow(handle, SW_SHOW);
    UpdateWindow(handle);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    windowRunning.store(false);
    enabled.store(false);
    windowThreadId.store(0);
}

void MediaRenderer::Impl::paint(HDC device) {
    RECT client{};
    GetClientRect(window.load(), &client);
    const auto targetWidth = client.right - client.left;
    const auto targetHeight = client.bottom - client.top;
    FillRect(
        device,
        &client,
        reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));

    std::lock_guard<std::mutex> lock(frameMutex);
    if (latestFrame.empty() || frameWidth <= 0 || frameHeight <= 0 ||
        targetWidth <= 0 || targetHeight <= 0) {
        return;
    }
    const auto scale = std::min(
        static_cast<double>(targetWidth) / frameWidth,
        static_cast<double>(targetHeight) / frameHeight);
    const auto width =
        static_cast<int>(frameWidth * scale);
    const auto height =
        static_cast<int>(frameHeight * scale);
    const auto x = (targetWidth - width) / 2;
    const auto y = (targetHeight - height) / 2;

    BITMAPINFO bitmap{};
    bitmap.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmap.bmiHeader.biWidth = frameWidth;
    bitmap.bmiHeader.biHeight = -frameHeight;
    bitmap.bmiHeader.biPlanes = 1;
    bitmap.bmiHeader.biBitCount = 32;
    bitmap.bmiHeader.biCompression = BI_RGB;
    StretchDIBits(
        device,
        x,
        y,
        width,
        height,
        0,
        0,
        frameWidth,
        frameHeight,
        latestFrame.data(),
        &bitmap,
        DIB_RGB_COLORS,
        SRCCOPY);
}

void MediaRenderer::Impl::openAudio(
    const iris::AudioFrameView& frame) {
    if (frame.format.sampleFormat !=
            iris::AudioSampleFormat::Signed16 ||
        frame.format.sampleRate <= 0 || frame.format.channels <= 0) {
        return;
    }
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels =
        static_cast<WORD>(frame.format.channels);
    format.nSamplesPerSec =
        static_cast<DWORD>(frame.format.sampleRate);
    format.wBitsPerSample = 16;
    format.nBlockAlign =
        static_cast<WORD>(
            format.nChannels * format.wBitsPerSample / 8);
    format.nAvgBytesPerSec =
        format.nSamplesPerSec * format.nBlockAlign;
    if (waveOutOpen(
            &audio, WAVE_MAPPER, &format,
            reinterpret_cast<DWORD_PTR>(nullptr), 0,
            CALLBACK_NULL) != MMSYSERR_NOERROR) {
        audio = nullptr;
        return;
    }
    audioFormat = frame.format;
    audioSlots.clear();
    for (int index = 0; index < 8; ++index) {
        audioSlots.push_back(std::make_unique<AudioSlot>());
    }
}

void MediaRenderer::Impl::closeAudio() {
    if (!audio) {
        return;
    }
    waveOutReset(audio);
    for (auto& slot : audioSlots) {
        if (slot->prepared) {
            waveOutUnprepareHeader(audio, &slot->header, sizeof(WAVEHDR));
            slot->prepared = false;
        }
    }
    audioSlots.clear();
    waveOutClose(audio);
    audio = nullptr;
}
#endif

void MediaRenderer::Impl::start() {
    if (enabled.exchange(true)) {
        return;
    }
#ifdef _WIN32
    if (windowThread.joinable()) {
        if (const auto handle = window.exchange(nullptr)) {
            PostMessageW(handle, WM_CLOSE, 0, 0);
        } else if (const auto threadId = windowThreadId.load()) {
            PostThreadMessageW(threadId, WM_QUIT, 0, 0);
        }
        windowThread.join();
    }
    enabled.store(true);
    windowThread = std::thread([this] {
        windowLoop();
    });
    while (windowThread.joinable() &&
           windowThreadId.load() == 0) {
        std::this_thread::yield();
    }
#else
    std::cerr << "render is currently implemented on Windows only\n";
    enabled.store(false);
#endif
}

void MediaRenderer::Impl::stop() {
    enabled.store(false);
#ifdef _WIN32
    {
        std::lock_guard<std::mutex> lock(audioMutex);
        closeAudio();
    }
    if (windowThread.joinable()) {
        if (const auto handle = window.exchange(nullptr)) {
            PostMessageW(handle, WM_CLOSE, 0, 0);
        } else if (const auto threadId = windowThreadId.load()) {
            PostThreadMessageW(threadId, WM_QUIT, 0, 0);
        }
        windowThread.join();
    }
#endif
}

MediaRenderer::MediaRenderer(bool initial)
    : impl_(std::make_unique<Impl>(initial)) {
    if (initial) {
        start();
    }
}

MediaRenderer::~MediaRenderer() {
    stop();
}

void MediaRenderer::start() {
    impl_->start();
}

void MediaRenderer::stop() {
    impl_->stop();
}

bool MediaRenderer::enabled() const noexcept {
    return impl_->enabled.load();
}

void MediaRenderer::set_enabled(bool enabled) {
    if (enabled) {
        start();
    } else {
        stop();
    }
}

void MediaRenderer::onAudioFrame(const iris::AudioFrameView& frame) {
#ifdef _WIN32
    if (!impl_->enabled.load() || !frame.data || frame.size == 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(impl_->audioMutex);
    if (!impl_->audio) {
        impl_->openAudio(frame);
    }
    if (!impl_->audio) {
        return;
    }
    for (auto& slot : impl_->audioSlots) {
        if (slot->prepared &&
            (slot->header.dwFlags & WHDR_DONE) != 0) {
            waveOutUnprepareHeader(
                impl_->audio, &slot->header, sizeof(WAVEHDR));
            slot->prepared = false;
        }
    }
    for (auto& slot : impl_->audioSlots) {
        if (slot->prepared) {
            continue;
        }
        slot->bytes.assign(frame.data, frame.data + frame.size);
        slot->header = {};
        slot->header.lpData =
            reinterpret_cast<LPSTR>(slot->bytes.data());
        slot->header.dwBufferLength =
            static_cast<DWORD>(slot->bytes.size());
        if (waveOutPrepareHeader(
                impl_->audio, &slot->header, sizeof(WAVEHDR)) !=
            MMSYSERR_NOERROR) {
            return;
        }
        slot->prepared = true;
        if (waveOutWrite(
                impl_->audio, &slot->header, sizeof(WAVEHDR)) !=
            MMSYSERR_NOERROR) {
            waveOutUnprepareHeader(
                impl_->audio, &slot->header, sizeof(WAVEHDR));
            slot->prepared = false;
        }
        return;
    }
#else
    (void)frame;
#endif
}

void MediaRenderer::onVideoFrame(const iris::VideoFrameView& frame) {
#ifdef _WIN32
    if (!impl_->enabled.load() || frame.kind != iris::VideoFrameKind::Remote ||
        !frame.data || frame.width <= 0 || frame.height <= 0 ||
        frame.stride < frame.width * 4) {
        return;
    }
    std::vector<std::uint8_t> pixels(
        static_cast<std::size_t>(frame.width) * frame.height * 4);
    for (int row = 0; row < frame.height; ++row) {
        std::memcpy(
            pixels.data() + static_cast<std::size_t>(row) *
                frame.width * 4,
            frame.data + static_cast<std::size_t>(row) * frame.stride,
            static_cast<std::size_t>(frame.width) * 4);
    }
    {
        std::lock_guard<std::mutex> lock(impl_->frameMutex);
        impl_->latestFrame = std::move(pixels);
        impl_->frameWidth = frame.width;
        impl_->frameHeight = frame.height;
    }
    if (const auto window = impl_->window.load()) {
        PostMessageW(window, Impl::kVideoMessage, 0, 0);
    }
#else
    (void)frame;
#endif
}
