#include "app/supervisor.hpp"
#include "app/supervisor_logic.hpp"
#include "common/time_utils.hpp"
#include <algorithm>
#include <string>
#include <thread>
#include <chrono>
#include <stdexcept>

Supervisor::Supervisor() = default;
Supervisor::~Supervisor() {
    request_stop();
    if (encode_thread_.joinable()) encode_thread_.join();
    if (metrics_thread_.joinable()) metrics_thread_.join();
}

bool Supervisor::init(const AppConfig& config, std::string& error) {
    config_ = config;

    log_ = std::make_shared<LogSink>();
    if (!log_->init(config_.app.log_dir, config_.app.instance_name, error))
        return false;

    metrics_ = std::make_shared<MetricsStore>();
    metrics_->mark_session_start();
    metrics_->set_rtsp_url(config_.rtsp.url);

    auto monitor = std::make_shared<SpoutMonitor>();
    if (!monitor->init(error)) {
        log_->log_error("SPOUT_INIT_FAILED", error);
        return false;
    }
    spout_monitor_ = std::move(monitor);

    if (!config_.spout.prefer_dx11)
        log_->log_event(spdlog::level::warn, "prefer_dx11_ignored",
                        {{"note", "SpoutDX always uses DirectX 11"}});

    state_machine_.on_transition([this](PublisherState from, PublisherState to) {
        std::string sf = state_name(from);
        std::string st = state_name(to);
        log_->log_state_changed(sf, st);
        metrics_->set_state(st);
    });

    rtsp_backoff_.reset(config_.rtsp.reconnect_delay_ms);
    return true;
}

void Supervisor::run() {
    state_machine_.transition_to(PublisherState::IDLE);

    // Start background metrics writer thread
    metrics_thread_ = std::thread(&Supervisor::metrics_writer_thread_func, this);

    while (!shutdown_requested_.load()) {
        switch (state_machine_.current_state()) {
            case PublisherState::IDLE:               handle_idle();               break;
            case PublisherState::PROBING:            handle_probing();            break;
            case PublisherState::PLACEHOLDER:        handle_placeholder();        break;
            case PublisherState::CONNECTING_OUTPUT:  handle_connecting_output();  break;
            case PublisherState::STREAMING:          handle_streaming();          break;
            case PublisherState::STALLED:            handle_stalled();            break;
            case PublisherState::RECONFIGURING:      handle_reconfiguring();      break;
            case PublisherState::RECONNECTING_OUTPUT:handle_reconnecting_output();break;
            case PublisherState::RECOVERING_DEVICE:  handle_recovering_device();  break;
            case PublisherState::STOPPING:           handle_stopping();           break;
            case PublisherState::FATAL:
                log_->log_event(spdlog::level::critical, "fatal_exit", {});
                log_->flush();
                stop_encode_thread();
                if (frame_pump_) { frame_pump_->stop(); frame_pump_.reset(); }
                teardown_rtsp();
                teardown_encoder();
                shutdown_requested_.store(true);
                break;
            default:
                break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    // Graceful shutdown
    if (state_machine_.current_state() != PublisherState::FATAL) {
        state_machine_.transition_to(PublisherState::STOPPING);
        handle_stopping();
    }

    if (metrics_thread_.joinable()) metrics_thread_.join();
    log_->flush();
}

void Supervisor::request_stop() {
    shutdown_requested_.store(true);
}

// ---- State handlers --------------------------------------------------------

void Supervisor::handle_idle() {
    // Wait briefly, then start probing
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    if (shutdown_requested_.load()) {
        state_machine_.transition_to(PublisherState::STOPPING);
        return;
    }
    probe_timer_.reset();
    state_machine_.transition_to(PublisherState::PROBING);
}

void Supervisor::handle_probing() {
    if (shutdown_requested_.load()) {
        state_machine_.transition_to(PublisherState::STOPPING);
        return;
    }

    bool found = spout_monitor_->probe_sender(config_.spout.sender_name);

    if (found) {
        std::string connect_err;
        if (!spout_monitor_->connect(config_.spout.sender_name, connect_err)) {
            log_->log_error("SPOUT_CONNECT_FAILED", connect_err);
        } else {
            log_->log_event(spdlog::level::info, "sender_found",
                            {{"name", config_.spout.sender_name}});
            state_machine_.transition_to(PublisherState::CONNECTING_OUTPUT);
            return;
        }
    }

    // Log a "still searching" message once per sender_missing_timeout_ms * 10 ms
    static constexpr int kProbeLogIntervalMultiplier = 10;
    if (probe_timer_.expired(
            static_cast<int64_t>(config_.spout.sender_missing_timeout_ms)
            * kProbeLogIntervalMultiplier)) {
        probe_timer_.reset();
        // Stay in PROBING but log periodically
        log_->log_event(spdlog::level::debug, "probe_no_sender",
                        {{"name", config_.spout.sender_name}});
    }

    // While no source is found, transition to PLACEHOLDER if streaming the
    // placeholder (NO SIGNAL) video is enabled.
    if (config_.placeholder.enabled) {
        // Anti-flicker: right after leaving PLACEHOLDER (e.g. CONNECTING_OUTPUT timed
        // out waiting for the first frame and returned to PROBING), suppress re-entering
        // PLACEHOLDER for reconnect_delay_ms so the encoder/RTSP are not
        // re-initialized at a high rate.
        if (placeholder_cooldown_active_ &&
            !placeholder_cooldown_timer_.expired(config_.rtsp.reconnect_delay_ms)) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(config_.spout.poll_interval_ms));
            return;
        }
        placeholder_cooldown_active_ = false;
        state_machine_.transition_to(PublisherState::PLACEHOLDER);
        return;
    }

    std::this_thread::sleep_for(
        std::chrono::milliseconds(config_.spout.poll_interval_ms));
}

