#include "metrics/metrics_store.hpp"
#include "common/time_utils.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <filesystem>
#include <sstream>

using json = nlohmann::json;
namespace fs = std::filesystem;

MetricsStore::MetricsStore() = default;

void MetricsStore::set_state(const std::string& state) {
    std::lock_guard<std::mutex> lk(mutex_);
    current_state_ = state;
}

void MetricsStore::set_sender_info(const std::string& name,
                                   uint32_t w, uint32_t h, float fps) {
    std::lock_guard<std::mutex> lk(mutex_);
    sender_name_   = name;
    sender_width_  = w;
    sender_height_ = h;
    sender_fps_    = fps;
}

void MetricsStore::set_bitrate_kbps(double kbps) {
    std::lock_guard<std::mutex> lk(mutex_);
    bitrate_kbps_ = kbps;
}

void MetricsStore::set_current_fps(double fps) {
    std::lock_guard<std::mutex> lk(mutex_);
    current_fps_ = fps;
}

void MetricsStore::set_rtsp_url(const std::string& url) {
    std::lock_guard<std::mutex> lk(mutex_);
    rtsp_url_ = url;
}

void MetricsStore::set_encoder_codec(const std::string& codec) {
    std::lock_guard<std::mutex> lk(mutex_);
    encoder_codec_ = codec;
}

void MetricsStore::increment_frames_received()    { frames_received_.fetch_add(1); }
void MetricsStore::increment_frames_encoded()     { frames_encoded_.fetch_add(1); }
void MetricsStore::increment_frames_dropped()     { frames_dropped_.fetch_add(1); }
void MetricsStore::increment_rtsp_errors()        { rtsp_errors_.fetch_add(1); }
void MetricsStore::increment_reconnect_attempts() { reconnect_attempts_.fetch_add(1); }

void MetricsStore::reset_session_counters() {
    frames_received_.store(0);
    frames_encoded_.store(0);
    frames_dropped_.store(0);
    rtsp_errors_.store(0);
    reconnect_attempts_.store(0);
}

void MetricsStore::mark_session_start() {
    session_start_ms_ = time_utils::system_now_ms();
}

std::string MetricsStore::build_metrics_json() const {
    json j;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        j["state"]         = current_state_;
        j["sender_name"]   = sender_name_;
        j["sender_width"]  = sender_width_;
        j["sender_height"] = sender_height_;
        j["sender_fps"]    = sender_fps_;
        j["bitrate_kbps"]  = bitrate_kbps_;
        j["current_fps"]   = current_fps_;
        j["rtsp_url"]      = rtsp_url_;
        j["encoder_codec"] = encoder_codec_;
        int64_t uptime_ms = time_utils::system_now_ms() - session_start_ms_;
        j["uptime_ms"]     = uptime_ms;
    }
    j["frames_received"]    = frames_received_.load();
    j["frames_encoded"]     = frames_encoded_.load();
    j["frames_dropped"]     = frames_dropped_.load();
    j["rtsp_errors"]        = rtsp_errors_.load();
    j["reconnect_attempts"] = reconnect_attempts_.load();
    j["ts"]                 = time_utils::iso8601_now();
    return j.dump(2);
}

std::string MetricsStore::build_health_json() const {
    json j;
    std::string state;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        state = current_state_;
    }
    bool healthy = (state == "STREAMING" || state == "CONNECTING_OUTPUT" ||
                    state == "PROBING"   || state == "IDLE");
    j["healthy"] = healthy;
    j["state"]   = state;
    j["ts"]      = time_utils::iso8601_now();
    return j.dump(2);
}

static bool write_atomic(const std::string& path, const std::string& content) {
    std::string tmp = path + ".tmp";
    try {
        auto parent = fs::path(path).parent_path();
        if (!parent.empty())
            fs::create_directories(parent);
        std::ofstream f(tmp);
        if (!f.is_open()) return false;
        f << content;
        f.close();
        fs::rename(tmp, path);
        return true;
    } catch (...) {
        return false;
    }
}

bool MetricsStore::save_metrics(const std::string& path) const {
    return write_atomic(path, build_metrics_json());
}

bool MetricsStore::save_health(const std::string& path) const {
    return write_atomic(path, build_health_json());
}
