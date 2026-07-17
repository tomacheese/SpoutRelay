#include "config/config_loader.hpp"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

/// @brief 文字列が "#RRGGBB" 形式の 16 進数カラーコードかどうかを判定する
static bool is_valid_hex_color(const std::string& s) {
    if (s.size() != 7 || s[0] != '#') return false;
    return std::all_of(s.begin() + 1, s.end(), [](unsigned char c) {
        return std::isxdigit(c) != 0;
    });
}

static void parse_encoder(const json& j, EncoderConfig& cfg) {
    if (j.contains("codec"))           cfg.codec           = j["codec"].get<std::string>();
    if (j.contains("fallback_codec"))  cfg.fallback_codec  = j["fallback_codec"].get<std::string>();
    if (j.contains("bitrate_kbps"))    cfg.bitrate_kbps    = j["bitrate_kbps"].get<int>();
    if (j.contains("fps"))             cfg.fps             = j["fps"].get<int>();
    if (j.contains("gop_size"))        cfg.gop_size        = j["gop_size"].get<int>();
    if (j.contains("max_b_frames"))    cfg.max_b_frames    = j["max_b_frames"].get<int>();
    if (j.contains("preset"))          cfg.preset          = j["preset"].get<std::string>();
    if (j.contains("tune"))            cfg.tune            = j["tune"].get<std::string>();
    if (j.contains("threads"))         cfg.threads         = j["threads"].get<int>();
}

static void parse_rtsp(const json& j, RtspConfig& cfg) {
    if (j.contains("url"))                        cfg.url                        = j["url"].get<std::string>();
    if (j.contains("connect_timeout_ms"))         cfg.connect_timeout_ms         = j["connect_timeout_ms"].get<int>();
    if (j.contains("send_timeout_ms"))            cfg.send_timeout_ms            = j["send_timeout_ms"].get<int>();
    if (j.contains("max_reconnect_attempts"))     cfg.max_reconnect_attempts     = j["max_reconnect_attempts"].get<int>();
    if (j.contains("reconnect_delay_ms"))         cfg.reconnect_delay_ms         = j["reconnect_delay_ms"].get<int>();
    if (j.contains("reconnect_max_delay_ms"))     cfg.reconnect_max_delay_ms     = j["reconnect_max_delay_ms"].get<int>();
    if (j.contains("reconnect_backoff_multiplier"))
        cfg.reconnect_backoff_multiplier = j["reconnect_backoff_multiplier"].get<float>();
}

