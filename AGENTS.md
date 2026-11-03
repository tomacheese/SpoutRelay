# AGENTS.md — SpoutRelay

SpoutRelay is a Windows C++17 application that captures [Spout2](https://spout.zeal.co/) GPU-shared textures, encodes them as H.264 via FFmpeg (NVENC, falling back to `h264_mf`), and publishes to an RTSP server.

## Language

- All deliverables are written in **English**: commit messages, PR/Issue titles and bodies, review comments, code comments, and documentation.
- Exceptions: quotations, technical identifiers, generated files, external source text, and `README-ja.md` (a Japanese translation of `README.md`; update it together with `README.md`).

## Build

Requires MinGW-w64 (GCC), CMake and Ninja; see [docs/build.md](docs/build.md) for dependency setup.

```powershell
cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=toolchain-mingw.cmake
cmake --build build
# FFmpeg DLLs must sit next to spout-relay.exe, or the exe crashes at startup
Copy-Item deps\ffmpeg\bin\*.dll build\
```

Targets: `spout-relay` (main app), `publisher_tests` (unit tests), `spout_test_sender` / `spout_static_sender` / `spout_res_changer` (E2E Spout senders).

## Testing

Run unit tests after every change; also run E2E tests for state-machine, RTSP or encoder changes (needs `mediamtx` and `spout_test_sender` in PATH or `build/`).

```powershell
cmake --build build --target publisher_tests -j 4
.\build\tests\publisher_tests.exe        # ends with "All tests passed."
pwsh e2e-test/run-tests.ps1
pwsh e2e-test/monkey-tests.ps1
```

Full procedure: the `run-tests` skill (`/run-tests`) and [docs/testing.md](docs/testing.md). The unit-test runner is custom (no GoogleTest/Catch2) by design.

## Conventions

- New pure-logic function: extract it into a small header under `src/` and add a unit test in `tests/unit/` (pattern: `src/app/supervisor_logic.hpp` + `tests/unit/test_supervisor_logic.cpp`).
- New config key: validate it in `src/config/`, add a case to `tests/unit/test_config_loader.cpp`, and document it in `docs/configuration.md`.
- New abnormal-path E2E scenario: add it to `e2e-test/monkey-tests.ps1`; GPU or real-RTSP-server scenarios go in the manual section of `docs/testing.md`.
- Commits follow Conventional Commits.
- Never put real IP addresses, credentials, or secrets in code, tests, docs, or logs (`192.168.0.100` is an RFC 1918 documentation placeholder).

## Gotchas

- `spout2_static` is compiled without `WIN32_LEAN_AND_MEAN` on purpose: `SpoutFrameCount.cpp` needs `mmsystem.h`, which that macro excludes.
- `config/config.json`, `state/`, `logs/` and `deps/ffmpeg/` are gitignored; copy `config/config.example.json` to `config/config.json` locally. `state/` and `logs/` are created on first run.
- `nlohmann/json` and `spdlog` are fetched by CMake `FetchContent` at pinned tags; no manual install needed.
- E2E scripts resolve paths via `$PSScriptRoot`, so they run from the repo root or from `e2e-test/`.

## Docs

| File | Contents |
|------|----------|
| [docs/build.md](docs/build.md) | Build and dependency setup |
| [docs/configuration.md](docs/configuration.md) | All config keys |
| [docs/state-machine.md](docs/state-machine.md) | State transitions |
| [docs/metrics.md](docs/metrics.md) | `health.json` / `metrics.json` and event log format |
| [docs/testing.md](docs/testing.md) | Test categories and scenarios |
| [docs/troubleshooting.md](docs/troubleshooting.md) | Common errors |
| [docs/architecture.md](docs/architecture.md) | Thread model and data flow |
