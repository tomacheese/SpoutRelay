#include <cstdio>
#include "app/state_machine.hpp"
#include "test_utils.hpp"

int run_state_machine_tests() {
    printf("=== State Machine Tests ===\n");

    {
        StateMachine sm;
        VERIFY(sm.current_state() == PublisherState::INIT);

        bool ok = sm.transition_to(PublisherState::IDLE);
        VERIFY_MSG(ok, "INIT → IDLE should succeed");
        VERIFY(sm.current_state() == PublisherState::IDLE);
        printf("[PASS] INIT → IDLE\n");
    }

    {
        StateMachine sm;
        sm.transition_to(PublisherState::IDLE);
        bool ok = sm.transition_to(PublisherState::PROBING);
        VERIFY_MSG(ok, "IDLE → PROBING should succeed");
        printf("[PASS] IDLE → PROBING\n");
    }

    {
        StateMachine sm;
        sm.transition_to(PublisherState::IDLE);
        sm.transition_to(PublisherState::PROBING);
        bool ok = sm.transition_to(PublisherState::CONNECTING_OUTPUT);
        VERIFY_MSG(ok, "PROBING → CONNECTING_OUTPUT should succeed");
        printf("[PASS] PROBING → CONNECTING_OUTPUT\n");
    }

    {
        StateMachine sm;
        sm.transition_to(PublisherState::IDLE);
        sm.transition_to(PublisherState::PROBING);
        sm.transition_to(PublisherState::CONNECTING_OUTPUT);
        bool ok = sm.transition_to(PublisherState::STREAMING);
        VERIFY_MSG(ok, "CONNECTING_OUTPUT → STREAMING should succeed");
        printf("[PASS] CONNECTING_OUTPUT → STREAMING\n");
    }

    {
        StateMachine sm;
        sm.transition_to(PublisherState::IDLE);
        sm.transition_to(PublisherState::PROBING);
        sm.transition_to(PublisherState::CONNECTING_OUTPUT);
        sm.transition_to(PublisherState::STREAMING);
        bool ok = sm.transition_to(PublisherState::STALLED);
        VERIFY_MSG(ok, "STREAMING → STALLED should succeed");
        printf("[PASS] STREAMING → STALLED\n");
    }

    {
        StateMachine sm;
        sm.transition_to(PublisherState::IDLE);
        sm.transition_to(PublisherState::PROBING);
        sm.transition_to(PublisherState::CONNECTING_OUTPUT);
        sm.transition_to(PublisherState::STREAMING);
        bool ok = sm.transition_to(PublisherState::RECONFIGURING);
        VERIFY_MSG(ok, "STREAMING → RECONFIGURING should succeed");
        printf("[PASS] STREAMING → RECONFIGURING\n");
    }

    {
        StateMachine sm;
        sm.transition_to(PublisherState::IDLE);
        sm.transition_to(PublisherState::PROBING);
        sm.transition_to(PublisherState::CONNECTING_OUTPUT);
        sm.transition_to(PublisherState::STREAMING);
        bool ok = sm.transition_to(PublisherState::RECONNECTING_OUTPUT);
        VERIFY_MSG(ok, "STREAMING → RECONNECTING_OUTPUT should succeed");
        printf("[PASS] STREAMING → RECONNECTING_OUTPUT\n");
    }

    {
        // Invalid transition: IDLE → STREAMING should fail
        StateMachine sm;
        sm.transition_to(PublisherState::IDLE);
        bool ok = sm.transition_to(PublisherState::STREAMING);
        VERIFY_MSG(!ok, "IDLE → STREAMING must be rejected");
        VERIFY(sm.current_state() == PublisherState::IDLE);
        printf("[PASS] Invalid IDLE → STREAMING rejected\n");
    }

    {
        // Invalid transition: INIT → PROBING should fail
        StateMachine sm;
        bool ok = sm.transition_to(PublisherState::PROBING);
        VERIFY_MSG(!ok, "INIT → PROBING must be rejected");
        printf("[PASS] Invalid INIT → PROBING rejected\n");
    }

    {
        // Callback fires on valid transition
        StateMachine sm;
        PublisherState cb_from = PublisherState::INIT;
        PublisherState cb_to   = PublisherState::INIT;
        sm.on_transition([&](PublisherState f, PublisherState t) {
            cb_from = f; cb_to = t;
        });
        sm.transition_to(PublisherState::IDLE);
        VERIFY(cb_from == PublisherState::INIT);
        VERIFY(cb_to   == PublisherState::IDLE);
        printf("[PASS] Transition callback fires correctly\n");
    }

    {
        // RECONNECTING_OUTPUT → FATAL
        StateMachine sm;
        sm.transition_to(PublisherState::IDLE);
        sm.transition_to(PublisherState::PROBING);
        sm.transition_to(PublisherState::CONNECTING_OUTPUT);
        sm.transition_to(PublisherState::STREAMING);
        sm.transition_to(PublisherState::RECONNECTING_OUTPUT);
        bool ok = sm.transition_to(PublisherState::FATAL);
        VERIFY_MSG(ok, "RECONNECTING_OUTPUT → FATAL should succeed");
        printf("[PASS] RECONNECTING_OUTPUT → FATAL\n");
    }

    {
        // CONNECTING_OUTPUT -> FATAL (encoder init failed for every codec)
        StateMachine sm;
        sm.transition_to(PublisherState::IDLE);
        sm.transition_to(PublisherState::PROBING);
        sm.transition_to(PublisherState::CONNECTING_OUTPUT);
        bool ok = sm.transition_to(PublisherState::FATAL);
        VERIFY_MSG(ok, "CONNECTING_OUTPUT → FATAL should succeed");
        VERIFY(sm.current_state() == PublisherState::FATAL);
        printf("[PASS] CONNECTING_OUTPUT → FATAL\n");
    }

    {
        // PROBING → PLACEHOLDER → CONNECTING_OUTPUT
        // (streams a NO SIGNAL picture while no source is found, recovers when one appears)
        StateMachine sm;
        sm.transition_to(PublisherState::IDLE);
        sm.transition_to(PublisherState::PROBING);
        bool ok1 = sm.transition_to(PublisherState::PLACEHOLDER);
        VERIFY_MSG(ok1, "PROBING → PLACEHOLDER should succeed");
        bool ok2 = sm.transition_to(PublisherState::CONNECTING_OUTPUT);
        VERIFY_MSG(ok2, "PLACEHOLDER → CONNECTING_OUTPUT should succeed");
        printf("[PASS] PROBING → PLACEHOLDER → CONNECTING_OUTPUT\n");
    }

    {
        // PLACEHOLDER -> STOPPING (shutdown requested)
        StateMachine sm;
        sm.transition_to(PublisherState::IDLE);
        sm.transition_to(PublisherState::PROBING);
        sm.transition_to(PublisherState::PLACEHOLDER);
        bool ok = sm.transition_to(PublisherState::STOPPING);
        VERIFY_MSG(ok, "PLACEHOLDER → STOPPING should succeed");
        printf("[PASS] PLACEHOLDER → STOPPING\n");
    }

    {
        // RECONFIGURING -> FATAL (encoder re-init after a resolution change failed)
        StateMachine sm;
        sm.transition_to(PublisherState::IDLE);
        sm.transition_to(PublisherState::PROBING);
        sm.transition_to(PublisherState::CONNECTING_OUTPUT);
        sm.transition_to(PublisherState::STREAMING);
        sm.transition_to(PublisherState::RECONFIGURING);
        bool ok = sm.transition_to(PublisherState::FATAL);
        VERIFY_MSG(ok, "RECONFIGURING → FATAL should succeed");
        VERIFY(sm.current_state() == PublisherState::FATAL);
        printf("[PASS] RECONFIGURING → FATAL\n");
    }

    // -----------------------------------------------------------------
    // Previously uncovered transitions
    // -----------------------------------------------------------------

    {
        // INIT -> FATAL (fatal error right after startup)
        StateMachine sm;
        bool ok = sm.transition_to(PublisherState::FATAL);
        VERIFY_MSG(ok, "INIT → FATAL should succeed");
        VERIFY(sm.current_state() == PublisherState::FATAL);
        printf("[PASS] INIT → FATAL\n");
    }

    {
        // IDLE -> STOPPING (shutdown requested right after startup)
        StateMachine sm;
        sm.transition_to(PublisherState::IDLE);
        bool ok = sm.transition_to(PublisherState::STOPPING);
        VERIFY_MSG(ok, "IDLE → STOPPING should succeed");
        printf("[PASS] IDLE → STOPPING\n");
    }

    {
        // PROBING -> IDLE (probing aborted, back to waiting)
        StateMachine sm;
        sm.transition_to(PublisherState::IDLE);
        sm.transition_to(PublisherState::PROBING);
        bool ok = sm.transition_to(PublisherState::IDLE);
        VERIFY_MSG(ok, "PROBING → IDLE should succeed");
        printf("[PASS] PROBING → IDLE\n");
    }

    {
        // PROBING -> STOPPING (shutdown requested while probing)
        StateMachine sm;
        sm.transition_to(PublisherState::IDLE);
        sm.transition_to(PublisherState::PROBING);
        bool ok = sm.transition_to(PublisherState::STOPPING);
        VERIFY_MSG(ok, "PROBING → STOPPING should succeed");
        printf("[PASS] PROBING → STOPPING\n");
    }

    {
        // CONNECTING_OUTPUT -> PROBING (source disappeared while connecting output)
        StateMachine sm;
        sm.transition_to(PublisherState::IDLE);
        sm.transition_to(PublisherState::PROBING);
        sm.transition_to(PublisherState::CONNECTING_OUTPUT);
        bool ok = sm.transition_to(PublisherState::PROBING);
        VERIFY_MSG(ok, "CONNECTING_OUTPUT → PROBING should succeed");
        printf("[PASS] CONNECTING_OUTPUT → PROBING\n");
    }

    {
        // CONNECTING_OUTPUT -> STOPPING (shutdown requested while connecting output)
        StateMachine sm;
        sm.transition_to(PublisherState::IDLE);
        sm.transition_to(PublisherState::PROBING);
        sm.transition_to(PublisherState::CONNECTING_OUTPUT);
        bool ok = sm.transition_to(PublisherState::STOPPING);
        VERIFY_MSG(ok, "CONNECTING_OUTPUT → STOPPING should succeed");
        printf("[PASS] CONNECTING_OUTPUT → STOPPING\n");
    }

    {
        // STREAMING -> STOPPING (shutdown requested while streaming)
        StateMachine sm;
        sm.transition_to(PublisherState::IDLE);
        sm.transition_to(PublisherState::PROBING);
        sm.transition_to(PublisherState::CONNECTING_OUTPUT);
        sm.transition_to(PublisherState::STREAMING);
        bool ok = sm.transition_to(PublisherState::STOPPING);
        VERIFY_MSG(ok, "STREAMING → STOPPING should succeed");
        printf("[PASS] STREAMING → STOPPING\n");
    }

    {
        // STREAMING -> IDLE (streaming stopped, back to waiting)
        StateMachine sm;
        sm.transition_to(PublisherState::IDLE);
        sm.transition_to(PublisherState::PROBING);
        sm.transition_to(PublisherState::CONNECTING_OUTPUT);
        sm.transition_to(PublisherState::STREAMING);
        bool ok = sm.transition_to(PublisherState::IDLE);
        VERIFY_MSG(ok, "STREAMING → IDLE should succeed");
        printf("[PASS] STREAMING → IDLE\n");
    }

    {
        // STREAMING -> PROBING (source disappeared while streaming, re-probe)
        StateMachine sm;
        sm.transition_to(PublisherState::IDLE);
        sm.transition_to(PublisherState::PROBING);
        sm.transition_to(PublisherState::CONNECTING_OUTPUT);
        sm.transition_to(PublisherState::STREAMING);
        bool ok = sm.transition_to(PublisherState::PROBING);
        VERIFY_MSG(ok, "STREAMING → PROBING should succeed");
        printf("[PASS] STREAMING → PROBING\n");
    }

    {
        // STALLED -> STREAMING (recovery from stalled output)
        StateMachine sm;
        sm.transition_to(PublisherState::IDLE);
        sm.transition_to(PublisherState::PROBING);
        sm.transition_to(PublisherState::CONNECTING_OUTPUT);
        sm.transition_to(PublisherState::STREAMING);
        sm.transition_to(PublisherState::STALLED);
        bool ok = sm.transition_to(PublisherState::STREAMING);
        VERIFY_MSG(ok, "STALLED → STREAMING should succeed");
        printf("[PASS] STALLED → STREAMING\n");
    }

    {
        // STALLED -> RECONNECTING_OUTPUT (RTSP output error while stalled)
        StateMachine sm;
        sm.transition_to(PublisherState::IDLE);
        sm.transition_to(PublisherState::PROBING);
        sm.transition_to(PublisherState::CONNECTING_OUTPUT);
        sm.transition_to(PublisherState::STREAMING);
        sm.transition_to(PublisherState::STALLED);
        bool ok = sm.transition_to(PublisherState::RECONNECTING_OUTPUT);
        VERIFY_MSG(ok, "STALLED → RECONNECTING_OUTPUT should succeed");
        printf("[PASS] STALLED → RECONNECTING_OUTPUT\n");
    }

    {
        // STALLED -> STOPPING (shutdown requested while stalled)
        StateMachine sm;
        sm.transition_to(PublisherState::IDLE);
        sm.transition_to(PublisherState::PROBING);
        sm.transition_to(PublisherState::CONNECTING_OUTPUT);
        sm.transition_to(PublisherState::STREAMING);
        sm.transition_to(PublisherState::STALLED);
        bool ok = sm.transition_to(PublisherState::STOPPING);
        VERIFY_MSG(ok, "STALLED → STOPPING should succeed");
        printf("[PASS] STALLED → STOPPING\n");
    }

    {
        // STALLED -> IDLE (stall resolved, back to waiting)
        StateMachine sm;
        sm.transition_to(PublisherState::IDLE);
        sm.transition_to(PublisherState::PROBING);
        sm.transition_to(PublisherState::CONNECTING_OUTPUT);
        sm.transition_to(PublisherState::STREAMING);
        sm.transition_to(PublisherState::STALLED);
        bool ok = sm.transition_to(PublisherState::IDLE);
        VERIFY_MSG(ok, "STALLED → IDLE should succeed");
        printf("[PASS] STALLED → IDLE\n");
    }

    {
        // STALLED -> PROBING (source fully disappeared while stalled, re-probe)
        StateMachine sm;
        sm.transition_to(PublisherState::IDLE);
        sm.transition_to(PublisherState::PROBING);
        sm.transition_to(PublisherState::CONNECTING_OUTPUT);
        sm.transition_to(PublisherState::STREAMING);
        sm.transition_to(PublisherState::STALLED);
        bool ok = sm.transition_to(PublisherState::PROBING);
        VERIFY_MSG(ok, "STALLED → PROBING should succeed");
        printf("[PASS] STALLED → PROBING\n");
    }

    {
        // STALLED -> PLACEHOLDER (source lost -> seamless switch to NO SIGNAL)
        StateMachine sm;
        sm.transition_to(PublisherState::IDLE);
        sm.transition_to(PublisherState::PROBING);
        sm.transition_to(PublisherState::CONNECTING_OUTPUT);
        sm.transition_to(PublisherState::STREAMING);
        sm.transition_to(PublisherState::STALLED);
        bool ok = sm.transition_to(PublisherState::PLACEHOLDER);
        VERIFY_MSG(ok, "STALLED → PLACEHOLDER should succeed");
        printf("[PASS] STALLED → PLACEHOLDER\n");
    }

    {
        // RECONFIGURING -> STREAMING (encoder re-init after a resolution change succeeded)
        StateMachine sm;
        sm.transition_to(PublisherState::IDLE);
        sm.transition_to(PublisherState::PROBING);
        sm.transition_to(PublisherState::CONNECTING_OUTPUT);
        sm.transition_to(PublisherState::STREAMING);
        sm.transition_to(PublisherState::RECONFIGURING);
        bool ok = sm.transition_to(PublisherState::STREAMING);
        VERIFY_MSG(ok, "RECONFIGURING → STREAMING should succeed");
        printf("[PASS] RECONFIGURING → STREAMING\n");
    }

    {
        // RECONFIGURING -> STOPPING (shutdown requested while reconfiguring)
        StateMachine sm;
        sm.transition_to(PublisherState::IDLE);
        sm.transition_to(PublisherState::PROBING);
        sm.transition_to(PublisherState::CONNECTING_OUTPUT);
        sm.transition_to(PublisherState::STREAMING);
        sm.transition_to(PublisherState::RECONFIGURING);
        bool ok = sm.transition_to(PublisherState::STOPPING);
        VERIFY_MSG(ok, "RECONFIGURING → STOPPING should succeed");
        printf("[PASS] RECONFIGURING → STOPPING\n");
    }

    {
        // RECONNECTING_OUTPUT -> STREAMING (RTSP reconnect succeeded)
        StateMachine sm;
        sm.transition_to(PublisherState::IDLE);
        sm.transition_to(PublisherState::PROBING);
        sm.transition_to(PublisherState::CONNECTING_OUTPUT);
        sm.transition_to(PublisherState::STREAMING);
        sm.transition_to(PublisherState::RECONNECTING_OUTPUT);
        bool ok = sm.transition_to(PublisherState::STREAMING);
        VERIFY_MSG(ok, "RECONNECTING_OUTPUT → STREAMING should succeed");
        printf("[PASS] RECONNECTING_OUTPUT → STREAMING\n");
    }

    {
        // RECONNECTING_OUTPUT -> STOPPING (shutdown requested while waiting for RTSP reconnect)
        StateMachine sm;
        sm.transition_to(PublisherState::IDLE);
        sm.transition_to(PublisherState::PROBING);
        sm.transition_to(PublisherState::CONNECTING_OUTPUT);
        sm.transition_to(PublisherState::STREAMING);
        sm.transition_to(PublisherState::RECONNECTING_OUTPUT);
        bool ok = sm.transition_to(PublisherState::STOPPING);
        VERIFY_MSG(ok, "RECONNECTING_OUTPUT → STOPPING should succeed");
        printf("[PASS] RECONNECTING_OUTPUT → STOPPING\n");
    }

    {
        // STOPPING -> IDLE (back to waiting after shutdown completes)
        StateMachine sm;
        sm.transition_to(PublisherState::IDLE);
        sm.transition_to(PublisherState::STOPPING);
        bool ok = sm.transition_to(PublisherState::IDLE);
        VERIFY_MSG(ok, "STOPPING → IDLE should succeed");
        printf("[PASS] STOPPING → IDLE\n");
    }

    // -----------------------------------------------------------------
    // RECOVERING_DEVICE transition tests
    // -----------------------------------------------------------------

    {
        // STREAMING -> RECOVERING_DEVICE (GPU TDR occurred)
        StateMachine sm;
        sm.transition_to(PublisherState::IDLE);
        sm.transition_to(PublisherState::PROBING);
        sm.transition_to(PublisherState::CONNECTING_OUTPUT);
        sm.transition_to(PublisherState::STREAMING);
        bool ok = sm.transition_to(PublisherState::RECOVERING_DEVICE);
        VERIFY_MSG(ok, "STREAMING → RECOVERING_DEVICE should succeed");
        VERIFY(sm.current_state() == PublisherState::RECOVERING_DEVICE);
        printf("[PASS] STREAMING → RECOVERING_DEVICE\n");
    }

    {
        // STALLED -> RECOVERING_DEVICE (GPU TDR occurred while STALLED)
        StateMachine sm;
        sm.transition_to(PublisherState::IDLE);
        sm.transition_to(PublisherState::PROBING);
        sm.transition_to(PublisherState::CONNECTING_OUTPUT);
        sm.transition_to(PublisherState::STREAMING);
        sm.transition_to(PublisherState::STALLED);
        bool ok = sm.transition_to(PublisherState::RECOVERING_DEVICE);
        VERIFY_MSG(ok, "STALLED → RECOVERING_DEVICE should succeed");
        VERIFY(sm.current_state() == PublisherState::RECOVERING_DEVICE);
        printf("[PASS] STALLED → RECOVERING_DEVICE\n");
    }

    {
        // RECOVERING_DEVICE -> PROBING (device re-created, re-probe and reconnect)
        StateMachine sm;
        sm.transition_to(PublisherState::IDLE);
        sm.transition_to(PublisherState::PROBING);
        sm.transition_to(PublisherState::CONNECTING_OUTPUT);
        sm.transition_to(PublisherState::STREAMING);
        sm.transition_to(PublisherState::RECOVERING_DEVICE);
        bool ok = sm.transition_to(PublisherState::PROBING);
        VERIFY_MSG(ok, "RECOVERING_DEVICE → PROBING should succeed");
        printf("[PASS] RECOVERING_DEVICE → PROBING\n");
    }

    {
        // RECOVERING_DEVICE -> FATAL (device re-creation failed)
        StateMachine sm;
        sm.transition_to(PublisherState::IDLE);
        sm.transition_to(PublisherState::PROBING);
        sm.transition_to(PublisherState::CONNECTING_OUTPUT);
        sm.transition_to(PublisherState::STREAMING);
        sm.transition_to(PublisherState::RECOVERING_DEVICE);
        bool ok = sm.transition_to(PublisherState::FATAL);
        VERIFY_MSG(ok, "RECOVERING_DEVICE → FATAL should succeed");
        VERIFY(sm.current_state() == PublisherState::FATAL);
        printf("[PASS] RECOVERING_DEVICE → FATAL\n");
    }

    {
        // RECOVERING_DEVICE -> STOPPING (shutdown requested during recovery)
        StateMachine sm;
        sm.transition_to(PublisherState::IDLE);
        sm.transition_to(PublisherState::PROBING);
        sm.transition_to(PublisherState::CONNECTING_OUTPUT);
        sm.transition_to(PublisherState::STREAMING);
        sm.transition_to(PublisherState::RECOVERING_DEVICE);
        bool ok = sm.transition_to(PublisherState::STOPPING);
        VERIFY_MSG(ok, "RECOVERING_DEVICE → STOPPING should succeed");
        printf("[PASS] RECOVERING_DEVICE → STOPPING\n");
    }

    {
        // Invalid transition: IDLE -> RECOVERING_DEVICE is rejected
        StateMachine sm;
        sm.transition_to(PublisherState::IDLE);
        bool ok = sm.transition_to(PublisherState::RECOVERING_DEVICE);
        VERIFY_MSG(!ok, "IDLE → RECOVERING_DEVICE must be rejected");
        VERIFY(sm.current_state() == PublisherState::IDLE);
        printf("[PASS] Invalid IDLE → RECOVERING_DEVICE rejected\n");
    }

    return 0;
}
