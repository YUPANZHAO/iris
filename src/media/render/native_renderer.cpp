#include "media/render/native_renderer.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#endif

namespace iris::media {

#ifdef _WIN32
namespace {
constexpr UINT kFrameMessage = WM_APP + 41;
constexpr wchar_t kWindowClass[] = L"IrisVideoRendererWindow";
} // namespace

struct NativeVideoRenderer::Impl {
    static LRESULT CALLBACK window_proc(HWND window,
                                        UINT message,
                                        WPARAM wparam,
                                        LPARAM lparam) {
        auto* renderer = reinterpret_cast<Impl*>(
            GetWindowLongPtrW(window, GWLP_USERDATA));
        if (renderer) {
            return renderer->handle_message(window, message, wparam, lparam);
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }

    bool start(const std::string& requestedTitle) {
        stop();
        std::wstring title(requestedTitle.begin(), requestedTitle.end());
        ready = false;
        thread = std::thread([this, title] {
            WNDCLASSW window_class{};
            window_class.lpfnWndProc = &Impl::window_proc;
            window_class.hInstance = GetModuleHandleW(nullptr);
            window_class.hCursor =
                LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
            window_class.lpszClassName = kWindowClass;
            window_class.style = CS_HREDRAW | CS_VREDRAW;
            RegisterClassW(&window_class);
            thread_id = GetCurrentThreadId();

            window = CreateWindowExW(
                0, kWindowClass, title.c_str(),
                WS_OVERLAPPEDWINDOW | WS_VISIBLE | WS_CLIPCHILDREN,
                CW_USEDEFAULT, CW_USEDEFAULT, 800, 480,
                nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
            if (!window) {
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    ready = true;
                }
                condition.notify_all();
                return;
            }
            SetWindowLongPtrW(
                window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
            ShowWindow(window, SW_SHOW);
            UpdateWindow(window);
            {
                std::lock_guard<std::mutex> lock(mutex);
                ready = true;
            }
            condition.notify_all();

            MSG message{};
            while (GetMessageW(&message, nullptr, 0, 0) > 0) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            window = nullptr;
        });

        std::unique_lock<std::mutex> lock(mutex);
        condition.wait(lock, [this] { return ready; });
        return window != nullptr;
    }

    void display(std::vector<std::uint8_t> next_bgra,
                 int next_width,
                 int next_height) {
        HWND target_window = nullptr;
        bool post_frame = false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            bgra = std::move(next_bgra);
            width = next_width;
            height = next_height;
            dirty = true;
            target = window;
            target_window = target;
            if (target_window && !frame_message_pending) {
                frame_message_pending = true;
                post_frame = true;
            }
        }
        if (post_frame &&
            !PostMessageW(target_window, kFrameMessage, 0, 0)) {
            std::lock_guard<std::mutex> lock(mutex);
            frame_message_pending = false;
        }
    }

    void stop() noexcept {
        HWND target_window = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex);
            target_window = window;
        }
        if (target_window) {
            PostMessageW(target_window, WM_CLOSE, 0, 0);
        } else if (thread.joinable()) {
            PostThreadMessageW(thread_id, WM_QUIT, 0, 0);
        }
        if (thread.joinable() &&
            thread.get_id() != std::this_thread::get_id()) {
            thread.join();
        }
        window = nullptr;
        target = nullptr;
        dirty = false;
        frame_message_pending = false;
    }

    LRESULT handle_message(HWND hwnd,
                           UINT message,
                           WPARAM wparam,
                           LPARAM lparam) {
        switch (message) {
        case kFrameMessage:
            {
                std::lock_guard<std::mutex> lock(mutex);
                frame_message_pending = false;
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_PAINT:
            paint(hwnd);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_SIZE:
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd, message, wparam, lparam);
        }
    }

