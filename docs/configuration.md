# 設定リファレンス

設定ファイルは JSON 形式です。`--config` オプションでパスを指定します。

```powershell
publisher-agent.exe --config config/config.json
```

## 完全なサンプル

```json
{
  "app": {
    "instance_name": "spoutrelay-publisher-01",
    "log_dir": "./logs",
    "metrics_path": "./state/metrics.json",
    "health_path": "./state/health.json"
  },
  "spout": {
    "sender_name": "VRCSender1",
    "poll_interval_ms": 50,
    "frame_timeout_ms": 300,
    "sender_missing_timeout_ms": 800,
    "prefer_dx11": true
  },
  "encoder": {
    "codec": "h264_nvenc",
    "fallback_codec": "libx264",
    "bitrate_kbps": 4000,
    "fps": 30,
    "gop_size": 30,
    "max_b_frames": 0,
    "preset": "fast",
    "tune": "zerolatency",
    "threads": 0
  },
  "rtsp": {
    "url": "rtsp://192.168.0.100:8554/live",
    "connect_timeout_ms": 5000,
    "send_timeout_ms": 5000,
    "max_reconnect_attempts": 5,
    "reconnect_delay_ms": 1000,
    "reconnect_max_delay_ms": 30000,
    "reconnect_backoff_multiplier": 2.0
  },
  "runtime": {
    "shutdown_grace_ms": 3000,
    "emit_metrics_interval_ms": 1000,
    "emit_health_interval_ms": 1000
  }
}
```

---

## `app` セクション

| キー | 型 | デフォルト | 説明 |
|-----|----|-----------|------|
| `instance_name` | string | `"spoutrelay-publisher-01"` | ログ・メトリクスに付与するインスタンス識別名 |
| `log_dir` | string | `"./logs"` | JSON Lines ログファイルの出力ディレクトリ |
| `metrics_path` | string | `"./state/metrics.json"` | ランタイムメトリクス JSON の書き出しパス |
| `health_path` | string | `"./state/health.json"` | ヘルスステータス JSON の書き出しパス |

---

## `spout` セクション

| キー | 型 | デフォルト | 説明 |
|-----|----|-----------|------|
| `sender_name` | string | **必須** | 接続する Spout センダーの名前（必須）。Spout 共有メモリに登録されている名前と完全一致。空文字は設定エラー |
| `poll_interval_ms` | int | `50` | センダー探索・フレーム受信のポーリング間隔（ミリ秒） |
| `frame_timeout_ms` | int | `300` | フレーム受信タイムアウト（ミリ秒）。この時間内にフレームが来なければ `SPOUT_RECEIVE_FAILED` |
| `sender_missing_timeout_ms` | int | `800` | センダーが見つからない場合の PROBING 状態での待機上限（ミリ秒） |
| `prefer_dx11` | bool | `true` | DX11 共有テクスチャを優先（現在は常に true で動作） |

### センダー名の調べ方

