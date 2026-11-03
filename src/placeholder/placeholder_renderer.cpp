#include "placeholder/placeholder_renderer.hpp"
#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <limits>

namespace {

/// @brief Converts a "#RRGGBB" string to a COLORREF.
///        Returns black (#000000) for a malformed string.
COLORREF parse_hex_color(const std::string& hex) {
    if (hex.size() != 7 || hex[0] != '#') return RGB(0, 0, 0);
    try {
        BYTE r = static_cast<BYTE>(std::stoul(hex.substr(1, 2), nullptr, 16));
        BYTE g = static_cast<BYTE>(std::stoul(hex.substr(3, 2), nullptr, 16));
        BYTE b = static_cast<BYTE>(std::stoul(hex.substr(5, 2), nullptr, 16));
        return RGB(r, g, b);
    } catch (...) {
        return RGB(0, 0, 0);
    }
}

/// @brief Converts a UTF-8 string to a UTF-16 (wide) string
std::wstring to_wide(const std::string& s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (len <= 0) return {};
    std::wstring w(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), len);
    if (!w.empty() && w.back() == L'\0') w.pop_back();
    return w;
}

/// @brief RAII wrapper owning a top-down 32bpp DIB section and the drawing DC
///        it is selected into.
class DibCanvas {
public:
    DibCanvas(uint32_t width, uint32_t height) {
        dc_ = CreateCompatibleDC(nullptr);
        if (!dc_) return;

        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth       = static_cast<LONG>(width);
        bmi.bmiHeader.biHeight      = -static_cast<LONG>(height); // negative = top-down DIB
        bmi.bmiHeader.biPlanes      = 1;
        bmi.bmiHeader.biBitCount    = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        bitmap_ = CreateDIBSection(dc_, &bmi, DIB_RGB_COLORS, &bits_, nullptr, 0);
        if (bitmap_) {
            HGDIOBJ prev = SelectObject(dc_, bitmap_);
            if (prev != nullptr && prev != HGDI_ERROR) {
                old_bitmap_ = static_cast<HBITMAP>(prev);
            } else {
                // On SelectObject failure, discard the DIB so valid() returns false
                DeleteObject(bitmap_);
                bitmap_ = nullptr;
                bits_   = nullptr;
            }
        }
    }

    ~DibCanvas() {
        if (dc_ && old_bitmap_) SelectObject(dc_, old_bitmap_);
        if (bitmap_) DeleteObject(bitmap_);
        if (dc_) DeleteDC(dc_);
    }

    DibCanvas(const DibCanvas&) = delete;
    DibCanvas& operator=(const DibCanvas&) = delete;

    HDC dc() const { return dc_; }
    bool valid() const { return dc_ != nullptr && bitmap_ != nullptr && bits_ != nullptr; }
    const uint8_t* bits() const { return static_cast<const uint8_t*>(bits_); }

private:
    HDC     dc_         = nullptr;
    HBITMAP bitmap_     = nullptr;
    HBITMAP old_bitmap_ = nullptr;
    void*   bits_       = nullptr;
};