void Supervisor::handle_placeholder() {
    if (shutdown_requested_.load()) {
        teardown_streaming();
        state_machine_.transition_to(PublisherState::STOPPING);
        return;
    }

    if (!placeholder_active_) {
        // Prefer the resolution of the most recently connected real source, if known.
        // This avoids an RTSP reconfiguration when the source returns at the same resolution.
        uint32_t w = last_source_width_  ? last_source_width_  : static_cast<uint32_t>(config_.placeholder.width);
        uint32_t h = last_source_height_ ? last_source_height_ : static_cast<uint32_t>(config_.placeholder.height);

        placeholder_frame_ = render_placeholder_frame(config_.placeholder, config_.spout.sender_name, w, h);

        // Validate the rendered result.
        // render_placeholder_frame() returns a FrameBuffer with empty data and
        // width/height = 0 if the resolution is 0x0 or over the limit.
        // Passing metadata inconsistent with the buffer to the encoder would crash,
        // so on failure stay in PLACEHOLDER and retry on the next loop.
        if (placeholder_frame_.data.empty() ||
            placeholder_frame_.width != w || placeholder_frame_.height != h) {
            log_->log_event(spdlog::level::err, "placeholder_render_failed",
                            {{"width", std::to_string(w)}, {"height", std::to_string(h)}});
            std::this_thread::sleep_for(
                std::chrono::milliseconds(config_.rtsp.reconnect_delay_ms));
            return;
        }

        placeholder_meta_  = FrameMeta{};
        placeholder_meta_.width  = w;
        placeholder_meta_.height = h;
        placeholder_content_changed_ = true;

        // Decide whether a seamless handoff is possible:
        // if seamless_handoff_ is true, encoder_/RTSP are alive and the resolution
        // matches, skip teardown/reinit and keep the RTSP session.
        bool can_seamless = supervisor_logic::can_seamless_handoff(
            seamless_handoff_,
            static_cast<bool>(encoder_),
            rtsp_client_ && rtsp_client_->is_connected(),
            w, h, current_width_, current_height_);
        seamless_handoff_ = false;

        if (can_seamless) {
            // Send the first placeholder frame as an IDR so viewers
            // can keep playing.
            encoder_->request_next_idr();
            log_->log_event(spdlog::level::info, "placeholder_seamless",
                            {{"name",   config_.spout.sender_name},
                             {"width",  std::to_string(w)},
                             {"height", std::to_string(h)}});
        } else {
            std::string init_err;
            if (!init_encoder_and_rtsp(w, h, init_err)) {
                teardown_rtsp();
                teardown_encoder();
                // Stay in PLACEHOLDER and retry next time
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(config_.rtsp.reconnect_delay_ms));
                return;
            }
            log_->log_publish_started(config_.rtsp.url,
                static_cast<int>(w), static_cast<int>(h), config_.encoder.fps);
            log_->log_event(spdlog::level::info, "placeholder_started",
                            {{"name", config_.spout.sender_name}});
        }

        current_width_  = w;
        current_height_ = h;

        placeholder_active_ = true;
        placeholder_frame_timer_.reset();
        placeholder_probe_timer_.reset();
        placeholder_frames_since_last_ = 0;
        placeholder_bytes_since_last_  = 0;
        placeholder_metrics_sw_.reset();
    }

    // Check every poll_interval_ms whether the real source has returned
    if (placeholder_probe_timer_.expired(config_.spout.poll_interval_ms)) {
        placeholder_probe_timer_.reset();
        if (spout_monitor_->probe_sender(config_.spout.sender_name)) {
            std::string connect_err;
            if (spout_monitor_->connect(config_.spout.sender_name, connect_err)) {
                log_->log_event(spdlog::level::info, "placeholder_source_found",
                                {{"name", config_.spout.sender_name}});
                // Seamless handoff: if encoder_+RTSP are alive, keep them and move to
                // CONNECTING_OUTPUT. The resolution check happens after the first frame
                // is received (handle_connecting_output).
                if (encoder_ && rtsp_client_ && rtsp_client_->is_connected()) {
                    seamless_handoff_ = true;
                } else {
                    teardown_rtsp();
                    teardown_encoder();
                }
                placeholder_active_ = false;
                // Start the timer that rate-limits re-entering PLACEHOLDER right after leaving it.
                // Even if CONNECTING_OUTPUT times out waiting for the first frame and returns
                // via PROBING, re-initialization is suppressed for reconnect_delay_ms.
                placeholder_cooldown_timer_.reset();
                placeholder_cooldown_active_ = true;
                state_machine_.transition_to(PublisherState::CONNECTING_OUTPUT);
                return;
            }
            log_->log_error("SPOUT_CONNECT_FAILED", connect_err);
        }
    }

    // Re-encode and send the placeholder frame at the configured fps
    const int64_t frame_interval_us = 1'000'000LL / config_.encoder.fps;
    if (placeholder_frame_timer_.elapsed_us() < frame_interval_us) {
        // If the send interval has not elapsed, do nothing and return.
        // The fixed sleep at the end of the run() loop already spaces calls, so an
        // extra sleep here would accumulate and lower the effective fps.
        return;
    }
    placeholder_frame_timer_.reset();
    placeholder_meta_.timestamp_us = time_utils::now_us();

    // Still image: from the second time on, skip sws_scale with content_changed=false
    std::vector<EncodedPacket> packets;
    if (!encoder_->encode(placeholder_frame_, placeholder_meta_, packets, placeholder_content_changed_)) {
        log_->log_error("ENCODER_ENCODE_FAILED", "placeholder encode() returned false");
        teardown_rtsp();
        teardown_encoder();
        placeholder_active_ = false;
        std::this_thread::sleep_for(std::chrono::milliseconds(config_.rtsp.reconnect_delay_ms));
        return;
    }
    placeholder_content_changed_ = false;

    for (auto& pkt : packets) {
        if (!rtsp_client_->send_packet(pkt)) {
            log_->log_error("RTSP_SEND_FAILED", "placeholder send_packet failed");
            teardown_rtsp();
            teardown_encoder();
            placeholder_active_ = false;
            // Reset metrics so they do not mix with the next encode_publish_thread_func
            placeholder_frames_since_last_ = 0;
            placeholder_bytes_since_last_  = 0;
            placeholder_metrics_sw_.reset();
            std::this_thread::sleep_for(std::chrono::milliseconds(config_.rtsp.reconnect_delay_ms));
            return;
        }
        placeholder_bytes_since_last_ += pkt.data.size();
    }

    if (!packets.empty()) {
        metrics_->increment_frames_encoded();
        placeholder_frames_since_last_++;

        // Update fps / bitrate_kbps once per second (same as encode_publish_thread_func)
        int64_t elapsed_ms = placeholder_metrics_sw_.elapsed_ms();
        if (elapsed_ms >= 1000) {
            double fps  = placeholder_frames_since_last_ * 1000.0 / elapsed_ms;
            double kbps = placeholder_bytes_since_last_  * 8.0   / elapsed_ms;
            metrics_->set_current_fps(fps);
            metrics_->set_bitrate_kbps(kbps);
            placeholder_frames_since_last_ = 0;
            placeholder_bytes_since_last_  = 0;
            placeholder_metrics_sw_.reset();
        }
    }
}

