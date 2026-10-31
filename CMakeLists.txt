cmake_minimum_required(VERSION 3.20)
project(ps2-native-recomp CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(PS2_GENERATED "" CACHE PATH "Folder with the C++ generated for a game")
set(PS2_RUNTIME_LIB "" CACHE FILEPATH "libps2runtime.a built earlier from this source tree (scripts/runtime.sh); a game links it instead of compiling the runtime again")

if(WIN32)
    add_link_options(-static-libgcc -static-libstdc++)
endif()

if(EMSCRIPTEN)
    add_compile_options(-msimd128 -mtail-call -fwasm-exceptions -sWASM_LEGACY_EXCEPTIONS=0 -pthread)
    add_link_options(-msimd128 -mtail-call -fwasm-exceptions -sWASM_LEGACY_EXCEPTIONS=0 -pthread)
endif()

if(NOT EMSCRIPTEN)
    add_executable(eerecomp
        recomp/main.cpp
        recomp/r5900.cpp
        recomp/elf.cpp
        recomp/analysis.cpp
        recomp/codegen.cpp
        recomp/eerecomp_gen.cpp
        recomp/snr2.cpp)
    target_include_directories(eerecomp PRIVATE runtime/include)
    target_compile_options(eerecomp PRIVATE -Wall -Wextra)

    add_executable(vujit_dump recomp/vujit_dump.cpp runtime/src/vu_jit.cpp)
    target_include_directories(vujit_dump PRIVATE runtime/include runtime/src)
    add_executable(vurecomp recomp/vurecomp.cpp recomp/vurecomp_gen.cpp)
    target_include_directories(vurecomp PRIVATE runtime/include)
    target_compile_options(vurecomp PRIVATE -Wall -Wextra)
endif()

if(PS2_RUNTIME_LIB AND NOT EMSCRIPTEN)
    set(PS2_PREBUILT ON)
    set(PS2_API INTERFACE)
    add_library(ps2runtime STATIC IMPORTED)
    set_target_properties(ps2runtime PROPERTIES IMPORTED_LOCATION "${PS2_RUNTIME_LIB}")
else()
    set(PS2_PREBUILT OFF)
    set(PS2_API PUBLIC)
    add_library(ps2runtime STATIC
        runtime/src/memory.cpp
        runtime/src/dispatch.cpp
        runtime/src/system.cpp
        runtime/src/kernel.cpp
        runtime/src/hw.cpp
        runtime/src/cop.cpp
        runtime/src/disc.cpp
        runtime/src/dmac.cpp
        runtime/src/vif.cpp
        runtime/src/vu_micro.cpp
        runtime/src/vu_jit.cpp
        runtime/src/vu_jit_web.cpp
        runtime/src/live_build.cpp
        runtime/src/vu_live.cpp
        runtime/src/ee_live.cpp
        runtime/src/ipu/ipu.cpp
        runtime/src/gs/gs.cpp
        runtime/src/gs/gif.cpp
        runtime/src/gs/queue.cpp
        runtime/src/gs/raster.cpp
        runtime/src/iop/cpu.cpp
        runtime/src/iop/kernel.cpp
        runtime/src/iop/libs.cpp
        runtime/src/iop/hw.cpp
        runtime/src/iop/spu2.cpp
        runtime/src/iop/sio2.cpp
        runtime/src/iop/io.cpp
        runtime/src/iop/cdvd.cpp
        runtime/src/iop/sif.cpp
        runtime/src/iop/cdvdfsv.cpp)
    target_compile_options(ps2runtime PRIVATE -Wall -Wextra)
    if(NOT EMSCRIPTEN)
        # Vector programs the build does not know are compiled while the game runs (runtime/src/vu_live.cpp, docs/vu-live.md): the
        # generator is the one of the vurecomp tool, and the headers its output includes are embedded in the runner.
        target_sources(ps2runtime PRIVATE recomp/vurecomp_gen.cpp)
        # Main processor code that the build does not know is translated, compiled and loaded while the game runs too
        # (runtime/src/ee_live.cpp, docs/ee-live.md): the translator is the one of the eerecomp tool, and the headers its output includes
        # are embedded in the runner as well.
        target_sources(ps2runtime PRIVATE recomp/eerecomp_gen.cpp recomp/codegen.cpp recomp/analysis.cpp recomp/r5900.cpp recomp/elf.cpp recomp/snr2.cpp)
        target_include_directories(ps2runtime PRIVATE recomp)
        foreach(entry "cpu:Cpu" "fpu:Fpu" "vu:Vu" "vu_ops:VuOps" "memory:Memory" "mmi:Mmi" "generated:Generated" "ee_live_abi:EeLiveAbi")
            string(REPLACE ":" ";" pair "${entry}")
            list(GET pair 0 hdr)
            list(GET pair 1 sym)
            set(live_hdr "${CMAKE_BINARY_DIR}/live_hdr_${hdr}.cpp")
            add_custom_command(OUTPUT "${live_hdr}"
                COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_SOURCE_DIR}/runtime/include/ps2/${hdr}.hpp -DOUT=${live_hdr} -DNAME=kLiveHdr${sym} -P ${CMAKE_SOURCE_DIR}/cmake/embed.cmake
                DEPENDS runtime/include/ps2/${hdr}.hpp cmake/embed.cmake)
            target_sources(ps2runtime PRIVATE "${live_hdr}")
        endforeach()
        # the compiler this runner was built with is the last place vu_live.cpp looks for one
        set_property(SOURCE runtime/src/live_build.cpp APPEND PROPERTY COMPILE_DEFINITIONS "PS2_LIVE_DEFAULT_CXX=\"${CMAKE_CXX_COMPILER}\"")
    endif()