    void paint(HWND hwnd) {
        PAINTSTRUCT paint_struct{};
        HDC device = BeginPaint(hwnd, &paint_struct);
        RECT client{};
        GetClientRect(hwnd, &client);
        const int client_width = client.right - client.left;
        const int client_height = client.bottom - client.top;
        if (client_width <= 0 || client_height <= 0) {
            EndPaint(hwnd, &paint_struct);
            return;
        }

        HDC buffer = CreateCompatibleDC(device);
        HBITMAP bitmap = buffer
            ? CreateCompatibleBitmap(device, client_width, client_height)
            : nullptr;
        if (buffer && bitmap) {
            const HGDIOBJ previous_bitmap = SelectObject(buffer, bitmap);
            FillRect(buffer, &client,
                     reinterpret_cast<HBRUSH>(
                         GetStockObject(BLACK_BRUSH)));

            {
                std::lock_guard<std::mutex> lock(mutex);
                if (!bgra.empty() && width > 0 && height > 0) {
                    BITMAPINFO source{};
                    source.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                    source.bmiHeader.biWidth = width;
                    source.bmiHeader.biHeight = -height;
                    source.bmiHeader.biPlanes = 1;
                    source.bmiHeader.biBitCount = 32;
                    source.bmiHeader.biCompression = BI_RGB;
                    SetStretchBltMode(buffer, HALFTONE);
                    SetBrushOrgEx(buffer, 0, 0, nullptr);
                    StretchDIBits(
                        buffer, 0, 0, client_width, client_height,
                        0, 0, width, height, bgra.data(), &source,
                        DIB_RGB_COLORS, SRCCOPY);
                }
                dirty = false;
            }

            BitBlt(
                device, 0, 0, client_width, client_height,
                buffer, 0, 0, SRCCOPY);
            SelectObject(buffer, previous_bitmap);
        }
        if (bitmap) {
            DeleteObject(bitmap);
        }
        if (buffer) {
            DeleteDC(buffer);
        }
        EndPaint(hwnd, &paint_struct);
    }

    std::mutex mutex;
    std::condition_variable condition;
    std::thread thread;
    HWND window = nullptr;
    HWND target = nullptr;
    std::vector<std::uint8_t> bgra;
    int width = 0;
    int height = 0;
    bool dirty = false;
    bool frame_message_pending = false;
    bool ready = false;
    DWORD thread_id = 0;
};

struct NativeAudioRenderer::Impl {
    struct Buffer {
        WAVEHDR header{};
        std::vector<std::int16_t> samples;
    };

