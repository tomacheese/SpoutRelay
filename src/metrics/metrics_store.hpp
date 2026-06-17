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
    void increment_device_lost_recoveries();

    void reset_session_counters();
    void mark_session_start();

    // Serialise to files.
    // Returns true when the file was written, false when the content was
    // unchanged since the last write (skip) or when a write error occurred.
    // NOTE: these are intentionally non-const because they update the cached
    //       "last written" payload used for diff-skip detection.
    bool save_metrics(const std::string& path);
    bool save_health(const std::string& path);

    // Read-only snapshot values
    uint64_t frames_received()         const { return frames_received_.load(); }
    uint64_t frames_encoded()          const { return frames_encoded_.load(); }
    uint64_t frames_dropped()          const { return frames_dropped_.load(); }
    uint64_t rtsp_errors()             const { return rtsp_errors_.load(); }
    uint64_t reconnect_attempts()      const { return reconnect_attempts_.load(); }
    uint64_t device_lost_recoveries()  const { return device_lost_recoveries_.load(); }

private:
    // Build JSON strings.
    //   with_ts=true  — full output: includes both "ts" (live ISO 8601 timestamp)
    //                   and "uptime_ms" (time since session start).
    //   with_ts=false — comparable output: omits "ts" and "uptime_ms" so the
    //                   result can be used for diff-skip without those
    //                   continuously-changing fields masking real diffs.
    std::string build_metrics_json(bool with_ts = true) const;
    std::string build_health_json(bool with_ts = true)  const;

    std::atomic<uint64_t> frames_received_{0};
    std::atomic<uint64_t> frames_encoded_{0};
    std::atomic<uint64_t> frames_dropped_{0};
    std::atomic<uint64_t> rtsp_errors_{0};
    std::atomic<uint64_t> reconnect_attempts_{0};
    std::atomic<uint64_t> device_lost_recoveries_{0};

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

    // Cached payloads (ts-less) from the most recent successful write.
    // Protected by mutex_ for save_metrics / save_health callers.
    std::string last_metrics_payload_;
    std::string last_health_payload_;
};
