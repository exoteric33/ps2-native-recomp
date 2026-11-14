cmake_minimum_required(VERSION 3.20)
project(ps2-native-recomp CXX)

# Only clang builds the project (Clang, AppleClang, llvm-mingw; emcc is clang too): the vector units use clang builtins such as
# __builtin_elementwise_min and __builtin_reduce_or (ps2/vu_ops.hpp) that GCC and MSVC lack. Stop here with a clear message
# instead of hundreds of compile errors later. scripts/cxx.sh finds a clang++.
if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "ps2-native-recomp needs clang++ (Clang, AppleClang, llvm-mingw or Emscripten's emcc), but the C++ compiler "
        "is ${CMAKE_CXX_COMPILER_ID} (${CMAKE_CXX_COMPILER}). The code uses clang builtins that other compilers lack. "
        "Install clang and configure again with -DCMAKE_CXX_COMPILER=clang++ in a new build folder (scripts/cxx.sh picks one).")
endif()

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
        runtime/src/os.cpp
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
    # the emulation thread's big stack (host/big_stack_thread.hpp), no game and no runtime needed: ninja ps2bigstacktest
    add_executable(ps2bigstacktest EXCLUDE_FROM_ALL tests/big_stack.cpp)
    target_link_libraries(ps2bigstacktest PRIVATE pthread)
    # the memory card file and save state writes, no game needed: ninja ps2savetest
    add_executable(ps2savetest EXCLUDE_FROM_ALL tests/save_files.cpp)
    target_include_directories(ps2savetest PRIVATE runtime/src)
    target_link_libraries(ps2savetest PRIVATE ps2runtime)
    if(WIN32)
        target_link_libraries(ps2savetest PRIVATE synchronization)
    endif()
    # which graphics chips get the draw program without wgpu's run-time checks by default (no game, no graphics): ninja ps2trustedtest
    add_executable(ps2trustedtest EXCLUDE_FROM_ALL tests/trusted_shader.cpp)
    target_include_directories(ps2trustedtest PRIVATE runtime/src)
    # the picture pace's hold (runtime/include/ps2/pace.hpp) with made-up games, no game and no runtime needed: ninja ps2pacetest
    add_executable(ps2pacetest EXCLUDE_FROM_ALL tests/pace.cpp)
    target_include_directories(ps2pacetest PRIVATE runtime/include)
    # the lines of the picture each video mode shows (480p must not be cut like a TV picture): ninja ps2displaylinestest
    add_executable(ps2displaylinestest EXCLUDE_FROM_ALL tests/display_lines.cpp)
    target_include_directories(ps2displaylinestest PRIVATE runtime/src)
    target_link_libraries(ps2displaylinestest PRIVATE ps2runtime)
    if(WIN32)
        target_link_libraries(ps2displaylinestest PRIVATE synchronization)
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
    # The experimental hardware drawing path (hwdraw*, hwcache*, hwstat*): off unless PS2_GPU_HW=1 is set. The browser build
    # compiles the three files as empty stubs and embeds none of its shaders.
    set(PS2_HW_SOURCES runtime/src/gs/gpu/hwdraw.cpp runtime/src/gs/gpu/hwstat.cpp runtime/src/gs/gpu/hwcache.cpp)
    if(NOT EMSCRIPTEN)
        set(PS2_HW_WGSL "${CMAKE_BINARY_DIR}/hw_wgsl.cpp")
        add_custom_command(OUTPUT "${PS2_HW_WGSL}"
            COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_SOURCE_DIR}/runtime/src/gs/gpu/hwdraw.wgsl -DOUT=${PS2_HW_WGSL} -DNAME=kHwShader -P ${CMAKE_SOURCE_DIR}/cmake/embed.cmake
            DEPENDS runtime/src/gs/gpu/hwdraw.wgsl cmake/embed.cmake)
        set(PS2_CACHE_WGSL "${CMAKE_BINARY_DIR}/cache_wgsl.cpp")
        add_custom_command(OUTPUT "${PS2_CACHE_WGSL}"
            COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_SOURCE_DIR}/runtime/src/gs/gpu/hwcache.wgsl -DOUT=${PS2_CACHE_WGSL} -DNAME=kCacheShader -P ${CMAKE_SOURCE_DIR}/cmake/embed.cmake
            DEPENDS runtime/src/gs/gpu/hwcache.wgsl cmake/embed.cmake)
        set(PS2_HW_SYNC_WGSL "${CMAKE_BINARY_DIR}/hw_sync_wgsl.cpp")
        add_custom_command(OUTPUT "${PS2_HW_SYNC_WGSL}"
            COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_SOURCE_DIR}/runtime/src/gs/gpu/hwdraw_sync.wgsl -DOUT=${PS2_HW_SYNC_WGSL} -DNAME=kHwSyncShader -P ${CMAKE_SOURCE_DIR}/cmake/embed.cmake
            DEPENDS runtime/src/gs/gpu/hwdraw_sync.wgsl cmake/embed.cmake)
        list(APPEND PS2_HW_SOURCES "${PS2_HW_WGSL}" "${PS2_HW_SYNC_WGSL}" "${PS2_CACHE_WGSL}")
    endif()
    if(NOT PS2_PREBUILT)
        target_sources(ps2runtime PRIVATE runtime/src/gs/gpu/gpu.cpp runtime/src/gs/gpu/tiles.cpp "${PS2_GS_WGSL}" "${PS2_BLIT_WGSL}" ${PS2_HW_SOURCES})
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
    # the GS's mipmapping in the software reference and the draw program, no game data (docs/mipmap.md): ninja ps2mipmaptest
    if(NOT EMSCRIPTEN)
        add_executable(ps2mipmaptest EXCLUDE_FROM_ALL tests/mipmap.cpp)
        target_include_directories(ps2mipmaptest PRIVATE runtime/src)
        target_link_libraries(ps2mipmaptest PRIVATE ps2runtime)
    endif()
