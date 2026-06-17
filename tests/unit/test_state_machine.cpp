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
        // CONNECTING_OUTPUT → FATAL (エンコーダー初期化が全コーデックで失敗した場合)
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
        // (ソースが見つからない間 NO SIGNAL 映像を配信し、ソース出現で復帰する)
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
        // PLACEHOLDER → STOPPING (シャットダウン要求時)
        StateMachine sm;
        sm.transition_to(PublisherState::IDLE);
        sm.transition_to(PublisherState::PROBING);
        sm.transition_to(PublisherState::PLACEHOLDER);
        bool ok = sm.transition_to(PublisherState::STOPPING);
        VERIFY_MSG(ok, "PLACEHOLDER → STOPPING should succeed");
        printf("[PASS] PLACEHOLDER → STOPPING\n");
    }

    {
        // RECONFIGURING → FATAL (解像度変更後のエンコーダー再初期化が失敗した場合)
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
    // 以下、issue #20 の調査で判明した未カバー遷移を網羅する
    // -----------------------------------------------------------------

    {
        // INIT → FATAL (起動直後の致命的エラー)
        StateMachine sm;
        bool ok = sm.transition_to(PublisherState::FATAL);
        VERIFY_MSG(ok, "INIT → FATAL should succeed");
        VERIFY(sm.current_state() == PublisherState::FATAL);
        printf("[PASS] INIT → FATAL\n");
    }

    {
        // IDLE → STOPPING (起動直後のシャットダウン要求)
        StateMachine sm;
        sm.transition_to(PublisherState::IDLE);
        bool ok = sm.transition_to(PublisherState::STOPPING);
        VERIFY_MSG(ok, "IDLE → STOPPING should succeed");
        printf("[PASS] IDLE → STOPPING\n");
    }

    {
        // PROBING → IDLE (探索を中断して待機状態へ戻る)
        StateMachine sm;
        sm.transition_to(PublisherState::IDLE);
        sm.transition_to(PublisherState::PROBING);
        bool ok = sm.transition_to(PublisherState::IDLE);
        VERIFY_MSG(ok, "PROBING → IDLE should succeed");
        printf("[PASS] PROBING → IDLE\n");
    }

    {
        // PROBING → STOPPING (探索中のシャットダウン要求)
        StateMachine sm;
        sm.transition_to(PublisherState::IDLE);
        sm.transition_to(PublisherState::PROBING);
        bool ok = sm.transition_to(PublisherState::STOPPING);
        VERIFY_MSG(ok, "PROBING → STOPPING should succeed");
        printf("[PASS] PROBING → STOPPING\n");
    }

    {
        // CONNECTING_OUTPUT → PROBING (出力接続中にソースが消失)
        StateMachine sm;
        sm.transition_to(PublisherState::IDLE);
        sm.transition_to(PublisherState::PROBING);
        sm.transition_to(PublisherState::CONNECTING_OUTPUT);
        bool ok = sm.transition_to(PublisherState::PROBING);
        VERIFY_MSG(ok, "CONNECTING_OUTPUT → PROBING should succeed");
        printf("[PASS] CONNECTING_OUTPUT → PROBING\n");
    }

    {
        // CONNECTING_OUTPUT → STOPPING (出力接続中のシャットダウン要求)
        StateMachine sm;
        sm.transition_to(PublisherState::IDLE);
        sm.transition_to(PublisherState::PROBING);
        sm.transition_to(PublisherState::CONNECTING_OUTPUT);
        bool ok = sm.transition_to(PublisherState::STOPPING);
        VERIFY_MSG(ok, "CONNECTING_OUTPUT → STOPPING should succeed");
        printf("[PASS] CONNECTING_OUTPUT → STOPPING\n");
    }

    {
        // STREAMING → STOPPING (配信中のシャットダウン要求)
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
        // STREAMING → IDLE (配信を停止し待機状態へ戻る)
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
        // STREAMING → PROBING (配信中にソースが消失し再探索へ)
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
        // STALLED → STREAMING (送出停滞からの復帰)
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
        // STALLED → RECONNECTING_OUTPUT (停滞中に RTSP 出力エラー発生)
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
        // STALLED → STOPPING (停滞中のシャットダウン要求)
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
        // STALLED → IDLE (停滞を解消し待機状態へ戻る)
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
        // STALLED → PROBING (停滞中にソースが完全に消失し再探索へ)
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
        // STALLED → PLACEHOLDER (送出元消失 → NO SIGNAL シームレス切替)
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
        // RECONFIGURING → STREAMING (解像度変更後のエンコーダー再初期化に成功)
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
        // RECONFIGURING → STOPPING (再構成中のシャットダウン要求)
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
        // RECONNECTING_OUTPUT → STREAMING (RTSP 再接続に成功)
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
        // RECONNECTING_OUTPUT → STOPPING (RTSP 再接続待ち中のシャットダウン要求)
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
        // STOPPING → IDLE (シャットダウン処理完了後、待機状態へ復帰)
        StateMachine sm;
        sm.transition_to(PublisherState::IDLE);
        sm.transition_to(PublisherState::STOPPING);
        bool ok = sm.transition_to(PublisherState::IDLE);
        VERIFY_MSG(ok, "STOPPING → IDLE should succeed");
        printf("[PASS] STOPPING → IDLE\n");
    }

    // -----------------------------------------------------------------
    // issue #26: RECOVERING_DEVICE 遷移テスト
    // -----------------------------------------------------------------

    {
        // STREAMING → RECOVERING_DEVICE (GPU TDR 発生時)
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
        // STALLED → RECOVERING_DEVICE (STALLED 中に GPU TDR 発生)
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
        // RECOVERING_DEVICE → PROBING (デバイス再作成成功 → 再探索・再接続)
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
        // RECOVERING_DEVICE → FATAL (デバイス再作成失敗)
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
        // RECOVERING_DEVICE → STOPPING (回復中にシャットダウン要求)
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
        // 不正遷移: IDLE → RECOVERING_DEVICE は拒否される
        StateMachine sm;
        sm.transition_to(PublisherState::IDLE);
        bool ok = sm.transition_to(PublisherState::RECOVERING_DEVICE);
        VERIFY_MSG(!ok, "IDLE → RECOVERING_DEVICE must be rejected");
        VERIFY(sm.current_state() == PublisherState::IDLE);
        printf("[PASS] Invalid IDLE → RECOVERING_DEVICE rejected\n");
    }

    return 0;
}
