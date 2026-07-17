# ステートマシン

## 状態一覧

| 状態 | 説明 |
|------|------|
| `INIT` | 起動直後の初期状態。コンポーネントの初期化を行う |
| `IDLE` | Spout センダーを待機中。設定ロード・ログ初期化済み |
| `PROBING` | 指定センダーの存在を確認中（SpoutDX GetSenderInfo） |
| `PLACEHOLDER` | センダー未検出時に「NO SIGNAL」プレースホルダ映像を配信中（`placeholder.enabled=true` の場合のみ） |
| `CONNECTING_OUTPUT` | エンコーダー初期化 → RTSP 接続 → 初回フレーム受信を試みる |
| `STREAMING` | 正常配信中。フレームキャプチャ → エンコード → RTSP 送信 |
| `STALLED` | フレームが一定時間届かない（センダー停止の可能性） |
| `RECONFIGURING` | センダーの解像度変更を検出。エンコーダーを再初期化 |
| `RECONNECTING_OUTPUT` | RTSP 切断後の再接続待機（バックオフ） |
| `RECOVERING_DEVICE` | GPU TDR によるデバイスロスト検出後、D3D11 デバイスを再作成中 |
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
              │ │        │                          │
              │ │        │  未検出 & placeholder 有効 │
              │ │        │        │                 │
              │ │        │        ▼                 │
              │ │        │  ┌─────────────┐          │
              │ │        │  │ PLACEHOLDER │          │
              │ │        │  └──────┬──────┘          │
              │ │        │         │ センダー発見     │
              │ │        ▼         ▼                 │
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
              │     ┌────────────────────┐
              │     │  RECOVERING_DEVICE │ (GPU TDR 検出)
              │     └─────────┬──────────┘
              │               │ 再作成成功 → PROBING へ
              │               │ 再作成失敗 → FATAL へ
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
| PROBING | PLACEHOLDER | センダー未検出 & `placeholder.enabled=true` |
| PROBING | STOPPING | シャットダウン要求 |
| PLACEHOLDER | CONNECTING_OUTPUT | センダー発見（プレースホルダ配信から復帰） |
| PLACEHOLDER | STOPPING | シャットダウン要求 |
| CONNECTING_OUTPUT | STREAMING | 全接続成功 |
| CONNECTING_OUTPUT | PROBING | フレームタイムアウト / エンコーダー失敗 |
| CONNECTING_OUTPUT | STOPPING | シャットダウン要求 |
| CONNECTING_OUTPUT | FATAL | 全コーデックでエンコーダー初期化失敗 |
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
| STALLED | RECOVERING_DEVICE | GPU TDR 検出 |
| STREAMING | RECOVERING_DEVICE | GPU TDR 検出 |
| RECOVERING_DEVICE | PROBING | デバイス再作成成功 → 再探索へ |
| RECOVERING_DEVICE | FATAL | デバイス再作成失敗 |
| RECOVERING_DEVICE | STOPPING | シャットダウン要求 |
| RECONFIGURING | STREAMING | 再設定成功 |
| RECONFIGURING | STOPPING | シャットダウン要求 |
| RECONFIGURING | FATAL | 解像度変更後のエンコーダー再初期化失敗 |
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
| `SPOUT_REINIT_FAILED` | `RECOVERING_DEVICE` | `FATAL` |
| `FATAL_ERROR` | 任意 | `FATAL` |

## GPU デバイスロスト回復（`RECOVERING_DEVICE`）

GPU の Timeout Detection and Recovery（TDR）が発生すると、NVENC が使用している
D3D11 デバイスが `DXGI_ERROR_DEVICE_REMOVED` 状態になります。

### 検出方式

Supervisor のメインループが `STREAMING` / `STALLED` 状態のたびに
`ID3D11Device::GetDeviceRemovedReason()` をポーリングし、`S_OK` 以外であれば
`RECOVERING_DEVICE` へ遷移します。

また `EncoderController::encode()` の GPU パス先頭でも同じチェックを行い、
`CopySubresourceRegion()` 前にアクセス違反を防止します。

### 回復フロー

```
STREAMING / STALLED
    ↓ GetDeviceRemovedReason() != S_OK
RECOVERING_DEVICE
    ↓ teardown_streaming()
    ↓ SpoutMonitor::reinit_device()
        CloseDirectX11() → OpenDirectX11() → マルチスレッド保護再設定
    ↓ 成功
PROBING → CONNECTING_OUTPUT → STREAMING  （自動復帰）
    ↓ 失敗
FATAL
```

### 関連ログイベント

| イベント | レベル | 説明 |
|---------|-------|------|
| `gpu_device_lost` | `warn` | デバイスロスト検出・回復開始 |
| `gpu_device_reinit_ok` | `info` | デバイス再作成成功 |
| `SPOUT_REINIT_FAILED` | `error` | デバイス再作成失敗（FATAL へ） |

## STALLED からの保険的ウォッチドッグ (`stalled_recovery_forced`)

`STALLED` からの復帰は通常 RTSP レイヤーのみの再接続 (`RECONNECTING_OUTPUT`) で行われますが、
GPU/CPU 受信パス切替時に spoutDX 内部の更新イベントフラグ (`m_bUpdated`) が正しく
再初期化されないなどの要因で、センダー自体は生存し続けているのに `frame_pump_` が
二度とフレームを受信できなくなる膠着状態が起こりえます。この場合 RTSP 層の再接続を
繰り返しても実際のフレーム受信は永遠に回復しません。

この保険的対策として、`handle_stalled()` は STALLED → RECONNECTING_OUTPUT を試みても
一度も本当の復帰 (フレーム受信の再開) に至らなかった連続回数
(`consecutive_stall_recoveries_`) をカウントし、`spout.stalled_recovery_max_attempts`
(既定 `10`) に達すると、RTSP のみの再接続では回復不能な膠着状態とみなして
Spout 受信側を含めた完全な再接続を強制します。

### 動作フロー

```
STALLED
  ↓ (frame_pump_ 生存確認 → 復帰していない)
  ↓ ++consecutive_stall_recoveries_
  ↓ consecutive_stall_recoveries_ >= stalled_recovery_max_attempts ?
  │
  ├─ No  → 従来どおり teardown_rtsp() → RECONNECTING_OUTPUT (RTSP のみ再接続)
  │
  └─ Yes → stalled_recovery_forced ログ出力 → consecutive_stall_recoveries_ = 0
           → frame_pump_ 停止・reset → spout_monitor_->disconnect()
           → teardown_rtsp() → teardown_encoder() → PROBING (完全な再接続)
```

`consecutive_stall_recoveries_` は、真にフレーム受信が回復した場合
(`handle_stalled()` の `stall_recovered`) と、`handle_connecting_output()` が
STREAMING へ正常に遷移した場合の両方でゼロにリセットされます。

`stalled_recovery_max_attempts` を `0` 以下に設定するとこのウォッチドッグは無効化され、
従来どおり RTSP のみの再接続を無制限に繰り返します。

### 関連ログイベント

| イベント | レベル | 説明 |
|---------|-------|------|
| `stalled_recovery_forced` | `warn` | RTSP のみの再接続を規定回数繰り返しても復帰しなかったため、Spout 側を含めた完全な再接続を強制した |
