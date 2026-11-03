#include <cstdio>
#include "app/supervisor_logic.hpp"
#include "test_utils.hpp"

using namespace supervisor_logic;

int run_supervisor_logic_tests() {
    printf("=== Supervisor Logic Tests ===\n");

    // --- can_seamless_handoff ----------------------------------------------

    {
        // Seamless switch is possible only when all conditions are met
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
        // Not possible when seamless transition was not requested
        bool ok = can_seamless_handoff(
            false, true, true, 1280, 720, 1280, 720);
        VERIFY_MSG(!ok, "seamless_handoff_requested=false → not seamless");
        printf("[PASS] can_seamless_handoff: seamless not requested\n");
    }

    {
        // Not possible when the encoder is not alive
        bool ok = can_seamless_handoff(
            true, false, true, 1280, 720, 1280, 720);
        VERIFY_MSG(!ok, "encoder_alive=false → not seamless");
        printf("[PASS] can_seamless_handoff: encoder not alive\n");
    }

    {
        // Not possible when the RTSP client is not connected
        bool ok = can_seamless_handoff(
            true, true, false, 1280, 720, 1280, 720);
        VERIFY_MSG(!ok, "rtsp_connected=false → not seamless");
        printf("[PASS] can_seamless_handoff: rtsp not connected\n");
    }

    {
        // Not possible when the width differs
        bool ok = can_seamless_handoff(
            true, true, true, 640, 720, 1280, 720);
        VERIFY_MSG(!ok, "width mismatch → not seamless");
        printf("[PASS] can_seamless_handoff: width mismatch\n");
    }

    {
        // Not possible when the height differs
        bool ok = can_seamless_handoff(
            true, true, true, 1280, 360, 1280, 720);
        VERIFY_MSG(!ok, "height mismatch → not seamless");
        printf("[PASS] can_seamless_handoff: height mismatch\n");
    }

    {
        // Seamless is possible even for 0x0 on both sides if the other conditions hold
        // (a 0x0 request from PLACEHOLDER's initial state current_width_=0, current_height_=0
        //   never happens in practice, but this confirms the function is
        //   a pure equality check)
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
        // First call (source_responded=false) returns true and sets the flag
        bool source_responded = false;
        bool result = should_reset_connect_timer_once(source_responded);
        VERIFY_MSG(result, "first call should return true");
        VERIFY_MSG(source_responded, "source_responded must be set to true after first call");
        printf("[PASS] should_reset_connect_timer_once: first call returns true\n");
    }

    {
        // Second and later calls (source_responded=true) return false (prevents an infinite loop)
        bool source_responded = true;
        bool result = should_reset_connect_timer_once(source_responded);
        VERIFY_MSG(!result, "subsequent call should return false");
        VERIFY_MSG(source_responded, "source_responded must remain true");
        printf("[PASS] should_reset_connect_timer_once: subsequent call returns false\n");
    }

    {
        // Repeated calls with the same flag: only the first returns true, the rest false
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
