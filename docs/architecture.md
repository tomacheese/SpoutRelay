# Architecture

## System overview

```text
┌─────────────────┐   DX11 shared texture   ┌──────────────────────┐
│  Spout sender   │ ──────────────────────▶ │   SpoutMonitor       │
│  (VRChat etc.)  │   (GPU shared memory)   │   (SpoutDX)          │
└─────────────────┘                         └──────────┬───────────┘
                                                       │ RGBA frame
                                                       ▼
                                            ┌──────────────────────┐
                                            │   FramePump          │
                                            │   (bounded queue, 4) │
                                            └──────────┬───────────┘
                                                       │
                                                       ▼
                                            ┌──────────────────────┐
                                            │  EncoderController   │
                                            │  NVENC → fallback    │
                                            │  (FFmpeg + swscale)  │
                                            └──────────┬───────────┘
                                                       │ H.264 packets
                                                       ▼
                                            ┌──────────────────────┐
                                            │  RtspPublisherClient │
                                            │  ANNOUNCE / RECORD   │
                                            │  (FFmpeg, TCP)       │
                                            └──────────┬───────────┘
                                                       │ RTP/TCP
                                                       ▼
                                            ┌──────────────────────┐
                                            │   MediaMTX (etc.)    │
                                            │   RTSP server        │
                                            └──────────────────────┘
```

## Components

| Component | Class | Role |
|-----------|-------|------|
| Orchestrator | `Supervisor` | Runs the state machine and manages the lifecycle of all components |
| State machine | `StateMachine` | Enforces the transition table and notifies via callback |
| Spout receiver | `SpoutMonitor` | Receives the DX11 shared texture through SpoutDX (GPU texture or RGBA pixels) |
| Capture queue | `FramePump` | Runs the capture thread and holds a bounded queue of at most 4 frames |
| Encoder | `EncoderController` | FFmpeg H.264 encoding (NVENC or a fallback codec), swscale RGBA→YUV on the CPU path |
| RTSP client | `RtspPublisherClient` | FFmpeg RTSP ANNOUNCE + SETUP + RECORD, sends RTP packets |
| Placeholder | `render_placeholder_frame()` | Renders the NO SIGNAL frame (see [configuration](configuration.md#placeholder-section)) |
| Metrics | `MetricsStore` | Thread-safe counters, writes `health.json` / `metrics.json` |
| Logging | `LogSink` | spdlog-based JSON Lines log (file and console) |
| Configuration | `ConfigLoader` | Config loading and validation with nlohmann/json |

## Data flow

```text
[SpoutDX receive]
       │
       │  ReceiveImage() → RGBA bytes (or ReceiveTexture() on the GPU path)
       ▼
[FrameBuffer]
       │
       │  push (non-blocking; when full, the oldest frame is dropped)
       ▼
[bounded queue (max=4)]
       │
       │  pop (encode thread)
       ▼
[sws_scale: RGBA → YUV420P/NV12]   (CPU path only)
       │
       │  avcodec_send_frame / avcodec_receive_packet
       ▼
[AVPacket (H.264)]
       │
       │  av_interleaved_write_frame
       ▼
[RTSP RTP/TCP → MediaMTX]
```

With a hardware codec and a D3D11 device, the GPU zero-copy path copies the Spout texture straight into the encoder's hardware frame pool instead (logged as `gpu_zero_copy_enabled`). If GPU initialisation fails, the encoder falls back to the CPU path above.

## Thread model

```text
Main thread (Supervisor::run)
│
├── State machine loop
│   └── Each state handler polls synchronously
│
├── Capture thread (FramePump)
│   └── Polls SpoutMonitor::receive_latest_frame() every spout.poll_interval_ms
│       → pushes FrameBuffer into the queue
│
├── Encode thread (Supervisor::start_encode_thread)
│   └── Pops frames, encodes and sends packets while STREAMING
│
└── Metrics writer thread (Supervisor::metrics_writer_thread_func)
    └── Writes health.json / metrics.json at the configured emit intervals
```

### Inter-thread communication

| From | To | Mechanism |
|------|----|-----------|
| Capture thread | Encode thread | `std::queue` + `std::mutex` + `std::condition_variable` |
| State machine loop / encode thread | Metrics | `std::atomic` counters + `std::mutex`-protected fields |
| `Supervisor::request_stop()` | All threads | `std::atomic<bool> shutdown_requested_` |

## Encoder fallback

```text
codec (default: h264_nvenc)
      │
      ├── found and avcodec_open2() succeeds → use it
      │
      └── otherwise → fallback_codec (default: h264_mf)
                        │
                        └── avcodec_open2() succeeds → use it
                            otherwise → ENCODER_INIT_FAILED
```

Both codec names come from the config (`encoder.codec`, `encoder.fallback_codec`); any FFmpeg encoder name available in the linked FFmpeg build can be used.

Codec-specific options:

| Codec type | Options |
|------------|---------|
| Hardware (name contains `nvenc`, `amf` or `qsv`) | `rc=cbr`, `delay=0`, plus `preset`; `tune` is not applied |
| Others (e.g. `h264_mf`, `libx264`, `libopenh264`) | `preset` and `tune` from the config when non-empty |

## Dependencies

| Library | Purpose | Linkage |
|---------|---------|---------|
| Spout2 SDK | DX11 texture sharing | Static (`spout2_static`) |
| FFmpeg (avcodec/avformat/avutil/swscale/swresample) | Encoding and RTSP publishing | Dynamic (.dll) |
| nlohmann/json | Config parsing | Header-only (CMake `FetchContent`) |
| spdlog | Structured logging | CMake `FetchContent` |

## Known technical constraints

- **RTSP transport**: TCP only (`rtsp_transport=tcp`); UDP is not supported.
- **Pixel format**: SpoutDX delivers RGBA on the CPU path (`bRGB=false`).
- **Spout sender name**: a fixed name from the config file; there is no auto-discovery.
- **First frame**: the first receive after connecting may only signal an update without pixel data, so the supervisor polls until a new frame arrives (up to `5 × spout.frame_timeout_ms`, then it returns to `PROBING`).
