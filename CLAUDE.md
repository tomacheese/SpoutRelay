# CLAUDE.md — SpoutRelay

SpoutRelay is a Windows app that captures [Spout2](https://spout.zeal.co/) GPU-shared textures, encodes them as H.264 (NVENC → `h264_mf` fallback), and pushes to an RTSP server via FFmpeg.

Docs in `docs/` are in Japanese. Japanese README: [README-ja.md](README-ja.md).

## Build

```powershell
# Configure (MinGW-w64 / Ninja required)
cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=toolchain-mingw.cmake

# Build all targets
cmake --build build

# Individual targets
cmake --build build --target spout-relay         # main app
cmake --build build --target publisher_tests     # unit tests
cmake --build build --target spout_test_sender   # E2E Spout sender
```

After building, copy FFmpeg DLLs from `deps/ffmpeg/bin/` into `build/` — the exe won't start without them:

```powershell
Copy-Item deps\ffmpeg\bin\*.dll build\
```

## Testing

After every change, run unit tests at minimum. For state-machine / RTSP / encoder changes also run E2E tests.

```powershell
# Unit tests (required, ~97 PASS lines expected)
cmake --build build --target publisher_tests -j 4
.\build\tests\publisher_tests.exe

# E2E tests (requires mediamtx + spout_test_sender in PATH or build/)
pwsh e2e-test/run-tests.ps1
pwsh e2e-test/monkey-tests.ps1
```

Full procedure: use the `/run-tests` slash command or `run-tests` skill.
Test documentation: [`docs/testing.md`](docs/testing.md)

## Conventions

- Commit messages and documentation are in **Japanese**.
- New pure-logic functions: extract into a small header under `src/`, add a unit test in `tests/unit/`. See `src/app/supervisor_logic.hpp` + `tests/unit/test_supervisor_logic.cpp` as a pattern.
- New config keys: add validation in `src/config/`, add a test in `tests/unit/test_config_loader.cpp`, document in `docs/configuration.md`.
- New E2E scenarios: add to `e2e-test/monkey-tests.ps1`; GPU/real-RTSP-server scenarios go in the manual section of `docs/testing.md`.
- Do **not** include real IP addresses, credentials, or secrets in code, tests, or logs.

## Gotchas

- **FFmpeg DLLs**: Must be in the same directory as `spout-relay.exe`. Builds succeed without them; the exe crashes at startup if they're missing.
- **`spout2_static` target**: Spout2 SDK is compiled without `WIN32_LEAN_AND_MEAN` in a separate static-lib target. This is intentional — `SpoutFrameCount.cpp` uses `mmsystem.h` which that macro excludes.
- **`config.json` is gitignored**: Use `config/config.example.json` as a template and copy to `config/config.json` locally.
- **`state/` and `logs/` are runtime-created** and gitignored. The app creates them on first run.
- **E2E tests use `$PSScriptRoot`**: Run from the repo root (`pwsh e2e-test/run-tests.ps1`) or from inside `e2e-test/` (`cd e2e-test; pwsh ./run-tests.ps1`) — both work.
- **`nlohmann/json` and `spdlog`** are fetched automatically by CMake `FetchContent` at configure time; no manual install needed.

## Key Docs

| File | What's in it |
|------|-------------|
| [`docs/build.md`](docs/build.md) | Full build & dependency setup |
| [`docs/configuration.md`](docs/configuration.md) | All config keys |
| [`docs/state-machine.md`](docs/state-machine.md) | State transitions |
| [`docs/metrics.md`](docs/metrics.md) | health.json / metrics.json format |
| [`docs/testing.md`](docs/testing.md) | All 4 test categories and scenario lists |
| [`docs/troubleshooting.md`](docs/troubleshooting.md) | Common errors |
| [`docs/architecture.md`](docs/architecture.md) | Thread model, data flow |
