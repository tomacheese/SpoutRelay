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

    return 0;
}
