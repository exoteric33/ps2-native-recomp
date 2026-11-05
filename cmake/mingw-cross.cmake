# CMake toolchain file: builds the Windows ps2run.exe on Linux (or macOS) with a Linux-hosted llvm-mingw, so scripts/wine-bench.sh
# can run it under Wine (docs/wine-bench.md). The same compiler family, libc++ and UCRT as the real Windows build (scripts/portable.ps1
# pins the llvm-mingw release; scripts/wine-bench.sh downloads the matching Linux-hosted one).
#
#   cmake -S . -B build/wine-<game> -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-cross.cmake -DPS2_MINGW_ROOT=<llvm-mingw folder> ...
#
# PS2_MINGW_ROOT (cache variable or environment variable) is the folder with bin/x86_64-w64-mingw32-clang++.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

if(NOT PS2_MINGW_ROOT AND DEFINED ENV{PS2_MINGW_ROOT})
    set(PS2_MINGW_ROOT "$ENV{PS2_MINGW_ROOT}")
endif()
if(NOT PS2_MINGW_ROOT)
    message(FATAL_ERROR "mingw-cross.cmake: pass -DPS2_MINGW_ROOT=<a Linux-hosted llvm-mingw folder> (scripts/wine-bench.sh setup fetches one)")
endif()
set(PS2_MINGW_ROOT "${PS2_MINGW_ROOT}" CACHE PATH "Linux-hosted llvm-mingw folder")
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES PS2_MINGW_ROOT)  # the compiler tests read this file again without the cache

set(_triple x86_64-w64-mingw32)
set(CMAKE_C_COMPILER   "${PS2_MINGW_ROOT}/bin/${_triple}-clang")
set(CMAKE_CXX_COMPILER "${PS2_MINGW_ROOT}/bin/${_triple}-clang++")
set(CMAKE_RC_COMPILER  "${PS2_MINGW_ROOT}/bin/${_triple}-windres")
set(CMAKE_AR           "${PS2_MINGW_ROOT}/bin/llvm-ar" CACHE FILEPATH "")
set(CMAKE_RANLIB       "${PS2_MINGW_ROOT}/bin/llvm-ranlib" CACHE FILEPATH "")

# Libraries and headers come from the toolchain's sysroot and from the folders named in CMAKE_PREFIX_PATH (SDL2 for mingw);
# programs (cmake, ninja, python) are the host's.
set(CMAKE_FIND_ROOT_PATH "${PS2_MINGW_ROOT}/${_triple}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)
