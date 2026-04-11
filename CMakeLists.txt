cmake_minimum_required(VERSION 3.20)
project(SpoutRtspPublisher VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(BUILD_TESTS "Build unit tests" ON)

# ---------------------------------------------------------------------------
# Spout2 (git submodule — compiled from source)
# ---------------------------------------------------------------------------
set(SPOUT2_GL_DIR "${CMAKE_SOURCE_DIR}/deps/Spout2/SPOUTSDK/SpoutGL")
set(SPOUT2_DX_DIR "${CMAKE_SOURCE_DIR}/deps/Spout2/SPOUTSDK/SpoutDirectX/SpoutDX")

# ---------------------------------------------------------------------------
# FFmpeg — prefer system install via pkg-config (MSYS2 / vcpkg / system);
#           fall back to a local BtbN-style tree in deps/ffmpeg/.
# ---------------------------------------------------------------------------
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(FFMPEG QUIET
        libavcodec libavformat libavutil libswscale libswresample)
endif()

if(NOT FFMPEG_FOUND)
    set(FFMPEG_ROOT "${CMAKE_SOURCE_DIR}/deps/ffmpeg" CACHE PATH
        "FFmpeg root directory (must contain include/ and lib/). \
Used when pkg-config is unavailable.")
    if(NOT EXISTS "${FFMPEG_ROOT}/include")
        message(FATAL_ERROR
            "FFmpeg not found.\n"
            "  Option A (MSYS2): pacman -S mingw-w64-x86_64-ffmpeg\n"
            "  Option B (manual): place a BtbN shared build at deps/ffmpeg/\n"
            "                     or pass -DFFMPEG_ROOT=<path> to cmake.")
    endif()
    set(FFMPEG_INCLUDE_DIRS "${FFMPEG_ROOT}/include")
    set(FFMPEG_LIBRARY_DIRS "${FFMPEG_ROOT}/lib")
    set(FFMPEG_LIBRARIES
        "${FFMPEG_ROOT}/lib/libavcodec.dll.a"
        "${FFMPEG_ROOT}/lib/libavformat.dll.a"
        "${FFMPEG_ROOT}/lib/libavutil.dll.a"
        "${FFMPEG_ROOT}/lib/libswscale.dll.a"
        "${FFMPEG_ROOT}/lib/libswresample.dll.a"
    )
    set(FFMPEG_FOUND TRUE)
endif()

# ---------------------------------------------------------------------------
# spdlog and nlohmann/json — fetched automatically at configure time
# ---------------------------------------------------------------------------
include(FetchContent)

FetchContent_Declare(spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG        v1.15.1
    GIT_SHALLOW    TRUE
)
set(SPDLOG_BUILD_EXAMPLES OFF CACHE INTERNAL "")
set(SPDLOG_BUILD_TESTS    OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(spdlog)

FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.12.0
    GIT_SHALLOW    TRUE
)
set(JSON_BuildTests OFF CACHE INTERNAL "")
set(JSON_Install    OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(nlohmann_json)

# -------------------------------------------------------------------
# spout2_static: compiled WITHOUT WIN32_LEAN_AND_MEAN so that
# SpoutFrameCount.cpp can reach the Multimedia Timer API (mmsystem.h)
# -------------------------------------------------------------------
add_library(spout2_static STATIC
    ${SPOUT2_DX_DIR}/SpoutDX.cpp
    ${SPOUT2_GL_DIR}/SpoutCopy.cpp
    ${SPOUT2_GL_DIR}/SpoutDirectX.cpp
    ${SPOUT2_GL_DIR}/SpoutFrameCount.cpp
    ${SPOUT2_GL_DIR}/SpoutSenderNames.cpp
    ${SPOUT2_GL_DIR}/SpoutSharedMemory.cpp
    ${SPOUT2_GL_DIR}/SpoutUtils.cpp
)

# SpoutCopy.cpp uses SSSE3/SSE4.1 intrinsics
set_source_files_properties(
    ${SPOUT2_GL_DIR}/SpoutCopy.cpp
    PROPERTIES COMPILE_FLAGS "-mssse3 -msse4.1")

target_include_directories(spout2_static PUBLIC
    ${SPOUT2_DX_DIR}
    ${SPOUT2_GL_DIR}
)

target_compile_definitions(spout2_static PUBLIC
    UNICODE
    _UNICODE
    NOMINMAX
)

target_link_libraries(spout2_static PUBLIC
    d3d11 dxgi ole32 gdi32 user32 psapi winmm version)

# -------------------------------------------------------------------
# publisher_lib: our application code (uses WIN32_LEAN_AND_MEAN)
# -------------------------------------------------------------------
set(LIB_SOURCES
    src/config/config_loader.cpp
    src/app/state_machine.cpp
    src/app/supervisor.cpp
    src/spout/spout_monitor.cpp
    src/capture/frame_pump.cpp
    src/encoder/encoder_controller.cpp
    src/rtsp/rtsp_publisher_client.cpp
    src/metrics/metrics_store.cpp
    src/logging/log_sink.cpp
)

add_library(publisher_lib STATIC ${LIB_SOURCES})

target_include_directories(publisher_lib PUBLIC
    src/
    ${FFMPEG_INCLUDE_DIRS}
    ${SPOUT2_DX_DIR}
    ${SPOUT2_GL_DIR}
)

target_compile_definitions(publisher_lib PUBLIC
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

target_link_libraries(publisher_lib PUBLIC
    spout2_static
    ${FFMPEG_LIBRARIES}
    spdlog::spdlog
    nlohmann_json::nlohmann_json
    ws2_32
)

if(FFMPEG_LIBRARY_DIRS)
    target_link_directories(publisher_lib PUBLIC ${FFMPEG_LIBRARY_DIRS})
endif()

add_executable(spout-relay src/main.cpp)
target_link_libraries(spout-relay PRIVATE publisher_lib)

# Semi-static: embed C++ runtime and winpthread into the EXE so the
# redistributable does not require libgcc/libstdc++/libwinpthread DLLs.
# FFmpeg and codec DLLs are bundled separately in the distribution package.
if(MINGW)
    target_link_options(spout-relay PRIVATE
        -static-libgcc
        -static-libstdc++
        -Wl,-Bstatic,-lwinpthread,-Bdynamic
    )
endif()

# Test Spout sender (for E2E testing)
add_executable(spout_test_sender tests/spout_test_sender/main.cpp)
target_include_directories(spout_test_sender PRIVATE ${SPOUT2_DX_DIR} ${SPOUT2_GL_DIR})
target_link_libraries(spout_test_sender PRIVATE spout2_static)

if(BUILD_TESTS)
    add_subdirectory(tests)
endif()
