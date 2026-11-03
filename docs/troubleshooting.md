# Troubleshooting

## Startup and configuration errors

### `[ERROR] Cannot open config file: config/config.json`

The config file was not found. If `--config` is not given, `config/config.json` relative to the working directory is used. Specify an absolute path with `--config`:

```powershell
.\spout-relay.exe --config C:\path\to\config\config.json
```

### Config validation errors (for example `spout.sender_name must not be empty`)

A config value is invalid. The message names the offending key and the process exits with code 1. See the full list of checks in [configuration.md](configuration.md#validation). Common causes:

- `spout.sender_name` is empty
- `rtsp.url` does not start with `rtsp://`
- `encoder.bitrate_kbps` is not greater than 0
- `encoder.fps` is outside 1 to 240

---

## Spout

### `probe_no_sender` keeps appearing / the sender is not found

**Cause:** the configured `sender_name` is not registered in Spout shared memory.

**How to check:**

```powershell
$mmf = [System.IO.MemoryMappedFiles.MemoryMappedFile]::OpenExisting("SpoutSenderNames")
$accessor = $mmf.CreateViewAccessor()
$buf = New-Object byte[] (256 * 10)
$accessor.ReadArray(0, $buf, 0, $buf.Length)
for ($i = 0; $i -lt 10; $i++) {
    $name = [System.Text.Encoding]::ASCII.GetString($buf, $i*256, 256).TrimEnd([char]0)
    if ($name) { Write-Host "Sender: $name" }
}
```

Set the sender name you find exactly as `spout.sender_name` in `config.json`.

### `SPOUT_RECEIVE_FAILED: Timeout waiting for first frame` (once)

**Cause:** after connecting, the first frame did not arrive within `5 × spout.frame_timeout_ms`. The state returns to `PROBING` and tries again, which usually succeeds if the sender is running. If it keeps recurring, see the next item.

### `SPOUT_RECEIVE_FAILED` keeps recurring

**Causes and fixes:**

1. **GPU adapter mismatch**: if the sender and receiver are on different physical GPUs, the receiver (which uses `SetAdapterAuto(false)`) does not recover automatically. The placeholder is not used either, and connection attempts repeat. This is rare on single-GPU machines; on a multi-GPU machine, change the GPU used by the sending application.
2. **The sender is not sending frames**: check that the sending application (VRChat, etc.) is actually running.
3. **`frame_timeout_ms` is too short**: under high GPU load, raise it from `300` to `500` or more.

### Endless `STALLED` ↔ `RECONNECTING_OUTPUT` loop (`healthy: false`, `frames_received` not increasing)

**Symptoms:** `health.json` stays at `state: STALLED` (or alternates with `RECONNECTING_OUTPUT`) and only `reconnect_attempts` keeps growing. `sender_fps` is `0.0` and `rtsp_errors` does not grow (RTSP reconnects fine, but no frames arrive from Spout).

**Cause:** when the GPU zero-copy path (`ReceiveTexture()`) and the CPU path (`ReceiveImage()`) are switched within one session (for example when returning from PLACEHOLDER to a real source), the spoutDX internal update-event flag (`m_bUpdated`) is consumed asymmetrically between the two paths. The one-time CPU path initialisation (`CheckStagingTextures()`) may then never run, and `receive_latest_frame()` keeps failing. Reconnecting RTSP alone does not clear this.

**Fix:** `handle_connecting_output()` explicitly calls `SpoutMonitor::disconnect()`/`connect()` right after the encoder decides which receive path to use, which resets `m_bUpdated` before the first frame is received.

**Safeguard:** if the same kind of deadlock still occurs, a watchdog forces a full reconnect (restart from `PROBING`, including the Spout receiver) after `spout.stalled_recovery_max_attempts` (default `10`) consecutive `STALLED → RECONNECTING_OUTPUT` cycles without frame recovery. See [the STALLED watchdog section in state-machine.md](state-machine.md#watchdog-for-stalled-loops-stalled_recovery_forced). A `stalled_recovery_forced` event in the log means the watchdog fired.

---

## GPU TDR (device loss)

### How the app recovers from a TDR without crashing

When Windows Timeout Detection and Recovery (TDR) triggers under heavy GPU load, the D3D11 device used by NVENC enters the `DXGI_ERROR_DEVICE_REMOVED` state. SpoutRelay detects the device loss automatically and recovers as follows:

1. Device loss is detected in `STREAMING` / `STALLED` → the state becomes `RECOVERING_DEVICE`
2. The encode thread, FramePump, RTSP and encoder are torn down in order
3. `SpoutDX::CloseDirectX11()` → `OpenDirectX11()` recreates the device
4. Success: automatically back through `PROBING` → `CONNECTING_OUTPUT` → `STREAMING`
5. Failure: the state becomes `FATAL` (the app exits)

The log records events in this order:

```json
{"event":"gpu_device_lost", ...}
{"event":"state_changed","from":"STREAMING","to":"RECOVERING_DEVICE", ...}
{"event":"gpu_device_reinit_ok", ...}
{"event":"state_changed","from":"RECOVERING_DEVICE","to":"PROBING", ...}
...
{"event":"state_changed","from":"CONNECTING_OUTPUT","to":"STREAMING", ...}
```

### If TDR happens frequently

Windows resets the GPU when it does not respond for a long time (2 seconds by default).

- **Reduce GPU load**: lower `encoder.bitrate_kbps`, or change `encoder.codec` to `h264_mf` (CPU encoding)
- **Extend the TDR timeout**: raise the registry value `HKLM\System\CurrentControlSet\Control\GraphicsDrivers\TdrDelay` (the default is 2 seconds)
- **Update drivers**: install the latest NVIDIA driver

### `SPOUT_REINIT_FAILED` and the app goes `FATAL`

This happens when device recreation (`OpenDirectX11()`) fails.

**Causes and fixes:**

- The GPU driver is corrupted: reinstall the driver
- The GPU is physically unavailable (for example in a remote desktop session)
- Restarting the app usually recovers

---

## Encoder

### `ENCODER_INIT_FAILED`

**Cause:** neither the configured `codec` nor `fallback_codec` can be used.

**How to check:**

```powershell
# Is NVENC available?
ffmpeg -encoders 2>$null | Select-String "nvenc"

# Is h264_mf (Windows Media Foundation) available?
ffmpeg -encoders 2>$null | Select-String "h264_mf"
```

If NVENC is not listed:

- There is no NVIDIA GPU, or the driver is too old
- FFmpeg was built without NVENC support

If h264_mf is not listed:

- Windows Media Foundation is unavailable (for example Server Core)

Set both `codec` and `fallback_codec` to encoders that exist in your FFmpeg build. Note that the LGPL FFmpeg builds shipped with releases do not include `libx264`.

### `avcodec_open2 failed: Option rc not found` (NVENC)

This can happen with an old FFmpeg. Use an FFmpeg with avcodec 62 or later (the version the DLLs in [build.md](build.md) come from).

---

## RTSP

### `RTSP_CONNECT_FAILED`

**Things to check:**

1. **Is MediaMTX running?**
   ```powershell
   Get-Process mediamtx -ErrorAction SilentlyContinue
   ```

2. **Network reachability**
   ```powershell
   Test-NetConnection -ComputerName 192.168.0.100 -Port 8554
   ```

3. **Firewall**: check that inbound TCP 8554 is allowed on the host running MediaMTX

4. **URL format**: it must start with `rtsp://` and have the correct port

### Frequent `RTSP_SEND_FAILED`

**Causes and fixes:**

- **Insufficient network bandwidth**: lower `encoder.bitrate_kbps` (for example 4000 → 2000)
- **Send timeout too short**: raise `rtsp.send_timeout_ms` (for example 5000 → 10000)
- **MediaMTX settings**: increase `writeQueueSize` in `mediamtx.yml`

### Reconnects exceed `max_reconnect_attempts` and the state goes `FATAL`

Increase `max_reconnect_attempts`, or run MediaMTX in a more stable environment. When the limit is exceeded, `RTSP_TIMEOUT` is logged.

```json
"rtsp": {
  "max_reconnect_attempts": 10,
  "reconnect_max_delay_ms": 60000
}
```

---

## Build

### `SpoutFrameCount.cpp` compile error (`TIMECAPS` undefined, etc.)

Compiling the Spout2 sources with `WIN32_LEAN_AND_MEAN` defined leaves out `mmsystem.h`, so the multimedia timer API is unavailable.

Check that `CMakeLists.txt` compiles the Spout2 sources as the `spout2_static` target without `WIN32_LEAN_AND_MEAN`.

### `_mm_shuffle_epi8 was not declared in this scope`

`SpoutCopy.cpp` uses SSSE3 instructions. Check that `CMakeLists.txt` contains:

```cmake
set_source_files_properties(
    ${SPOUT2_GL_DIR}/SpoutCopy.cpp
    PROPERTIES COMPILE_FLAGS "-mssse3 -msse4.1")
```

### Link error: unresolved `GetFileVersionInfoA`

The `version` library must be linked. Check that the `spout2_static` target in `CMakeLists.txt` links it:

```cmake
target_link_libraries(spout2_static PUBLIC
    d3d11 dxgi ole32 gdi32 user32 psapi winmm version)
```

---

## Manual end-to-end check

For automated E2E tests see [testing.md](testing.md). A quick manual check:

```powershell
# 1. Start MediaMTX (installed separately)
Start-Process mediamtx

# 2. Start the test Spout sender (when there is no real Spout sender)
Start-Process build\spout_test_sender.exe

# 3. Start the publisher
.\build\spout-relay.exe --config config\config.json

# 4. Check with ffprobe
ffprobe rtsp://192.168.0.100:8554/live

# 5. Record 5 seconds and check the frame count
ffmpeg -i rtsp://192.168.0.100:8554/live -t 5 -c copy test_out.mp4
ffprobe -v quiet -select_streams v:0 -count_frames -show_entries stream=nb_read_frames -of csv test_out.mp4
```

Expected: roughly 140 to 150 frames in 5 seconds at 30 fps. `spout_test_sender.exe` takes the sender name as an optional first argument (default `TestSpoutSender`); make sure `spout.sender_name` matches it.