bool ConfigLoader::load(const std::string& path, AppConfig& out, std::string& error) {
    std::ifstream file(path);
    if (!file.is_open()) {
        error = "Cannot open config file: " + path;
        return false;
    }

    json j;
    try {
        file >> j;
    } catch (const json::parse_error& e) {
        error = std::string("JSON parse error: ") + e.what();
        return false;
    }

    try {
        // app section
        if (j.contains("app")) {
            const auto& a = j["app"];
            if (a.contains("instance_name")) out.app.instance_name = a["instance_name"].get<std::string>();
            if (a.contains("log_dir"))        out.app.log_dir        = a["log_dir"].get<std::string>();
            if (a.contains("metrics_path"))   out.app.metrics_path   = a["metrics_path"].get<std::string>();
            if (a.contains("health_path"))    out.app.health_path    = a["health_path"].get<std::string>();
        }

        // spout section
        if (j.contains("spout")) {
            const auto& s = j["spout"];
            if (s.contains("sender_name"))             out.spout.sender_name             = s["sender_name"].get<std::string>();
            if (s.contains("poll_interval_ms"))        out.spout.poll_interval_ms        = s["poll_interval_ms"].get<int>();
            if (s.contains("frame_timeout_ms"))        out.spout.frame_timeout_ms        = s["frame_timeout_ms"].get<int>();
            if (s.contains("sender_missing_timeout_ms"))
                out.spout.sender_missing_timeout_ms = s["sender_missing_timeout_ms"].get<int>();
            if (s.contains("prefer_dx11"))             out.spout.prefer_dx11             = s["prefer_dx11"].get<bool>();
            if (s.contains("stalled_recovery_max_attempts"))
                out.spout.stalled_recovery_max_attempts = s["stalled_recovery_max_attempts"].get<int>();
        }

        // placeholder section
        if (j.contains("placeholder")) {
            const auto& p = j["placeholder"];
            if (p.contains("enabled"))          out.placeholder.enabled          = p["enabled"].get<bool>();
            if (p.contains("width"))            out.placeholder.width            = p["width"].get<int>();
            if (p.contains("height"))           out.placeholder.height           = p["height"].get<int>();
            if (p.contains("message"))          out.placeholder.message          = p["message"].get<std::string>();
            if (p.contains("background_hex"))   out.placeholder.background_hex   = p["background_hex"].get<std::string>();
            if (p.contains("text_hex"))         out.placeholder.text_hex         = p["text_hex"].get<std::string>();
            if (p.contains("show_sender_name")) out.placeholder.show_sender_name = p["show_sender_name"].get<bool>();
        }

        // encoder section
        if (j.contains("encoder")) parse_encoder(j["encoder"], out.encoder);

        // rtsp section
        if (j.contains("rtsp")) parse_rtsp(j["rtsp"], out.rtsp);

        // runtime section
        if (j.contains("runtime")) {
            const auto& r = j["runtime"];
            if (r.contains("shutdown_grace_ms"))        out.runtime.shutdown_grace_ms        = r["shutdown_grace_ms"].get<int>();
            if (r.contains("emit_metrics_interval_ms")) out.runtime.emit_metrics_interval_ms = r["emit_metrics_interval_ms"].get<int>();
            if (r.contains("emit_health_interval_ms"))  out.runtime.emit_health_interval_ms  = r["emit_health_interval_ms"].get<int>();
        }

    } catch (const json::exception& e) {
        error = std::string("Config read error: ") + e.what();
        return false;
    }

    // Validation
    if (out.spout.sender_name.empty()) {
        error = "spout.sender_name must not be empty";
        return false;
    }
    if (out.rtsp.url.empty() || out.rtsp.url.rfind("rtsp://", 0) != 0) {
        error = "rtsp.url must start with rtsp://";
        return false;
    }
    if (out.encoder.fps <= 0 || out.encoder.fps > 240) {
        error = "encoder.fps must be between 1 and 240";
        return false;
    }
    if (out.encoder.bitrate_kbps <= 0) {
        error = "encoder.bitrate_kbps must be positive";
        return false;
    }
    if (out.spout.poll_interval_ms <= 0) {
        error = "spout.poll_interval_ms must be positive";
        return false;
    }
    if (out.spout.frame_timeout_ms <= 0) {
        error = "spout.frame_timeout_ms must be positive";
        return false;
    }
    if (out.rtsp.connect_timeout_ms <= 0) {
        error = "rtsp.connect_timeout_ms must be positive";
        return false;
    }
    if (out.rtsp.send_timeout_ms <= 0) {
        error = "rtsp.send_timeout_ms must be positive";
        return false;
    }
    if (out.rtsp.max_reconnect_attempts < 0) {
        error = "rtsp.max_reconnect_attempts must be non-negative";
        return false;
    }
    if (out.rtsp.reconnect_delay_ms <= 0) {
        error = "rtsp.reconnect_delay_ms must be positive";
        return false;
    }
    if (out.rtsp.reconnect_max_delay_ms <= 0) {
        error = "rtsp.reconnect_max_delay_ms must be positive";
        return false;
    }
    if (out.rtsp.reconnect_max_delay_ms < out.rtsp.reconnect_delay_ms) {
        error = "rtsp.reconnect_max_delay_ms must be >= rtsp.reconnect_delay_ms";
        return false;
    }
    if (out.rtsp.reconnect_backoff_multiplier < 1.0f) {
        error = "rtsp.reconnect_backoff_multiplier must be >= 1.0";
        return false;
    }
    if (out.placeholder.width <= 0 || out.placeholder.height <= 0) {
        error = "placeholder.width and placeholder.height must be positive";
        return false;
    }
    if (!is_valid_hex_color(out.placeholder.background_hex)) {
        error = "placeholder.background_hex must be in #RRGGBB format";
        return false;
    }
    if (!is_valid_hex_color(out.placeholder.text_hex)) {
        error = "placeholder.text_hex must be in #RRGGBB format";
        return false;
    }

    return true;
}
