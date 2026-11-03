# Testing

SpoutRelay is tested in four categories. After any implementation or change, run the categories affected by the change thoroughly.

| Category | Purpose | Environment | Command |
|----------|---------|-------------|---------|
| [Unit tests](#unit-tests) | Verify individual pieces of logic | CI / local (no GPU or Spout needed) | `build/tests/publisher_tests.exe` |
| [E2E tests](#e2e-tests) | Integration checks with the real binary and a real RTSP server | Local (Windows with Spout/GPU recommended) | `pwsh e2e-test/run-tests.ps1` |
| [Monkey tests](#monkey-tests) | Black-box checks of abnormal cases and combinations, based on the design docs | Local (Windows with Spout/GPU recommended) | `pwsh e2e-test/monkey-tests.ps1` |
| [User tests](#user-tests) | Final check with a real Spout source and RTSP server | Real machine (production-like) | Manual |

---

## Unit tests

### Overview

The test files under `tests/unit/` are built into the `publisher_tests` executable, linked against `publisher_lib`. They cover pure logic, configuration and state transitions without depending on a GPU or the Spout SDK itself. They also run in CI (GitHub Actions).

A small custom test framework (`tests/unit/test_utils.hpp`) is used; assertions are made with the `VERIFY` / `VERIFY_MSG` macros. On failure the process prints `[FAIL] ...` to standard error and exits immediately with `std::exit(1)`.

### Test files

| File | Content |
|------|---------|
| `tests/unit/test_config_loader.cpp` | Loading and validation rules of the config file (also holds the `main()` entry point) |
| `tests/unit/test_state_machine.cpp` | `StateMachine` transition rules (valid and invalid combinations) |
| `tests/unit/test_metrics_store.cpp` | `MetricsStore` aggregation and JSON output |
| `tests/unit/test_atomic_file.cpp` | Atomic replacement by `atomic_file::write` (keeping the old file, tmp cleanup, behaviour on write/replace failures) |
| `tests/unit/test_backoff.cpp` | Reconnect back-off calculation |
| `tests/unit/test_placeholder_renderer.cpp` | Generation of the NO SIGNAL placeholder frame |
| `tests/unit/test_log_sink.cpp` | `LogSink` JSON Lines format, escaping and error handling |
| `tests/unit/test_frame_pump.cpp` | `FramePump` queue, behaviour on overflow, and stopping |
| `tests/unit/test_supervisor_logic.cpp` | Pure logic extracted from `Supervisor` (`supervisor_logic.hpp`): seamless handoff decision, resolution-change detection |

### How to run

```powershell
# Build (first time or after source changes)
cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=toolchain-mingw.cmake
cmake --build build --target publisher_tests -j 4

# Run
.\build\tests\publisher_tests.exe
```

### Expected output

Each test block prints `[PASS] ...` lines, and the run ends with:

```text
All tests passed.
```

The number of `[PASS]` lines changes as tests are added. If any `[FAIL]` is printed, the failing assertion and its location (file and line) are shown and the process exits immediately.

### Guidelines when adding logic

- When adding a pure function (no side effects, threads, GPU or file I/O), extract it into a small header or function under `src/app/` or similar and add a unit test.
- Decision logic written directly inside a large class such as `Supervisor` is hard to unit test. Follow the `supervisor_logic.hpp` pattern: extract the function and verify it from `tests/unit/test_supervisor_logic.cpp`.
- When adding configuration validation, add a test function to `test_config_loader.cpp` that passes an invalid value and checks that `ConfigLoader::load()` returns `false`.

---

## E2E tests

### Overview

The PowerShell 7+ scripts under `e2e-test/` start the real `spout-relay.exe`, mediamtx (RTSP server) and Spout test senders (`spout_test_sender` etc.), and verify state transitions and streaming behaviour through real RTSP streams.

Running on a Spout/GPU environment with GPU encoding (D3D11VA + NVENC) is recommended, but the tests also cover CPU fallback (`libopenh264` / `h264_mf`).

### Prerequisites

- Windows and PowerShell 7+ (`pwsh`)
- `mediamtx` and `ffmpeg`/`ffprobe` on `PATH`
- `spout-relay.exe`, `spout_test_sender.exe`, `spout_static_sender.exe` and `spout_res_changer.exe` built (under `build/`)
- Per-scenario config files in `e2e-test/config/`: `config.json`, `config_fatal.json`, `config_libopenh264.json`, `config_placeholder.json`

### How to run

The scripts resolve paths from `$PSScriptRoot`, so they can be run from the repository root or from inside `e2e-test/`.

```powershell
cd e2e-test
pwsh ./run-tests.ps1             # Tests 1-3 (RTSP reconnect, freeze frame, stability)
pwsh ./monkey-tests.ps1          # Tests 4-11 (abnormal cases and combinations)
pwsh ./run-new-tests.ps1         # Tests 8-11 only (a subset of monkey-tests.ps1)
pwsh ./cli-tests.ps1             # CLI arguments, exit codes, Ctrl+Break graceful shutdown
pwsh ./state-files-tests.ps1     # Direct checks of health.json / metrics.json
pwsh ./multi-instance-tests.ps1  # Conflicts when several instances run at once
pwsh ./encoder-config-tests.ps1  # Encoder setting variations (h264_mf, extreme bitrates, etc.)
```

Each script starts and stops its own `mediamtx` / `spout-relay` processes, so make sure no leftover processes exist before running (the `Stop-AllProcs` function at the top of each script cleans up).

### Scenarios

#### `run-tests.ps1` (Tests 1-3)

| # | Scenario | What is verified |
|---|----------|------------------|
| 1 | IDR right after RTSP reconnect (Bug #3) | After restarting mediamtx, an IDR (I-frame) is sent immediately on reconnect |
| 2 | Freeze-frame retention (Bug #5') | After reconnecting, the stream resumes immediately using the last frame |
| 3 | Long-running stability (Bug #7) | No encode errors occur during 15 seconds of continuous streaming |

#### `monkey-tests.ps1` (Tests 4-11)

| # | Scenario | What is verified |
|---|----------|------------------|
| 4 | Sender disconnect → STALLED → recovery | When the Spout source disappears during STREAMING the state becomes STALLED, and returns to STREAMING after it comes back |
| 5 | Placeholder mode | NO SIGNAL video is published while no sender is connected, and hands off seamlessly once the sender connects |
| 6 | Three consecutive RTSP reconnects | The relay reconnects after mediamtx is killed and restarted three times in a row |
| 7 | CPU codec (`libopenh264`) used directly | Encoding and publishing work without a GPU |
| 8 | STALLED → sender_disappeared → PROBING | With the placeholder disabled, the sender disappearing leads back to probing and recovers |
| 9 | GPU STREAMING → sender_disappeared → PLACEHOLDER (seamless) | In GPU mode, losing the sender switches seamlessly to PLACEHOLDER |
| 10 | Resolution change (640x360 → 320x240, RECONFIGURING) | A resolution change by `spout_res_changer` goes through RECONFIGURING and returns to STREAMING |
| 11 | `max_reconnect_attempts` exceeded → FATAL | Exceeding the reconnect limit ends in the FATAL state |

#### `run-new-tests.ps1`

Runs only Tests 8-11 of `monkey-tests.ps1` on their own. Use it to re-verify just those tests.

#### `cli-tests.ps1`

Verifies the CLI argument handling, exit codes and graceful shutdown of `main.cpp`. Neither mediamtx nor a Spout sender is needed.

| # | Scenario | What is verified |
|---|----------|------------------|
| 1 | `--help` | Prints usage and exits with code 0 |
| 2 | `-h` | Same as `--help` |
| 3 | Nonexistent config file | Exit code 1 and `[ERROR]` is printed |
| 4 | Config file with invalid JSON | Exit code 1 and `[ERROR]` is printed |
| 5 | `-c` (short form) | Behaves like `--config` |
| 6 | No config file given | The default path (`config/config.json`) is used; exit code 1 if it does not exist |
| 7 | `Supervisor::init` failure (`log_dir` occupied by a file) | Exit code 1 and `[ERROR]` is printed |
| 8 | `CTRL_BREAK_EVENT` | `console_ctrl_handler` shuts down gracefully with exit code 0 |

#### `state-files-tests.ps1`

Verifies the structure and values of the `state/health.json` / `state/metrics.json` files written by `MetricsStore` during real relay runs.

| # | Scenario | What is verified |
|---|----------|------------------|
| 1 | While publishing PLACEHOLDER | `health.json` has `healthy`/`state`/`ts`, with `state == "PLACEHOLDER"`, `healthy == true` and a valid ISO 8601 `ts`. `metrics.json` has all fields, with plausible `state`/`rtsp_url`/`encoder_codec`/`uptime_ms`/`frames_encoded` |
| 2 | `max_reconnect_attempts` exceeded → FATAL | After the relay exits, `health.json` has `state == "FATAL"` and `healthy == false` |

#### `multi-instance-tests.ps1`

Starts two spout-relay instances at once on one machine, with different `instance_name` / `sender_name` / `rtsp.url` / `metrics_path` / `health_path`, and verifies that log files, state files and RTSP streams neither collide nor mix.

| # | Scenario | What is verified |
|---|----------|------------------|
| 1 | Two instances start together | Each independently reaches `publish_started` (STREAMING) |
| 2 | Log file isolation | `{instance_name}.jsonl` is separate per instance and events do not mix |
| 3 | State file isolation | `metrics_*.json` / `health_*.json` are separate per instance and reflect each one's `sender_name`/`rtsp_url` |
| 4 | RTSP stream independence | Both RTSP streams can be played separately |

#### `encoder-config-tests.ps1`

Verifies that spout-relay reaches STREAMING and publishes an RTSP stream with various `encoder`/`spout` settings.

| # | Scenario | What is verified |
|---|----------|------------------|
| 1 | `codec: "h264_mf"` set directly (no fallback) | Reaches STREAMING and publishes |
| 2 | Extremely low bitrate (`bitrate_kbps: 100`) | Reaches STREAMING and publishes |
| 3 | Extremely high bitrate (`bitrate_kbps: 50000`) | Reaches STREAMING and publishes |
| 4 | `max_b_frames: 2` (h264_mf) | Reaches STREAMING and publishes |
| 5 | `prefer_dx11: false` | Publishes normally even though a `prefer_dx11_ignored` warning is logged |
| 6 | Low FPS (`fps: 5`) | Reaches STREAMING and publishes |
| 7 | High FPS (`fps: 60`) | Reaches STREAMING and publishes |

### Expected output

Each test prints `[PASS] ...` / `[FAIL] ...`, and a summary of `Pass`/`Fail` counts is shown when the script ends. Fix the problem if even one `[FAIL]` appears.

---

## Monkey tests

The "monkey tests" defined in the project issues are exploratory black-box tests that try unexpected inputs, timings and combinations based on the design documents (the architecture and state machine docs under `docs/`). In this repository `e2e-test/monkey-tests.ps1` (Tests 4-11) is that suite, run in the same way and with the same commands as the E2E tests above.

When you add a new abnormal-case scenario (for example a new state transition or a new combination of config options), add a matching test case to `monkey-tests.ps1`.

### Additional scenarios to check manually (hard to automate)

The following need things such as RTSP server authentication settings or special tools that change the Spout sender's resolution freely, so the automated E2E tests do not cover them. Check them manually when your change relates to them.

- Connecting and reconnecting with an RTSP server that requires authentication (user name/password)
- Behaviour when RTSP server timeout settings change (boundary values of `connect_timeout_ms` / `send_timeout_ms`)
- Reconnection when the codec accepted by the RTSP server changes (for example after a server restart)
- Spout sources with extreme resolutions (very small or large, for example 64x64 or above 3840x2160)
- **First-connection check in GPU mode (`h264_nvenc`)**: on a real GPU machine, start `spout-relay.exe` against an already running Spout sender and confirm that `SPOUT_RECEIVE_FAILED` is not recorded in `logs/*.jsonl` and the state goes directly `CONNECTING_OUTPUT → STREAMING` (this verifies the fix for the black frames caused by the first-connection timing of the argument-less `ReceiveTexture()`)

Prepare the matching config files under `e2e-test/config/`, start `spout-relay.exe` manually, and check `state/health.json`, `state/metrics.json` and `logs/*.jsonl`.

> **Note**: `h264_mf` set directly, extreme bitrates, `max_b_frames`, `prefer_dx11: false` and extreme FPS values are verified automatically by `e2e-test/encoder-config-tests.ps1`.

- **Triggering the `spout.stalled_recovery_max_attempts` watchdog**: a STALLED loop that RTSP-only reconnects cannot fix is hard to produce artificially, so it is not covered by automated E2E. When changing this logic, set `stalled_recovery_max_attempts` to a small value (for example `2`), make the `SpoutMonitor` reconnect fail on purpose (for example with a debug build), and confirm manually that a `stalled_recovery_forced` log is recorded and the state goes to `PROBING`.

---

## Long-running and network-fault tests (manual procedures)

These take a long time or require manipulating the network, so they are not part of CI or the regular E2E tests; run them manually when needed. Run them before a release when you have made large changes to the encoder, frame pipeline, RTSP reconnect logic or metrics collection.

### Long-running endurance test (leak check)

**Goal**: confirm that memory usage and handle counts do not keep growing during long continuous operation (several hours to 24 hours or more), and that log rotation works.

**Steps**:

1. Based on `e2e-test/config/config.json`, start `spout-relay.exe` with `app.log_dir` set. (`LogSink` rotates at 10 MB with 5 files, so rotation actually happens during a long run.)
2. Keep a real Spout source (or `spout_test_sender.exe`) connected and keep playing the stream in an RTSP client (ffmpeg, VLC, etc.).
3. At regular intervals (for example every 30 minutes) record:
   - `Get-Process spout-relay | Select WorkingSet, PrivateMemorySize, HandleCount`
   - `frames_received` / `frames_encoded` / `frames_dropped` / `rtsp_errors` / `reconnect_attempts` / `uptime_ms` in `state/metrics.json`
   - The size of `logs/*.jsonl` and the presence of rotated files (`*.1`, `*.2`, ...)
4. After several hours to 24 hours, confirm:
   - `WorkingSet` / `PrivateMemorySize` / `HandleCount` do not keep growing monotonically (they settle to a constant after some growth)
   - `frames_dropped` does not keep growing (the frame queue is being consumed)
   - Log files rotate as `LogSink` is configured (10 MB, 5 files) and disk usage is not growing without bound
   - The RTSP stream plays without interruption

### Network faults (packet loss and bandwidth limits)

**Goal**: confirm that the reconnect logic of `RtspPublisherClient` (`reconnect_delay_ms` / `reconnect_max_delay_ms` / `reconnect_backoff_multiplier` / `max_reconnect_attempts`) behaves as expected under packet loss or bandwidth limits on the RTSP path, and that the stream resumes when the network recovers.

**Steps**:

1. Prepare a tool that simulates packet loss and bandwidth limits on Windows (for example [clumsy](https://jagt.github.io/clumsy/), a third-party tool equivalent to `netsh interface tc`, or `tc`/`netem` on WSL).
2. Start `spout-relay.exe` and mediamtx, and confirm that STREAMING (`publish_started`) is reached.
3. Apply the following conditions to the path to the RTSP server one after another, observing `logs/*.jsonl` and `state/metrics.json` each time:
   - **Mild packet loss (5-10%)**: `rtsp_errors` increases, but the stream continues or recovers after a short reconnect
   - **Heavy packet loss (30-50%)**: a `STREAMING → RECONNECTING_OUTPUT` `state_changed` event is recorded and `reconnect_attempts` increases
   - **Bandwidth limit (below the configured bitrate)**: the encoder keeps running without errors, or `rtsp_errors` / reconnect events are recorded
   - **Complete outage then recovery**: if the network is restored before `max_reconnect_attempts` is reached, the state returns `RECONNECTING_OUTPUT → STREAMING`. If the outage lasts beyond `max_reconnect_attempts`, the state becomes `FATAL` and the process exits (the same check as Test 11 in `e2e-test/monkey-tests.ps1`)
4. After each condition, restore the network and confirm that the stream plays normally again.

---

## User tests

As a final check, run against a real Spout source (OBS, Resolume, etc.) and a real RTSP server.

### Steps

1. Start the Spout sending application and set the target `sender_name` in SpoutRelay's `config.json`.
2. Make the RTSP server (mediamtx etc.) ready to accept a stream at `rtsp://192.168.0.100:8554/spout-test` (use your own server address).
3. Start `spout-relay.exe -c config.json`.
4. Connect to `rtsp://192.168.0.100:8554/spout-test` with an RTSP client such as VLC and confirm that the video displays correctly.
5. Stop and restart the Spout sender, and check the NO SIGNAL placeholder (if enabled) and the reconnect behaviour.
6. Check `state/health.json`, `state/metrics.json` and `logs/*.jsonl` for the expected state and metrics.

---

## Post-implementation checklist

After any implementation or change, run the tests that match the affected area.

- [ ] **Unit tests**: run `build/tests/publisher_tests.exe` and confirm `All tests passed.` is printed. If you added or changed logic, add the matching test cases.
- [ ] **E2E tests**: for changes to the state machine, RTSP publishing or the encoder, run `pwsh e2e-test/run-tests.ps1` and `pwsh e2e-test/monkey-tests.ps1` and confirm everything is `[PASS]`.
- [ ] **Monkey tests**: if you added new abnormal-case scenarios or config options, add matching test cases to `monkey-tests.ps1` and confirm they `[PASS]`.
- [ ] **User tests**: for changes that affect production-like behaviour (publishing protocol, encode settings, Spout connection handling), run a final check with a real Spout source and RTSP server.

Commit and open a PR only after all items are done and any problems are resolved.
