# Metrics & Health

While running, the application writes two JSON files to disk. The write intervals are set by `runtime.emit_metrics_interval_ms` / `runtime.emit_health_interval_ms` (default: 1 second each).

> **Write optimisation (unchanged payloads are skipped)**
>
> If the payload excluding `ts` and `uptime_ms` is identical to the previous write, the actual file write is skipped.
>
> - **health.json**: rewritten essentially only when `state` / `healthy` changes (practically never during steady `STREAMING`).
> - **metrics.json**: rewritten on every interval while counters such as `frames_received` increase, but skipped in states where nothing changes, such as `IDLE` or `STALLED`.
>
> As a result, the `ts` field means the following:
>
> | File | Meaning of `ts` |
> |------|-----------------|
> | `health.json` | **Time the health state (`state`/`healthy`) last changed** |
> | `metrics.json` | **Time the metrics (frame counters etc.) last changed** |
>
> **Impact on liveness monitoring**:
> - During `STREAMING` the frame counters increase every second, so `ts` in `metrics.json` is updated roughly every second.
> - **During `IDLE` / `STALLED`, neither counters nor state change, so `ts` is not updated in either file.** To check that the process is alive in those states, use an OS-level process check (for example `Get-Process spout-relay`) instead of relying on `ts`.

> **Emit interval and storage write frequency**
>
> The default 1-second interval (`emit_metrics_interval_ms = 1000`) is a general-purpose setting and can be tuned.
> - **Shorter**: better monitoring responsiveness, but more writes while the state or counters change frequently.
> - **Longer**: fewer writes. Matching the polling interval of the monitoring tool is reasonable (for example 5000 to 10000 ms).
> - On slow or low-endurance media such as SD cards, eMMC or network shares, lengthen the interval or consider the RAM disk setup below.