void Supervisor::handle_connecting_output() {
    if (shutdown_requested_.load()) {
        teardown_streaming();
        state_machine_.transition_to(PublisherState::STOPPING);
        return;
    }

    // Receive first frame to know dimensions
    FrameBuffer buf;
    FrameMeta   meta;
    bool is_new = false;
    time_utils::Stopwatch wait;
    bool source_responded = false;

    while (!is_new && !shutdown_requested_.load()) {
        bool ok = spout_monitor_->receive_latest_frame(buf, meta, is_new);
        if (ok) {
            if (is_new) break;
            if (supervisor_logic::should_reset_connect_timer_once(source_responded)) {
                wait.reset();
            }
        }
        if (wait.expired(static_cast<int64_t>(config_.spout.frame_timeout_ms) * 5)) {
            log_->log_error("SPOUT_RECEIVE_FAILED", "Timeout waiting for first frame");
            spout_monitor_->disconnect();
            state_machine_.transition_to(PublisherState::PROBING);
            return;
        }
        std::this_thread::sleep_for(
            std::chrono::milliseconds(config_.spout.poll_interval_ms));
    }

    if (shutdown_requested_.load()) {
        state_machine_.transition_to(PublisherState::STOPPING);
        return;
    }

    // Save the first frame as the encode thread's initial freeze frame.
    // If the Spout sender stops calling SendImage() on a static scene, FramePump does
    // not queue frames. Starting encode_publish_thread_func() with this saved frame
    // lets video go out immediately even when the sender is static.
    initial_frame_buf_  = std::move(buf);
    initial_frame_meta_ = meta;

    uint32_t source_w = meta.width;
    uint32_t source_h = meta.height;

    // Record the resolution of the most recently connected real source. Preferring it
    // in PLACEHOLDER avoids an RTSP reconfiguration when the source returns.
    last_source_width_  = source_w;
    last_source_height_ = source_h;

    // Decide whether a seamless handoff is possible:
    // if seamless_handoff_ is true, encoder_/RTSP are alive and the resolution
    // matches, skip teardown/reinit and keep the RTSP session.
    bool can_seamless = supervisor_logic::can_seamless_handoff(
        seamless_handoff_,
        static_cast<bool>(encoder_),
        rtsp_client_ && rtsp_client_->is_connected(),
        source_w, source_h, current_width_, current_height_);
    seamless_handoff_ = false;

    if (can_seamless) {
        // Switch seamlessly from the placeholder to the real source.
        // Request an IDR frame so viewers can keep playing.
        current_width_  = source_w;
        current_height_ = source_h;
        encoder_->request_next_idr();
        log_->log_event(spdlog::level::info, "source_seamless_start",
                        {{"name",   config_.spout.sender_name},
                         {"width",  std::to_string(source_w)},
                         {"height", std::to_string(source_h)}});
    } else {
        // Resolution mismatch or first start: tear down any existing encoder_/RTSP and re-init
        if (encoder_)      teardown_encoder();
        if (rtsp_client_)  teardown_rtsp();
        current_width_  = source_w;
        current_height_ = source_h;
        std::string init_err;
        if (!init_encoder_and_rtsp(current_width_, current_height_, init_err)) {
            teardown_rtsp();
            teardown_encoder();
            spout_monitor_->disconnect();
            state_machine_.transition_to(PublisherState::PROBING);
            return;
        }
        log_->log_publish_started(config_.rtsp.url,
            static_cast<int>(current_width_), static_cast<int>(current_height_),
            config_.encoder.fps);

        // The receive path (GPU ReceiveTexture vs CPU ReceiveImage) was just decided by
        // set_gpu_mode(), after the frame-wait loop had already received with the old mode.
        // disconnect()+connect() must run here so the chosen path re-initializes; otherwise
        // the other path may have consumed spoutDX's update flag and receive_latest_frame()
        // would fail forever (STREAMING -> STALLED loop).
        //
        // disconnect() frees spoutDX's internal texture, so a GPU-path pointer held in
        // initial_frame_buf_ would dangle (use-after-free); discard it after disconnect.
        std::string reset_err;
        spout_monitor_->disconnect();
        initial_frame_buf_.gpu_texture = nullptr;
        if (!spout_monitor_->connect(config_.spout.sender_name, reset_err)) {
            log_->log_error("SPOUT_RECONNECT_FAILED", reset_err);
            teardown_rtsp();
            teardown_encoder();
            state_machine_.transition_to(PublisherState::PROBING);
            return;
        }
    }

    metrics_->set_sender_info(
        config_.spout.sender_name, current_width_, current_height_,
        spout_monitor_->get_sender_info().fps);

    // Start frame pump
    frame_pump_ = std::make_unique<FramePump>();
    frame_pump_->start(spout_monitor_, config_.spout.poll_interval_ms);

    last_frame_time_ms_.store(time_utils::now_ms());
    start_encode_thread();
    rtsp_backoff_.reset(config_.rtsp.reconnect_delay_ms);
    reconnect_attempts_ = 0;
    consecutive_stall_recoveries_ = 0;
    state_machine_.transition_to(PublisherState::STREAMING);
}

