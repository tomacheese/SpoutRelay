#pragma once
#include <cstdint>

/// @brief Supervisor の状態遷移判定のうち、純粋な条件式として切り出せる部分を
///        まとめた名前空間。
///
///        Supervisor 本体は Spout/エンコーダー/RTSP などの実 I/O に依存するため
///        単体テストが難しいが、ここに切り出した判定ロジックは入力値のみに
///        依存する純粋関数であり、ユニットテストで網羅的に検証できる。
namespace supervisor_logic {

/// @brief PLACEHOLDER ⇔ 実ソース間でシームレス切替が可能かどうかを判定する。
///
///        handle_placeholder()（実ソース復帰時）と
///        handle_connecting_output()（PLACEHOLDER 復帰後の最初のフレーム受信時）
///        の両方で使われる共通条件。
///
///        以下を全て満たす場合のみ true を返す:
///          - シームレス移行が要求されている (seamless_handoff_requested)
///          - エンコーダーが生存している (encoder_alive)
///          - RTSP クライアントが接続済みである (rtsp_connected)
///          - 新しい解像度が現在の解像度と一致している
///
///        いずれかが false の場合、呼び出し側は encoder/RTSP を再初期化する
///        必要がある。
///
/// @param seamless_handoff_requested シームレス移行が要求されているか
/// @param encoder_alive   エンコーダーインスタンスが生存しているか
/// @param rtsp_connected  RTSP クライアントが接続済みか
/// @param new_width       新しい映像の幅
/// @param new_height      新しい映像の高さ
/// @param current_width   現在エンコーダーが使用している幅
/// @param current_height  現在エンコーダーが使用している高さ
/// @return シームレス切替が可能なら true
inline bool can_seamless_handoff(bool seamless_handoff_requested,
                                  bool encoder_alive,
                                  bool rtsp_connected,
                                  uint32_t new_width, uint32_t new_height,
                                  uint32_t current_width, uint32_t current_height) {
    return seamless_handoff_requested &&
           encoder_alive && rtsp_connected &&
           new_width == current_width && new_height == current_height;
}

/// @brief 受信フレームの解像度が現在エンコーダーが使用している解像度と
///        異なるかどうかを判定する。
///
///        encode_publish_thread_func() で新規フレーム受信時に呼ばれ、
///        true の場合 RECONFIGURING への遷移がトリガーされる。
///
/// @param frame_width    受信フレームの幅
/// @param frame_height   受信フレームの高さ
/// @param current_width  現在エンコーダーが使用している幅
/// @param current_height 現在エンコーダーが使用している高さ
/// @return 解像度が変化していれば true
inline bool resolution_changed(uint32_t frame_width, uint32_t frame_height,
                                uint32_t current_width, uint32_t current_height) {
    return frame_width != current_width || frame_height != current_height;
}

} // namespace supervisor_logic
