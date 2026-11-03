#pragma once
#include "common/types.hpp"
#include <memory>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include "spout/spout_monitor.hpp"

class FramePump {
public:
    FramePump();
    ~FramePump();

    void start(std::shared_ptr<ISpoutMonitor> monitor, int poll_interval_ms);
    void stop();

    bool try_pop(FrameBuffer& buf, FrameMeta& meta);
    bool wait_pop(FrameBuffer& buf, FrameMeta& meta, int timeout_ms);

    bool is_running() const { return running_.load(); }
    int64_t last_frame_time_ms() const { return last_frame_time_ms_.load(); }

    /**
     * @brief Returns the time (ms) the Spout source last responded successfully
     *
     * The last time receive_latest_frame() returned ok=true (Spout connected),
     * regardless of is_frame_new. A static scene produces no new frames, so
     * last_frame_time_ms() is not updated, but this keeps advancing while the source
     * is alive. Use this for stall detection to avoid false STALLED on static scenes.
     */
    int64_t last_source_alive_ms() const { return last_source_alive_ms_.load(); }

private:
    void capture_thread_func();

    static constexpr int MAX_QUEUE_SIZE = 4;

    std::shared_ptr<ISpoutMonitor> monitor_;
    int poll_interval_ms_ = 50;

    std::atomic<bool>    running_{false};
    std::atomic<int64_t> last_frame_time_ms_{0};
    std::atomic<int64_t> last_source_alive_ms_{0};

    mutable std::mutex      mutex_;
    std::condition_variable cv_;
    std::queue<std::pair<FrameBuffer, FrameMeta>> queue_;

    std::thread thread_;
};
