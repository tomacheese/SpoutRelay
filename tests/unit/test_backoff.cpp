#include <cstdio>
#include "common/types.hpp"
#include "test_utils.hpp"

int run_backoff_tests() {
    printf("=== Backoff Tests ===\n");

    {
        // Initial state
        BackoffState b;
        VERIFY(b.attempt  == 0);
        VERIFY(b.delay_ms == 1000);
        printf("[PASS] BackoffState default initialised\n");
    }

    {
        // Reset with custom initial value
        BackoffState b;
        b.reset(500);
        VERIFY(b.attempt  == 0);
        VERIFY(b.delay_ms == 500);
        printf("[PASS] reset(500) sets delay_ms=500\n");
    }

    {
        // next_delay increments attempt and doubles delay
        BackoffState b;
        b.reset(1000);
        int d1 = b.next_delay(30000, 2.0f);
        VERIFY(d1 == 1000);
        VERIFY(b.attempt  == 1);
        VERIFY(b.delay_ms == 2000);

        int d2 = b.next_delay(30000, 2.0f);
        VERIFY(d2 == 2000);
        VERIFY(b.attempt  == 2);
        VERIFY(b.delay_ms == 4000);
        printf("[PASS] Exponential backoff doubles correctly\n");
    }

    {
        // Delay capped at max_ms
        BackoffState b;
        b.reset(16000);
        int d = b.next_delay(30000, 2.0f);
        VERIFY(d == 16000);
        VERIFY(b.delay_ms == 30000);

        int d2 = b.next_delay(30000, 2.0f);
        VERIFY(d2 == 30000);
        VERIFY(b.delay_ms == 30000); // capped
        printf("[PASS] Backoff capped at max_ms=30000\n");
    }

    {
        // Reset after reaching max restores initial value
        BackoffState b;
        b.reset(1000);
        for (int i = 0; i < 6; ++i) b.next_delay(30000, 2.0f);
        b.reset(1000);
        VERIFY(b.attempt  == 0);
        VERIFY(b.delay_ms == 1000);
        printf("[PASS] reset() restores initial delay after escalation\n");
    }

    return 0;
}