Spout センダー名は送信側アプリケーションが登録します。VRChat の場合は通常 `VRCSender1` です。[SpoutPanel](https://spout.zeal.co/) や本プロジェクトの PowerShell スニペットで確認できます。

```powershell
# Spout共有メモリからセンダー名一覧を取得
$mmf = [System.IO.MemoryMappedFiles.MemoryMappedFile]::OpenExisting("SpoutSenderNames")
$accessor = $mmf.CreateViewAccessor()
$buf = New-Object byte[] (256 * 10)
$accessor.ReadArray(0, $buf, 0, $buf.Length)
for ($i = 0; $i -lt 10; $i++) {
    $name = [System.Text.Encoding]::ASCII.GetString($buf, $i*256, 256).TrimEnd([char]0)
    if ($name) { Write-Host "Sender: $name" }
}
```

---

## `encoder` セクション

| キー | 型 | デフォルト | 説明 |
|-----|----|-----------|------|
| `codec` | string | `"h264_nvenc"` | 優先エンコーダー。FFmpeg コーデック名（`h264_nvenc`, `libx264`, `h264_amf` 等） |
| `fallback_codec` | string | `"libx264"` | `codec` が利用できない場合のフォールバック。空文字でフォールバック無効 |
| `bitrate_kbps` | int | `4000` | 目標ビットレート（kbps） |
| `fps` | int | `30` | エンコードフレームレート |
| `gop_size` | int | `30` | GOP サイズ（キーフレーム間隔）。`fps` と同値で 1 秒ごとにキーフレーム |
| `max_b_frames` | int | `0` | B フレームの最大数。低遅延配信では `0` 推奨 |
| `preset` | string | `"fast"` | エンコーダープリセット（速度 vs 品質）。`ultrafast` / `fast` / `medium` 等 |
| `tune` | string | `"zerolatency"` | libx264 用チューン。**NVENC では無視される** |
| `threads` | int | `0` | エンコードスレッド数。`0` で自動 |

### コーデック選択の優先順位

```text
codec → avcodec_open2 成功？ → 使用
                     ↓ 失敗
fallback_codec → avcodec_open2 成功？ → 使用
                              ↓ 失敗
ENCODER_INIT_FAILED エラー → FATAL 状態
```

### NVENC 使用時の注意

NVENC では `tune` オプションがサポートされていないため自動的にスキップされます。代わりに以下が設定されます：

- `rc=cbr`（固定ビットレート）
- `delay=0`（低遅延）

---

## `rtsp` セクション

| キー | 型 | デフォルト | 説明 |
|-----|----|-----------|------|
| `url` | string | **必須** | RTSP サーバーの URL。例: `rtsp://192.168.0.100:8554/live` |
| `connect_timeout_ms` | int | `5000` | RTSP 接続タイムアウト（ミリ秒） |
| `send_timeout_ms` | int | `5000` | パケット送信タイムアウト（ミリ秒） |
| `max_reconnect_attempts` | int | `5` | 最大再接続試行回数。超過すると `FATAL` 状態 |
| `reconnect_delay_ms` | int | `1000` | 初回再接続待機時間（ミリ秒） |
| `reconnect_max_delay_ms` | int | `30000` | 再接続待機の上限（ミリ秒） |
| `reconnect_backoff_multiplier` | float | `2.0` | 再接続待機のバックオフ乗数 |

### 再接続バックオフの計算例

`reconnect_delay_ms=1000`, `multiplier=2.0`, `max=30000` の場合:

| 試行 | 待機時間 |
|-----|---------|
| 1回目 | 1000ms |
| 2回目 | 2000ms |
| 3回目 | 4000ms |
| 4回目 | 8000ms |
| 5回目 | 16000ms |

---

## `runtime` セクション

| キー | 型 | デフォルト | 説明 |
|-----|----|-----------|------|
| `shutdown_grace_ms` | int | `3000` | シャットダウン時のグレースフル待機時間（ミリ秒） |
| `emit_metrics_interval_ms` | int | `1000` | `metrics.json` の書き出し間隔（ミリ秒） |
| `emit_health_interval_ms` | int | `1000` | `health.json` の書き出し間隔（ミリ秒） |

---

## 設定のバリデーション

起動時に以下がチェックされます：

- `spout.sender_name` が空でないこと
- `rtsp.url` が空でないこと
- `encoder.bitrate_kbps` > 0
- `encoder.fps` > 0
- `spout.poll_interval_ms` > 0
- `spout.frame_timeout_ms` > 0
- `rtsp.connect_timeout_ms` > 0
- `rtsp.send_timeout_ms` > 0
- `rtsp.max_reconnect_attempts` ≥ 0
- `rtsp.reconnect_delay_ms` > 0
- `rtsp.reconnect_max_delay_ms` > 0
- `rtsp.reconnect_max_delay_ms` ≥ `rtsp.reconnect_delay_ms`
- `rtsp.reconnect_backoff_multiplier` ≥ 1.0

バリデーションエラーは `CONFIG_VALIDATION_ERROR` コードで標準エラーへ出力されます。