endif()

# Differential test of the experimental card path (hwdraw.cpp) against the painter: ninja ps2hwalpha. Links the runtime (the draw path is the game's own).
if(NOT EMSCRIPTEN AND EXISTS "${PS2_WGPU_DIR}/lib/libwgpu_native.a")
    add_executable(ps2hwalpha EXCLUDE_FROM_ALL tests/hwalpha.cpp)
    target_include_directories(ps2hwalpha PRIVATE runtime/src "${PS2_WGPU_DIR}/include")
    target_link_libraries(ps2hwalpha PRIVATE ps2runtime)
    if(WIN32)
        target_link_libraries(ps2hwalpha PRIVATE synchronization)
    endif()
endif()

# Stand-alone measurements of what the card path asks of a graphics card: ninja ps2hwprobe. Uses wgpu only, not the runtime.
if(NOT EMSCRIPTEN AND EXISTS "${PS2_WGPU_DIR}/lib/libwgpu_native.a")
    add_executable(ps2hwprobe EXCLUDE_FROM_ALL tests/hwprobe.cpp)
    target_include_directories(ps2hwprobe PRIVATE "${PS2_WGPU_DIR}/include")
    if(WIN32)
        target_link_libraries(ps2hwprobe PRIVATE "${PS2_WGPU_DIR}/lib/libwgpu_native.a" ws2_32 userenv ntdll synchronization pthread m)
    else()
        target_link_libraries(ps2hwprobe PRIVATE "${PS2_WGPU_DIR}/lib/libwgpu_native.a" dl pthread m)
    endif()
    if(APPLE)
        target_link_libraries(ps2hwprobe PRIVATE "-framework Metal" "-framework QuartzCore" "-framework Foundation" "-framework CoreFoundation" "-framework IOKit" "-framework IOSurface" objc)
    endif()
endif()