void Supervisor::handle_streaming() {
    if (shutdown_requested_.load()) {
        stop_encode_thread();
        teardown_streaming();
        state_machine_.transition_to(PublisherState::STOPPING);
        return;
    }

    // Check for device loss before encode_error_flag_.
    // On a TDR, encode() returns false and sets encode_error_flag_, but going
    // PROBING -> CONNECTING_OUTPUT without recreating the device would crash repeatedly,
    // so go through RECOVERING_DEVICE to recreate the device first.
    if (spout_monitor_ && spout_monitor_->is_device_removed()) {
        encode_error_flag_.exchange(false);  // clear the collateral flag
        stop_encode_thread();
        state_machine_.transition_to(PublisherState::RECOVERING_DEVICE);
        return;
    }

    if (encode_error_flag_.exchange(false)) {
        stop_encode_thread();
        teardown_streaming();
        state_machine_.transition_to(PublisherState::PROBING);
        return;
    }

    if (rtsp_error_flag_.exchange(false)) {
        stop_encode_thread();
        teardown_rtsp();
        state_machine_.transition_to(PublisherState::RECONNECTING_OUTPUT);
        return;
    }

    if (resolution_changed_flag_.exchange(false)) {
        stop_encode_thread();
        current_width_  = pending_width_.load();
        current_height_ = pending_height_.load();
        state_machine_.transition_to(PublisherState::RECONFIGURING);
        return;
    }

    // Detect Spout source disconnection.
    // On a static scene no new frames arrive but ReceiveImage itself succeeds, so
    // last_source_alive_ms() keeps updating. Lack of new frames (static) alone does
    // not cause STALLED; only a completely unresponsive source does.
    if (frame_pump_) {
        int64_t elapsed = time_utils::now_ms() - frame_pump_->last_source_alive_ms();
        if (elapsed > config_.spout.frame_timeout_ms) {
            log_->log_event(spdlog::level::warn, "source_disconnected_detected", {});
            stall_timer_.reset();
            state_machine_.transition_to(PublisherState::STALLED);
            return;
        }
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(10));
}