endif()
if(NOT WIN32 AND NOT EMSCRIPTEN)
    target_link_libraries(ps2runtime ${PS2_API} ${CMAKE_DL_LIBS})  # vu_live.cpp loads libraries with dlopen
endif()
target_include_directories(ps2runtime ${PS2_API} "${CMAKE_CURRENT_SOURCE_DIR}/runtime/include")

# the IOP thread manager tested through a synthetic module, no game needed: ninja ps2thbasetest
if(NOT EMSCRIPTEN)
    add_executable(ps2thbasetest EXCLUDE_FROM_ALL tests/iop_thbase.cpp)
    target_include_directories(ps2thbasetest PRIVATE runtime/src)
    target_link_libraries(ps2thbasetest PRIVATE ps2runtime)
    if(WIN32)
        target_link_libraries(ps2thbasetest PRIVATE synchronization)  # the GS queue's WaitOnAddress, which the wgpu line above adds only with wgpu
    endif()
endif()

set(PS2_WGPU_DIR "${CMAKE_SOURCE_DIR}/third_party/wgpu-native" CACHE PATH "wgpu-native release folder")
if(EMSCRIPTEN OR EXISTS "${PS2_WGPU_DIR}/lib/libwgpu_native.a")
    set(PS2_GS_WGSL "${CMAKE_BINARY_DIR}/gs_wgsl.cpp")
    add_custom_command(OUTPUT "${PS2_GS_WGSL}"
        COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_SOURCE_DIR}/runtime/src/gs/gpu/gs.wgsl -DOUT=${PS2_GS_WGSL} -DNAME=kGsShader -P ${CMAKE_SOURCE_DIR}/cmake/embed.cmake
        DEPENDS runtime/src/gs/gpu/gs.wgsl cmake/embed.cmake)
    set(PS2_BLIT_WGSL "${CMAKE_BINARY_DIR}/blit_wgsl.cpp")
    add_custom_command(OUTPUT "${PS2_BLIT_WGSL}"
        COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_SOURCE_DIR}/runtime/src/gs/gpu/blit.wgsl -DOUT=${PS2_BLIT_WGSL} -DNAME=kBlitShader -P ${CMAKE_SOURCE_DIR}/cmake/embed.cmake
        DEPENDS runtime/src/gs/gpu/blit.wgsl cmake/embed.cmake)
    if(NOT PS2_PREBUILT)
        target_sources(ps2runtime PRIVATE runtime/src/gs/gpu/gpu.cpp "${PS2_GS_WGSL}" "${PS2_BLIT_WGSL}")
    endif()
    target_compile_definitions(ps2runtime ${PS2_API} PS2_GPU)
    if(EMSCRIPTEN)
        target_compile_options(ps2runtime PUBLIC --use-port=emdawnwebgpu)
        target_link_options(ps2runtime PUBLIC --use-port=emdawnwebgpu)
    else()
        if(NOT PS2_PREBUILT)
            target_include_directories(ps2runtime PRIVATE "${PS2_WGPU_DIR}/include")
        endif()
        if(WIN32)
            target_link_libraries(ps2runtime ${PS2_API} "${PS2_WGPU_DIR}/lib/libwgpu_native.a" ws2_32 userenv ntdll synchronization pthread m)
        else()
            target_link_libraries(ps2runtime ${PS2_API} "${PS2_WGPU_DIR}/lib/libwgpu_native.a" dl pthread m)
        endif()
        if(APPLE)
            target_link_libraries(ps2runtime ${PS2_API} "-framework Metal" "-framework QuartzCore" "-framework Foundation" "-framework CoreFoundation" "-framework IOKit" "-framework IOSurface" objc)
        endif()
    endif()
endif()

if(NOT EMSCRIPTEN)
    add_executable(ps2sifcmdtest EXCLUDE_FROM_ALL tests/sif_cmd_intr.cpp)
    target_include_directories(ps2sifcmdtest PRIVATE runtime/src)
    target_link_libraries(ps2sifcmdtest PRIVATE ps2runtime)
    if(WIN32)
        target_link_libraries(ps2sifcmdtest PRIVATE synchronization)  # the GS queue's WaitOnAddress, which the wgpu line above adds only with wgpu
    endif()
endif()

