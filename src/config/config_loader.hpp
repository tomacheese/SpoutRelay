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

        /// @brief Number of consecutive STALLED recoveries (RTSP reconnects via
        ///        RECONNECTING_OUTPUT) that may fail to restore frame reception before a
        ///        full reconnect including the Spout receiver (SpoutMonitor) is forced
        ///        (restart from PROBING). <= 0 disables.
        ///        Safety net for stuck states that RTSP-layer reconnects cannot fix
        ///        (e.g. inconsistent spoutDX internal flag after a GPU/CPU path switch).
        int stalled_recovery_max_attempts = 10;
    } spout;

    /// @brief Settings for the placeholder (NO SIGNAL) video streamed while no Spout
    ///        source/device is found.
    struct Placeholder {
        bool enabled               = false;        // disabled by default (opt-in); preserves existing behavior
        int  width                 = 1280;         // width used when the last source resolution is unknown
        int  height                = 720;          // height used when the last source resolution is unknown
        std::string message        = "NO SIGNAL";  // message shown at the center
        std::string background_hex = "#000000";    // background color (#RRGGBB)
        std::string text_hex       = "#FFFFFF";    // text color (#RRGGBB)
        bool show_sender_name      = true;         // show the awaited sender_name below the message
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