void Supervisor::handle_stalled() {
    if (shutdown_requested_.load()) {
        stop_encode_thread();
        teardown_streaming();
        state_machine_.transition_to(PublisherState::STOPPING);
        return;
    }

    // Also check for device loss while STALLED.
    // A TDR during STALLED also recovers via RECOVERING_DEVICE.
    if (spout_monitor_ && spout_monitor_->is_device_removed()) {
        encode_error_flag_.exchange(false);
        stop_encode_thread();
        state_machine_.transition_to(PublisherState::RECOVERING_DEVICE);
        return;
    }

    // Return to STREAMING if the source has returned
    if (frame_pump_) {
        int64_t elapsed = time_utils::now_ms() - frame_pump_->last_source_alive_ms();
        if (elapsed < config_.spout.frame_timeout_ms) {
            log_->log_event(spdlog::level::info, "stall_recovered", {});
            // Frame reception actually recovered, so clear the safety-net watchdog counter
            // (an RTSP-only reconnect does not recover it). The counter is also reset when
            // handle_connecting_output() transitions normally to STREAMING.
            consecutive_stall_recoveries_ = 0;
            state_machine_.transition_to(PublisherState::STREAMING);
            return;
        }
    }

    // Error flags from encode thread
    if (encode_error_flag_.exchange(false)) {
        stop_encode_thread();
        teardown_streaming();
        state_machine_.transition_to(PublisherState::PROBING);
        return;
    }
    if (rtsp_error_flag_.exchange(false)) {
        stop_encode_thread();
        teardown_rtsp();
        state_machine_.transition_to(PublisherState::RECONNECTING_OUTPUT);
        return;
    }

    // Sender disappeared check
    if (stall_timer_.expired(config_.spout.sender_missing_timeout_ms)) {
        if (!spout_monitor_->probe_sender(config_.spout.sender_name)) {
            log_->log_event(spdlog::level::warn, "sender_disappeared", {});
            stop_encode_thread();
            // Release Spout/FramePump. encoder_/rtsp_client_ are kept for the seamless
            // handoff to PLACEHOLDER, so release them individually rather than via
            // teardown_streaming().
            if (frame_pump_) { frame_pump_->stop(); frame_pump_.reset(); }
            spout_monitor_->disconnect();
            if (config_.placeholder.enabled) {
                // Going directly to PLACEHOLDER skips the IDLE->PROBING wait and
                // minimizes the video freeze time.
                // If encoder_+RTSP are alive, attempt a seamless handoff.
                seamless_handoff_ = encoder_ && rtsp_client_ &&
                                    rtsp_client_->is_connected();
                placeholder_cooldown_active_ = false;  // cancel the cooldown
                state_machine_.transition_to(PublisherState::PLACEHOLDER);
            } else {
                teardown_rtsp();
                teardown_encoder();
                state_machine_.transition_to(PublisherState::IDLE);
            }
            return;
        }
    }

    // Reconnect RTSP after prolonged stall
    if (stall_timer_.expired(
            static_cast<int64_t>(config_.rtsp.reconnect_delay_ms) * 3)) {
        stop_encode_thread();
        // If the placeholder is enabled and the sender is gone, hand off seamlessly
        // to PLACEHOLDER instead of RECONNECTING_OUTPUT.
        if (config_.placeholder.enabled &&
            !spout_monitor_->probe_sender(config_.spout.sender_name)) {
            log_->log_event(spdlog::level::warn, "sender_disappeared", {});
            if (frame_pump_) { frame_pump_->stop(); frame_pump_.reset(); }
            spout_monitor_->disconnect();
            seamless_handoff_ = encoder_ && rtsp_client_ &&
                                rtsp_client_->is_connected();
            placeholder_cooldown_active_ = false;
            state_machine_.transition_to(PublisherState::PLACEHOLDER);
            return;
        }

        // probe_sender() confirms the sender is alive, so reaching here can mean an
        // abnormal deadlock where an RTSP-only reconnect does not restore
        // frame_pump_ reception (the main cause was fixed permanently in
        // handle_connecting_output(); this stays as insurance against unknown similar causes).
        // If RTSP reconnects keep failing to reach stall_recovered for a set number of
        // attempts, force a full reconnect including the Spout receiver.
        ++consecutive_stall_recoveries_;
        if (supervisor_logic::should_force_spout_recovery(
                consecutive_stall_recoveries_,
                config_.spout.stalled_recovery_max_attempts)) {
            log_->log_event(spdlog::level::warn, "stalled_recovery_forced",
                            {{"attempts", std::to_string(consecutive_stall_recoveries_)}});
            consecutive_stall_recoveries_ = 0;
            if (frame_pump_) { frame_pump_->stop(); frame_pump_.reset(); }
            spout_monitor_->disconnect();
            teardown_rtsp();
            teardown_encoder();
            state_machine_.transition_to(PublisherState::PROBING);
        } else {
            teardown_rtsp();
            state_machine_.transition_to(PublisherState::RECONNECTING_OUTPUT);
        }
        return;
    }

    std::this_thread::sleep_for(
        std::chrono::milliseconds(config_.spout.poll_interval_ms));
}

void Supervisor::handle_recovering_device() {
    if (shutdown_requested_.load()) {
        teardown_streaming();
        state_machine_.transition_to(PublisherState::STOPPING);
        return;
    }

    log_->log_event(spdlog::level::warn, "gpu_device_lost", {});

    // Tear down the encode thread, FramePump, RTSP and encoder in order.
    // teardown_encoder() calls set_gpu_mode(false), which prevents a
    // use-after-free of the GPU texture pointer.
    teardown_streaming();

    std::string reinit_err;
    if (!spout_monitor_->reinit_device(reinit_err)) {
        log_->log_error("SPOUT_REINIT_FAILED", reinit_err);
        state_machine_.transition_to(PublisherState::FATAL);
        return;
    }

    // Device recreation succeeded. Reconnect the sender from PROBING and re-initialize
    // the encoder with the new device in CONNECTING_OUTPUT.
    metrics_->increment_device_lost_recoveries();
    log_->log_event(spdlog::level::info, "gpu_device_reinit_ok", {});
    state_machine_.transition_to(PublisherState::PROBING);
}