> **Writing to a RAM disk (tmpfs)**
>
> Where storage lifetime is a concern (embedded devices, SD-card boot, etc.), point `metrics_path` / `health_path` at a RAM disk to eliminate disk writes.
>
> ```jsonc
> // config.json
> {
>   "app": {
>     "metrics_path": "R:\\state\\metrics.json",
>     "health_path":  "R:\\state\\health.json"
>   }
> }
> ```
>
> On Windows, tools such as [ImDisk](https://sourceforge.net/projects/imdisk-toolkit/) can provide a RAM disk drive (for example `R:\`). The files disappear when the machine restarts, but external monitoring tools can still read them in real time.

Both files are written atomically (a temporary file is replaced over the target), so readers never see a half-written file. If a write fails, the previous file is kept and a `STATE_FILE_WRITE_FAILED` error is logged.

## health.json

Represents the current state of the app. It can be used by external liveness monitors or Docker health checks.

### Format

```json
{
  "healthy": true,
  "state": "STREAMING",
  "ts": "2026-04-09T22:10:00.000Z"
}
```

### Fields

| Field | Type | Description |
|-------|------|-------------|
| `healthy` | bool | `true` in a normal state (IDLE/PROBING/PLACEHOLDER/CONNECTING_OUTPUT/STREAMING), `false` otherwise |
| `state` | string | Current state machine state name |
| `ts` | string | Time (ISO 8601 UTC) at which the health state (`state` / `healthy`) **last changed**. Not updated while the state is stable |

### How `healthy` is determined

| State | healthy |
|-------|---------|
| `IDLE`, `PROBING`, `PLACEHOLDER`, `CONNECTING_OUTPUT`, `STREAMING` | `true` |
| `STALLED`, `RECONNECTING_OUTPUT`, `RECONFIGURING`, `RECOVERING_DEVICE`, `STOPPING`, `FATAL` | `false` |

`PLACEHOLDER` (with `placeholder.enabled=true`) is healthy because the NO SIGNAL video is being published to RTSP continuously.

---

## metrics.json

Contains detailed counters about publishing performance.

### Format

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
  "device_lost_recoveries": 0,
  "ts": "2026-04-09T22:10:00.000Z"
}
```

### Fields

| Field | Type | Description |
|-------|------|-------------|
| `state` | string | Current state machine state name |
| `sender_name` | string | Spout sender name |
| `sender_width` / `sender_height` | int | Frame resolution (pixels) |
| `sender_fps` | float | Frame rate reported by the sender |
| `bitrate_kbps` | float | Measured bitrate (kbps) |
| `current_fps` | float | Effective encoder frame rate |
| `rtsp_url` | string | Configured RTSP URL |
| `encoder_codec` | string | Codec in use (`h264_nvenc`, etc.) |
| `uptime_ms` | int | Milliseconds since the session started |
| `frames_received` | int | Total frames received from Spout |
| `frames_encoded` | int | Total frames encoded |
| `frames_dropped` | int | Frames dropped because of encode failures |
| `rtsp_errors` | int | Cumulative RTSP send errors |
| `reconnect_attempts` | int | Cumulative RTSP reconnect attempts |
| `device_lost_recoveries` | int | Cumulative automatic recoveries from GPU TDR device loss |
| `ts` | string | Time (ISO 8601 UTC) at which these metrics **last changed**. Not updated while nothing changes (`IDLE`/`STALLED`, etc.) |

---

## Event log (JSON Lines)

Logs are written in JSON Lines format to the directory given by `app.log_dir`.

### File name

```text
logs/<instance_name>.jsonl
```

The file rotates at 10 MB, keeping 5 files.

### Event types

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

#### Other events

`publish_stopped`, `reconnecting`, `stall_recovered`, `sender_disappeared`, `source_disconnected_detected`, `stalled_recovery_forced`, `gpu_device_lost`, `gpu_device_reinit_ok`, `gpu_zero_copy_enabled` / `gpu_zero_copy_disabled`, `placeholder_started`, `placeholder_source_found`, `placeholder_seamless`, `source_seamless_start`, `placeholder_render_failed`, `prefer_dx11_ignored`, `state_file_write_recovered`, `stopped` and `fatal_exit` are also emitted. See `src/app/supervisor.cpp` for when each occurs.

### Error codes

> **Note**: Configuration errors are printed to standard error only, as `[ERROR] <message>`, and the process exits with code 1. They are not recorded in the event log because logging has not been initialised yet.

| Code | Description |
|------|-------------|
| `SPOUT_INIT_FAILED` | SpoutDX initialisation failed |
| `SPOUT_CONNECT_FAILED` | Connecting to the Spout sender failed |
| `SPOUT_RECONNECT_FAILED` | Re-connecting the Spout receiver after the GPU/CPU receive path was decided failed |
| `SPOUT_RECEIVE_FAILED` | Timed out waiting for the first frame |
| `SPOUT_REINIT_FAILED` | Device recreation after a GPU TDR failed |
| `ENCODER_INIT_FAILED` | Encoder initialisation failed (after trying all codecs) |
| `ENCODER_ENCODE_FAILED` | Error during encoding |
| `RTSP_CONNECT_FAILED` | Connecting to the RTSP server failed |
| `RTSP_SEND_FAILED` | RTP packet send error |
| `RTSP_TIMEOUT` | Maximum reconnect attempts exceeded |
| `STATE_FILE_WRITE_FAILED` | Writing `metrics.json` / `health.json` failed (the old file is kept; `target` / `path` are attached; logged only when the failure starts or its cause changes) |

---

## Usage examples

### Health check with PowerShell

```powershell
$health = Get-Content state\health.json | ConvertFrom-Json
if (-not $health.healthy) {
    Write-Error "Publisher is not healthy: $($health.state)"
}
```

### Checking the frame drop rate

```powershell
$m = Get-Content state\metrics.json | ConvertFrom-Json
$dropRate = $m.frames_dropped / $m.frames_received * 100
Write-Host "Drop rate: $([math]::Round($dropRate, 2))%"
```
