#include "app/supervisor.hpp"
#include "common/time_utils.hpp"
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
            case PublisherState::CONNECTING_OUTPUT:  handle_connecting_output();  break;
            case PublisherState::STREAMING:          handle_streaming();          break;
            case PublisherState::STALLED:            handle_stalled();            break;
            case PublisherState::RECONFIGURING:      handle_reconfiguring();      break;
            case PublisherState::RECONNECTING_OUTPUT:handle_reconnecting_output();break;
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

    std::this_thread::sleep_for(
        std::chrono::milliseconds(config_.spout.poll_interval_ms));
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

    while (!is_new && !shutdown_requested_.load()) {
        if (spout_monitor_->receive_latest_frame(buf, meta, is_new)) {
            if (is_new) break;
        }
        if (wait.expired(config_.spout.frame_timeout_ms * 5)) {
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
    initial_frame_buf_  = buf;
    initial_frame_meta_ = meta;

    current_width_  = meta.width;
    current_height_ = meta.height;

    // Init encoder
    encoder_ = std::make_unique<EncoderController>();
    std::string enc_err;
    if (!encoder_->init(config_.encoder, current_width_, current_height_, enc_err)) {
        log_->log_error("ENCODER_INIT_FAILED", enc_err);
        teardown_encoder();
        spout_monitor_->disconnect();
        state_machine_.transition_to(PublisherState::PROBING);
        return;
    }
    log_->log_encoder_initialized(
        config_.encoder.codec,
        static_cast<int>(current_width_), static_cast<int>(current_height_),
        config_.encoder.fps, config_.encoder.bitrate_kbps);
    metrics_->set_encoder_codec(config_.encoder.codec);

    // Init RTSP client
    rtsp_client_ = std::make_unique<RtspPublisherClient>();
    std::string rtsp_err;
    auto codec_info = encoder_->get_codec_info();
    if (!rtsp_client_->connect(config_.rtsp, codec_info, rtsp_err)) {
        log_->log_error("RTSP_CONNECT_FAILED", rtsp_err);
        teardown_rtsp();
        teardown_encoder();
        spout_monitor_->disconnect();
        state_machine_.transition_to(PublisherState::PROBING);
        return;
    }

    log_->log_publish_started(config_.rtsp.url,
        static_cast<int>(current_width_), static_cast<int>(current_height_),
        config_.encoder.fps);
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
            teardown_streaming();
            spout_monitor_->disconnect();
            state_machine_.transition_to(PublisherState::IDLE);
            return;
        }
    }

    // Reconnect RTSP after prolonged stall
    if (stall_timer_.expired(
            static_cast<int64_t>(config_.rtsp.reconnect_delay_ms) * 3)) {
        stop_encode_thread();
        teardown_rtsp();
        state_machine_.transition_to(PublisherState::RECONNECTING_OUTPUT);
        return;
    }

    std::this_thread::sleep_for(
        std::chrono::milliseconds(config_.spout.poll_interval_ms));
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

    std::string enc_err;
    encoder_ = std::make_unique<EncoderController>();
    if (!encoder_->init(config_.encoder, current_width_, current_height_, enc_err)) {
        log_->log_error("ENCODER_INIT_FAILED", enc_err);
        teardown_encoder();
        spout_monitor_->disconnect();
        state_machine_.transition_to(PublisherState::PROBING);
        return;
    }

    std::string rtsp_err;
    rtsp_client_ = std::make_unique<RtspPublisherClient>();
    auto codec_info = encoder_->get_codec_info();
    if (!rtsp_client_->connect(config_.rtsp, codec_info, rtsp_err)) {
        log_->log_error("RTSP_CONNECT_FAILED", rtsp_err);
        teardown_rtsp();
        teardown_encoder();
        spout_monitor_->disconnect();
        state_machine_.transition_to(PublisherState::PROBING);
        return;
    }

    log_->log_encoder_initialized(config_.encoder.codec,
        static_cast<int>(current_width_), static_cast<int>(current_height_),
        config_.encoder.fps, config_.encoder.bitrate_kbps);

    frame_pump_->start(spout_monitor_, config_.spout.poll_interval_ms);
    last_frame_time_ms_.store(time_utils::now_ms());
    start_encode_thread();
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
    bool        has_freeze  = !freeze_buf.data.empty();
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
            if (freeze_meta.width != current_width_ || freeze_meta.height != current_height_) {
                pending_width_.store(freeze_meta.width);
                pending_height_.store(freeze_meta.height);
                // 新解像度フレームをスレッド再起動時の初期フリーズフレームとして保存する。
                // handle_reconfiguring() がエンコーダーを新解像度で再初期化した後、
                // 次のエンコードスレッドが has_freeze=true で起動できるようにする。
                initial_frame_buf_  = freeze_buf;
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
            initial_frame_buf_  = freeze_buf;
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
                initial_frame_buf_  = freeze_buf;
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
}

void Supervisor::teardown_encoder() {
    if (encoder_) {
        std::vector<EncodedPacket> flush_pkts;
        encoder_->flush(flush_pkts);
        // Discard flush packets (stream is being torn down)
        encoder_->reset();
        encoder_.reset();
    }
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
