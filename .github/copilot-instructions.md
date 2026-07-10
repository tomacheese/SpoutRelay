# Copilot code review instructions — SpoutRelay

SpoutRelay is a Windows C++17 application: it captures Spout2 GPU-shared textures,
encodes them to H.264 via FFmpeg (NVENC, falling back to `h264_mf`), and publishes
to an RTSP server. Build system is CMake + Ninja under MinGW-w64. There is no
external unit-test framework — a custom runner (`publisher_tests`) is used.

Write review comments in **Japanese** (this repository's commits, PRs, and docs are in Japanese).

## Review priorities

- **Resource lifetime (highest priority).** FFmpeg objects (`AVCodecContext`,
  `AVFormatContext`, `AVFrame`, `AVPacket`, `SwsContext`) and DirectX/COM objects
  (SpoutDX, D3D11 textures) must be released on every path, including error and
  early-return paths. Flag leaks and double-frees.
- **FFmpeg / Win32 return codes.** Return values from `avcodec_*`, `avformat_*`,
  `av_*`, and Win32/DX11 calls must be checked; failures must surface as an error
  (see `src/common/errors.hpp`), not be silently ignored.
- **Thread safety.** Capture runs on a separate thread feeding a bounded queue
  (`src/capture/frame_pump.*`), orchestrated by the supervisor
  (`src/app/supervisor.*`). Flag shared state accessed without synchronization,
  and blocking/unbounded growth in the capture path.
- **State machine.** Transitions in `src/app/state_machine.*` are explicit. Flag
  new states/events that are added without handling every transition, or that
  bypass the state machine.
- **Reconnection / back-off.** Encoder and RTSP failures recover via exponential
  back-off. Flag changes that can busy-loop, retry with no delay, or drop the
  self-recovery behavior.
- **Config validation.** New config keys must be validated in `src/config/` and
  rejected clearly when malformed (see `src/config/config_loader.*`).
- **Secrets.** Flag any real credential, token, or production hostname added to
  code, tests, or logs.

## Testing expectations

- New pure-logic function → extract a small header under `src/` and add a unit
  test in `tests/unit/` (pattern: `src/app/supervisor_logic.hpp` +
  `tests/unit/test_supervisor_logic.cpp`).
- New config key → add validation + a case in `tests/unit/test_config_loader.cpp`
  + document it in `docs/configuration.md`.
- New abnormal-path E2E scenario → add to `e2e-test/monkey-tests.ps1`.
- Flag logic/branch/config additions that ship without matching tests.

## Known non-issues (do NOT flag)

- **`spout2_static` omits `WIN32_LEAN_AND_MEAN`** while `publisher_lib` defines it.
  Intentional — `SpoutFrameCount.cpp` needs `mmsystem.h`, which that macro excludes.
- **`--whole-archive` around `-lwinpthread`** in `CMakeLists.txt` is intentional
  (forces static embedding regardless of link order).
- **`192.168.0.100` in docs / `config.example.json` / test scripts** is an RFC1918
  placeholder used consistently as a documentation example, not a leaked address.
- **Custom test runner (no GoogleTest/Catch2)** is a deliberate choice; do not
  suggest adopting a test framework.
- **`FetchContent` pinned to fixed tags** (spdlog, nlohmann/json) is intentional;
  do not suggest floating versions.
- **Japanese commit messages, docs, and PR text** are the repository convention.
- **`config.json`, `state/`, `logs/`, `deps/ffmpeg/` gitignored** — expected;
  `config.example.json` is the committed template.
