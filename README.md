# Spout RTSP Publisher

A Windows application that captures frames from a [Spout2](https://spout.zeal.co/) GPU-shared texture sender, encodes them as H.264 via FFmpeg (NVENC → libx264 fallback), and publishes to an RTSP server (e.g. [MediaMTX](https://github.com/bluenviron/mediamtx)).

## Features

- **Zero-copy Spout capture** — receives DX11 shared-texture frames via SpoutDX
- **Hardware H.264 encoding** — prefers NVIDIA NVENC; falls back to software libx264
- **RTSP ANNOUNCE/RECORD** — pushes stream to any RTSP server over TCP
- **Automatic reconnection** — exponential back-off on encoder or RTSP failures
- **Structured JSON logging** — JSON Lines event log with timestamps
- **Runtime metrics** — live `health.json` / `metrics.json` written to disk every second
- **State-machine driven** — explicit state transitions; no undefined behaviour on failure

## Requirements

| Component | Version |
|-----------|---------|
| Windows | 10 / 11 (x64) |
| NVIDIA GPU (optional) | any NVENC-capable card |
| [FFmpeg shared libs](https://github.com/BtbN/FFmpeg-Builds/releases) | win64-gpl-shared, avcodec ≥ 61 |
| [Spout2 SDK](https://github.com/leadedge/Spout2) | included in `deps/` |
| [MediaMTX](https://github.com/bluenviron/mediamtx) | v1.x (receiver side) |
| GCC (MinGW-w64) | ≥ 13 recommended |
| CMake | ≥ 3.20 |
| Ninja | any recent |

## Quick Start

```bash
# 1. Clone / extract into a directory
cd publisher

# 2. Configure CMake with the MinGW toolchain
cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=toolchain-mingw.cmake

# 3. Build
cmake --build build

# 4. Copy FFmpeg DLLs next to the executable
#    (avcodec-62.dll, avformat-62.dll, avutil-60.dll, swscale-9.dll, swresample-6.dll)

# 5. Edit config
cp config/config.example.json config/config.json
# Set spout.sender_name and rtsp.url

# 6. Start MediaMTX (or any RTSP server)
deps/mediamtx/mediamtx.exe

# 7. Run
build/publisher-agent.exe --config config/config.json
```

The stream is then available at `rtsp://<server-ip>:8554/live` (VLC, FFplay, etc.).

## Usage

```
publisher-agent.exe --config <path-to-config.json>
publisher-agent.exe --help
```

The process responds to `Ctrl+C` / `CTRL_CLOSE_EVENT` for graceful shutdown.

## Configuration

See [`config/config.example.json`](config/config.example.json) for a fully-annotated example.  
Full reference: [`docs/configuration.md`](docs/configuration.md)

### Minimum viable config

```json
{
  "spout": { "sender_name": "YourSpoutSender" },
  "rtsp":  { "url": "rtsp://192.168.0.100:8554/live" }
}
```

## Project Layout

```
publisher/
├── src/
│   ├── app/          # Supervisor (orchestrator) + StateMachine
│   ├── capture/      # FramePump – capture thread + bounded queue
│   ├── common/       # Types, error codes, time utilities
│   ├── config/       # JSON config loader
│   ├── encoder/      # FFmpeg H.264 encoder (NVENC / libx264)
│   ├── logging/      # spdlog JSON Lines sink
│   ├── metrics/      # MetricsStore → health.json / metrics.json
│   ├── rtsp/         # FFmpeg RTSP ANNOUNCE/RECORD client
│   └── spout/        # SpoutDX receiver (SpoutMonitor)
├── tests/
│   ├── unit/         # GTest unit tests (22 tests)
│   └── spout_test_sender/  # Color-cycling DX11 Spout sender for E2E testing
├── config/
│   ├── config.example.json
│   └── config.json   # (gitignored, your local config)
└── docs/             # Category-specific documentation (Japanese)
```

## Documentation

| Doc | Contents |
|-----|----------|
| [docs/architecture.md](docs/architecture.md) | System architecture, data flow, thread model |
| [docs/build.md](docs/build.md) | Detailed build instructions and dependency setup |
| [docs/configuration.md](docs/configuration.md) | Full configuration reference |
| [docs/state-machine.md](docs/state-machine.md) | State machine: states, transitions, error handling |
| [docs/metrics.md](docs/metrics.md) | Metrics/health JSON format, event log format |
| [docs/troubleshooting.md](docs/troubleshooting.md) | Common errors and solutions |

> All docs are written in Japanese. See [README-ja.md](README-ja.md) for the Japanese README.

## Running Tests

```bash
build/tests/publisher_tests.exe
# Expected: [==========] 22 tests from 4 test suites. PASSED.
```

## License

This project uses:
- [Spout2](https://github.com/leadedge/Spout2) — BSD-2-Clause
- [FFmpeg](https://ffmpeg.org/) — LGPL / GPL (depending on build)
- [nlohmann/json](https://github.com/nlohmann/json) — MIT
- [spdlog](https://github.com/gabime/spdlog) — MIT
- [GoogleTest](https://github.com/google/googletest) — BSD-3-Clause