    bool start(int requested_sample_rate, int requested_channels) {
        stop();
        if (requested_channels != 1 && requested_channels != 2) {
            return false;
        }
        sample_rate = requested_sample_rate;
        channels = requested_channels;
        WAVEFORMATEX format{};
        format.wFormatTag = WAVE_FORMAT_PCM;
        format.nChannels = static_cast<WORD>(channels);
        format.nSamplesPerSec = sample_rate;
        format.wBitsPerSample = 16;
        format.nBlockAlign = static_cast<WORD>(channels * 2);
        format.nAvgBytesPerSec = sample_rate * format.nBlockAlign;
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event ||
            waveOutOpen(&device, WAVE_MAPPER, &format,
                        reinterpret_cast<DWORD_PTR>(event),
                        0, CALLBACK_EVENT) != MMSYSERR_NOERROR) {
            stop();
            return false;
        }
        running = true;
        worker = std::thread([this] { run(); });
        return true;
    }

    void submit(std::vector<std::int16_t> samples) {
        if (!running || samples.empty()) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (pending.size() >= 16) {
                return;
            }
            pending.push_back(std::move(samples));
        }
        condition.notify_one();
    }

    void stop() noexcept {
        running = false;
        condition.notify_all();
        if (device) {
            waveOutReset(device);
        }
        if (worker.joinable() &&
            worker.get_id() != std::this_thread::get_id()) {
            worker.join();
        }
        for (auto& buffer : inflight) {
            if (device) {
                waveOutUnprepareHeader(
                    device, &buffer->header, sizeof(WAVEHDR));
            }
        }
        inflight.clear();
        pending.clear();
        if (device) {
            waveOutClose(device);
            device = nullptr;
        }
        if (event) {
            CloseHandle(event);
            event = nullptr;
        }
    }

    void run() {
        while (running) {
            if (event) {
                WaitForSingleObject(event, 10);
            }
            complete_finished();
            std::vector<std::int16_t> samples;
            {
                std::unique_lock<std::mutex> lock(mutex);
                condition.wait_for(lock, std::chrono::milliseconds(10), [this] {
                    return !running || (!pending.empty() && inflight.size() < 4);
                });
                if (!running) {
                    break;
                }
                if (pending.empty() || inflight.size() >= 4) {
                    continue;
                }
                samples = std::move(pending.front());
                pending.pop_front();
            }
            play(std::move(samples));
        }
    }

    void play(std::vector<std::int16_t> samples) {
        auto buffer = std::make_unique<Buffer>();
        buffer->samples = std::move(samples);
        buffer->header.lpData =
            reinterpret_cast<LPSTR>(buffer->samples.data());
        buffer->header.dwBufferLength = static_cast<DWORD>(
            buffer->samples.size() * sizeof(std::int16_t));
        if (waveOutPrepareHeader(
                device, &buffer->header, sizeof(WAVEHDR)) != MMSYSERR_NOERROR) {
            return;
        }
        if (waveOutWrite(
                device, &buffer->header, sizeof(WAVEHDR)) != MMSYSERR_NOERROR) {
            waveOutUnprepareHeader(
                device, &buffer->header, sizeof(WAVEHDR));
            return;
        }
        inflight.push_back(std::move(buffer));
    }

    void complete_finished() {
        while (inflight.empty() == false &&
               (inflight.front()->header.dwFlags & WHDR_DONE) != 0) {
            waveOutUnprepareHeader(
                device, &inflight.front()->header, sizeof(WAVEHDR));
            inflight.pop_front();
        }
    }

    std::mutex mutex;
    std::condition_variable condition;
    std::thread worker;
    HWAVEOUT device = nullptr;
    HANDLE event = nullptr;
    std::deque<std::vector<std::int16_t>> pending;
    std::deque<std::unique_ptr<Buffer>> inflight;
    int sample_rate = 48000;
    int channels = 2;
    std::atomic<bool> running{false};
};
#else
struct NativeVideoRenderer::Impl {
    bool start(const std::string&) { return false; }
    void display(std::vector<std::uint8_t>, int, int) {}
    void stop() noexcept {}
};

struct NativeAudioRenderer::Impl {
    bool start(int, int) { return false; }
    void submit(std::vector<std::int16_t>) {}
    void stop() noexcept {}
};
#endif

NativeVideoRenderer::NativeVideoRenderer()
    : impl_(std::make_unique<Impl>()) {
}

NativeVideoRenderer::~NativeVideoRenderer() {
    stop();
}

bool NativeVideoRenderer::start(const std::string& title) {
    return impl_->start(title);
}

void NativeVideoRenderer::display(std::vector<std::uint8_t> bgra,
                                  int width,
                                  int height) {
    impl_->display(std::move(bgra), width, height);
}

void NativeVideoRenderer::stop() noexcept {
    impl_->stop();
}

NativeAudioRenderer::NativeAudioRenderer()
    : impl_(std::make_unique<Impl>()) {
}

NativeAudioRenderer::~NativeAudioRenderer() {
    stop();
}

bool NativeAudioRenderer::start(int sampleRate, int channels) {
    return impl_->start(sampleRate, channels);
}

void NativeAudioRenderer::submit(std::vector<std::int16_t> samples) {
    impl_->submit(std::move(samples));
}

void NativeAudioRenderer::stop() noexcept {
    impl_->stop();
}

} // namespace iris::media

