# メトリクス & ヘルス

アプリケーションは実行中に 2 種類の JSON ファイルをディスクへ書き出します。書き出し間隔は `runtime.emit_metrics_interval_ms` / `emit_health_interval_ms` で設定します（デフォルト: 各 1 秒）。

## health.json

アプリの現在の状態と基本情報を表します。外部の死活監視ツールや Docker ヘルスチェックに利用できます。

### フォーマット

```json
{
  "healthy": true,
  "state": "STREAMING",
  "ts": "2026-04-09T22:10:00.000Z"
}
```

### フィールド説明

| フィールド | 型 | 説明 |
|----------|----|------|
| `healthy` | bool | `true`: 正常状態（IDLE/PROBING/CONNECTING_OUTPUT/STREAMING）、`false`: それ以外 |
| `state` | string | 現在のステートマシン状態名 |
| `ts` | string | ISO 8601 タイムスタンプ（UTC） |

### `healthy` の判定ロジック

| 状態 | healthy |
|------|---------|
| `IDLE`, `PROBING`, `CONNECTING_OUTPUT`, `STREAMING` | `true` |
| `STALLED`, `RECONNECTING_OUTPUT`, `RECONFIGURING`, `FATAL`, その他 | `false` |

---

## metrics.json

配信パフォーマンスの詳細なカウンターを含みます。

### フォーマット

```json
{
  "state": "STREAMING",
  "sender_name": "VRCSender1",
  "sender_width": 3840,
  "sender_height": 2160,
  "sender_fps": 30.0,
  "bitrate_kbps": 1984.5,
  "current_fps": 29.97,
  "rtsp_url": "rtsp://192.168.0.100:8554/live",
  "encoder_codec": "h264_nvenc",
  "uptime_ms": 42000,
  "frames_received": 1234,
  "frames_encoded": 1230,
  "frames_dropped": 4,
  "rtsp_errors": 0,
  "reconnect_attempts": 0,
  "ts": "2026-04-09T22:10:00.000Z"
}
```

### フィールド説明

| フィールド | 型 | 説明 |
|----------|----|------|
| `state` | string | 現在のステートマシン状態名 |
| `sender_name` | string | Spout センダー名 |
| `sender_width` / `sender_height` | int | フレーム解像度（ピクセル） |
| `sender_fps` | float | センダー側のフレームレート |
| `bitrate_kbps` | float | 計測ビットレート（kbps） |
| `current_fps` | float | エンコーダーの実効フレームレート |
| `rtsp_url` | string | 設定された RTSP URL |
| `encoder_codec` | string | 使用中のコーデック（`h264_nvenc` 等） |
| `uptime_ms` | int | セッション開始からの経過ミリ秒数 |
| `frames_received` | int | Spout から受信したフレーム総数 |
| `frames_encoded` | int | エンコード完了フレーム総数 |
| `frames_dropped` | int | エンコード失敗でドロップしたフレーム数 |
| `rtsp_errors` | int | RTSP 送信エラーの累計数 |
| `reconnect_attempts` | int | RTSP 再接続試行の累計数 |
| `ts` | string | ISO 8601 タイムスタンプ（UTC） |

---

## イベントログ (JSON Lines)

`app.log_dir` に指定したディレクトリへ JSON Lines 形式でログが書き出されます。

### ファイル名

```
logs/<instance_name>.jsonl
```

### イベント種別

#### state_changed

```json
{"ts":"2026-04-09T22:05:44.473Z","event":"state_changed","from":"CONNECTING_OUTPUT","to":"STREAMING"}
```

#### sender_found

```json
{"ts":"2026-04-09T22:05:42.475Z","event":"sender_found","name":"VRCSender1"}
```

#### probe_no_sender

```json
{"ts":"2026-04-09T22:05:42.135Z","event":"probe_no_sender","name":"VRCSender1"}
```

#### encoder_initialized

```json
{"ts":"2026-04-09T22:05:44.335Z","event":"encoder_initialized","codec":"h264_nvenc","width":"3840","height":"2160","fps":"30","bitrate_kbps":"2000"}
```

#### publish_started

```json
{"ts":"2026-04-09T22:05:44.473Z","event":"publish_started","url":"rtsp://192.168.0.100:8554/live","width":"3840","height":"2160","fps":"30"}
```

#### error

```json
{"ts":"2026-04-09T22:05:44.707Z","event":"error","code":"SPOUT_RECEIVE_FAILED","message":"Timeout waiting for first frame"}
```

### エラーコード一覧

> **注意**: 設定ファイルエラー（`CONFIG_NOT_FOUND` / `CONFIG_PARSE_ERROR` / `CONFIG_VALIDATION_ERROR`）は起動時に標準エラー出力へのみ出力されます。アプリケーションが起動できなかった場合はイベントログに記録されません。

| コード | 説明 |
|--------|------|
| `SPOUT_INIT_FAILED` | SpoutDX の初期化失敗 |
| `SPOUT_CONNECT_FAILED` | Spout センダーへの接続失敗 |
| `SPOUT_RECEIVE_FAILED` | フレーム受信タイムアウト |
| `ENCODER_INIT_FAILED` | エンコーダー初期化失敗（全コーデック試行後） |
| `ENCODER_ENCODE_FAILED` | エンコード中のエラー |
| `RTSP_CONNECT_FAILED` | RTSP サーバーへの接続失敗 |
| `RTSP_SEND_FAILED` | RTP パケット送信エラー |
| `RTSP_TIMEOUT` | RTSP 送信タイムアウト |
| `FATAL_ERROR` | 回復不能なエラー |

---

## メトリクスの活用例

### シェルスクリプトによるヘルスチェック

```powershell
$health = Get-Content state\health.json | ConvertFrom-Json
if (-not $health.healthy) {
    Write-Error "Publisher is not healthy: $($health.state)"
}
```

### フレームドロップ率の確認

```powershell
$m = Get-Content state\metrics.json | ConvertFrom-Json
$dropRate = $m.frames_dropped / $m.frames_received * 100
Write-Host "Drop rate: $([math]::Round($dropRate, 2))%"
```
