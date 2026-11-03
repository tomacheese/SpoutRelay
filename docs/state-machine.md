# State Machine

## States

| State | Description |
|-------|-------------|
| `INIT` | Initial state right after startup; components are initialised |
| `IDLE` | Short wait before (re)starting to look for the Spout sender |
| `PROBING` | Checking whether the configured sender exists (SpoutDX sender lookup) |
| `PLACEHOLDER` | Publishing the "NO SIGNAL" placeholder video while no sender is found (only when `placeholder.enabled=true`) |
| `CONNECTING_OUTPUT` | Receives the first frame, then initialises the encoder and the RTSP connection |
| `STREAMING` | Normal publishing: capture → encode → RTSP send |
| `STALLED` | No frames or source response for a while (the sender may have stopped) |
| `RECONFIGURING` | A sender resolution change was detected; the encoder is re-initialised |
| `RECONNECTING_OUTPUT` | Waiting (with back-off) and reconnecting RTSP after a disconnect |
| `RECOVERING_DEVICE` | Recreating the D3D11 device after a GPU TDR device loss |
| `STOPPING` | Shutting down (after Ctrl+C) |
| `FATAL` | Unrecoverable error; the app exits |

## Typical flow

```text
INIT → IDLE → PROBING ──sender found──▶ CONNECTING_OUTPUT → STREAMING
                 │                              ▲
                 └─not found & placeholder──▶ PLACEHOLDER ─sender found─┘

STREAMING ──frame timeout──────────▶ STALLED ──frames resume──▶ STREAMING
STREAMING ──resolution change──────▶ RECONFIGURING ──ok──▶ STREAMING
STREAMING ──RTSP send error────────▶ RECONNECTING_OUTPUT ──ok──▶ STREAMING
STREAMING / STALLED ──device lost──▶ RECOVERING_DEVICE ──ok──▶ PROBING
RECONNECTING_OUTPUT ──too many failures──▶ FATAL
any running state ──shutdown request──▶ STOPPING ──▶ IDLE
```

## Permitted transitions

The transition table in `src/app/state_machine.cpp` is authoritative. Any transition not listed here is rejected by `StateMachine::transition_to()`. Some permitted transitions (for example `INIT → FATAL`, `PROBING → IDLE`, `CONNECTING_OUTPUT → FATAL`) are not currently taken by the supervisor. Conversely, a failed re-initialisation in `RECONFIGURING` asks for `PROBING`, which the table does not allow, so that request is ignored and the state stays `RECONFIGURING`.

| From | To | Condition |
|------|----|-----------|
| INIT | IDLE | Initialisation succeeded |
| INIT | FATAL | Initialisation failed |
| IDLE | PROBING | Start |
| IDLE | STOPPING | Shutdown requested |
| PROBING | CONNECTING_OUTPUT | Sender found and connected |
| PROBING | IDLE | Permitted, unused by the supervisor |
| PROBING | PLACEHOLDER | Sender not found and `placeholder.enabled=true` |
| PROBING | STOPPING | Shutdown requested |
| PLACEHOLDER | CONNECTING_OUTPUT | Sender found (recovering from the placeholder) |
| PLACEHOLDER | STOPPING | Shutdown requested |
| CONNECTING_OUTPUT | STREAMING | Everything connected |
| CONNECTING_OUTPUT | PROBING | First-frame timeout, or encoder/RTSP initialisation failed |
| CONNECTING_OUTPUT | STOPPING | Shutdown requested |
| CONNECTING_OUTPUT | FATAL | Permitted, unused by the supervisor |
| STREAMING | STALLED | The source has not responded for `spout.frame_timeout_ms` |
| STREAMING | RECONFIGURING | Resolution change detected |
| STREAMING | RECONNECTING_OUTPUT | RTSP send error |
| STREAMING | STOPPING | Shutdown requested |
| STREAMING | IDLE | Permitted, unused by the supervisor |
| STREAMING | PROBING | Encode error |
| STREAMING | RECOVERING_DEVICE | GPU device loss detected |
| STALLED | STREAMING | Frames resumed |
| STALLED | RECONNECTING_OUTPUT | Stalled for `3 × rtsp.reconnect_delay_ms` while the sender is still alive |
| STALLED | PROBING | Encode error, or the forced full reconnect (see below) |
| STALLED | PLACEHOLDER | Sender disappeared (also checked after `3 × rtsp.reconnect_delay_ms`) and `placeholder.enabled=true` |
| STALLED | IDLE | Sender disappeared (stalled for `spout.sender_missing_timeout_ms` and the sender is gone) while the placeholder is disabled |
| STALLED | STOPPING | Shutdown requested |
| STALLED | RECOVERING_DEVICE | GPU device loss detected |
| RECOVERING_DEVICE | PROBING | Device recreated; probing restarts |
| RECOVERING_DEVICE | FATAL | Device recreation failed |
| RECOVERING_DEVICE | STOPPING | Shutdown requested |
| RECONFIGURING | STREAMING | Reconfiguration succeeded |
| RECONFIGURING | STOPPING | Shutdown requested |
| RECONFIGURING | FATAL | Permitted, unused by the supervisor |
| RECONNECTING_OUTPUT | STREAMING | Reconnected |
| RECONNECTING_OUTPUT | FATAL | `rtsp.max_reconnect_attempts` exceeded |
| RECONNECTING_OUTPUT | STOPPING | Shutdown requested |
| STOPPING | IDLE | Stopped (back to the waiting state) |