void Supervisor::handle_reconfiguring() {
    if (shutdown_requested_.load()) {
        teardown_streaming();
        state_machine_.transition_to(PublisherState::STOPPING);
        return;
    }

    // Stop frame pump while we reinitialise
    if (frame_pump_) frame_pump_->stop();
    teardown_rtsp();
    teardown_encoder();

    std::string init_err;
    if (!init_encoder_and_rtsp(current_width_, current_height_, init_err)) {
        teardown_rtsp();
        teardown_encoder();
        spout_monitor_->disconnect();
        state_machine_.transition_to(PublisherState::PROBING);
        return;
    }

    // Update the last source resolution on a resolution change too
    last_source_width_  = current_width_;
    last_source_height_ = current_height_;

    frame_pump_->start(spout_monitor_, config_.spout.poll_interval_ms);
    last_frame_time_ms_.store(time_utils::now_ms());
    start_encode_thread();
    // Announce the restart after a resolution change via publish_started.
    // As in handle_connecting_output(), this lets tests/monitoring detect it.
    log_->log_publish_started(config_.rtsp.url,
        static_cast<int>(current_width_), static_cast<int>(current_height_),
        config_.encoder.fps);
    state_machine_.transition_to(PublisherState::STREAMING);
}

void Supervisor::handle_reconnecting_output() {
    if (shutdown_requested_.load()) {
        teardown_streaming();
        state_machine_.transition_to(PublisherState::STOPPING);
        return;
    }

    ++reconnect_attempts_;
    metrics_->increment_reconnect_attempts();

    if (reconnect_attempts_ > config_.rtsp.max_reconnect_attempts) {
        log_->log_error("RTSP_TIMEOUT",
                        "Exceeded max reconnect attempts (" +
                        std::to_string(config_.rtsp.max_reconnect_attempts) + ")");
        teardown_streaming();
        state_machine_.transition_to(PublisherState::FATAL);
        return;
    }

    int delay = rtsp_backoff_.next_delay(
        config_.rtsp.reconnect_max_delay_ms,
        config_.rtsp.reconnect_backoff_multiplier);

    log_->log_reconnect(reconnect_attempts_, delay);

    std::this_thread::sleep_for(std::chrono::milliseconds(delay));

    if (shutdown_requested_.load()) {
        teardown_streaming();
        state_machine_.transition_to(PublisherState::STOPPING);
        return;
    }

    std::string rtsp_err;
    rtsp_client_ = std::make_unique<RtspPublisherClient>();
    auto codec_info = encoder_->get_codec_info();
    if (!rtsp_client_->connect(config_.rtsp, codec_info, rtsp_err)) {
        log_->log_error("RTSP_CONNECT_FAILED", rtsp_err);
        teardown_rtsp();
        // Stay in RECONNECTING_OUTPUT by re-entering next loop iteration
        return;
    }

    // Reset backoff on success
    rtsp_backoff_.reset(config_.rtsp.reconnect_delay_ms);
    reconnect_attempts_ = 0;
    last_frame_time_ms_.store(time_utils::now_ms());
    // Send the first frame after reconnecting as an IDR.
    // It is inserted into the GOP without resetting the encoder, so PTS stays continuous.
    // This lets the receiver show video without a black screen right after connecting.
    if (encoder_) encoder_->request_next_idr();
    start_encode_thread();
    state_machine_.transition_to(PublisherState::STREAMING);
}

void Supervisor::handle_stopping() {
    teardown_streaming();
    // Wait for any in-flight operations to settle (L-03)
    if (config_.runtime.shutdown_grace_ms > 0)
        std::this_thread::sleep_for(
            std::chrono::milliseconds(config_.runtime.shutdown_grace_ms));
    log_->log_event(spdlog::level::info, "stopped", {});
    log_->flush();
    if (state_machine_.can_transition(PublisherState::IDLE))
        state_machine_.transition_to(PublisherState::IDLE);
}

// ---- Init helpers -----------------------------------------------------------

bool Supervisor::init_encoder_and_rtsp(uint32_t width, uint32_t height, std::string& error) {
    encoder_ = std::make_unique<EncoderController>();

    // Try the GPU zero-copy path: pass SpoutMonitor's D3D11 device and the sender's
    // actual DXGI texture format.
    // Hard-coding BGRA would make CopySubresourceRegion fail silently on a format
    // mismatch for RGBA senders (Unity/VRChat etc.) and always show black, so the
    // actual format must be fetched dynamically and passed.
    void* gpu_dev = spout_monitor_ ? spout_monitor_->gpu_device() : nullptr;
    const uint32_t sender_fmt = spout_monitor_ ? spout_monitor_->get_sender_dxgi_format() : 0;
    if (!encoder_->init(config_.encoder, width, height, error, gpu_dev, sender_fmt)) {
        log_->log_error("ENCODER_INIT_FAILED", error);
        return false;
    }

    // If the GPU path was established, switch SpoutMonitor to GPU texture mode
    if (encoder_->gpu_path_active()) {
        spout_monitor_->set_gpu_mode(true);
        log_->log_event(spdlog::level::info, "gpu_zero_copy_enabled",
                        {{"codec", config_.encoder.codec}});
    } else {
        spout_monitor_->set_gpu_mode(false);
        log_->log_event(spdlog::level::info, "gpu_zero_copy_disabled",
                        {{"codec", config_.encoder.codec}});
    }

    log_->log_encoder_initialized(
        config_.encoder.codec,
        static_cast<int>(width), static_cast<int>(height),
        config_.encoder.fps, config_.encoder.bitrate_kbps);
    metrics_->set_encoder_codec(config_.encoder.codec);

    rtsp_client_ = std::make_unique<RtspPublisherClient>();
    auto codec_info = encoder_->get_codec_info();
    if (!rtsp_client_->connect(config_.rtsp, codec_info, error)) {
        log_->log_error("RTSP_CONNECT_FAILED", error);
        return false;
    }
    return true;
}