/// @brief RAII wrapper that reliably releases a font handle
class GdiFont {
public:
    GdiFont(int pixel_height, bool bold) {
        font_ = CreateFontW(
            -pixel_height, 0, 0, 0,
            bold ? FW_BOLD : FW_NORMAL,
            FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    }
    ~GdiFont() { if (font_) DeleteObject(font_); }

    GdiFont(const GdiFont&) = delete;
    GdiFont& operator=(const GdiFont&) = delete;

    HFONT handle() const { return font_; }

private:
    HFONT font_ = nullptr;
};

/// @brief Draws text centered in the given area
void draw_centered_text(HDC dc, const RECT& rect, const std::wstring& text,
                         COLORREF color, int font_height, bool bold) {
    if (text.empty()) return;
    GdiFont font(font_height, bold);

    // If CreateFontW/SelectObject fails, draw with the original font (fallback).
    // Avoid keeping HGDI_ERROR in old_font.
    HFONT old_font = nullptr;
    if (font.handle()) {
        HGDIOBJ prev = SelectObject(dc, font.handle());
        if (prev != nullptr && prev != HGDI_ERROR) old_font = static_cast<HFONT>(prev);
    }

    SetTextColor(dc, color);
    SetBkMode(dc, TRANSPARENT);
    RECT r = rect;
    DrawTextW(dc, text.c_str(), -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    if (old_font) SelectObject(dc, old_font);
}

} // namespace

FrameBuffer render_placeholder_frame(const AppConfig::Placeholder& cfg,
                                      const std::string& sender_name,
                                      uint32_t width, uint32_t height) {
    FrameBuffer out;
    out.width  = width;
    out.height = height;
    out.format = PixelFormat::RGBA;

    // 0x0 returns an empty buffer instead of crashing
    if (width == 0 || height == 0) return out;

    // Return an empty buffer above the practical maximum resolution (8192x8192).
    // The placeholder loops one still image, so a huge size is treated as a
    // misconfiguration or bogus value; this also prevents bad_alloc.
    constexpr uint32_t kMaxDim    = 8192u;
    constexpr uint64_t kMaxPixels = static_cast<uint64_t>(kMaxDim) * kMaxDim;
    if (width > kMaxDim || height > kMaxDim ||
        static_cast<uint64_t>(width) * static_cast<uint64_t>(height) > kMaxPixels) {
        out.width  = 0;
        out.height = 0;
        return out;
    }

    // Also return an empty buffer if width*height*4 overflows size_t
    // (safety for environments where size_t is 32-bit)
    if (static_cast<uint64_t>(width) * static_cast<uint64_t>(height) >
        std::numeric_limits<size_t>::max() / 4) {
        out.width  = 0;
        out.height = 0;
        return out;
    }

    out.data.assign(static_cast<size_t>(width) * height * 4, 0);

    DibCanvas canvas(width, height);
    if (!canvas.valid()) return out;

    HDC dc = canvas.dc();
    COLORREF bg = parse_hex_color(cfg.background_hex);
    COLORREF fg = parse_hex_color(cfg.text_hex);

    // Fill the background
    RECT full{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    HBRUSH brush = CreateSolidBrush(bg);
    FillRect(dc, &full, brush);
    DeleteObject(brush);

    // Choose the font size from the resolution
    int message_font_height = std::max(16, static_cast<int>(height) / 12);
    int sub_font_height     = std::max(12, static_cast<int>(height) / 24);

    if (cfg.show_sender_name && !sender_name.empty()) {
        // Message in the upper half, awaited sender name in the lower half
        LONG mid = full.bottom / 2;
        RECT message_rect{full.left, full.top, full.right, mid};
        RECT sub_rect{full.left, mid, full.right, full.bottom};
        draw_centered_text(dc, message_rect, to_wide(cfg.message), fg, message_font_height, true);
        draw_centered_text(dc, sub_rect, L"Waiting for: " + to_wide(sender_name),
                            fg, sub_font_height, false);
    } else {
        draw_centered_text(dc, full, to_wide(cfg.message), fg, message_font_height, true);
    }

    GdiFlush();

    // The DIB stores 0xAARRGGBB little-endian, i.e. BGRA byte order in memory.
    // The encoder always takes AV_PIX_FMT_RGBA input, so swap R/B to get
    // RGBA byte order. GDI does not draw alpha, so set it explicitly to
    // opaque (255).
    const uint8_t* src = canvas.bits();
    for (size_t i = 0, n = static_cast<size_t>(width) * height; i < n; ++i) {
        const uint8_t* p = src + i * 4;
        uint8_t* o = out.data.data() + i * 4;
        o[0] = p[2]; // R
        o[1] = p[1]; // G
        o[2] = p[0]; // B
        o[3] = 255;  // A
    }

    return out;
}
