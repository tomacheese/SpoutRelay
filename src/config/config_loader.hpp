#pragma once
#include "common/types.hpp"
#include <string>

struct AppConfig {
    struct App {
        std::string instance_name = "spoutrelay-publisher-01";
        std::string log_dir       = "./logs";
        std::string metrics_path  = "./state/metrics.json";
        std::string health_path   = "./state/health.json";
    } app;

    struct Spout {
        std::string sender_name;
        int poll_interval_ms          = 50;
        int frame_timeout_ms          = 300;
        int sender_missing_timeout_ms = 800;
        bool prefer_dx11              = true;

        /// @brief STALLED からの RTSP 再接続 (RECONNECTING_OUTPUT) を試みても
        ///        一度もフレーム受信が回復しないまま連続で何回失敗したら、
        ///        Spout 受信側 (SpoutMonitor) を含めた完全な再接続
        ///        (PROBING からのやり直し) を強制するか。0 以下で無効化。
        ///        (GPU/CPU 受信パス切替時の spoutDX 内部フラグ不整合等、
        ///        RTSP 層の再接続だけでは解消しない膠着状態への保険的対策)
        int stalled_recovery_max_attempts = 10;
    } spout;

    /// @brief Spout 映像/デバイスが見つからない間に配信する
    ///        プレースホルダ（NO SIGNAL）映像の設定。
    struct Placeholder {
        bool enabled               = false;        // 既定で無効（オプトイン）。既存挙動を維持する
        int  width                 = 1280;         // 直近のソース解像度が不明な場合に使用する幅
        int  height                = 720;          // 直近のソース解像度が不明な場合に使用する高さ
        std::string message        = "NO SIGNAL";  // 中央に表示するメッセージ
        std::string background_hex = "#000000";    // 背景色 (#RRGGBB)
        std::string text_hex       = "#FFFFFF";    // 文字色 (#RRGGBB)
        bool show_sender_name      = true;         // 待機中の sender_name をメッセージ下に表示する
    } placeholder;

    EncoderConfig encoder;
    RtspConfig    rtsp;

    struct Runtime {
        int shutdown_grace_ms         = 3000;
        int emit_metrics_interval_ms  = 1000;
        int emit_health_interval_ms   = 1000;
    } runtime;
};

class ConfigLoader {
public:
    static bool load(const std::string& path, AppConfig& out, std::string& error);
};
