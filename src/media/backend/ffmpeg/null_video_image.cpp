#include "media/backend/ffmpeg/null_video_image.h"

#include <algorithm>
#include <cstring>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace iris::media {
namespace {

#ifdef _WIN32
std::wstring utf8_to_wide(const std::string& value) {
    if (value.empty()) {
        return {};
    }
    const int length = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    if (length <= 0) {
        return std::wstring(value.begin(), value.end());
    }
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), result.data(), length);
    return result;
}

bool create_fitted_font(HDC device,
                        const std::wstring& text,
                        int available_width,
                        int initial_size,
                        HFONT& font,
                        SIZE& extent) {
    for (int size = initial_size; size >= 10; size -= 2) {
        font = CreateFontW(
            -size, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
            L"Microsoft YaHei");
        if (!font) {
            return false;
        }
        const HGDIOBJ previous = SelectObject(device, font);
        const BOOL measured = GetTextExtentPoint32W(
            device, text.c_str(), static_cast<int>(text.size()), &extent);
        SelectObject(device, previous);
        if (measured && extent.cx <= available_width) {
            return true;
        }
        DeleteObject(font);
        font = nullptr;
    }
    return false;
}
#endif

} // namespace

NullVideoImage renderNullVideoImage(
    int requested_width,
    int requested_height,
    const std::string& localUri) {
    NullVideoImage image;
    image.width = std::max(1, requested_width);
    image.height = std::max(1, requested_height);
    image.bgra.resize(
        static_cast<std::size_t>(image.width) *
        static_cast<std::size_t>(image.height) * 4);

    for (int y = 0; y < image.height; ++y) {
        const int denominator = std::max(1, image.height - 1);
        const int gray = 16 + (40 * y) / denominator;
        auto* row = image.bgra.data() +
            static_cast<std::size_t>(y) * image.width * 4;
        for (int x = 0; x < image.width; ++x) {
            row[x * 4] = static_cast<std::uint8_t>(gray);
            row[x * 4 + 1] = static_cast<std::uint8_t>(gray);
            row[x * 4 + 2] = static_cast<std::uint8_t>(gray);
            row[x * 4 + 3] = 255;
        }
    }

#ifdef _WIN32
    const std::wstring text = utf8_to_wide(
        localUri.empty() ? std::string("Iris") : localUri);
    HDC screen = GetDC(nullptr);
    if (!screen) {
        return image;
    }
    HDC memory = CreateCompatibleDC(screen);
    if (!memory) {
        ReleaseDC(nullptr, screen);
        return image;
    }

    BITMAPINFO bitmap{};
    bitmap.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmap.bmiHeader.biWidth = image.width;
    bitmap.bmiHeader.biHeight = -image.height;
    bitmap.bmiHeader.biPlanes = 1;
    bitmap.bmiHeader.biBitCount = 32;
    bitmap.bmiHeader.biCompression = BI_RGB;

    void* pixels = nullptr;
    HBITMAP dib = CreateDIBSection(
        screen, &bitmap, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (!dib || !pixels) {
        if (dib) {
            DeleteObject(dib);
        }
        DeleteDC(memory);
        ReleaseDC(nullptr, screen);
        return image;
    }
    const HGDIOBJ previous_bitmap = SelectObject(memory, dib);
    std::memcpy(pixels, image.bgra.data(), image.bgra.size());

    const int initial_size =
        std::clamp(image.height / 11, 16, 56);
    const int margin = std::max(8, image.width / 18);
    const int available_width =
        std::max(1, image.width - margin * 2);
    HFONT font = nullptr;
    SIZE extent{};
    if (create_fitted_font(
            memory, text, available_width,
            initial_size, font, extent)) {
        const HGDIOBJ previous_font = SelectObject(memory, font);
        SetBkMode(memory, TRANSPARENT);
        SetTextColor(memory, RGB(255, 255, 255));
        RECT text_rect{
            margin, 0, image.width - margin, image.height
        };
        DrawTextW(
            memory, text.c_str(), static_cast<int>(text.size()),
            &text_rect,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE |
                DT_NOPREFIX | DT_END_ELLIPSIS);
        SelectObject(memory, previous_font);
        DeleteObject(font);
    }

    GdiFlush();
    std::memcpy(image.bgra.data(), pixels, image.bgra.size());
    SelectObject(memory, previous_bitmap);
    DeleteObject(dib);
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
#endif

    return image;
}

} // namespace iris::media
