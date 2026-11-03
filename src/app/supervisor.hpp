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
#include "placeholder/placeholder_renderer.hpp"
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
    void handle_placeholder();
    void handle_connecting_output();
    void handle_streaming();
    void handle_stalled();
    void handle_reconfiguring();
    void handle_reconnecting_output();
    void handle_recovering_device();
    void handle_stopping();

    void metrics_writer_thread_func();
    void teardown_streaming();
    void teardown_encoder();
    void teardown_rtsp();

    /// @brief Initializes the encoder at the given resolution and connects the RTSP client.
    ///
    /// handle_connecting_output() / handle_reconfiguring() / handle_placeholder()
    /// Shared by handle_connecting_output(), handle_reconfiguring() and handle_placeholder(),
    /// which previously duplicated this logic. On failure the caller must call
    /// teardown_encoder()/teardown_rtsp().
    /// @param width,height video resolution
    /// @param error error message on failure
    /// @return true on success
    bool init_encoder_and_rtsp(uint32_t width, uint32_t height, std::string& error);

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

    /// @brief Consecutive STALLED -> RECONNECTING_OUTPUT (RTSP-only reconnect) attempts that
    ///        never led to a real recovery (stall_recovered in handle_stalled()).
    ///        Safety-net watchdog counter used by
    ///        supervisor_logic::should_force_spout_recovery(). Reset to 0 when frame
    ///        reception actually recovers (stall_recovered in handle_stalled()) and also
    ///        when handle_connecting_output() transitions normally to STREAMING.
    int consecutive_stall_recoveries_ = 0;

    /// @brief Resolution of the most recently connected real source.
    ///        Preferred over the configured default when rendering PLACEHOLDER video,
    ///        to avoid an RTSP reconfiguration when the source returns.
    uint32_t last_source_width_  = 0;
    uint32_t last_source_height_ = 0;

    /// @brief Frame streamed in the PLACEHOLDER state. A still image, so it is generated
    ///        once and re-encoded repeatedly at the configured fps.
    FrameBuffer placeholder_frame_;
    FrameMeta   placeholder_meta_{};
    bool        placeholder_active_         = false;
    bool        placeholder_content_changed_ = true;
    time_utils::Stopwatch placeholder_frame_timer_;
    time_utils::Stopwatch placeholder_probe_timer_;

    /// @brief Counters for fps/bitrate measurement in the PLACEHOLDER state.
    ///        Reflected into metrics once per second, like the equivalent logic in
    ///        encode_publish_thread_func().
    uint64_t              placeholder_frames_since_last_ = 0;
    uint64_t              placeholder_bytes_since_last_  = 0;
    time_utils::Stopwatch placeholder_metrics_sw_;

    /// @brief Timer measuring the minimum interval between leaving PLACEHOLDER and
    ///        returning to it via PROBING/CONNECTING_OUTPUT.
    ///        When the source answers probes but never starts sending frames
    ///        (CONNECTING_OUTPUT times out), this prevents the encoder/RTSP from being
    ///        re-initialized at a high rate in a PROBING <-> PLACEHOLDER loop.
    time_utils::Stopwatch placeholder_cooldown_timer_;
    bool placeholder_cooldown_active_ = false;

    /// @brief Seamless PLACEHOLDER <-> STREAMING handoff flag.
    ///
    ///        When true, encoder_ and rtsp_client_ are already initialized; if the
    ///        resolution matches, teardown/reinit is skipped and the video source is
    ///        switched while the RTSP session is kept.
    ///        This avoids a PTS reset and re-buffering on the viewer side.
    ///
    ///        Set in:
    ///          - handle_stalled(): on sender lost -> PLACEHOLDER
    ///          - handle_placeholder(): on source detected -> CONNECTING_OUTPUT
    ///        Cleared in:
    ///          - the init block of handle_placeholder()
    ///          - handle_connecting_output(), after the resolution check
    bool seamless_handoff_ = false;

    /// @brief First frame obtained in handle_connecting_output().
    ///        Passed to encode_publish_thread_func() as the initial freeze frame so the
    ///        stream carries video even if the Spout sender stopped calling SendImage()
    ///        on a static scene. Released after use to save memory.
    ///
    /// @note Thread safety:
    ///       The main thread writes it before starting the encode thread
    ///       (start_encode_thread()); the encode thread reads it once via std::move right
    ///       after starting. If the encode thread writes it back (early return on an
    ///       RTSP/encode error), it sets rtsp_error_flag_/encode_error_flag_ and returns,
    ///       and the main thread reads it only after stop_encode_thread() (join).
    ///       The join gives a happens-before relation, so no explicit mutex is needed.
    FrameBuffer initial_frame_buf_;
    FrameMeta   initial_frame_meta_{};
};
