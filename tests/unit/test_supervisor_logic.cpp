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

    return 0;
}