if(NOT EMSCRIPTEN)
    add_executable(ps2sifcmdtest EXCLUDE_FROM_ALL tests/sif_cmd_intr.cpp)
    target_include_directories(ps2sifcmdtest PRIVATE runtime/src)
    target_link_libraries(ps2sifcmdtest PRIVATE ps2runtime)
    if(WIN32)
        target_link_libraries(ps2sifcmdtest PRIVATE synchronization)  # the GS queue's WaitOnAddress, which the wgpu line above adds only with wgpu
    endif()
    # the four-lane VU0 macro helpers the recompiler calls against the one-lane ones, and the flag liveness that goes with
    # them (headers and the decoder only, no game needed): ninja ps2vu0inlinetest && ./ps2vu0inlinetest [rounds]
    add_executable(ps2vu0inlinetest EXCLUDE_FROM_ALL tests/vu0_inline.cpp recomp/r5900.cpp)
    target_include_directories(ps2vu0inlinetest PRIVATE runtime/include recomp)
    # VU control-flow, E-bit return, and ILW/branch-register timing regression fixture: ninja ps2vuregtest
    add_executable(ps2vuregtest EXCLUDE_FROM_ALL tests/test_vu_regression.cpp)
    target_include_directories(ps2vuregtest PRIVATE runtime/src)
    target_link_libraries(ps2vuregtest PRIVATE ps2runtime)
    if(WIN32)
        target_link_libraries(ps2vuregtest PRIVATE synchronization)
    endif()
    # the GS message ring between two threads (wrap, full ring, held batches, sleeps and wake-ups), no game needed: ninja ps2queuestresstest
    add_executable(ps2queuestresstest EXCLUDE_FROM_ALL tests/gs_queue_stress.cpp)
    target_include_directories(ps2queuestresstest PRIVATE runtime/src)
    target_link_libraries(ps2queuestresstest PRIVATE ps2runtime)
    if(WIN32)
        target_link_libraries(ps2queuestresstest PRIVATE synchronization)
    endif()
    # the late float clean-up of the compiled VU1 programs against the old one, on every bit pattern and with the processor's
    # "denormals are zero" bit set (ps2/vu_ops.hpp), no game needed: ninja ps2vucleanuptest
    add_executable(ps2vucleanuptest EXCLUDE_FROM_ALL tests/test_vu_cleanup.cpp)
    target_include_directories(ps2vucleanuptest PRIVATE runtime/include)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64")
        target_compile_options(ps2vucleanuptest PRIVATE -msse4.1)
    endif()
    # the decode cache's blocks (runtime/src/gs/gpu/gpu.cpp, for_cells) against the drawing program's address tables, no game needed: ninja ps2decodecachetest
    add_executable(ps2decodecachetest EXCLUDE_FROM_ALL tests/decode_cache.cpp)
    target_link_libraries(ps2decodecachetest PRIVATE ps2runtime)
    # the triangle set-up of the drawing thread (vectors, runtime/src/gs/gpu/setup.hpp) against the scalar arithmetic, bit for bit,
    # no game needed: ninja ps2drawsetuptest
    add_executable(ps2drawsetuptest EXCLUDE_FROM_ALL tests/draw_setup.cpp)
    target_include_directories(ps2drawsetuptest PRIVATE runtime/include runtime/src)
    # the tile lists of a drawing batch (runtime/src/gs/gpu/tiles.cpp) against the way they were built before, word for word; also times
    # batches recorded from a game. No game needed: ninja ps2tilelisttest
    add_executable(ps2tilelisttest EXCLUDE_FROM_ALL tests/tile_lists.cpp)
    target_include_directories(ps2tilelisttest PRIVATE runtime/include runtime/src)
    target_link_libraries(ps2tilelisttest PRIVATE ps2runtime)
    if(WIN32)
        target_link_libraries(ps2tilelisttest PRIVATE synchronization)
    endif()
    # damaged disc images, IOP modules and boot ELFs refused cleanly, no game needed: ninja ps2discinputtest
    add_executable(ps2discinputtest EXCLUDE_FROM_ALL tests/disc_input.cpp)
    target_include_directories(ps2discinputtest PRIVATE runtime/src)
    target_link_libraries(ps2discinputtest PRIVATE ps2runtime)
    if(WIN32)
        target_link_libraries(ps2discinputtest PRIVATE synchronization)
    endif()
    # bad guest pointers and sizes at the IOP's libraries and the SIF cut or refused, no game needed: ninja ps2iopboundstest
    add_executable(ps2iopboundstest EXCLUDE_FROM_ALL tests/iop_bounds.cpp)
    target_include_directories(ps2iopboundstest PRIVATE runtime/src)
    target_link_libraries(ps2iopboundstest PRIVATE ps2runtime)
    if(WIN32)
        target_link_libraries(ps2iopboundstest PRIVATE synchronization)
    endif()
    # the IOP interpreter's decode table against the interpreter before it, and code that changes under it (no game needed): ninja ps2iopdecodetest
    add_executable(ps2iopdecodetest EXCLUDE_FROM_ALL tests/iop_decode.cpp)
    target_include_directories(ps2iopdecodetest PRIVATE runtime/src)
    target_link_libraries(ps2iopdecodetest PRIVATE ps2runtime)
    if(WIN32)
        target_link_libraries(ps2iopdecodetest PRIVATE synchronization)
    endif()
    # modload #9 and cdvdman #44 (Tales of Destiny 2 / Director's Cut, no game needed): ninja ps2talestest
    add_executable(ps2talestest EXCLUDE_FROM_ALL tests/iop_tales.cpp)
    target_include_directories(ps2talestest PRIVATE runtime/src)
    target_link_libraries(ps2talestest PRIVATE ps2runtime)
    if(WIN32)
        target_link_libraries(ps2talestest PRIVATE synchronization)
    endif()
    # the sound lines of the F9 problem report, no game needed: ninja ps2spu2summarytest
    add_executable(ps2spu2summarytest EXCLUDE_FROM_ALL tests/spu2_summary.cpp)
    target_include_directories(ps2spu2summarytest PRIVATE runtime/src)
    target_link_libraries(ps2spu2summarytest PRIVATE ps2runtime)
    if(WIN32)
        target_link_libraries(ps2spu2summarytest PRIVATE synchronization)
    endif()
    # an EE sceSifSetDma whose transfer list or packet is in the scratchpad (issues 96 and 101, no game needed): ninja ps2sifsprtest
    add_executable(ps2sifsprtest EXCLUDE_FROM_ALL tests/sif_spr_dma.cpp)
    target_include_directories(ps2sifsprtest PRIVATE runtime/src)
    target_link_libraries(ps2sifsprtest PRIVATE ps2runtime)
    if(WIN32)
        target_link_libraries(ps2sifsprtest PRIVATE synchronization)
    endif()
    # an IOP reset with no image (SifIopReset("", 0), Need for Speed) and the waits after it, no game needed: ninja ps2iopresettest
    add_executable(ps2iopresettest EXCLUDE_FROM_ALL tests/iop_reboot_empty.cpp)
    target_include_directories(ps2iopresettest PRIVATE runtime/src)
    target_link_libraries(ps2iopresettest PRIVATE ps2runtime)
    if(WIN32)
        target_link_libraries(ps2iopresettest PRIVATE synchronization)
    endif()
    # SMFLAG's SIFINIT, CMDINIT and BOOTEND while the IOP boots after a reset (public issue 61, no game needed): ninja ps2iopbootflagstest
    add_executable(ps2iopbootflagstest EXCLUDE_FROM_ALL tests/iop_boot_flags.cpp)
    target_include_directories(ps2iopbootflagstest PRIVATE runtime/src)
    target_link_libraries(ps2iopbootflagstest PRIVATE ps2runtime)
    if(WIN32)
        target_link_libraries(ps2iopbootflagstest PRIVATE synchronization)
    endif()
    # a SIF command from the EE that no IOP module handles (public issue 70, Madagascar's 0x80000004), no game needed: ninja ps2sifunhandledtest
    add_executable(ps2sifunhandledtest EXCLUDE_FROM_ALL tests/sif_unhandled_cmd.cpp)
    target_include_directories(ps2sifunhandledtest PRIVATE runtime/src)
    target_link_libraries(ps2sifunhandledtest PRIVATE ps2runtime)
    if(WIN32)
        target_link_libraries(ps2sifunhandledtest PRIVATE synchronization)
    endif()
    # an interrupt pending at EI (or MTC0 Status, or a system call's return) is taken at once (issue 100, GTA Vice City, no game needed): ninja ps2eipendingtest
    add_executable(ps2eipendingtest EXCLUDE_FROM_ALL tests/ei_pending.cpp)
    target_include_directories(ps2eipendingtest PRIVATE runtime/src)
    target_link_libraries(ps2eipendingtest PRIVATE ps2runtime)
    if(WIN32)
        target_link_libraries(ps2eipendingtest PRIVATE synchronization)
    endif()
    # a sound stream sent by auto-DMA moves the IOP DMA channel's address on as the chip takes it in (issue 100, GTA Vice City, no game needed): ninja ps2admaprogresstest
    add_executable(ps2admaprogresstest EXCLUDE_FROM_ALL tests/adma_progress.cpp)
    target_include_directories(ps2admaprogresstest PRIVATE runtime/src)
    target_link_libraries(ps2admaprogresstest PRIVATE ps2runtime)
    if(WIN32)
        target_link_libraries(ps2admaprogresstest PRIVATE synchronization)
    endif()
    # an IOP sceSifSetDma with SIF_DMA_INT_O raises the EE's SIF0 interrupt, channel 5 (public report 71, no game needed): ninja ps2sifdmatest
    add_executable(ps2sifdmatest EXCLUDE_FROM_ALL tests/sif_dma_intr.cpp)
    target_include_directories(ps2sifdmatest PRIVATE runtime/src)
    target_link_libraries(ps2sifdmatest PRIVATE ps2runtime)
    if(WIN32)
        target_link_libraries(ps2sifdmatest PRIVATE synchronization)
    endif()
    # the SIF log names the server of an EE call and the run's summary counts the calls per server (public report 71, no game needed): ninja ps2sifcalllogtest
    add_executable(ps2sifcalllogtest EXCLUDE_FROM_ALL tests/sif_call_log.cpp)
    target_include_directories(ps2sifcalllogtest PRIVATE runtime/src)
    target_link_libraries(ps2sifcalllogtest PRIVATE ps2runtime)
    if(WIN32)
        target_link_libraries(ps2sifcalllogtest PRIVATE synchronization)
    endif()
    # cdvdman's sceCdSC (#50) and sceCdRC (#51) answer like a drive with a disc (issue 57, no game needed): ninja ps2cdvdsctest
    add_executable(ps2cdvdsctest EXCLUDE_FROM_ALL tests/cdvd_sc_rc.cpp)
    target_include_directories(ps2cdvdsctest PRIVATE runtime/src)
    target_link_libraries(ps2cdvdsctest PRIVATE ps2runtime)
    if(WIN32)
        target_link_libraries(ps2cdvdsctest PRIVATE synchronization)
    endif()
    # a two-layer disc's plain search reads layer 0, cdvdman sceCdRM (#64), the i.LINK link chip with no cable (issues 57 and 91, no game needed): ninja ps2disclayerstest
    add_executable(ps2disclayerstest EXCLUDE_FROM_ALL tests/disc_layers_ilink.cpp)
    target_include_directories(ps2disclayerstest PRIVATE runtime/src)
    target_link_libraries(ps2disclayerstest PRIVATE ps2runtime)
    if(WIN32)
        target_link_libraries(ps2disclayerstest PRIVATE synchronization)
    endif()
    # the EE's FILEIO calls in the old and the newer packet layout, rom0:ROMVER (issue 78, no game needed): ninja ps2fileiolayoutstest
    add_executable(ps2fileiolayoutstest EXCLUDE_FROM_ALL tests/fileio_layouts.cpp)
    target_include_directories(ps2fileiolayoutstest PRIVATE runtime/src)
    target_link_libraries(ps2fileiolayoutstest PRIVATE ps2runtime)
    if(WIN32)
        target_link_libraries(ps2fileiolayoutstest PRIVATE synchronization)
    endif()
    # LOADFILE call 1 (sceSifLoadElf) copies an EE program from the disc into the EE's memory (issue 98, no game needed): ninja ps2loadfileelftest
    add_executable(ps2loadfileelftest EXCLUDE_FROM_ALL tests/loadfile_elf.cpp)
    target_include_directories(ps2loadfileelftest PRIVATE runtime/src)
    target_link_libraries(ps2loadfileelftest PRIVATE ps2runtime)
    if(WIN32)
        target_link_libraries(ps2loadfileelftest PRIVATE synchronization)
    endif()
    # the stream command sceCdStStat answers the sectors in the stream buffer (issue 44, no game needed): ninja ps2cdvdstreamstattest
    add_executable(ps2cdvdstreamstattest EXCLUDE_FROM_ALL tests/cdvd_stream_stat.cpp)
    target_include_directories(ps2cdvdstreamstattest PRIVATE runtime/src)
    target_link_libraries(ps2cdvdstreamstattest PRIVATE ps2runtime)
    if(WIN32)
        target_link_libraries(ps2cdvdstreamstattest PRIVATE synchronization)
    endif()
    # ioman AddDrv and DelDrv call the driver's init and deinit and keep a table of its devices (no game needed): ninja ps2iomandrvtest
    add_executable(ps2iomandrvtest EXCLUDE_FROM_ALL tests/ioman_drivers.cpp)
    target_include_directories(ps2iomandrvtest PRIVATE runtime/src)
    target_link_libraries(ps2iomandrvtest PRIVATE ps2runtime)
    if(WIN32)
        target_link_libraries(ps2iomandrvtest PRIVATE synchronization)
    endif()
    # an UNPACK with CL 0 (or before any STCYCL) takes its data and the stream stays in step, no game needed: ninja ps2viftest
    add_executable(ps2viftest EXCLUDE_FROM_ALL tests/vif_cycle.cpp)
    target_include_directories(ps2viftest PRIVATE runtime/src)
    target_link_libraries(ps2viftest PRIVATE ps2runtime)
    if(WIN32)
        target_link_libraries(ps2viftest PRIVATE synchronization)
    endif()
    # every UNPACK form under every STMOD, mask and write cycle equals the lane-by-lane rules, no game needed: ninja ps2vifunpacktest
    add_executable(ps2vifunpacktest EXCLUDE_FROM_ALL tests/vif_unpack_modes.cpp)
    target_include_directories(ps2vifunpacktest PRIVATE runtime/src)
    target_link_libraries(ps2vifunpacktest PRIVATE ps2runtime)
    if(WIN32)
        target_link_libraries(ps2vifunpacktest PRIVATE synchronization)
    endif()
    # an XGKICK packet without an end does not swallow the next kick's packet (public issue 69), no game needed: ninja ps2gifrunawaytest
    add_executable(ps2gifrunawaytest EXCLUDE_FROM_ALL tests/gif_runaway.cpp)
    target_include_directories(ps2gifrunawaytest PRIVATE runtime/src)
    target_link_libraries(ps2gifrunawaytest PRIVATE ps2runtime)
    if(WIN32)
        target_link_libraries(ps2gifrunawaytest PRIVATE synchronization)
    endif()
    # VIF1 holds a program started by MSCAL until the UNPACKs behind it are done, and PS2_VIF_DEFER=0 starts it at the MSCAL (public issue 69), no game
    # needed: ninja ps2vifdefertest, run it twice (once with the argument off)
    add_executable(ps2vifdefertest EXCLUDE_FROM_ALL tests/vif_defer.cpp)
    target_include_directories(ps2vifdefertest PRIVATE runtime/src)
    target_link_libraries(ps2vifdefertest PRIVATE ps2runtime)
    if(WIN32)
        target_link_libraries(ps2vifdefertest PRIVATE synchronization)
    endif()
