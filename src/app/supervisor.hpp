#pragma once
#include "config/config_loader.hpp"
#include "common/time_utils.hpp"
#include "app/state_machine.hpp"
#include "logging/log_sink.hpp"
#include "metrics/metrics_store.hpp"
#include "spout/spout_monitor.hpp"
#include "capture/frame_pump.hpp"
#include "encoder/encoder_controller.hpp"
#include "rtsp/rtsp_publisher_client.hpp"
#include <atomic>
#include <thread>
#include <memory>

class Supervisor {
public:
    Supervisor();
    ~Supervisor();

    bool init(const AppConfig& config, std::string& error);
    void run();
    void request_stop();

private:
    void handle_idle();
    void handle_probing();
    void handle_connecting_output();
    void handle_streaming();
    void handle_stalled();
    void handle_reconfiguring();
    void handle_reconnecting_output();
    void handle_stopping();

    void metrics_writer_thread_func();
    void teardown_streaming();
    void teardown_encoder();
    void teardown_rtsp();

    // Encode/publish thread (H-05)
    void encode_publish_thread_func();
    void start_encode_thread();
    void stop_encode_thread();

    AppConfig config_;
    StateMachine state_machine_;

    std::shared_ptr<LogSink>       log_;
    std::shared_ptr<MetricsStore>  metrics_;
    std::shared_ptr<ISpoutMonitor> spout_monitor_;
    std::unique_ptr<FramePump>     frame_pump_;
    std::unique_ptr<EncoderController>    encoder_;
    std::unique_ptr<RtspPublisherClient>  rtsp_client_;

    std::atomic<bool> shutdown_requested_{false};
    std::thread metrics_thread_;

    std::thread encode_thread_;
    std::atomic<bool>     encode_stop_{false};
    std::atomic<bool>     encode_error_flag_{false};
    std::atomic<bool>     rtsp_error_flag_{false};
    std::atomic<bool>     resolution_changed_flag_{false};
    std::atomic<uint32_t> pending_width_{0};
    std::atomic<uint32_t> pending_height_{0};
    std::atomic<int64_t>  last_frame_time_ms_{0};

    BackoffState rtsp_backoff_;
    time_utils::Stopwatch probe_timer_;
    time_utils::Stopwatch stall_timer_;

    uint32_t current_width_  = 0;
    uint32_t current_height_ = 0;
    int reconnect_attempts_  = 0;
};
