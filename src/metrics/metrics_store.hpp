#pragma once
#include "common/types.hpp"
#include <string>
#include <atomic>
#include <mutex>
#include <memory>
#include <cstdint>

class MetricsStore {
public:
    MetricsStore();

    void set_state(const std::string& state);
    void set_sender_info(const std::string& name, uint32_t w, uint32_t h, float fps);
    void set_bitrate_kbps(double kbps);
    void set_current_fps(double fps);
    void set_rtsp_url(const std::string& url);
    void set_encoder_codec(const std::string& codec);

    void increment_frames_received();
    void increment_frames_encoded();
    void increment_frames_dropped();
    void increment_rtsp_errors();
    void increment_reconnect_attempts();

    void reset_session_counters();
    void mark_session_start();

    // Serialise to files
    bool save_metrics(const std::string& path) const;
    bool save_health(const std::string& path) const;

    // Read-only snapshot values
    uint64_t frames_received()    const { return frames_received_.load(); }
    uint64_t frames_encoded()     const { return frames_encoded_.load(); }
    uint64_t frames_dropped()     const { return frames_dropped_.load(); }
    uint64_t rtsp_errors()        const { return rtsp_errors_.load(); }
    uint64_t reconnect_attempts() const { return reconnect_attempts_.load(); }

private:
    std::string build_metrics_json() const;
    std::string build_health_json()  const;

    std::atomic<uint64_t> frames_received_{0};
    std::atomic<uint64_t> frames_encoded_{0};
    std::atomic<uint64_t> frames_dropped_{0};
    std::atomic<uint64_t> rtsp_errors_{0};
    std::atomic<uint64_t> reconnect_attempts_{0};

    mutable std::mutex mutex_;
    std::string current_state_;
    std::string sender_name_;
    uint32_t    sender_width_  = 0;
    uint32_t    sender_height_ = 0;
    float       sender_fps_    = 0.0f;
    double      bitrate_kbps_  = 0.0;
    double      current_fps_   = 0.0;
    std::string rtsp_url_;
    std::string encoder_codec_;
    int64_t     session_start_ms_ = 0;
};
