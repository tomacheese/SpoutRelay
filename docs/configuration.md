# Configuration Reference

The configuration file is JSON. Pass its path with `--config` (or `-c`); if omitted, `config/config.json` is used. Any key that is omitted falls back to the default listed below. A template is provided in [`config/config.example.json`](../config/config.example.json).

```powershell
spout-relay.exe --config config/config.json
```

## Full example

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
    "prefer_dx11": true,
    "stalled_recovery_max_attempts": 10
  },
  "encoder": {
    "codec": "h264_nvenc",
    "fallback_codec": "h264_mf",
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
  },
  "placeholder": {
    "enabled": false,
    "width": 1280,
    "height": 720,
    "message": "NO SIGNAL",
    "background_hex": "#000000",
    "text_hex": "#FFFFFF",
    "show_sender_name": true
  }
}
```

---

## `app` section

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `instance_name` | string | `"spoutrelay-publisher-01"` | Instance identifier; also the log file name (`<instance_name>.jsonl`) |
| `log_dir` | string | `"./logs"` | Output directory for JSON Lines log files |
| `metrics_path` | string | `"./state/metrics.json"` | Output path for the runtime metrics JSON |
| `health_path` | string | `"./state/health.json"` | Output path for the health status JSON |

---

## `spout` section

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `sender_name` | string | **required** | Name of the Spout sender to connect to. Must match the name registered in Spout shared memory exactly. An empty string is a configuration error |
| `poll_interval_ms` | int | `50` | Polling interval (ms) for sender probing and frame reception |
| `frame_timeout_ms` | int | `300` | Frame reception timeout (ms). If no frame or source response arrives within this time while `STREAMING`, the state becomes `STALLED`. While `CONNECTING_OUTPUT`, the wait for the first frame is `5 ×` this value, after which `SPOUT_RECEIVE_FAILED` is logged and the state returns to `PROBING` |
| `sender_missing_timeout_ms` | int | `800` | In `STALLED`, how long to wait (ms) before checking whether the sender has disappeared. Also scales the `probe_no_sender` debug log in `PROBING` (once every `10 ×` this value) |
| `prefer_dx11` | bool | `true` | Accepted for compatibility but ignored: DX11 shared textures are always used. Setting `false` logs a `prefer_dx11_ignored` warning |
| `stalled_recovery_max_attempts` | int | `10` | Number of consecutive `STALLED` → `RECONNECTING_OUTPUT` cycles without any frame recovery after which a full reconnect including the Spout receiver (restart from `PROBING`) is forced. `0` or less disables this watchdog |

### Finding the sender name

The sending application registers the Spout sender name. For VRChat it is usually `VRCSender1`. You can check it with [SpoutPanel](https://spout.zeal.co/) or with this PowerShell snippet.

```powershell
# List sender names from Spout shared memory
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

## `encoder` section

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `codec` | string | `"h264_nvenc"` | Preferred encoder, as an FFmpeg encoder name (`h264_nvenc`, `h264_mf`, `h264_amf`, `libopenh264`, etc.). Must exist in the FFmpeg build in use; the LGPL builds shipped with releases do not include `libx264` |
| `fallback_codec` | string | `"h264_mf"` | Used when `codec` cannot be opened. An empty string disables the fallback |
| `bitrate_kbps` | int | `4000` | Target bitrate (kbps). Also used as the maximum rate and buffer size |
| `fps` | int | `30` | Encode frame rate (1 to 240) |
| `gop_size` | int | `30` | GOP size (keyframe interval). Equal to `fps` gives a keyframe every second |
| `max_b_frames` | int | `0` | Maximum number of B-frames. `0` is recommended for low-latency streaming |
| `preset` | string | `"fast"` | Encoder preset (speed vs. quality), passed to the codec when non-empty |
| `tune` | string | `"zerolatency"` | Codec tune option. **Not applied to hardware codecs** (`nvenc`, `amf`, `qsv`) |
| `threads` | int | `0` | Encoder thread count. `0` means automatic |

### Codec selection order

```text
codec → avcodec_open2 succeeds? → use it
                       ↓ fails
fallback_codec → avcodec_open2 succeeds? → use it
                                ↓ fails
ENCODER_INIT_FAILED error → FATAL state
```

(During the initial connection, an encoder failure returns to `PROBING` first; `FATAL` is reached from `CONNECTING_OUTPUT` when all codecs fail.)

