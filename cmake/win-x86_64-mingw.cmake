set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

get_filename_component(_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(_LLVM_MINGW_ROOT "${_REPO_ROOT}/third_party/llvm-mingw")

set(CMAKE_C_COMPILER "${_LLVM_MINGW_ROOT}/bin/x86_64-w64-mingw32-clang")
set(CMAKE_CXX_COMPILER "${_LLVM_MINGW_ROOT}/bin/x86_64-w64-mingw32-clang++")
set(CMAKE_RC_COMPILER "${_LLVM_MINGW_ROOT}/bin/x86_64-w64-mingw32-windres")

# Clear host environment flags (such as Conda's Linux ELF-only -Wl,-z,relro / -rpath-link)
set(ENV{CFLAGS} "")
set(ENV{CXXFLAGS} "")
set(ENV{CPPFLAGS} "")
set(ENV{LDFLAGS} "")
set(CMAKE_C_FLAGS_INIT "")
set(CMAKE_CXX_FLAGS_INIT "-mavx2 -ffp-contract=off")
set(CMAKE_EXE_LINKER_FLAGS_INIT "")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "")

set(PS2_WGPU_DIR "${_REPO_ROOT}/third_party/wgpu-native-win" CACHE PATH "wgpu-native Windows release folder" FORCE)

set(CMAKE_FIND_ROOT_PATH
    "${_LLVM_MINGW_ROOT}/x86_64-w64-mingw32"
    "${_REPO_ROOT}/third_party/sdl2"
    "${_REPO_ROOT}/third_party/wgpu-native-win"
)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
