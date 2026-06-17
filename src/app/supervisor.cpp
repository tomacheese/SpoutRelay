#include "app/supervisor.hpp"
#include "app/supervisor_logic.hpp"
#include "common/time_utils.hpp"
#include <algorithm>
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

    // ソースが見つからない間、プレースホルダ (NO SIGNAL) 映像の配信が
    // 有効であれば PLACEHOLDER 状態へ遷移する。
    if (config_.placeholder.enabled) {
        // フリッカー対策: PLACEHOLDER を抜けた直後（CONNECTING_OUTPUT が
        // 初回フレーム待ちでタイムアウトし PROBING に戻ってきた場合等）は、
        // reconnect_delay_ms の間 PLACEHOLDER への再突入を抑制し、
        // エンコーダー/RTSP の再初期化が高頻度に繰り返されることを防ぐ。
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
        // 直近に接続した実ソースの解像度が分かっていれば優先する。
        // ソースが同解像度で復帰した際の RTSP 再構成を避けられる。
        uint32_t w = last_source_width_  ? last_source_width_  : static_cast<uint32_t>(config_.placeholder.width);
        uint32_t h = last_source_height_ ? last_source_height_ : static_cast<uint32_t>(config_.placeholder.height);

        placeholder_frame_ = render_placeholder_frame(config_.placeholder, config_.spout.sender_name, w, h);

        // 描画結果を検証する。
        // render_placeholder_frame() は解像度が 0x0 や上限超過の場合に
        // data が空・width/height = 0 の FrameBuffer を返す。
        // メタ情報とバッファ実体が不整合なままエンコーダに渡すとクラッシュするため、
        // 異常時は PLACEHOLDER 状態のまま次回ループでリトライする。
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

        // シームレス移行可能か判定:
        // seamless_handoff_ が true かつ encoder_/RTSP が生存しており解像度も一致
        // していれば、teardown/reinit をスキップして RTSP セッションを維持する。
        bool can_seamless = supervisor_logic::can_seamless_handoff(
            seamless_handoff_,
            static_cast<bool>(encoder_),
            rtsp_client_ && rtsp_client_->is_connected(),
            w, h, current_width_, current_height_);
        seamless_handoff_ = false;

        if (can_seamless) {
            // プレースホルダの先頭フレームを IDR として送出し、
            // 視聴者が再生を継続できるようにする。
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
                // PLACEHOLDER のまま次回リトライする
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

    // 実ソースが復帰したか poll_interval_ms ごとに確認する
    if (placeholder_probe_timer_.expired(config_.spout.poll_interval_ms)) {
        placeholder_probe_timer_.reset();
        if (spout_monitor_->probe_sender(config_.spout.sender_name)) {
            std::string connect_err;
            if (spout_monitor_->connect(config_.spout.sender_name, connect_err)) {
                log_->log_event(spdlog::level::info, "placeholder_source_found",
                                {{"name", config_.spout.sender_name}});
                // シームレス移行: encoder_+RTSP が生存していれば維持したまま
                // CONNECTING_OUTPUT へ遷移する。解像度チェックは最初のフレームを
                // 受け取ってから行う (handle_connecting_output)。
                if (encoder_ && rtsp_client_ && rtsp_client_->is_connected()) {
                    seamless_handoff_ = true;
                } else {
                    teardown_rtsp();
                    teardown_encoder();
                }
                placeholder_active_ = false;
                // PLACEHOLDER を抜けた直後の再突入をクールダウンするためのタイマーを開始する。
                // CONNECTING_OUTPUT が初回フレーム待ちでタイムアウトし PROBING 経由で
                // 戻ってきた場合でも、reconnect_delay_ms の間は再初期化を抑制する。
                placeholder_cooldown_timer_.reset();
                placeholder_cooldown_active_ = true;
                state_machine_.transition_to(PublisherState::CONNECTING_OUTPUT);
                return;
            }
            log_->log_error("SPOUT_CONNECT_FAILED", connect_err);
        }
    }

    // 設定 fps に基づいてプレースホルダフレームを再エンコード・送出する
    const int64_t frame_interval_us = 1'000'000LL / config_.encoder.fps;
    if (placeholder_frame_timer_.elapsed_us() < frame_interval_us) {
        // 送信間隔未満の場合は何もせず return する。
        // run() ループ末尾の固定 sleep により次回呼び出しまでの間隔が確保されるため、
        // ここで追加の sleep を行うと sleep が積み重なり実効 fps が低下してしまう。
        return;
    }
    placeholder_frame_timer_.reset();
    placeholder_meta_.timestamp_us = time_utils::now_us();

    // 静止画のため 2 回目以降は content_changed=false で sws_scale をスキップする
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
            // メトリクスをリセットして次回の encode_publish_thread_func と混在しないようにする
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

        // 1 秒ごとに fps / bitrate_kbps を更新する（encode_publish_thread_func と同じ方式）
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

    // 最初のフレームをエンコードスレッドの初期フリーズフレームとして保存する。
    // Spout 送信側が静止画面のまま SendImage() を止めると FramePump はフレームを
    // キューに積まない。encode_publish_thread_func() がこの保存フレームで起動
    // することで、送信側が静止していても直ちに映像を送れるようになる。
    initial_frame_buf_  = std::move(buf);
    initial_frame_meta_ = meta;

    uint32_t source_w = meta.width;
    uint32_t source_h = meta.height;

    // 直近に接続した実ソースの解像度を記録する。PLACEHOLDER 状態で
    // この解像度を優先することで、ソース復帰時の RTSP 再構成を避ける。
    last_source_width_  = source_w;
    last_source_height_ = source_h;

    // シームレス移行可能か判定:
    // seamless_handoff_ が true かつ encoder_/RTSP が生存しており解像度も一致
    // していれば、teardown/reinit をスキップして RTSP セッションを維持する。
    bool can_seamless = supervisor_logic::can_seamless_handoff(
        seamless_handoff_,
        static_cast<bool>(encoder_),
        rtsp_client_ && rtsp_client_->is_connected(),
        source_w, source_h, current_width_, current_height_);
    seamless_handoff_ = false;

    if (can_seamless) {
        // プレースホルダから実ソースへシームレスに切り替える。
        // IDR フレームを要求して視聴者が再生を継続できるようにする。
        current_width_  = source_w;
        current_height_ = source_h;
        encoder_->request_next_idr();
        log_->log_event(spdlog::level::info, "source_seamless_start",
                        {{"name",   config_.spout.sender_name},
                         {"width",  std::to_string(source_w)},
                         {"height", std::to_string(source_h)}});
    } else {
        // 解像度不一致か初回起動: 既存の encoder_/RTSP があれば解体して再初期化
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
    state_machine_.transition_to(PublisherState::STREAMING);
}

void Supervisor::handle_streaming() {
    if (shutdown_requested_.load()) {
        stop_encode_thread();
        teardown_streaming();
        state_machine_.transition_to(PublisherState::STOPPING);
        return;
    }

    // encode_error_flag_ より前にデバイスロストを確認する。
    // TDR 発生時は encode() が false を返して encode_error_flag_ がセットされるが、
    // デバイスを再作成せずに PROBING → CONNECTING_OUTPUT してもクラッシュが繰り返すため、
    // RECOVERING_DEVICE 状態を経由してデバイスを再作成してから復帰する。
    if (spout_monitor_ && spout_monitor_->is_device_removed()) {
        encode_error_flag_.exchange(false);  // 巻き添えフラグをクリア
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

    // Spout ソース切断検出。
    // 静止画面では新規フレームが来ないが ReceiveImage 自体は成功するため
    // last_source_alive_ms() は更新される。新規フレームが来ない（静止）だけでは
    // STALLED に遷移せず、ソースが完全に応答しなくなった場合のみ遷移する。
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

    // STALLED 中もデバイスロストを確認する。
    // TDR が STALLED 中に発生した場合も RECOVERING_DEVICE を経由して回復する。
    if (spout_monitor_ && spout_monitor_->is_device_removed()) {
        encode_error_flag_.exchange(false);
        stop_encode_thread();
        state_machine_.transition_to(PublisherState::RECOVERING_DEVICE);
        return;
    }

    // ソースが復帰した場合は STREAMING に戻る
    if (frame_pump_) {
        int64_t elapsed = time_utils::now_ms() - frame_pump_->last_source_alive_ms();
        if (elapsed < config_.spout.frame_timeout_ms) {
            log_->log_event(spdlog::level::info, "stall_recovered", {});
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
            // Spout/FramePump を解放する。encoder_/rtsp_client_ は
            // PLACEHOLDER へのシームレス移行のために維持するため
            // teardown_streaming() ではなく個別に解放する。
            if (frame_pump_) { frame_pump_->stop(); frame_pump_.reset(); }
            spout_monitor_->disconnect();
            if (config_.placeholder.enabled) {
                // PLACEHOLDER へ直接遷移することで IDLE→PROBING の待機を省略し
                // 映像フリーズ時間を最小化する。
                // encoder_+RTSP が生存していればシームレス移行を試みる。
                seamless_handoff_ = encoder_ && rtsp_client_ &&
                                    rtsp_client_->is_connected();
                placeholder_cooldown_active_ = false;  // クールダウンをキャンセル
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
        // プレースホルダが有効で送信元が消失している場合は
        // RECONNECTING_OUTPUT ではなく PLACEHOLDER へシームレス移行する。
        if (config_.placeholder.enabled &&
            !spout_monitor_->probe_sender(config_.spout.sender_name)) {
            log_->log_event(spdlog::level::warn, "sender_disappeared", {});
            if (frame_pump_) { frame_pump_->stop(); frame_pump_.reset(); }
            spout_monitor_->disconnect();
            seamless_handoff_ = encoder_ && rtsp_client_ &&
                                rtsp_client_->is_connected();
            placeholder_cooldown_active_ = false;
            state_machine_.transition_to(PublisherState::PLACEHOLDER);
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

    // エンコードスレッド・FramePump・RTSP・エンコーダーを順番に解体する。
    // teardown_encoder() が set_gpu_mode(false) を呼ぶため、
    // GPU テクスチャポインタの use-after-free を防止できる。
    teardown_streaming();

    // SpoutDX デバイスを再作成する
    std::string reinit_err;
    if (!spout_monitor_->reinit_device(reinit_err)) {
        log_->log_error("SPOUT_REINIT_FAILED", reinit_err);
        state_machine_.transition_to(PublisherState::FATAL);
        return;
    }

    // デバイス再作成成功。PROBING から sender 再接続 → CONNECTING_OUTPUT で
    // 新デバイスを使ってエンコーダーを再初期化する。
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

    // 解像度変更でも直近のソース解像度を更新しておく
    last_source_width_  = current_width_;
    last_source_height_ = current_height_;

    frame_pump_->start(spout_monitor_, config_.spout.poll_interval_ms);
    last_frame_time_ms_.store(time_utils::now_ms());
    start_encode_thread();
    // 解像度変更後の再開始を publish_started で明示する。
    // handle_connecting_output() と同様にテスト・監視系が検出できるようにする。
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
    // 再接続直後の最初のフレームを IDR として送出する。
    // エンコーダーをリセットせず GOP 内に挿入するため PTS の連続性は保たれる。
    // これにより接続直後から受信側が黒画面なく映像を表示できる。
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

    // GPU ゼロコピーパスを試みる: SpoutMonitor が保持する D3D11 デバイスを渡す
    void* gpu_dev = spout_monitor_ ? spout_monitor_->gpu_device() : nullptr;
    if (!encoder_->init(config_.encoder, width, height, error, gpu_dev)) {
        log_->log_error("ENCODER_INIT_FAILED", error);
        return false;
    }

    // GPU パスが確立された場合は SpoutMonitor を GPU テクスチャモードに切り替える
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

    // 静止画面フリーズフレームキャッシュ。
    // Spout は画面に変化がない場合新規フレームを送らないため、最後に受信した
    // フレームを設定 fps で繰り返しエンコードして RTSP ストリームを維持する。
    //
    // handle_connecting_output() が取得した最初のフレームで初期化することで、
    // 送信側が接続直後から静止状態（SendImage を止めている）でも直ちに映像を
    // 送れるようになる（ScreenRelay PR #6 と同じ修正）。
    FrameBuffer freeze_buf  = std::move(initial_frame_buf_);
    FrameMeta   freeze_meta = initial_frame_meta_;
    // GPU パスでは buf.data が空で gpu_texture のみ設定される。
    // どちらか一方でも有効ならフリーズフレームとして扱う。
    bool        has_freeze  = !freeze_buf.data.empty() || freeze_buf.gpu_texture != nullptr;
    initial_frame_meta_ = {};  // メモリ解放（コピー済み）
    bool        content_changed = true;  // sws_scale スキップ判定用

    // 設定 fps に基づくフレーム送信間隔
    const int64_t frame_interval_us = 1'000'000LL / config_.encoder.fps;
    time_utils::Stopwatch frame_sw;

    while (!encode_stop_.load()) {
        // 次のフレーム送信期限までの残り待機時間を計算する
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
            // 新規フレームを受信: フリーズバッファを更新する
            freeze_buf      = std::move(buf);
            freeze_meta     = meta;
            has_freeze      = true;
            content_changed = true;
            last_frame_time_ms_.store(time_utils::now_ms());
            metrics_->increment_frames_received();
            log_->log_frame_received(meta.sequence, meta.width, meta.height);

            // 解像度変更
            if (supervisor_logic::resolution_changed(
                    freeze_meta.width, freeze_meta.height, current_width_, current_height_)) {
                pending_width_.store(freeze_meta.width);
                pending_height_.store(freeze_meta.height);
                // 新解像度フレームをスレッド再起動時の初期フリーズフレームとして保存する。
                // handle_reconfiguring() がエンコーダーを新解像度で再初期化した後、
                // 次のエンコードスレッドが has_freeze=true で起動できるようにする。
                initial_frame_buf_  = std::move(freeze_buf);
                initial_frame_meta_ = freeze_meta;
                resolution_changed_flag_.store(true);
                return;
            }
        }

        // 送信間隔に達していなければスキップ（新規フレーム受信時も同様）
        if (frame_sw.elapsed_us() < frame_interval_us) {
            continue;
        }

        // 最初のフレームがまだ来ていない
        if (!has_freeze) {
            continue;
        }

        // ソース切断中はフリーズフレーム送信を抑止する。
        // STALLED 状態でもエンコードスレッドは動作し続けるが、ソースが
        // frame_timeout_ms 以上応答していない間はエンコード/送信を止めることで
        // 無用な CPU・帯域消費を防ぐ。RTSP クライアントにも切断状態が伝わる。
        // ソースが復帰した瞬間（last_source_alive_ms が更新される）に自動再開する。
        if (frame_pump_) {
            int64_t source_age_ms = time_utils::now_ms() - frame_pump_->last_source_alive_ms();
            if (source_age_ms > static_cast<int64_t>(config_.spout.frame_timeout_ms)) {
                frame_sw.reset();  // 復帰直後に即送信できるようタイマーをリセット
                continue;
            }
        }

        // フレームタイマーをリセットし、タイムスタンプを現在時刻に統一する
        frame_sw.reset();
        freeze_meta.timestamp_us = time_utils::now_us();

        // エンコード。content_changed が false の場合 sws_scale をスキップし
        // 直前の変換済み YUV フレームを再利用する（静止画面での CPU 負荷削減）
        std::vector<EncodedPacket> packets;
        if (!encoder_->encode(freeze_buf, freeze_meta, packets, content_changed)) {
            log_->log_error("ENCODER_ENCODE_FAILED", "encode() returned false");
            metrics_->increment_frames_dropped();
            // フリーズフレームを保存する。エラー後に PROBING→CONNECTING_OUTPUT と
            // 遷移するため実際には handle_connecting_output() が上書きするが、
            // 保存しておくことで再接続時の初期フレームとして使われうる。
            initial_frame_buf_  = std::move(freeze_buf);
            initial_frame_meta_ = freeze_meta;
            encode_error_flag_.store(true);
            return;
        }
        // YUV フレームは消費済み。次に新規フレームを受信するまで再変換不要
        content_changed = false;

        // Publish
        for (auto& pkt : packets) {
            if (!rtsp_client_->send_packet(pkt)) {
                log_->log_error("RTSP_SEND_FAILED", "send_packet failed");
                metrics_->increment_rtsp_errors();
                // RTSP 再接続後は handle_reconnecting_output() がエンコードスレッドを
                // 再起動するだけで handle_connecting_output() は通らない。
                // フリーズフレームを保存しておくことで、再接続直後の
                // has_freeze=true 起動を保証する（静止画面の黒画面防止）。
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

    // STALLED → RECONNECTING_OUTPUT のように外部から stop_encode_thread() で
    // ループを抜けた場合、initial_frame_buf_ は起動時に std::move 済みで空のため、
    // 次回 start_encode_thread() が has_freeze=false で起動し黒画面になる。
    // ループ終了時に最後のフリーズフレームを保存し、再起動時に即送信できるようにする。
    // (解像度変更・encode エラー・RTSP エラーの早期 return パスは既に保存済み)
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
    // GPU テクスチャポインタを保持したまま SpoutMonitor が disconnect すると
    // use-after-free になるため、エンコーダー解体時に必ず CPU パスへ戻す。
    if (spout_monitor_) spout_monitor_->set_gpu_mode(false);
    // initial_frame_buf_ が GPU テクスチャポインタを持っている場合もクリアする
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

    while (!shutdown_requested_.load()) {
        if (metrics_sw.expired(config_.runtime.emit_metrics_interval_ms)) {
            metrics_->save_metrics(config_.app.metrics_path);
            metrics_sw.reset();
        }
        if (health_sw.expired(config_.runtime.emit_health_interval_ms)) {
            metrics_->save_health(config_.app.health_path);
            health_sw.reset();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // Final flush
    metrics_->save_metrics(config_.app.metrics_path);
    metrics_->save_health(config_.app.health_path);
}
