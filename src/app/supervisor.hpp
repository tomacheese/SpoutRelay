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
    void handle_stopping();

    void metrics_writer_thread_func();
    void teardown_streaming();
    void teardown_encoder();
    void teardown_rtsp();

    /// @brief 指定解像度でエンコーダーを初期化し、RTSP クライアントを接続する。
    ///
    /// handle_connecting_output() / handle_reconfiguring() / handle_placeholder()
    /// で重複していたエンコーダー初期化・RTSP 接続処理を共通化したもの。
    /// 失敗時は呼び出し側で teardown_encoder()/teardown_rtsp() を行うこと。
    ///
    /// @param width,height 映像解像度
    /// @param error 失敗時のエラーメッセージ
    /// @return 成功時 true
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

    /// @brief 直近に接続した実ソースの解像度。
    ///        PLACEHOLDER 状態で映像を生成する際、config の既定解像度より
    ///        この値を優先することで、ソース復帰時の RTSP 再構成を避ける。
    uint32_t last_source_width_  = 0;
    uint32_t last_source_height_ = 0;

    /// @brief PLACEHOLDER 状態で配信中のフレーム。静止画のため一度だけ生成し、
    ///        設定 fps で繰り返しエンコードする。
    FrameBuffer placeholder_frame_;
    FrameMeta   placeholder_meta_{};
    bool        placeholder_active_         = false;
    bool        placeholder_content_changed_ = true;
    time_utils::Stopwatch placeholder_frame_timer_;
    time_utils::Stopwatch placeholder_probe_timer_;

    /// @brief PLACEHOLDER 状態での fps/bitrate 計測用カウンター。
    ///        encode_publish_thread_func() の同等ロジックと同じ方式で
    ///        1 秒ごとに metrics へ反映する。
    uint64_t              placeholder_frames_since_last_ = 0;
    uint64_t              placeholder_bytes_since_last_  = 0;
    time_utils::Stopwatch placeholder_metrics_sw_;

    /// @brief PLACEHOLDER 状態を抜けてから PROBING/CONNECTING_OUTPUT を経て
    ///        再び PLACEHOLDER へ戻るまでの最小間隔を測るタイマー。
    ///        ソースが probe には応答するもののフレーム送出が始まらない
    ///        (CONNECTING_OUTPUT がタイムアウトする) ケースで、
    ///        PROBING ⇔ PLACEHOLDER 間でエンコーダー/RTSP の再初期化が
    ///        高頻度に繰り返されることを防ぐ。
    time_utils::Stopwatch placeholder_cooldown_timer_;
    bool placeholder_cooldown_active_ = false;

    /// @brief handle_connecting_output() で取得した最初のフレーム。
    ///        encode_publish_thread_func() の初期フリーズフレームとして渡すことで、
    ///        Spout 送信側が静止画面のまま SendImage() を止めている場合でも
    ///        ストリームに映像が届くようにする。使用後は解放してメモリを節約する。
    ///
    /// @note スレッド安全性:
    ///       メインスレッドはエンコードスレッド起動前 (start_encode_thread()) に書き込み、
    ///       エンコードスレッドは起動直後に一度だけ std::move で読み出す。
    ///       エンコードスレッドが書き戻す場合（RTSP/encode エラーの早期 return）は
    ///       直後に rtsp_error_flag_/encode_error_flag_ をセットして return するため、
    ///       メインスレッドは stop_encode_thread() (join) を経由してから参照する。
    ///       このスレッド間の join による happens-before 関係により、明示的な mutex は不要。
    FrameBuffer initial_frame_buf_;
    FrameMeta   initial_frame_meta_{};
};