if(PS2_GENERATED)
    file(GLOB PS2_UNITS CONFIGURE_DEPENDS "${PS2_GENERATED}/*.cpp")
    add_library(ps2game STATIC ${PS2_UNITS})
    target_include_directories(ps2game PRIVATE "${PS2_GENERATED}")
    target_link_libraries(ps2game PUBLIC ps2runtime)
    target_compile_options(ps2game PRIVATE -Wno-unused-label -Wno-unused-variable)

    if(EMSCRIPTEN)
        set(PS2_WEB_ELF "" CACHE FILEPATH "Game executable preloaded as /game/core.elf")
        set(PS2_WEB_BOOT_FILE "" CACHE FILEPATH "Text file holding the argv[0] the game sees, preloaded as /game/boot.txt")
        set(PS2_WEB_PATCHES "" CACHE FILEPATH "Patch list the runtime writes into memory, preloaded as /game/patches.txt")
        add_executable(ps2web host/web.cpp)
        if(PS2_WEB_BOOT_FILE)
            target_link_options(ps2web PRIVATE "SHELL:--preload-file \"${PS2_WEB_BOOT_FILE}@/game/boot.txt\"")
        endif()
        if(PS2_WEB_PATCHES)
            target_link_options(ps2web PRIVATE "SHELL:--preload-file \"${PS2_WEB_PATCHES}@/game/patches.txt\"")
        endif()
        target_link_libraries(ps2web PRIVATE ps2game ps2runtime)
        target_link_options(ps2web PRIVATE --profiling-funcs -sJSPI -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=268435456 -sPTHREAD_POOL_SIZE=2 -sALLOW_TABLE_GROWTH=1 -lidbfs.js -sSTACK_SIZE=16777216 -sENVIRONMENT=web
            -sEXPORTED_FUNCTIONS=_main,_ps2web_pad,_ps2web_pad_connected,_ps2web_resize,_ps2web_gpu_ms,_ps2web_frames,_ps2web_primitives,_ps2web_audio_ring,_ps2web_audio_queued_ms,_ps2web_idle_ms,_ps2web_vu_interpreted,_ps2web_quick_state,_ps2web_video_options,_ps2web_stream
            -sEXPORTED_RUNTIME_METHODS=callMain,addRunDependency,removeRunDependency,HEAPU8,ENV
            --shell-file ${CMAKE_SOURCE_DIR}/web/shell.html --preload-file ${PS2_WEB_ELF}@/game/core.elf)
        set_target_properties(ps2web PROPERTIES SUFFIX ".html" LINK_DEPENDS ${CMAKE_SOURCE_DIR}/web/shell.html)
        return()
    endif()

    add_executable(ps2run host/main.cpp)
    target_link_libraries(ps2run PRIVATE ps2game ps2runtime)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
        target_sources(ps2run PRIVATE host/profile.cpp)
        target_compile_definitions(ps2run PRIVATE PS2_PROFILE)
    elseif(WIN32 AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
        target_sources(ps2run PRIVATE host/profile_win.cpp)
        target_compile_definitions(ps2run PRIVATE PS2_PROFILE)
        target_link_libraries(ps2run PRIVATE winmm)
    endif()
    if(WIN32)
        list(APPEND CMAKE_PREFIX_PATH "${CMAKE_SOURCE_DIR}/third_party/sdl2")
        set(SDL2_NO_MWINDOWS ON)
    endif()
    find_package(SDL2 QUIET)
    if(SDL2_FOUND AND NOT EMSCRIPTEN)
        target_sources(ps2run PRIVATE host/window.cpp host/keymap.cpp host/stretch.cpp)
        target_compile_definitions(ps2run PRIVATE PS2_WINDOW)
        target_link_libraries(ps2run PRIVATE SDL2::SDL2)
        if(WIN32)
            add_custom_command(TARGET ps2run POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:SDL2::SDL2> $<TARGET_FILE_DIR:ps2run>)
            # Microsoft's shader compiler for Direct3D 12 (scripts/fetch_dxc.sh): used when the two DLLs sit next to ps2run.exe,
            # FXC otherwise. Not shipped with the source: each PC downloads it.
            foreach(dxc_dll dxcompiler.dll dxil.dll)
                if(EXISTS "${CMAKE_SOURCE_DIR}/third_party/dxc/${dxc_dll}")
                    add_custom_command(TARGET ps2run POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/third_party/dxc/${dxc_dll}" $<TARGET_FILE_DIR:ps2run>)
                endif()
            endforeach()
            # A window (GUI) program, so a double-click or a shortcut opens no console window (-mwindows; SDL2_NO_MWINDOWS above
            # keeps SDL's own package from adding it). The entry point still reaches main() in host/main.cpp, which gives stdout
            # and stderr back: a pipe or file of the parent as it is, else the console of the parent, else NUL.
            set_target_properties(ps2run PROPERTIES WIN32_EXECUTABLE ON)
        endif()
        # the game controller path tested with SDL's virtual controllers, no game needed: ninja -C build/native-<game> ps2padtest
        add_executable(ps2padtest EXCLUDE_FROM_ALL host/pad_test.cpp host/window.cpp host/keymap.cpp host/stretch.cpp)
        target_include_directories(ps2padtest PRIVATE runtime/src)
        target_link_libraries(ps2padtest PRIVATE ps2runtime SDL2::SDL2)
    endif()
endif()