### Notes for hardware codecs

For hardware codecs (names containing `nvenc`, `amf` or `qsv`) the `tune` option is skipped automatically. Instead the following are set:

- `rc=cbr` (constant bitrate)
- `delay=0` (low latency)

---

## `rtsp` section

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `url` | string | **required** | RTSP server URL; must start with `rtsp://`. Example: `rtsp://192.168.0.100:8554/live` |
| `connect_timeout_ms` | int | `5000` | RTSP connection timeout (ms) |
| `send_timeout_ms` | int | `5000` | Packet send timeout (ms) |
| `max_reconnect_attempts` | int | `5` | Maximum reconnection attempts. Exceeding it leads to `FATAL` |
| `reconnect_delay_ms` | int | `1000` | Initial reconnect wait (ms) |
| `reconnect_max_delay_ms` | int | `30000` | Upper bound of the reconnect wait (ms) |
| `reconnect_backoff_multiplier` | float | `2.0` | Back-off multiplier for the reconnect wait |

### Reconnect back-off example

With `reconnect_delay_ms=1000`, `multiplier=2.0` and `max=30000`:

| Attempt | Wait |
|---------|------|
| 1st | 1000 ms |
| 2nd | 2000 ms |
| 3rd | 4000 ms |
| 4th | 8000 ms |
| 5th | 16000 ms |

---

## `runtime` section

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `shutdown_grace_ms` | int | `3000` | Grace period on shutdown (ms) |
| `emit_metrics_interval_ms` | int | `1000` | Interval for writing `metrics.json` (ms) |
| `emit_health_interval_ms` | int | `1000` | Interval for writing `health.json` (ms) |

---

## `placeholder` section

Settings for continuing to publish a placeholder ("NO SIGNAL") video to RTSP while no Spout source is available (not connected at startup, or the source disappeared while streaming).

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `enabled` | bool | `false` | Enable placeholder publishing. With `false` (default), nothing is published while the source is unavailable |
| `width` | int | `1280` | Placeholder width (px) used when the resolution of the last connected source is unknown. Must be a positive integer |
| `height` | int | `720` | Placeholder height (px) used when the resolution of the last connected source is unknown. Must be a positive integer |
| `message` | string | `"NO SIGNAL"` | Message shown in the centre of the placeholder video |
| `background_hex` | string | `"#000000"` | Background colour in `#RRGGBB` form |
| `text_hex` | string | `"#FFFFFF"` | Text colour for the message and sender name, in `#RRGGBB` form |
| `show_sender_name` | bool | `true` | Show the awaited Spout sender name below the message |

### Behavior

- With `enabled: true`, the state moves to `PLACEHOLDER` while the source is missing and the placeholder video is published at `encoder.fps` (the RTSP stream itself is kept alive).
- The resolution of the last real source is preferred when known; otherwise `width`/`height` are used (this minimizes RTSP reconfiguration on resolution changes).
- When a real source appears, publishing returns automatically to the normal flow (`CONNECTING_OUTPUT` onwards).

---

## Validation

The following are checked at startup:

- `spout.sender_name` is not empty
- `rtsp.url` starts with `rtsp://`
- `encoder.fps` is between 1 and 240
- `encoder.bitrate_kbps` > 0
- `spout.poll_interval_ms` > 0
- `spout.frame_timeout_ms` > 0
- `rtsp.connect_timeout_ms` > 0
- `rtsp.send_timeout_ms` > 0
- `rtsp.max_reconnect_attempts` ≥ 0
- `rtsp.reconnect_delay_ms` > 0
- `rtsp.reconnect_max_delay_ms` > 0
- `rtsp.reconnect_max_delay_ms` ≥ `rtsp.reconnect_delay_ms`
- `rtsp.reconnect_backoff_multiplier` ≥ 1.0
- `placeholder.width` > 0
- `placeholder.height` > 0
- `placeholder.background_hex` is in `#RRGGBB` form
- `placeholder.text_hex` is in `#RRGGBB` form

A failure is printed to standard error as `[ERROR] <message>` (for example `[ERROR] spout.sender_name must not be empty`) and the process exits with code 1.

> **Note**: `spout.stalled_recovery_max_attempts` is intentionally not range-checked, because `0` or less is a valid value meaning "watchdog disabled".
