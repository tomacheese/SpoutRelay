# MinGW-w64 toolchain for Windows
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

if(DEFINED ENV{MINGW_BIN})
    set(MINGW_BIN "$ENV{MINGW_BIN}")
else()
    # Default to the standard MSYS2/MinGW64 installation path.
    # Override with -DMINGW_BIN=... or set MINGW_BIN env var for other layouts.
    set(MINGW_BIN "/mingw64/bin")
endif()

set(CMAKE_C_COMPILER   "${MINGW_BIN}/gcc.exe")
set(CMAKE_CXX_COMPILER "${MINGW_BIN}/g++.exe")
set(CMAKE_RC_COMPILER  "${MINGW_BIN}/windres.exe")
set(CMAKE_AR           "${MINGW_BIN}/ar.exe")
set(CMAKE_RANLIB       "${MINGW_BIN}/ranlib.exe")
set(CMAKE_STRIP        "${MINGW_BIN}/strip.exe")

if(DEFINED ENV{MINGW_ROOT})
    set(CMAKE_FIND_ROOT_PATH "$ENV{MINGW_ROOT}")
else()
    set(CMAKE_FIND_ROOT_PATH "/mingw64/x86_64-w64-mingw32")
endif()
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