endif()

if(PS2_GENERATED)
    file(GLOB PS2_UNITS CONFIGURE_DEPENDS "${PS2_GENERATED}/*.cpp")
    add_library(ps2game STATIC ${PS2_UNITS})
    # a unit that crashed the compiler is built with lower optimisation (scripts/build_units.sh); no list, no change
    include("${CMAKE_SOURCE_DIR}/cmake/lowopt.cmake")
    ps2_lowopt_apply(ps2game)
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

    add_executable(ps2run host/main.cpp host/crash.cpp)  # crash.cpp: the lines a crash writes to the log
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
        # The main thread of a headless run (and the window thread) get 1 MB on Windows, 8 MB on Linux; the emulation thread's own
        # size is set in host/big_stack_thread.hpp. Untested on Windows (no Windows build on the Linux PC): the llvm-mingw linker
        # (lld) takes --stack,<bytes> as the stack reserve of the executable.
        # PS2_WIN_SMALL_STACK=ON (scripts/wine-bench.sh build --small-stack) builds the old way, with Windows' default 1 MB stacks, to
        # check that the test bench still sees the stack overflow of public issue 42 (docs/wine-bench.md).
        option(PS2_WIN_SMALL_STACK "Windows: no big stacks (the behaviour before lane boot-stack), only for the test bench" OFF)
        if(PS2_WIN_SMALL_STACK)
            target_compile_definitions(ps2run PRIVATE PS2_SMALL_STACK)
        else()
            target_link_options(ps2run PRIVATE -Wl,--stack,16777216)
        endif()
        list(APPEND CMAKE_PREFIX_PATH "${CMAKE_SOURCE_DIR}/third_party/sdl2")
        set(SDL2_NO_MWINDOWS ON)
    endif()
    find_package(SDL2 QUIET)
    if(SDL2_FOUND AND NOT EMSCRIPTEN)
        target_sources(ps2run PRIVATE host/window.cpp host/keymap.cpp host/quickmenu.cpp host/stretch.cpp)
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
        add_executable(ps2padtest EXCLUDE_FROM_ALL host/pad_test.cpp host/window.cpp host/keymap.cpp host/quickmenu.cpp host/stretch.cpp)
        target_include_directories(ps2padtest PRIVATE runtime/src)
        target_link_libraries(ps2padtest PRIVATE ps2runtime SDL2::SDL2)
    endif()
endif()
