#include <cstdio>
#include <chrono>
#include <thread>
#include <memory>
#include "capture/frame_pump.hpp"
#include "spout/spout_monitor.hpp"
#include "test_utils.hpp"

/// @brief Mock ISpoutMonitor for tests.
///        receive_latest_frame() returns a new sequentially numbered frame on every call.
///        Independent of real Spout/GPU, so it also runs in CI.
class MockSpoutMonitor : public ISpoutMonitor {
public:
    bool init(std::string& /*error*/) override { return true; }
    bool probe_sender(const std::string& /*name*/) override { return true; }
    bool connect(const std::string& /*name*/, std::string& /*error*/) override { return true; }
    void disconnect() override {}

    bool receive_latest_frame(FrameBuffer& buf, FrameMeta& meta, bool& is_new) override {
        meta.sequence  = sequence_++;
        meta.width     = 4;
        meta.height    = 4;
        buf.width      = 4;
        buf.height     = 4;
        buf.data.assign(4 * 4 * 4, 0);
        is_new = true;
        return true;
    }

    SenderInfo get_sender_info() const override { return {}; }
    bool is_connected() const override { return true; }
    void* gpu_device() override { return nullptr; }
    void set_gpu_mode(bool /*enabled*/) override {}
    uint32_t get_sender_dxgi_format() const override { return 0; }
    bool is_device_removed() const override { return false; }
    bool reinit_device(std::string& /*error*/) override { return true; }

private:
    uint64_t sequence_ = 0;
};

int run_frame_pump_tests() {
    printf("=== Frame Pump Tests ===\n");

    {
        // start(nullptr, ...) is a no-op
        FramePump pump;
        pump.start(nullptr, 10);
        VERIFY_MSG(!pump.is_running(), "start(nullptr) should be a no-op");
        printf("[PASS] start(nullptr) is a no-op\n");
    }

    {
        // try_pop on a pump that was never started returns false
        FramePump pump;
        FrameBuffer buf;
        FrameMeta meta;
        bool ok = pump.try_pop(buf, meta);
        VERIFY_MSG(!ok, "try_pop on an empty/unstarted pump should return false");
        printf("[PASS] try_pop returns false when empty\n");
    }

    {
        // When not started (running_=false), wait_pop returns false immediately
        // even if the queue is empty
        // (the internal wait condition `!queue_.empty() || !running_.load()`
        //   is already true because running_=false)
        FramePump pump;
        FrameBuffer buf;
        FrameMeta meta;
        bool ok = pump.wait_pop(buf, meta, 50);
        VERIFY_MSG(!ok, "wait_pop should return false immediately when pump is not running");
        printf("[PASS] wait_pop returns false immediately when not running\n");
    }

    {
        // wait_pop receives frames while the pump is running
        auto mock = std::make_shared<MockSpoutMonitor>();
        FramePump pump;
        pump.start(mock, 10);

        FrameBuffer buf;
        FrameMeta meta;
        bool ok = pump.wait_pop(buf, meta, 2000);
        pump.stop();

        VERIFY_MSG(ok, "wait_pop should receive a frame once the pump is running");
        VERIFY(buf.width == 4 && buf.height == 4);
        printf("[PASS] wait_pop receives a frame from a running pump\n");
    }

    {
        // last_frame_time_ms / last_source_alive_ms are updated while running
        auto mock = std::make_shared<MockSpoutMonitor>();
        FramePump pump;
        VERIFY(pump.last_frame_time_ms() == 0);

        pump.start(mock, 10);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        VERIFY_MSG(pump.last_frame_time_ms() > 0,
                   "last_frame_time_ms should advance once frames flow");
        VERIFY_MSG(pump.last_source_alive_ms() > 0,
                   "last_source_alive_ms should advance once the source responds");
        pump.stop();
        printf("[PASS] last_frame_time_ms / last_source_alive_ms update while running\n");
    }

    {
        // When more than FramePump::MAX_QUEUE_SIZE frames are produced,
        // old frames are dropped and the queue stays bounded
        auto mock = std::make_shared<MockSpoutMonitor>();
        FramePump pump;
        pump.start(mock, 1); // poll_interval_ms=1 to spin the capture thread fast

        // wait until enough backlog has built up in the queue
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        int popped = 0;
        uint64_t first_seq = 0;
        uint64_t last_seq  = 0;
        FrameBuffer buf;
        FrameMeta meta;
        while (pump.try_pop(buf, meta)) {
            if (popped == 0) first_seq = meta.sequence;
            last_seq = meta.sequence;
            ++popped;
        }
        pump.stop();

        VERIFY_MSG(popped > 0, "expected at least one frame to be queued");
        VERIFY_MSG(popped <= 4, "queue should never exceed MAX_QUEUE_SIZE (4)");
        VERIFY_MSG(first_seq > 0,
                   "older frames should have been dropped once the queue overflowed");
        VERIFY(last_seq >= first_seq);
        printf("[PASS] queue overflow drops oldest frames and stays bounded (popped=%d, "
               "first_seq=%llu, last_seq=%llu)\n",
               popped,
               static_cast<unsigned long long>(first_seq),
               static_cast<unsigned long long>(last_seq));
    }

    {
        // stop() drains the queue; try_pop returns nothing afterwards
        auto mock = std::make_shared<MockSpoutMonitor>();
        FramePump pump;
        pump.start(mock, 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        pump.stop();
        VERIFY_MSG(!pump.is_running(), "pump should not be running after stop()");

        FrameBuffer buf;
        FrameMeta meta;
        bool ok = pump.try_pop(buf, meta);
        VERIFY_MSG(!ok, "queue should be drained after stop()");
        printf("[PASS] stop() drains the queue and halts capture\n");
    }

    return 0;
}