// ---- Cleanup helpers -------------------------------------------------------

void Supervisor::teardown_streaming() {
    stop_encode_thread();
    if (frame_pump_) { frame_pump_->stop(); frame_pump_.reset(); }
    teardown_rtsp();
    teardown_encoder();
}

void Supervisor::start_encode_thread() {
    encode_stop_.store(false);
    encode_error_flag_.store(false);
    rtsp_error_flag_.store(false);
    resolution_changed_flag_.store(false);
    encode_thread_ = std::thread(&Supervisor::encode_publish_thread_func, this);
}

void Supervisor::stop_encode_thread() {
    encode_stop_.store(true);
    if (encode_thread_.joinable()) encode_thread_.join();
}

void Supervisor::encode_publish_thread_func() {
    time_utils::Stopwatch metrics_sw;
    uint64_t bytes_since_last  = 0;
    uint64_t frames_since_last = 0;

    // Freeze-frame cache for static scenes.
    // Spout sends no new frames when the screen does not change, so the last received
    // frame is re-encoded repeatedly at the configured fps to keep the RTSP stream alive.
    //
    // Initializing with the first frame obtained by handle_connecting_output() lets
    // video go out immediately even if the sender is static right after connecting
    // (SendImage stopped).
    FrameBuffer freeze_buf  = std::move(initial_frame_buf_);
    FrameMeta   freeze_meta = initial_frame_meta_;
    // On the GPU path buf.data is empty and only gpu_texture is set.
    // Either one being valid makes it a usable freeze frame.
    bool        has_freeze  = !freeze_buf.data.empty() || freeze_buf.gpu_texture != nullptr;
    initial_frame_meta_ = {};  // release memory (already copied)
    bool        content_changed = true;  // for the sws_scale skip decision

    // Frame send interval from the configured fps
    const int64_t frame_interval_us = 1'000'000LL / config_.encoder.fps;
    time_utils::Stopwatch frame_sw;

    while (!encode_stop_.load()) {
        int64_t wait_us = frame_interval_us - frame_sw.elapsed_us();
        int wait_ms = (wait_us > 0)
            ? std::min(static_cast<int>(wait_us / 1000) + 1,
                       config_.spout.poll_interval_ms * 2)
            : 0;

        FrameBuffer buf;
        FrameMeta   meta;
        bool got_frame = (wait_ms > 0)
            ? frame_pump_->wait_pop(buf, meta, wait_ms)
            : frame_pump_->try_pop(buf, meta);

        if (got_frame) {
            // New frame received: update the freeze buffer
            freeze_buf      = std::move(buf);
            freeze_meta     = meta;
            has_freeze      = true;
            content_changed = true;
            last_frame_time_ms_.store(time_utils::now_ms());
            metrics_->increment_frames_received();
            log_->log_frame_received(meta.sequence, meta.width, meta.height);

            // Resolution change
            if (supervisor_logic::resolution_changed(
                    freeze_meta.width, freeze_meta.height, current_width_, current_height_)) {
                pending_width_.store(freeze_meta.width);
                pending_height_.store(freeze_meta.height);
                // Save the new-resolution frame as the initial freeze frame when the thread restarts.
                // This lets the next encode thread start with has_freeze=true after
                // handle_reconfiguring() re-initializes the encoder at the new resolution.
                initial_frame_buf_  = std::move(freeze_buf);
                initial_frame_meta_ = freeze_meta;
                resolution_changed_flag_.store(true);
                return;
            }
        }

        // Skip if the send interval has not elapsed (also when a new frame arrived)
        if (frame_sw.elapsed_us() < frame_interval_us) {
            continue;
        }

        // The first frame has not arrived yet
        if (!has_freeze) {
            continue;
        }

        // Suppress freeze-frame sending while the source is disconnected.
        // The encode thread keeps running in STALLED, but stopping encode/send while
        // the source has not responded for frame_timeout_ms or longer avoids useless
        // CPU and bandwidth use. The RTSP client also learns of the disconnection.
        // Resumes automatically the moment the source returns (last_source_alive_ms updates).
        if (frame_pump_) {
            int64_t source_age_ms = time_utils::now_ms() - frame_pump_->last_source_alive_ms();
            if (source_age_ms > static_cast<int64_t>(config_.spout.frame_timeout_ms)) {
                frame_sw.reset();  // reset the timer so sending can happen right after recovery
                continue;
            }
        }

        // Reset the frame timer and unify timestamps to the current time
        frame_sw.reset();
        freeze_meta.timestamp_us = time_utils::now_us();

        // Encode. If content_changed is false, skip sws_scale and reuse the previous
        // converted YUV frame (reduces CPU load on static scenes)
        std::vector<EncodedPacket> packets;
        if (!encoder_->encode(freeze_buf, freeze_meta, packets, content_changed)) {
            log_->log_error("ENCODER_ENCODE_FAILED", "encode() returned false");
            metrics_->increment_frames_dropped();
            // Save the freeze frame. After an error the state goes PROBING ->
            // CONNECTING_OUTPUT, so handle_connecting_output() actually overwrites it,
            // but saving may let it serve as the initial frame on reconnect.
            initial_frame_buf_  = std::move(freeze_buf);
            initial_frame_meta_ = freeze_meta;
            encode_error_flag_.store(true);
            return;
        }
        // The YUV frame is consumed; no reconversion until the next new frame arrives
        content_changed = false;

        // Publish
        for (auto& pkt : packets) {
            if (!rtsp_client_->send_packet(pkt)) {
                log_->log_error("RTSP_SEND_FAILED", "send_packet failed");
                metrics_->increment_rtsp_errors();
                // After an RTSP reconnect, handle_reconnecting_output() only restarts the
                // encode thread and does not go through handle_connecting_output().
                // Saving the freeze frame guarantees has_freeze=true right after reconnecting
                // (prevents a black screen on static scenes).
                initial_frame_buf_  = std::move(freeze_buf);
                initial_frame_meta_ = freeze_meta;
                rtsp_error_flag_.store(true);
                return;
            }
        }

        if (!packets.empty()) {
            metrics_->increment_frames_encoded();
            frames_since_last++;
            for (const auto& pkt : packets) bytes_since_last += pkt.data.size();

            // H-03: Update bitrate/fps metrics every second
            int64_t elapsed_ms = metrics_sw.elapsed_ms();
            if (elapsed_ms >= 1000) {
                double fps  = frames_since_last * 1000.0 / elapsed_ms;
                double kbps = bytes_since_last  * 8.0   / elapsed_ms;
                metrics_->set_current_fps(fps);
                metrics_->set_bitrate_kbps(kbps);
                frames_since_last = 0;
                bytes_since_last  = 0;
                metrics_sw.reset();
            }
        }
    }

    // When the loop is exited externally via stop_encode_thread() (e.g. STALLED ->
    // RECONNECTING_OUTPUT), initial_frame_buf_ was already moved out at startup and is
    // empty, so the next start_encode_thread() would start with has_freeze=false
    // and show black. Save the last freeze frame on loop exit so it can be sent
    // immediately on restart.
    // (The early-return paths for resolution change, encode error and RTSP error already save it.)
    if (has_freeze) {
        initial_frame_buf_  = std::move(freeze_buf);
        initial_frame_meta_ = freeze_meta;
    }
}

