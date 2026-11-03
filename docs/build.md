# Build Guide

## Prerequisites

### Compiler

GCC 15.2.0 (MinGW-w64) is recommended. It can be installed with [Scoop](https://scoop.sh/):

```powershell
scoop install gcc
```

Alternatively, use the MSYS2 MinGW64 toolchain, as CI does.

### Build tools

```powershell
scoop install cmake ninja
```

### Preparing dependencies

The following are required.

#### FFmpeg development libraries

Either option works:

- **Local tree**: download `ffmpeg-master-latest-win64-lgpl-shared.zip` from [BtbN/FFmpeg-Builds](https://github.com/BtbN/FFmpeg-Builds/releases), extract it and place the contents in `deps/ffmpeg/` (or pass `-DFFMPEG_ROOT=<path>` to CMake).
- **pkg-config / MSYS2**: if `pkg-config` finds `libavcodec`, `libavformat`, `libavutil`, `libswscale` and `libswresample`, they are used instead (for example `pacman -S mingw-w64-x86_64-ffmpeg`).

```text
deps/ffmpeg/
├── bin/          # DLLs such as avcodec-62.dll
├── include/      # headers such as libavcodec/
└── lib/          # import libraries such as libavcodec.dll.a
```

#### Spout2 SDK

Spout2 is a git submodule at `deps/Spout2/`. Fetch it with:

```powershell
git submodule update --init
```

```text
deps/Spout2/
└── SPOUTSDK/
    ├── SpoutGL/          # SpoutCopy, SpoutDirectX, SpoutFrameCount, etc.
    └── SpoutDirectX/
        └── SpoutDX/      # SpoutDX.h / SpoutDX.cpp
```

#### nlohmann/json and spdlog (automatic)

Both are downloaded automatically by CMake `FetchContent` at configure time. No manual setup is needed.

## Steps

### 1. Configure CMake

```powershell
cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=toolchain-mingw.cmake
```

`toolchain-mingw.cmake` defaults to the standard MSYS2/MinGW64 install path (`/mingw64/bin`). If MinGW is installed elsewhere, override it with `-DMINGW_BIN=...` / `-DMINGW_ROOT=...` or the `MINGW_BIN` / `MINGW_ROOT` environment variables.

### 2. Build (all targets)

```powershell
cmake --build build
```

#### Individual targets

```powershell
# Main app only
cmake --build build --target spout-relay

# Unit tests only
cmake --build build --target publisher_tests

# Test Spout senders only
cmake --build build --target spout_test_sender
cmake --build build --target spout_static_sender
cmake --build build --target spout_res_changer
```

### 3. Copy the FFmpeg DLLs

After the build, copy the DLLs from `deps/ffmpeg/bin/` next to the executable. The build succeeds without them, but `spout-relay.exe` fails to start.

```powershell
Copy-Item deps\ffmpeg\bin\*.dll build\
```

Required DLLs:

| DLL | Purpose |
|-----|---------|
| `avcodec-62.dll` | H.264 encoding |
| `avformat-62.dll` | RTSP muxer |
| `avutil-60.dll` | FFmpeg utilities |
| `swscale-9.dll` | RGBA→YUV conversion |
| `swresample-6.dll` | Dependency of avformat |

The version suffixes match the FFmpeg release the project is built against; adjust them if you use a different FFmpeg major version.

## Build output

```text
build/
├── spout-relay.exe           # Main app
├── spout_test_sender.exe     # Spout sender for E2E tests
├── spout_static_sender.exe   # Freeze-frame sender for E2E tests
├── spout_res_changer.exe     # Resolution-changing sender for E2E tests
└── tests/
    └── publisher_tests.exe   # Unit tests
```

## Running the tests

```powershell
.\build\tests\publisher_tests.exe
```

Expected output ends with:

```text
All tests passed.
```

Each passing assertion block prints a `[PASS]` line. For test categories and details, see [testing.md](testing.md).

## Customizing the toolchain

Edit `toolchain-mingw.cmake` to change the compiler paths, or override `MINGW_BIN` as described above.

## CMake targets

| Target | Type | Description |
|--------|------|-------------|
| `spout2_static` | Static library | Spout2 SDK (compiled without `WIN32_LEAN_AND_MEAN`) |
| `publisher_lib` | Static library | All application logic |
| `spout-relay` | Executable | Main app |
| `publisher_tests` | Executable | Unit tests with a custom test runner (built when `BUILD_TESTS=ON`, the default) |
| `spout_test_sender` | Executable | Sender for E2E tests |
| `spout_static_sender` | Executable | Freeze-frame sender for E2E tests |
| `spout_res_changer` | Executable | Resolution-changing sender for E2E tests |

### Why `spout2_static` is separate

`SpoutFrameCount.cpp` uses the multimedia timer API (`TIMECAPS`, `timeBeginPeriod`). When `WIN32_LEAN_AND_MEAN` is defined, `mmsystem.h` is not included and compilation fails. The Spout2 sources are therefore compiled as a separate target without `WIN32_LEAN_AND_MEAN`.
