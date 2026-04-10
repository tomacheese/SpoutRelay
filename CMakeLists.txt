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
    # Detection only (no STATIC — pkg-config --static can fail for some FFmpeg
    # transitive deps like rav1e that lack proper .pc files on MSYS2)
    pkg_check_modules(FFMPEG QUIET
        libavcodec libavformat libavutil libswscale libswresample)

    if(FFMPEG_FOUND AND MINGW)
        # For static single-binary builds, expand to full transitive dep list.
        # Separate -L dirs from -l/-framework/other link flags.
        execute_process(
            COMMAND "${PKG_CONFIG_EXECUTABLE}" --static --libs
                libavcodec libavformat libavutil libswscale libswresample
            OUTPUT_VARIABLE _ffmpeg_static_raw
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
            RESULT_VARIABLE _ffmpeg_static_ok
        )
        if(_ffmpeg_static_ok EQUAL 0 AND _ffmpeg_static_raw)
            separate_arguments(_ffmpeg_static_all UNIX_COMMAND "${_ffmpeg_static_raw}")
            set(_ffmpeg_static_libs "")
            set(_ffmpeg_static_dirs "")
            foreach(_f ${_ffmpeg_static_all})
                if(_f MATCHES "^-L(.+)$")
                    list(APPEND _ffmpeg_static_dirs "${CMAKE_MATCH_1}")
                else()
                    list(APPEND _ffmpeg_static_libs "${_f}")
                endif()
            endforeach()
            # -ldl is Linux-only (Windows uses LoadLibrary; no libdl exists in MinGW)
            list(REMOVE_ITEM _ffmpeg_static_libs "-ldl")
            # shaderc_shared is DLL-only; replace with the static libshaderc.a
            # (provided by mingw-w64-x86_64-shaderc; installed in ci.yml)
            list(TRANSFORM _ffmpeg_static_libs REPLACE "^-lshaderc_shared$" "-lshaderc")
            # soxr.a is compiled with OpenMP but its .pc does not list -lgomp;
            # libgomp.a is provided by GCC's own package (mingw-w64-x86_64-gcc).
            if(NOT "-lgomp" IN_LIST _ffmpeg_static_libs)
                list(APPEND _ffmpeg_static_libs "-lgomp")
            endif()
            # libjxl.a uses the Highway SIMD library but pkg-config omits -lhwy
            if(NOT "-lhwy" IN_LIST _ffmpeg_static_libs)
                list(APPEND _ffmpeg_static_libs "-lhwy")
            endif()
            # rav1e (Rust) calls Windows NT APIs (NtWriteFile, RtlNtStatusToDosError)
            # that are not in MinGW's default auto-linked libs
            if(NOT "-lntdll" IN_LIST _ffmpeg_static_libs)
                list(APPEND _ffmpeg_static_libs "-lntdll")
            endif()

            set(FFMPEG_LIBRARIES ${_ffmpeg_static_libs})
            if(_ffmpeg_static_dirs)
                list(APPEND FFMPEG_LIBRARY_DIRS ${_ffmpeg_static_dirs})
                list(REMOVE_DUPLICATES FFMPEG_LIBRARY_DIRS)
            endif()
            message(STATUS "FFmpeg: resolved (static, dl/shaderc_shared excluded)")
        else()
            message(STATUS "pkg-config --static unavailable; using dynamic FFmpeg")
        endif()
    endif()
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

add_executable(publisher-agent src/main.cpp)
target_link_libraries(publisher-agent PRIVATE publisher_lib)

# Static-link the C++ runtime and prefer static archives for all deps.
# Special exclusions in FFMPEG_LIBRARIES (see above):
#   -ldl removed (Linux-only), shaderc_shared→shaderc, -lgomp/-lhwy/-lntdll added.
# rav1e and rsvg-2 both embed the Rust standard library; --allow-multiple-definition
# prevents a duplicate-symbol link failure.
if(MINGW)
    target_link_options(publisher-agent PRIVATE
        -static
        -static-libgcc
        -static-libstdc++
        -Wl,--allow-multiple-definition
    )
endif()

# Test Spout sender (for E2E testing)
add_executable(spout_test_sender tests/spout_test_sender/main.cpp)
target_include_directories(spout_test_sender PRIVATE ${SPOUT2_DX_DIR} ${SPOUT2_GL_DIR})
target_link_libraries(spout_test_sender PRIVATE spout2_static)

if(BUILD_TESTS)
    add_subdirectory(tests)
endif()