void Supervisor::teardown_encoder() {
    if (encoder_) {
        std::vector<EncodedPacket> flush_pkts;
        encoder_->flush(flush_pkts);
        // Discard flush packets (stream is being torn down)
        encoder_->reset();
        encoder_.reset();
    }
    // If SpoutMonitor disconnects while a GPU texture pointer is held, it becomes
    // a use-after-free, so always switch back to the CPU path when tearing down the encoder.
    if (spout_monitor_) spout_monitor_->set_gpu_mode(false);
    // Also clear initial_frame_buf_ if it holds a GPU texture pointer
    initial_frame_buf_.gpu_texture = nullptr;
}

void Supervisor::teardown_rtsp() {
    if (rtsp_client_) {
        if (rtsp_client_->is_connected())
            log_->log_publish_stopped("teardown");
        rtsp_client_->disconnect();
        rtsp_client_.reset();
    }
}

// ---- Background metrics writer ---------------------------------------------

void Supervisor::metrics_writer_thread_func() {
    time_utils::Stopwatch metrics_sw;
    time_utils::Stopwatch health_sw;

    // Log write failures only when the failure starts or its cause changes (avoids per-interval log flooding)
    std::string metrics_last_error, health_last_error;
    auto save = [this](const char* target, const std::string& path,
                       std::string& last_error, bool is_metrics) {
        std::string error;
        bool ok = is_metrics ? metrics_->save_metrics(path, &error)
                             : metrics_->save_health(path, &error);
        if (!error.empty()) {
            if (error != last_error) {
                log_->log_error("STATE_FILE_WRITE_FAILED", error,
                                {{"target", target}, {"path", path}});
                last_error = error;
            }
        } else if (ok && !last_error.empty()) {
            log_->log_event(spdlog::level::info, "state_file_write_recovered",
                            {{"target", target}, {"path", path}});
            last_error.clear();
        }
    };

    while (!shutdown_requested_.load()) {
        if (metrics_sw.expired(config_.runtime.emit_metrics_interval_ms)) {
            save("metrics", config_.app.metrics_path, metrics_last_error, true);
            metrics_sw.reset();
        }
        if (health_sw.expired(config_.runtime.emit_health_interval_ms)) {
            save("health", config_.app.health_path, health_last_error, false);
            health_sw.reset();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // Final flush
    save("metrics", config_.app.metrics_path, metrics_last_error, true);
    save("health", config_.app.health_path, health_last_error, false);
}
