#pragma once
#include <cstdint>
#include <vector>
#include <string>
#include <algorithm>

enum class PublisherState {
    INIT,
    IDLE,
    PROBING,
    CONNECTING_OUTPUT,
    STREAMING,
    STALLED,
    RECONFIGURING,
    RECONNECTING_OUTPUT,
    STOPPING,
    FATAL
};

inline const char* state_name(PublisherState s) {
    switch (s) {
        case PublisherState::INIT:               return "INIT";
        case PublisherState::IDLE:               return "IDLE";
        case PublisherState::PROBING:            return "PROBING";
        case PublisherState::CONNECTING_OUTPUT:  return "CONNECTING_OUTPUT";
        case PublisherState::STREAMING:          return "STREAMING";
        case PublisherState::STALLED:            return "STALLED";
        case PublisherState::RECONFIGURING:      return "RECONFIGURING";
        case PublisherState::RECONNECTING_OUTPUT:return "RECONNECTING_OUTPUT";
        case PublisherState::STOPPING:           return "STOPPING";
        case PublisherState::FATAL:              return "FATAL";
        default:                                 return "UNKNOWN";
    }
}

enum class PixelFormat {
    BGRA,
    RGBA,
    RGB,
    BGR
};

struct FrameBuffer {
    std::vector<uint8_t> data;
    uint32_t width  = 0;
    uint32_t height = 0;
    PixelFormat format = PixelFormat::BGRA;
};

struct FrameMeta {
    uint64_t sequence    = 0;
    int64_t  timestamp_us = 0;
    uint32_t width       = 0;
    uint32_t height      = 0;
};

struct SenderInfo {
    std::string name;
    uint32_t    width  = 0;
    uint32_t    height = 0;
    float       fps    = 0.0f;
    bool        valid  = false;
};

struct EncoderConfig {
    std::string codec          = "h264_nvenc";
    std::string fallback_codec = "libx264";
    int bitrate_kbps           = 4000;
    int fps                    = 30;
    int gop_size               = 30;
    int max_b_frames           = 0;
    std::string preset         = "fast";
    std::string tune           = "zerolatency";
    int threads                = 0;
};

struct RtspConfig {
    std::string url;
    int connect_timeout_ms          = 5000;
    int send_timeout_ms             = 5000;
    int max_reconnect_attempts      = 5;
    int reconnect_delay_ms          = 1000;
    int reconnect_max_delay_ms      = 30000;
    float reconnect_backoff_multiplier = 2.0f;
};

struct BackoffState {
    int attempt  = 0;
    int delay_ms = 1000;

    void reset(int initial_ms = 1000) {
        attempt  = 0;
        delay_ms = initial_ms;
    }

    int next_delay(int max_ms = 30000, float multiplier = 2.0f) {
        int d    = delay_ms;
        delay_ms = static_cast<int>(
            std::min(static_cast<float>(delay_ms) * multiplier,
                     static_cast<float>(max_ms)));
        ++attempt;
        return d;
    }
};
