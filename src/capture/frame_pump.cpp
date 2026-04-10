#include "capture/frame_pump.hpp"
#include "common/time_utils.hpp"
#include <chrono>

FramePump::FramePump()  = default;
FramePump::~FramePump() { stop(); }

void FramePump::start(std::shared_ptr<ISpoutMonitor> monitor, int poll_interval_ms) {
    if (!monitor) return;
    if (running_.load()) stop();
    monitor_          = std::move(monitor);
    poll_interval_ms_ = poll_interval_ms;
    running_.store(true);
    thread_ = std::thread(&FramePump::capture_thread_func, this);
}

void FramePump::stop() {
    if (!running_.load()) return;
    running_.store(false);
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    // Drain queue
    std::lock_guard<std::mutex> lk(mutex_);
    while (!queue_.empty()) queue_.pop();
}

bool FramePump::try_pop(FrameBuffer& buf, FrameMeta& meta) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (queue_.empty()) return false;
    auto& front = queue_.front();
    buf  = std::move(front.first);
    meta = std::move(front.second);
    queue_.pop();
    return true;
}

bool FramePump::wait_pop(FrameBuffer& buf, FrameMeta& meta, int timeout_ms) {
    std::unique_lock<std::mutex> lk(mutex_);
    bool got = cv_.wait_for(lk,
        std::chrono::milliseconds(timeout_ms),
        [this] { return !queue_.empty() || !running_.load(); });

    if (!got || queue_.empty()) return false;

    auto& front = queue_.front();
    buf  = std::move(front.first);
    meta = std::move(front.second);
    queue_.pop();
    return true;
}

void FramePump::capture_thread_func() {
    FrameBuffer buf;
    FrameMeta   meta;

    while (running_.load()) {
        bool is_new = false;
        bool ok     = monitor_->receive_latest_frame(buf, meta, is_new);

        if (ok && is_new) {
            last_frame_time_ms_.store(time_utils::now_ms());
            std::lock_guard<std::mutex> lk(mutex_);
            if (static_cast<int>(queue_.size()) >= MAX_QUEUE_SIZE) {
                queue_.pop(); // drop oldest frame to prevent lag
            }
            queue_.emplace(std::move(buf), std::move(meta));
            cv_.notify_one();
        }

        std::this_thread::sleep_for(
            std::chrono::milliseconds(poll_interval_ms_));
    }
}
