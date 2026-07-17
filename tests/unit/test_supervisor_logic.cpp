#include <cstdio>
#include "app/supervisor_logic.hpp"
#include "test_utils.hpp"

using namespace supervisor_logic;

int run_supervisor_logic_tests() {
    printf("=== Supervisor Logic Tests ===\n");

    // --- can_seamless_handoff ----------------------------------------------

    {
        // 全条件を満たす場合のみシームレス切替可能
        bool ok = can_seamless_handoff(
            /*seamless_handoff_requested=*/true,
            /*encoder_alive=*/true,
            /*rtsp_connected=*/true,
            /*new_width=*/1280, /*new_height=*/720,
            /*current_width=*/1280, /*current_height=*/720);
        VERIFY_MSG(ok, "all conditions satisfied → seamless handoff possible");
        printf("[PASS] can_seamless_handoff: all conditions satisfied\n");
    }

    {
        // シームレス移行が要求されていない場合は不可
        bool ok = can_seamless_handoff(
            false, true, true, 1280, 720, 1280, 720);
        VERIFY_MSG(!ok, "seamless_handoff_requested=false → not seamless");
        printf("[PASS] can_seamless_handoff: seamless not requested\n");
    }

    {
        // エンコーダーが生存していない場合は不可
        bool ok = can_seamless_handoff(
            true, false, true, 1280, 720, 1280, 720);
        VERIFY_MSG(!ok, "encoder_alive=false → not seamless");
        printf("[PASS] can_seamless_handoff: encoder not alive\n");
    }

    {
        // RTSP クライアントが未接続の場合は不可
        bool ok = can_seamless_handoff(
            true, true, false, 1280, 720, 1280, 720);
        VERIFY_MSG(!ok, "rtsp_connected=false → not seamless");
        printf("[PASS] can_seamless_handoff: rtsp not connected\n");
    }

    {
        // 幅が一致しない場合は不可
        bool ok = can_seamless_handoff(
            true, true, true, 640, 720, 1280, 720);
        VERIFY_MSG(!ok, "width mismatch → not seamless");
        printf("[PASS] can_seamless_handoff: width mismatch\n");
    }

    {
        // 高さが一致しない場合は不可
        bool ok = can_seamless_handoff(
            true, true, true, 1280, 360, 1280, 720);
        VERIFY_MSG(!ok, "height mismatch → not seamless");
        printf("[PASS] can_seamless_handoff: height mismatch\n");
    }

    {
        // 解像度 0x0 同士でも、他条件を満たせばシームレス成立
        // (PLACEHOLDER の初期状態 current_width_=0, current_height_=0 から
        //  同じ 0x0 を指定するケースは実際には発生しないが、関数自体は
        //  純粋な等値判定であることを確認する)
        bool ok = can_seamless_handoff(
            true, true, true, 0, 0, 0, 0);
        VERIFY_MSG(ok, "0x0 == 0x0 with other conditions true → seamless");
        printf("[PASS] can_seamless_handoff: zero resolution edge case\n");
    }

    // --- resolution_changed --------------------------------------------------

    {
        bool changed = resolution_changed(1280, 720, 1280, 720);
        VERIFY_MSG(!changed, "identical resolution should not be reported as changed");
        printf("[PASS] resolution_changed: identical resolution\n");
    }

    {
        bool changed = resolution_changed(640, 360, 1280, 720);
        VERIFY_MSG(changed, "width and height both differ → changed");
        printf("[PASS] resolution_changed: both dimensions differ\n");
    }

    {
        bool changed = resolution_changed(640, 720, 1280, 720);
        VERIFY_MSG(changed, "width differs → changed");
        printf("[PASS] resolution_changed: width differs only\n");
    }

    {
        bool changed = resolution_changed(1280, 360, 1280, 720);
        VERIFY_MSG(changed, "height differs → changed");
        printf("[PASS] resolution_changed: height differs only\n");
    }

    // --- should_reset_connect_timer_once -------------------------------------

    {
        // 初回 (source_responded=false) は true を返しフラグを true にセットする
        bool source_responded = false;
        bool result = should_reset_connect_timer_once(source_responded);
        VERIFY_MSG(result, "first call should return true");
        VERIFY_MSG(source_responded, "source_responded must be set to true after first call");
        printf("[PASS] should_reset_connect_timer_once: first call returns true\n");
    }

    {
        // 2 回目以降 (source_responded=true) は false を返す (無限ループ防止)
        bool source_responded = true;
        bool result = should_reset_connect_timer_once(source_responded);
        VERIFY_MSG(!result, "subsequent call should return false");
        VERIFY_MSG(source_responded, "source_responded must remain true");
        printf("[PASS] should_reset_connect_timer_once: subsequent call returns false\n");
    }

    {
        // 同一フラグで連続呼び出し: 1 回目のみ true、2 回目以降は false
        bool source_responded = false;
        bool first  = should_reset_connect_timer_once(source_responded);
        bool second = should_reset_connect_timer_once(source_responded);
        bool third  = should_reset_connect_timer_once(source_responded);
        VERIFY_MSG(first,   "first call must be true");
        VERIFY_MSG(!second, "second call must be false");
        VERIFY_MSG(!third,  "third call must be false");
        printf("[PASS] should_reset_connect_timer_once: only first of repeated calls is true\n");
    }

    // --- should_force_spout_recovery -----------------------------------------

    {
        bool force = should_force_spout_recovery(9, 10);
        VERIFY_MSG(!force, "consecutive_stall_recoveries below max_attempts → no forced recovery");
        printf("[PASS] should_force_spout_recovery: below threshold\n");
    }

    {
        bool force = should_force_spout_recovery(10, 10);
        VERIFY_MSG(force, "consecutive_stall_recoveries == max_attempts → forced recovery");
        printf("[PASS] should_force_spout_recovery: at threshold\n");
    }

    {
        bool force = should_force_spout_recovery(15, 10);
        VERIFY_MSG(force, "consecutive_stall_recoveries above max_attempts → forced recovery");
        printf("[PASS] should_force_spout_recovery: above threshold\n");
    }

    {
        bool force = should_force_spout_recovery(1000, 0);
        VERIFY_MSG(!force, "max_attempts=0 → watchdog disabled");
        printf("[PASS] should_force_spout_recovery: disabled when max_attempts=0\n");
    }

    {
        bool force = should_force_spout_recovery(1000, -1);
        VERIFY_MSG(!force, "max_attempts negative → watchdog disabled");
        printf("[PASS] should_force_spout_recovery: disabled when max_attempts negative\n");
    }

    return 0;
}
