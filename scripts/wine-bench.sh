#!/usr/bin/env bash
# The Windows test bench for Linux: cross-compiles the Windows ps2run.exe of a game with a Linux-hosted llvm-mingw and runs it
# under Wine, the way a Windows tester runs it (docs/wine-bench.md). Nothing is installed system-wide: the tools go into the
# git-ignored tools/wine-bench/, the Wine prefix is its own (default ~/.cache/ps2-wine-bench, never ~/.wine).
#
#   scripts/wine-bench.sh setup                  download the tools (SHA-256 checked), make the Wine prefix
#   scripts/wine-bench.sh build <game> [--small-stack]
#                                                translate nothing, build build/wine-<game>/ps2run.exe from generated/<game>
#                                                (port.sh <game> <iso> --native-only makes generated/<game> and games/<game> first).
#                                                --small-stack builds without the 64 MB emulation thread and without
#                                                -Wl,--stack (the old behaviour) in build/wine-<game>-small/
#   scripts/wine-bench.sh run <game> [options] [-- ps2run arguments]
#       --window            a real window (as a tester plays); without it the run is headless
#       --cold              empty ee-live and vu1-live before the run (a first start); without it the cache of the last run stays
#       --small-stack       run the build made with --small-stack
#       --gpu vulkan|dx12   PS2_GPU_BACKEND (default: the program's own order, Direct3D 12 first)
#       --timeout SECONDS   stop the run after that long (default 600)
#       --label NAME        log file name tag (default: a time stamp)
#       --threads           headless run with the three threads of a window run (PS2_THREADS=1)
#     The ps2run arguments after -- are added to the standard ones (`<elf> --disc <iso> --memcard <card>`), e.g.
#     `-- --frames 900 --press 360:start`. The log is tools/wine-bench/run/<game>/logs/<label>.log, its last line is
#     "[bench] exit code N" (Windows codes: 0xC00000FD = stack overflow).
#
# Test aids passed through to ps2run.exe: PS2_STACK_REPORT=1 (stack use at the end, meaningless under Wine: it commits whole stacks),
# PS2_STACK_EAT_KB=<n> (the emulation thread starts with n KB of its stack used: shows what a deeper game does with a 1 MB stack).
# Environment: PS2_BENCH_PREFIX (Wine prefix), PS2_BENCH_WS (Hyprland workspace the window of --window is moved to, default 9; no
# hyprctl means no move), PS2_JOBS (build jobs, default 3). Never kills processes by name: the wine process this script started is
# stopped by its process ID, and `wineserver -k` is only ever run for the bench's own prefix.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BENCH="$ROOT/tools/wine-bench"
PREFIX="${PS2_BENCH_PREFIX:-$HOME/.cache/ps2-wine-bench}"
MINGW_LINUX="$BENCH/linux-llvm-mingw"
MINGW_WIN="$BENCH/win-llvm-mingw"
WGPU_WIN="$BENCH/wgpu-win"
SDL2_WIN="$BENCH/sdl2-win"

# Pinned like scripts/portable.ps1 (llvm-mingw), scripts/fetch_wgpu.sh and scripts/fetch_sdl2.sh; the Windows files are the
# same downloads those pick on Windows, the Linux llvm-mingw is the same release for the host.
LLVM=20260908
SHA_LLVM_LINUX=2258c745e3155870c80793f3e8c80b28fbde11b9ff73c4c78783635b3440b092
SHA_LLVM_WIN=1bcf74d06b724aeecaa6412ca85f5b26fb1da770e7cdcefa9263c9c5c3ad34b6
WGPU=v29.0.1.1
SHA_WGPU_WIN=d471e3614733c1d4ddd61bfd19868356477d0d37bf531bf8c6cb64a7f579bd2a
SDL2=2.32.10
SHA_SDL2_WIN=f15cff5fca62ec9381a016ef1d42a95c638cd72d2f226ba5781c76fe43dbd1ac
DXC_VERSION=v1.9.2609  # scripts/fetch_dxc.sh: Microsoft's shader compiler, which the Direct3D 12 path of a tester's PC uses
DXC_FILE=dxc_2026_09_29.zip
SHA_DXC=ad31b1fc8443175d204f77a611fdb3ef2ec42759bdc2f1167368de24a4a7e7f1

die() { echo "wine-bench: $*" >&2; exit 1; }
need() { command -v "$1" > /dev/null || die "$1 is needed (install it)"; }

fetch() {  # fetch <url> <sha256> <file in dl/>
  local url="$1" sha="$2" file="$BENCH/dl/$3"
  mkdir -p "$BENCH/dl"
  if [ -f "$file" ] && [ "$(sha256sum "$file" | cut -d' ' -f1)" = "$sha" ]; then return 0; fi
  echo "downloading $3"
  curl -fL -sS -o "$file.part" "$url" || { rm -f "$file.part"; die "download failed: $url"; }
  mv "$file.part" "$file"
  [ "$(sha256sum "$file" | cut -d' ' -f1)" = "$sha" ] || { rm -f "$file"; die "wrong SHA-256 for $3"; }
}