## State change log entries

State changes are recorded in the log in this form:

```json
{"ts":"2026-04-09T22:05:44.473Z","event":"state_changed","from":"CONNECTING_OUTPUT","to":"STREAMING"}
```

## Error codes and states

| Error code | Occurs in | Next state |
|------------|-----------|------------|
| `SPOUT_RECEIVE_FAILED` | `CONNECTING_OUTPUT` (no first frame within `5 × spout.frame_timeout_ms`) | `PROBING` |
| `ENCODER_INIT_FAILED` | `CONNECTING_OUTPUT` / `RECONFIGURING` (all codecs failed) | `PROBING` |
| `RTSP_CONNECT_FAILED` | `CONNECTING_OUTPUT` | `PROBING` |
| `RTSP_CONNECT_FAILED` | `RECONNECTING_OUTPUT` | stays; retried with back-off |
| `RTSP_SEND_FAILED` | `STREAMING` | `RECONNECTING_OUTPUT` |
| `RTSP_TIMEOUT` | `RECONNECTING_OUTPUT` (max attempts exceeded) | `FATAL` |
| `SPOUT_REINIT_FAILED` | `RECOVERING_DEVICE` | `FATAL` |

## GPU device-loss recovery (`RECOVERING_DEVICE`)

When a GPU Timeout Detection and Recovery (TDR) event occurs, the D3D11 device used by NVENC enters the `DXGI_ERROR_DEVICE_REMOVED` state.

### Detection

While the state is `STREAMING` or `STALLED`, the supervisor main loop polls `ID3D11Device::GetDeviceRemovedReason()` (through `SpoutMonitor::is_device_removed()`) and moves to `RECOVERING_DEVICE` if it returns anything other than `S_OK`. `EncoderController::encode()` performs the same check at the start of its GPU path, so `CopySubresourceRegion()` is never called on a lost device.

### Recovery flow

```text
STREAMING / STALLED
    ↓ device removed
RECOVERING_DEVICE
    ↓ teardown_streaming()
    ↓ SpoutMonitor::reinit_device()
        CloseDirectX11() → OpenDirectX11() → re-apply multithread protection
    ↓ success
PROBING → CONNECTING_OUTPUT → STREAMING  (automatic recovery)
    ↓ failure
FATAL
```

### Related log events

| Event | Level | Description |
|-------|-------|-------------|
| `gpu_device_lost` | `warn` | Device loss detected; recovery started |
| `gpu_device_reinit_ok` | `info` | Device recreated successfully |
| `SPOUT_REINIT_FAILED` | `error` | Device recreation failed (goes to `FATAL`) |

## Watchdog for STALLED loops (`stalled_recovery_forced`)

Recovery from `STALLED` is normally done by reconnecting only the RTSP layer (`RECONNECTING_OUTPUT`). In some cases, for example when the spoutDX internal update-event flag (`m_bUpdated`) is not re-initialised properly after switching between the GPU and CPU receive paths, the sender stays alive but `frame_pump_` never receives a frame again. Repeating RTSP reconnects never fixes this.

As a safeguard, `handle_stalled()` counts how many consecutive `STALLED → RECONNECTING_OUTPUT` attempts happened without a real recovery (`consecutive_stall_recoveries_`). When the count reaches `spout.stalled_recovery_max_attempts` (default `10`), it forces a full reconnect including the Spout receiver.

### Flow

```text
STALLED
  ↓ (sender alive, frames not recovered)
  ↓ ++consecutive_stall_recoveries_
  ↓ consecutive_stall_recoveries_ >= stalled_recovery_max_attempts ?
  │
  ├─ No  → teardown_rtsp() → RECONNECTING_OUTPUT (RTSP-only reconnect)
  │
  └─ Yes → log stalled_recovery_forced → consecutive_stall_recoveries_ = 0
           → stop and reset frame_pump_ → spout_monitor_->disconnect()
           → teardown_rtsp() → teardown_encoder() → PROBING (full reconnect)
```

`consecutive_stall_recoveries_` is reset to zero both when frame reception really recovers (`stall_recovered` in `handle_stalled()`) and when `handle_connecting_output()` reaches `STREAMING`.

Setting `stalled_recovery_max_attempts` to `0` or less disables the watchdog; RTSP-only reconnects then repeat without limit.

### Related log events

| Event | Level | Description |
|-------|-------|-------------|
| `stalled_recovery_forced` | `warn` | RTSP-only reconnects did not recover within the configured number of attempts, so a full reconnect including the Spout side was forced |
