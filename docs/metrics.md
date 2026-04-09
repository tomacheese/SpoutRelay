# メトリクス & ヘルス

アプリケーションは実行中に 2 種類の JSON ファイルをディスクへ書き出します。書き出し間隔は `runtime.emit_metrics_interval_ms` / `emit_health_interval_ms` で設定します（デフォルト: 各 1 秒）。

## health.json

アプリの現在の状態と基本情報を表します。外部の死活監視ツールや Docker ヘルスチェックに利用できます。

### フォーマット

```json
{
  "status": "ok",
  "state": "STREAMING",
  "instance": "spoutrelay-publisher-01",
  "uptime_sec": 42,
  "sender": "VRCSender1",
  "rtsp_url": "rtsp://192.168.0.100:8554/live",
  "encoder_codec": "h264_nvenc",
  "ts": "2026-04-09T22:10:00.000Z"
}
```

### フィールド説明

| フィールド | 型 | 説明 |
|----------|----|------|
| `status` | string | `"ok"` / `"degraded"` / `"error"` |
| `state` | string | 現在のステートマシン状態名 |
| `instance` | string | 設定の `app.instance_name` |
| `uptime_sec` | int | セッション開始からの経過秒数 |
| `sender` | string | 接続中の Spout センダー名 |
| `rtsp_url` | string | 設定された RTSP URL |
| `encoder_codec` | string | 使用中のエンコーダーコーデック名 |
| `ts` | string | ISO 8601 タイムスタンプ（UTC） |

### `status` の判定ロジック

| 状態 | status |
|------|--------|
| `STREAMING` | `ok` |
| `STALLED`, `RECONNECTING_OUTPUT`, `RECONFIGURING` | `degraded` |
| `FATAL` | `error` |
| その他 | `ok` |

---

## metrics.json

配信パフォーマンスの詳細なカウンターを含みます。

### フォーマット

```json
{
  "instance": "spoutrelay-publisher-01",
  "state": "STREAMING",
  "sender": {
    "name": "VRCSender1",
    "width": 3840,
    "height": 2160,
    "fps": 30.0
  },
  "encoder": {
    "codec": "h264_nvenc",
    "bitrate_kbps": 1984.5,
    "current_fps": 29.97
  },
  "counters": {
    "frames_received": 1234,
    "frames_encoded": 1230,
    "frames_dropped": 4,
    "rtsp_errors": 0,
    "reconnect_attempts": 0
  },
  "rtsp_url": "rtsp://192.168.0.100:8554/live",
  "uptime_sec": 42,
  "ts": "2026-04-09T22:10:00.000Z"
}
```

### フィールド説明

#### `sender`

| フィールド | 説明 |
|----------|------|
| `name` | Spout センダー名 |
| `width` / `height` | フレーム解像度（ピクセル） |
| `fps` | センダー側のフレームレート |

#### `encoder`

| フィールド | 説明 |
|----------|------|
| `codec` | 使用中のコーデック（`h264_nvenc` 等） |
| `bitrate_kbps` | 計測ビットレート（kbps） |
| `current_fps` | エンコーダーの実効フレームレート |

#### `counters`

| フィールド | 説明 |
|----------|------|
| `frames_received` | Spout から受信したフレーム総数 |
| `frames_encoded` | エンコード完了フレーム総数 |
| `frames_dropped` | キューが満杯でドロップしたフレーム数 |
| `rtsp_errors` | RTSP 送信エラーの累計数 |
| `reconnect_attempts` | RTSP 再接続試行の累計数 |

---

## イベントログ (JSON Lines)

`app.log_dir` に指定したディレクトリへ JSON Lines 形式でログが書き出されます。

### ファイル名

```
logs/<instance_name>_<YYYYMMDD>.jsonl
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

| コード | 説明 |
|--------|------|
| `CONFIG_NOT_FOUND` | 設定ファイルが見つからない |
| `CONFIG_PARSE_ERROR` | 設定ファイルの JSON パースエラー |
| `CONFIG_VALIDATION_ERROR` | 設定値のバリデーションエラー |
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
if ($health.status -ne "ok") {
    Write-Error "Publisher is not healthy: $($health.state)"
}
```

### フレームドロップ率の確認

```powershell
$m = Get-Content state\metrics.json | ConvertFrom-Json
$dropRate = $m.counters.frames_dropped / $m.counters.frames_received * 100
Write-Host "Drop rate: $([math]::Round($dropRate, 2))%"
```