wine_env() {
  export WINEPREFIX="$PREFIX" WINEARCH=win64 WINEDEBUG="${WINEDEBUG:--all}" WINEDLLOVERRIDES="mscoree,mshtml="
  export SDL_AUDIODRIVER="${SDL_AUDIODRIVER:-dummy}"
}

cmd_setup() {
  need curl; need wine; need cmake; need ninja; need unzip; need tar
  fetch "https://github.com/mstorsjo/llvm-mingw/releases/download/$LLVM/llvm-mingw-$LLVM-ucrt-ubuntu-22.04-x86_64.tar.xz" "$SHA_LLVM_LINUX" llvm-mingw-linux.tar.xz
  fetch "https://github.com/mstorsjo/llvm-mingw/releases/download/$LLVM/llvm-mingw-$LLVM-ucrt-x86_64.zip" "$SHA_LLVM_WIN" llvm-mingw-win.zip
  fetch "https://github.com/gfx-rs/wgpu-native/releases/download/$WGPU/wgpu-windows-x86_64-gnu-release.zip" "$SHA_WGPU_WIN" wgpu-win.zip
  fetch "https://github.com/libsdl-org/SDL/releases/download/release-$SDL2/SDL2-devel-$SDL2-mingw.zip" "$SHA_SDL2_WIN" sdl2-mingw.zip
  fetch "https://github.com/microsoft/DirectXShaderCompiler/releases/download/$DXC_VERSION/$DXC_FILE" "$SHA_DXC" dxc.zip
  if [ ! -f "$BENCH/dxc/dxil.dll" ]; then  # the zip stores its paths with backslashes: unzip warns and turns them into folders
    rm -rf "$BENCH/dxc" "$BENCH/dxc.tmp"; mkdir -p "$BENCH/dxc" "$BENCH/dxc.tmp"
    (cd "$BENCH/dxc.tmp" && unzip -q -o ../dl/dxc.zip 2> /dev/null || true)
    cp "$BENCH/dxc.tmp/bin/x64/dxcompiler.dll" "$BENCH/dxc.tmp/bin/x64/dxil.dll" "$BENCH/dxc/"; rm -rf "$BENCH/dxc.tmp"
  fi
  if [ ! -x "$MINGW_LINUX/bin/x86_64-w64-mingw32-clang++" ]; then
    rm -rf "$MINGW_LINUX"; mkdir -p "$MINGW_LINUX"
    tar xJf "$BENCH/dl/llvm-mingw-linux.tar.xz" -C "$MINGW_LINUX" --strip-components=1
  fi
  if [ ! -f "$MINGW_WIN/bin/clang++.exe" ]; then  # the compiler the Windows runner calls under Wine for its live translations
    rm -rf "$MINGW_WIN" "$BENCH/win.tmp"; mkdir -p "$BENCH/win.tmp"
    unzip -q "$BENCH/dl/llvm-mingw-win.zip" -d "$BENCH/win.tmp"
    mv "$BENCH/win.tmp"/*/ "$MINGW_WIN"; rmdir "$BENCH/win.tmp"
  fi
  if [ ! -f "$WGPU_WIN/lib/libwgpu_native.a" ]; then
    rm -rf "$WGPU_WIN"; mkdir -p "$WGPU_WIN"; unzip -q "$BENCH/dl/wgpu-win.zip" -d "$WGPU_WIN"
  fi
  if [ ! -f "$SDL2_WIN/bin/SDL2.dll" ]; then
    rm -rf "$SDL2_WIN" "$BENCH/sdl2.tmp"; mkdir -p "$SDL2_WIN" "$BENCH/sdl2.tmp"
    unzip -q "$BENCH/dl/sdl2-mingw.zip" -d "$BENCH/sdl2.tmp"
    cp -R "$BENCH/sdl2.tmp/SDL2-$SDL2/x86_64-w64-mingw32/." "$SDL2_WIN"; rm -rf "$BENCH/sdl2.tmp"
  fi
  wine_env
  if [ ! -f "$PREFIX/system.reg" ]; then echo "making the Wine prefix $PREFIX"; wineboot -i > /dev/null 2>&1 || true; fi
  # A crash of ps2run.exe ends the run (exit code 253 for a stack overflow, 0xC00000FD cut to 8 bits) instead of waiting for Wine's debugger.
  wine reg add 'HKLM\Software\Microsoft\Windows NT\CurrentVersion\AeDebug' /v Debugger /t REG_SZ /d "" /f > /dev/null 2>&1 || true
  wine reg add 'HKLM\Software\Microsoft\Windows NT\CurrentVersion\AeDebug' /v Auto /t REG_SZ /d 1 /f > /dev/null 2>&1 || true
  wine --version | sed 's/^/wine: /'
  echo "wine-bench: ready in ${BENCH#$ROOT/}"
}

check_setup() { [ -x "$MINGW_LINUX/bin/x86_64-w64-mingw32-clang++" ] && [ -f "$WGPU_WIN/lib/libwgpu_native.a" ] && [ -f "$SDL2_WIN/bin/SDL2.dll" ] && [ -f "$BENCH/dxc/dxil.dll" ] || die "run scripts/wine-bench.sh setup first"; }

builddir() { if [ "${2:-}" = small ]; then echo "$ROOT/build/wine-$1-small"; else echo "$ROOT/build/wine-$1"; fi; }

cmd_build() {
  local game="${1:-}" small=
  [ -n "$game" ] || die "build: which game? (scripts/wine-bench.sh build tekken5)"
  shift
  for a in "$@"; do case "$a" in --small-stack) small=small ;; *) die "build: unknown option $a" ;; esac; done
  check_setup
  [ -d "$ROOT/generated/$game" ] || die "no generated/$game: run bash scripts/port.sh $game <iso> --native-only first"
  local dir; dir="$(builddir "$game" "$small")"
  local flags="-ffp-contract=off"
  # the same AVX2 rule as scripts/cxx.sh: on when the building PC has it (the Windows tester's PC builds for itself)
  if "$MINGW_LINUX/bin/x86_64-w64-mingw32-clang++" -march=native -dM -E -x c++ - < /dev/null 2> /dev/null | grep -q '^#define __AVX2__ 1'; then flags="-mavx2 $flags"; fi
  if [ ! -f "$dir/build.ninja" ] || [ "$(cat "$dir/.bench-flags" 2> /dev/null)" != "$flags" ]; then
    cmake -S "$ROOT" -B "$dir" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/mingw-cross.cmake" -DPS2_MINGW_ROOT="$MINGW_LINUX" \
      -DCMAKE_BUILD_TYPE=Release "-DCMAKE_CXX_FLAGS=$flags" -DPS2_GENERATED="$ROOT/generated/$game" -DPS2_WGPU_DIR="$WGPU_WIN" \
      -DCMAKE_PREFIX_PATH="$SDL2_WIN" "-DPS2_WIN_SMALL_STACK=$([ -n "$small" ] && echo ON || echo OFF)" > "$dir.cmake.log" 2>&1 || { tail -20 "$dir.cmake.log"; die "cmake failed"; }
    echo "$flags" > "$dir/.bench-flags"
  fi
  ninja -C "$dir" -j"${PS2_JOBS:-3}" ps2run > "$dir.ninja.log" 2>&1 || { tail -30 "$dir.ninja.log"; die "the build failed (log: ${dir#$ROOT/}.ninja.log)"; }
  ls -la "$dir/ps2run.exe" | awk '{print "built " $NF " (" $5 " bytes)"}'
}

winpath() { printf 'Z:%s' "$(printf '%s' "$1" | sed 's|/|\\|g')"; }

cmd_run() {
  local game="${1:-}" window= cold= small= gpu= timeout=600 label= threads= extra=()
  [ -n "$game" ] || die "run: which game?"
  shift
  while [ $# -gt 0 ]; do
    case "$1" in
      --window) window=1 ;;
      --cold) cold=1 ;;
      --small-stack) small=small ;;
      --threads) threads=1 ;;
      --gpu) gpu="$2"; shift ;;
      --timeout) timeout="$2"; shift ;;
      --label) label="$2"; shift ;;
      --) shift; extra=("$@"); break ;;
      *) die "run: unknown option $1 (ps2run arguments go after --)" ;;
    esac
    shift
  done
  check_setup; need wine
  local dir; dir="$(builddir "$game" "$small")"
  [ -f "$dir/ps2run.exe" ] || die "no ${dir#$ROOT/}/ps2run.exe: run scripts/wine-bench.sh build $game${small:+ --small-stack}"
  local gdir="$ROOT/games/$game"
  [ -d "$gdir/disc" ] || die "no games/$game/disc: run bash scripts/port.sh $game <iso> --native-only first"
  # The run folder looks like a tester's install: ps2run.exe and SDL2.dll next to a compiler\ folder (the Windows llvm-mingw),
  # and games\<game>\ with disc\, disc.iso, entries.txt, overlays\ and, after a run, ee-live\ and the memory card.
  local run="$BENCH/run/$game${small:+-small}" g
  g="$run/games/$game"
  mkdir -p "$run/logs" "$g"
  cp -f "$dir/ps2run.exe" "$run/"
  cp -f "$SDL2_WIN/bin/SDL2.dll" "$run/"
  # ps2run.exe imports this DLL (winpthreads); on a tester's PC it is found through the PATH of the llvm-mingw it was built with
  cp -f "$MINGW_WIN/x86_64-w64-mingw32/bin/libwinpthread-1.dll" "$run/"
  # DXC next to the program, like the real build's post-build step: Direct3D 12 then compiles its shaders with DXC (Wine's own
  # d3dcompiler cannot do the shader model they need). Without them wgpu falls back to FXC, which fails under Wine.
  cp -f "$BENCH/dxc/dxcompiler.dll" "$BENCH/dxc/dxil.dll" "$run/" 2> /dev/null || true
  ln -sfn "$MINGW_WIN" "$run/compiler"
  ln -sfn "$gdir/disc" "$g/disc"
  ln -sfn "$(readlink -f "$gdir/disc.iso")" "$g/disc.iso"
  if [ ! -f "$g/entries.txt" ]; then cp -f "$gdir/entries.txt" "$g/entries.txt" 2> /dev/null || : > "$g/entries.txt"; fi
  if [ ! -d "$g/overlays" ]; then cp -r "$gdir/overlays" "$g/overlays" 2> /dev/null || mkdir -p "$g/overlays"; fi
  mkdir -p "$g/vu1"
  if [ -n "$cold" ]; then rm -rf "$g/ee-live" "$g/vu1-live"; fi
  local elf; elf=$(ls "$gdir/disc" | grep -v -i 'SYSTEM.CNF' | head -1)
  [ -n "$elf" ] || die "games/$game/disc has no program"
  [ -n "$label" ] || label=$(date +%Y-%m-%d_%H-%M-%S)
  local log="$run/logs/$label.log"
  local args=("$(winpath "$g/disc/$elf")" --disc "$(winpath "$g/disc.iso")" --memcard "$(winpath "$g/$game.ps2")")
  [ -z "$window" ] || args+=(--window)
  args+=("${extra[@]}")
  wine_env
  export PS2_GPU_BACKEND="${gpu:-${PS2_GPU_BACKEND:-}}"
  [ -n "$PS2_GPU_BACKEND" ] || unset PS2_GPU_BACKEND
  if [ -n "$threads" ]; then export PS2_THREADS=1; fi
  if [ -z "$window" ]; then
    # headless: the program starts no window, the live translation is off unless asked (a window run has it on)
    export PS2_EE_LIVE="${PS2_EE_LIVE:-1}"
  fi
  echo "[bench] ${args[*]}" > "$log"
  echo "[bench] prefix $PREFIX, wine $(wine --version), gpu ${PS2_GPU_BACKEND:-default}, ${cold:+cold start, }${window:+window}${window:-headless}" >> "$log"
  local code=0 pid
  ( cd "$run" && exec timeout -k 5 "$timeout" wine ps2run.exe "${args[@]}" >> "$log" 2>&1 ) &
  pid=$!
  if [ -n "$window" ] && command -v hyprctl > /dev/null; then move_window "${PS2_BENCH_WS:-9}" "$pid" & fi
  wait "$pid" || code=$?
  wait 2> /dev/null || true
  echo "[bench] exit code $code" | tee -a "$log"
  # only the bench's own prefix: stops the wine servers this prefix started (never another prefix, never by name)
  WINEPREFIX="$PREFIX" wineserver -k 2> /dev/null || true
  echo "log: ${log#$ROOT/}"
  return 0
}

# Moves the first new ps2run window to a workspace that is not shown (Hyprland, Lua config: hl.dsp). A window shows up within
# a few seconds of the start; the loop gives up after 60 s.
move_window() {
  local ws="$1" pid="$2" i addr
  for ((i = 0; i < 120; i++)); do
    kill -0 "$pid" 2> /dev/null || return 0
    addr=$(hyprctl clients -j 2> /dev/null | python3 -I -c "
import json,sys
try: c=json.load(sys.stdin)
except Exception: c=[]
for w in c:
    if 'ps2run' in (w.get('class','')+w.get('title','')).lower() and w.get('workspace',{}).get('id')!=$ws: print(w['address']); break
" 2> /dev/null || true)
    if [ -n "$addr" ]; then
      hyprctl dispatch "hl.dsp.window.move({ workspace = $ws, follow = false, window = \"address:$addr\" })" > /dev/null 2>&1 || true
      return 0
    fi
    sleep 0.5
  done
}

case "${1:-}" in
  setup) cmd_setup ;;
  build) shift; cmd_build "$@" ;;
  run) shift; cmd_run "$@" ;;
  *) sed -n '2,/^set -euo/p' "$0" | sed '$d' | sed 's/^# \{0,1\}//' ; exit 2 ;;
esac
