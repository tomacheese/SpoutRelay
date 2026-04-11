# ステートマシン

## 状態一覧

| 状態 | 説明 |
|------|------|
| `INIT` | 起動直後の初期状態。コンポーネントの初期化を行う |
| `IDLE` | Spout センダーを待機中。設定ロード・ログ初期化済み |
| `PROBING` | 指定センダーの存在を確認中（SpoutDX GetSenderInfo） |
| `CONNECTING_OUTPUT` | エンコーダー初期化 → RTSP 接続 → 初回フレーム受信を試みる |
| `STREAMING` | 正常配信中。フレームキャプチャ → エンコード → RTSP 送信 |
| `STALLED` | フレームが一定時間届かない（センダー停止の可能性） |
| `RECONFIGURING` | センダーの解像度変更を検出。エンコーダーを再初期化 |
| `RECONNECTING_OUTPUT` | RTSP 切断後の再接続待機（バックオフ） |
| `STOPPING` | シャットダウン処理中（Ctrl+C 受信後） |
| `FATAL` | 回復不能エラー。アプリ終了 |

## 状態遷移図

```text
                    ┌─────────┐
                    │  INIT   │
                    └────┬────┘
                         │ 初期化成功
                         ▼
                    ┌─────────┐
              ┌────▶│  IDLE   │
              │     └────┬────┘
              │          │ センダー待機開始
              │          ▼
              │     ┌──────────┐
              │ ┌──▶│ PROBING  │◀──────────────────┐
              │ │   └────┬─────┘                    │
              │ │        │ センダー発見              │
              │ │        ▼                          │
              │ │   ┌───────────────────┐           │
              │ │   │ CONNECTING_OUTPUT │           │
              │ │   └────────┬──────────┘           │
              │ │            │ エンコーダー初期化成功  │
              │ │            │ RTSP 接続成功         │
              │ │            │ 初回フレーム受信成功   │
              │ │            ▼                      │
              │ │      ┌──────────────┐             │
              │ │      │  STREAMING   │─────────────┘
              │ │      └──┬───────────┘ センダー消失/RTSP切断
              │ │         │                         │
              │ │    フレーム                  RTSP エラー
              │ │    タイムアウト                    │
              │ │         ▼                         ▼
              │ │    ┌──────────┐      ┌────────────────────────┐
              │ │    │ STALLED  │      │  RECONNECTING_OUTPUT   │
              │ │    └────┬─────┘      └───────────┬────────────┘
              │ │         │ 回復                    │ 再接続成功
              │ │         │                        │
              │ │         └────────────────────────┘
              │ │
              │ │   ┌──────────────────┐
              │ └───│  RECONFIGURING   │ (解像度変更検出)
              │     └──────────────────┘
              │
              │     ┌──────────┐
              └─────│ STOPPING │ (Ctrl+C)
                    └──────────┘
                         │
                         ▼
                    ┌─────────┐
                    │  FATAL  │ (回復不能エラー)
                    └─────────┘
```

## 有効な遷移一覧

| From | To | 条件 |
|------|----|------|
| INIT | IDLE | 初期化成功 |
| INIT | FATAL | 初期化失敗 |
| IDLE | PROBING | 起動 |
| IDLE | STOPPING | シャットダウン要求 |
| PROBING | CONNECTING_OUTPUT | センダー発見 |
| PROBING | IDLE | タイムアウト（センダー未検出） |
| PROBING | STOPPING | シャットダウン要求 |
| CONNECTING_OUTPUT | STREAMING | 全接続成功 |
| CONNECTING_OUTPUT | PROBING | フレームタイムアウト / エンコーダー失敗 |
| CONNECTING_OUTPUT | STOPPING | シャットダウン要求 |
| STREAMING | STALLED | フレームタイムアウト |
| STREAMING | RECONFIGURING | 解像度変更検出 |
| STREAMING | RECONNECTING_OUTPUT | RTSP 送信エラー |
| STREAMING | STOPPING | シャットダウン要求 |
| STREAMING | IDLE | センダー消失（長時間 STALLED） |
| STREAMING | PROBING | エンコードエラー |
| STALLED | STREAMING | フレーム再開 |
| STALLED | RECONNECTING_OUTPUT | RTSP エラー検出 |
| STALLED | PROBING | センダー消失確認 |
| STALLED | IDLE | センダー消失（長時間 STALLED） |
| STALLED | STOPPING | シャットダウン要求 |
| RECONFIGURING | STREAMING | 再設定成功 |
| RECONFIGURING | STOPPING | シャットダウン要求 |
| RECONNECTING_OUTPUT | STREAMING | 再接続成功 |
| RECONNECTING_OUTPUT | FATAL | 最大再接続回数超過 |
| RECONNECTING_OUTPUT | STOPPING | シャットダウン要求 |
| STOPPING | IDLE | 停止完了（待機状態へ戻る） |

## イベントログの状態変化エントリ

配信中のログには以下の形式で状態変化が記録されます:

```json
{"ts":"2026-04-09T22:05:44.473Z","event":"state_changed","from":"CONNECTING_OUTPUT","to":"STREAMING"}
```

## エラーコードと状態の関係

| エラーコード | 発生状態 | 次の状態 |
|------------|---------|---------|
| `SPOUT_RECEIVE_FAILED` | `CONNECTING_OUTPUT` | `PROBING` |
| `ENCODER_INIT_FAILED` | `CONNECTING_OUTPUT` | `PROBING` / `FATAL` |
| `RTSP_CONNECT_FAILED` | `CONNECTING_OUTPUT` | `RECONNECTING_OUTPUT` |
| `RTSP_SEND_FAILED` | `STREAMING` | `RECONNECTING_OUTPUT` |
| `RTSP_TIMEOUT` | `STREAMING` | `RECONNECTING_OUTPUT` |
| `FATAL_ERROR` | 任意 | `FATAL` |
