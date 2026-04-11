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
    } spout;

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
