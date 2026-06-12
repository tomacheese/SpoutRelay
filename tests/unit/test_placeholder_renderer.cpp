#include <cstdio>
#include "common/types.hpp"
#include "config/config_loader.hpp"
#include "placeholder/placeholder_renderer.hpp"
#include "test_utils.hpp"

int run_placeholder_renderer_tests() {
    printf("=== Placeholder Renderer Tests ===\n");

    {
        // 既定設定での描画: サイズ・フォーマットが要求どおりであること
        AppConfig::Placeholder cfg;
        FrameBuffer frame = render_placeholder_frame(cfg, "MySender", 320, 180);

        VERIFY(frame.width  == 320);
        VERIFY(frame.height == 180);
        VERIFY(frame.format == PixelFormat::RGBA);
        VERIFY(frame.data.size() == static_cast<size_t>(320) * 180 * 4);
        printf("[PASS] render_placeholder_frame returns RGBA buffer with expected size\n");
    }

    {
        // 背景色がそのまま四隅ピクセルに反映されること (RGBA バイト順)
        AppConfig::Placeholder cfg;
        cfg.background_hex  = "#102030";
        cfg.text_hex        = "#FFFFFF";
        cfg.show_sender_name = false;
        const uint32_t w = 200, h = 100;
        FrameBuffer frame = render_placeholder_frame(cfg, "MySender", w, h);

        VERIFY(frame.data.size() == static_cast<size_t>(w) * h * 4);

        auto pixel_at = [&](uint32_t x, uint32_t y) {
            return frame.data.data() + (static_cast<size_t>(y) * w + x) * 4;
        };

        // 四隅 (テキスト描画範囲外) は背景色のままであること
        for (auto* p : {pixel_at(0, 0), pixel_at(w - 1, 0), pixel_at(0, h - 1), pixel_at(w - 1, h - 1)}) {
            VERIFY(p[0] == 0x10); // R
            VERIFY(p[1] == 0x20); // G
            VERIFY(p[2] == 0x30); // B
            VERIFY(p[3] == 0xFF); // A (常に不透明)
        }
        printf("[PASS] corner pixels match background_hex in RGBA order\n");
    }

    {
        // 0x0 解像度では空でないバッファ (size 0) を返し、クラッシュしないこと
        AppConfig::Placeholder cfg;
        FrameBuffer frame = render_placeholder_frame(cfg, "MySender", 0, 0);
        VERIFY(frame.data.empty());
        printf("[PASS] zero-size request does not crash\n");
    }

    return 0;
}
