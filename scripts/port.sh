#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
here="$ROOT/scripts"

usage() {
  cat <<'EOF'
usage: scripts/port.sh <name> <game.iso | game.zip | game.7z> [options]

Turns a PS2 disc image you own into a browser build in build/web-<name> and a
native one in build/native-<name>. It reads the boot program named in
SYSTEM.CNF, translates its machine code to C++, runs it headless to find code
the static pass missed (adding those entry points and translating again),
compiles the vector unit programs those runs recorded and builds both.

options:
  --frames N         frames per exploring run (default 3600)
  --press F:BUTTON   input for the exploring runs, repeatable; the default
                     presses start at frame 300 and cross every 300 frames
  --rounds N         exploring runs at most (default 8)
  --no-explore       translate and build without running the game
  --native-only      skip the browser build and the Emscripten SDK it needs
  --pnach FILE       apply a PCSX2 patch file, repeatable; patches that land in
                     the executable are translated with it, the rest are
                     written into memory at boot or every frame
  --pnach-section S  also apply the patches under [S] in those files, the way
                     enabling them in PCSX2 does; lines outside any section
                     always apply
  --widescreen       tell the game the TV is 16:9 and show the picture that
                     shape; the exploring runs and the line to play with get it
                     too, and a patch section made for a 16:9 screen turns it on
EOF
}

[ $# -ge 2 ] || { usage; exit 2; }
name="$1"; source_path="$2"; shift 2
frames=3600; rounds=8; explore=1; web=1; presses=(); pnach=(); wide=()
while [ $# -gt 0 ]; do
  case "$1" in
    --frames) frames="$2"; shift ;;
    --press) presses+=(--press "$2"); shift ;;
    --rounds) rounds="$2"; shift ;;
    --no-explore) explore=0 ;;
    --native-only) web=0 ;;
    --pnach) [ -f "$2" ] || { echo "$2: no such file" >&2; exit 1; }; pnach+=("$(cd "$(dirname "$2")" && pwd)/$(basename "$2")"); shift ;;
    --pnach-section) pnach=(--section "$2" "${pnach[@]}"); shift ;;
    --widescreen) wide=(--widescreen) ;;
    -h|--help) usage; exit 0 ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
  esac
  shift
done
if [ ${#presses[@]} -eq 0 ]; then
  presses=(--press 300:start)
  for ((f = 600; f < frames; f += 300)); do presses+=(--press "$f:cross"); done
fi

py=()
for candidate in python3 python "py -3"; do
  if $candidate -c 'import sys; sys.exit(sys.version_info < (3,))' > /dev/null 2>&1; then py=($candidate); break; fi
done
[ ${#py[@]} -gt 0 ] || { echo "no Python 3 found: install one and put it on the PATH" >&2; exit 1; }
pyname="${py[*]}"
py+=(-X utf8)  # print paths with non-ASCII letters (the scripts run with -I, which ignores PYTHONUTF8)

windows=0
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) windows=1 ;; esac
game="$ROOT/games/$name"
generated="$ROOT/generated/$name"
native="$ROOT/build/native-$name"
tools="$ROOT/build/tools"
export EMSDK="${EMSDK:-$ROOT/tools/emsdk}"
[ -e "$source_path" ] || [ -e "$game/disc.iso" ] || { echo "$source_path: no such file" >&2; exit 1; }
setup=()
[ "$web" = 1 ] || setup=(--native-only)
if { [ "$web" = 1 ] && [ ! -f "$EMSDK/emsdk_env.sh" ]; } || [ ! -x "$tools/eerecomp" ] || [ ! -x "$tools/vurecomp" ]; then "$here/setup.sh" "${setup[@]}"; fi
mkdir -p "$game/disc" "$game/vu1" "$game/vu0" "$game/shots" "$generated"

echo "== [1/6] disc"
iso="$game/disc.iso"
if [ -e "$iso" ]; then
  echo "already in place"
else
  case "${source_path,,}" in
    *.zip)
      "${py[@]}" -I - "$source_path" "$iso" <<'EOF'
import shutil, sys, zipfile
with zipfile.ZipFile(sys.argv[1]) as archive:
    images = [m for m in archive.infolist() if m.filename.lower().endswith('.iso')]
    if not images:
        sys.exit('no .iso inside the archive')
    with archive.open(max(images, key=lambda m: m.file_size)) as src, open(sys.argv[2], 'wb') as dst:
        shutil.copyfileobj(src, dst, 1 << 24)
EOF
      ;;
    *.7z)
      sevenzip=7z
      [ "$windows" = 0 ] || command -v 7z > /dev/null || sevenzip="${PROGRAMFILES:-/c/Program Files}/7-Zip/7z.exe"
      command -v "$sevenzip" > /dev/null || { echo "7z not found: install 7-Zip" >&2; exit 1; }
      "$sevenzip" e -so -r "$source_path" '*.iso' > "$iso" ;;
    *)
      # No realpath in MinGit or old macOS. On Windows pwd -W gives D:/...: Git's shell leaves /d/... as it is for native
      # programs (ln is BusyBox's, Python, ps2run) when the name has [ ] ' * or ? in it, as in "Game [NTSC].iso"
      if [ "$windows" = 0 ]; then dir=$(cd "$(dirname "$source_path")" && pwd); else dir=$(cd "$(dirname "$source_path")" && pwd -W); fi
      image="$dir/$(basename "$source_path")"
      if [ "$windows" = 0 ]; then ln -s "$image" "$iso"; else ln "$image" "$iso" 2> /dev/null || iso=$image; fi ;;
  esac
fi
"${py[@]}" -I "$here/disc.py" extract "$iso" "$game/disc" SYSTEM.CNF > /dev/null
boot=$(sed -n 's/^BOOT2 *= *//p' "$game/disc/SYSTEM.CNF" | tr -d '\r')
program=${boot#*\\}
program=${program%;*}
[ -n "$program" ] || { echo "SYSTEM.CNF names no BOOT2 program" >&2; exit 1; }
"${py[@]}" -I "$here/disc.py" extract "$iso" "$game/disc" "$program" > /dev/null
echo "boots $boot"
crc=$("${py[@]}" -I "$here/pnach.py" crc "$game/disc/$program")
echo "PCSX2 patch files for it end in _$crc.pnach"

echo "== [2/6] executable"
elf="$game/disc/$program"
if [ -f "$game/core.elf" ]; then
  elf="$game/core.elf"
elif "${py[@]}" -I "$here/disc.py" list "$iso" | awk '{print $4}' | grep -qx CORE.GT4; then
  "${py[@]}" -I "$here/disc.py" extract "$iso" "$game/disc" CORE.GT4 > /dev/null
  "${py[@]}" -I "$here/gt4_core.py" "$game/disc/CORE.GT4" "$game/core.elf" > /dev/null
  elf="$game/core.elf"
fi
rm -rf "$game/patched" "$game/patches.txt" "$game/toggles.txt"
patches=()
if [ ${#pnach[@]} -gt 0 ]; then
  mkdir -p "$game/patched"
  applied=$("${py[@]}" -I "$here/pnach.py" apply "$elf" "$game/patched/$(basename "$elf")" "$game/patches.txt" --crc "$crc" "${pnach[@]}")
  echo "$applied"
  if grep -q '^applied .*meant for a 16:9 screen' <<< "$applied"; then wide=(--widescreen); fi
  elf="$game/patched/$(basename "$elf")"
  patches=(--patches "$game/patches.txt")
fi
echo "${elf#$ROOT/}"
toggles=()
[ ! -f "$game/toggles.txt" ] || toggles=(--toggles "$game/toggles.txt")  # instructions a patch switches at run time (pnach.py)
modules=()
if grep -q SNR2 "$elf" || grep -qa xff2 "$elf"; then
  "${py[@]}" -I "$here/disc.py" modules "$iso" "$game/modules"
  modules=(--modules "$game/modules")
fi
overlays=()
if windows=$("${py[@]}" -I "$here/windows.py" list "$elf") && [ -n "$windows" ]; then
  mkdir -p "$game/overlays"
  overlays=(--overlays "$game/overlays")
  echo "code windows the game rewrites: $(echo "$windows" | tr '\n' ' ')"
  # a folder from before the limits (or from a long run) may hold hundreds of images: keep the newest few (windows.py)
  "${py[@]}" -I "$here/windows.py" trim "$game/overlays" "$game/entries.txt" || true
fi

translate() { "$tools/eerecomp" "$elf" --entries "$game/entries.txt" "${modules[@]}" "${overlays[@]}" "${toggles[@]}" --out "$generated" | tail -1; }

echo "== [3/6] machine code to C++"
touch "$game/entries.txt"
translate

ninja_jobs=
if [ -n "${PS2_JOBS:-}" ]; then
  case "$PS2_JOBS" in ''|*[!0-9]*|0*) echo "PS2_JOBS=$PS2_JOBS: expected a positive number" >&2; exit 2 ;; esac
  ninja_jobs="-j$PS2_JOBS"
fi
. "$here/runtime.sh"

build_native() {
  . "$here/cxx.sh"
  shared_runtime
  config="${cxx_flags[*]} ${runtime_lib:-own runtime}"
  if [ ! -f "$native/build.ninja" ] || [ "$(cat "$native/.port-config" 2> /dev/null)" != "$config" ]; then
    cmake -S "$ROOT" -B "$native" -G Ninja -DCMAKE_BUILD_TYPE=$build_type "${cxx_flags[@]}" -DPS2_GENERATED="$generated" -DPS2_RUNTIME_LIB="$runtime_lib" > /dev/null
    echo "$config" > "$native/.port-config"
  fi
  ninja -C "$native" $ninja_jobs ps2run > "$native/ninja.log" 2>&1 || { tail -30 "$native/ninja.log"; exit 1; }
}

echo "== [4/6] exploring"
if [ "$explore" = 1 ]; then
  for ((round = 1; round <= rounds; round++)); do
    build_native
    rm -f "$game/misses.txt" "$game/explore.ps2" "$game/explore_ram.bin"
    code=0
    PS2_DUMP="$game/explore_ram.bin" PS2_GPU_LOW_POWER=1 PS2_VU0_RECORD="$game/vu0" PS2_VU_RECORD="$game/vu1" "$native/ps2run" "$elf" --disc "$iso" --misses "$game/misses.txt" --memcard "$game/explore.ps2" \
      "${wide[@]}" "${patches[@]}" --frames "$frames" "${presses[@]}" --frames-dir "$game/shots" --frame-every 300 > "$game/explore.log" 2>&1 || code=$?
    [ "$code" = 0 ] || echo "[port] ps2run exited with code $code" | tee -a "$game/explore.log"
    captured=0
    if [ ${#overlays[@]} -gt 0 ] && [ -f "$game/explore_ram.bin" ]; then
      captured=$("${py[@]}" -I "$here/windows.py" capture "$elf" "$game/explore_ram.bin" "$game/overlays" "$game/misses.txt" | grep -c '^captured' || true)
    fi
    reason=$(sed -n 's/^\[ee\] halted: //p' "$game/explore.log" | head -1)
    added=$("${py[@]}" -I - "$game/entries.txt" "$game/misses.txt" <<'EOF'
import os, sys
sys.stdout.reconfigure(newline='\n')
entries, misses = sys.argv[1], sys.argv[2]
def token(t):
    name, _, at = t.rpartition(':')
    return (name + ':' if name else '') + '%x' % int(at, 16)
known = {token(t) for t in open(entries).read().split()}
found = [token(t) for t in open(misses).read().split()] if os.path.exists(misses) else []
new = sorted({t for t in found if t not in known})
with open(entries, 'a') as out:
    for t in new:
        out.write(t + '\n')
print(len(new))
EOF
)
    echo "round $round: ${reason:-ran to the end}, $added new entry points, $captured new window images"
    [ "$added" -gt 0 ] || [ "$captured" -gt 0 ] || break
    translate
  done
  echo "frames of the last run are in games/$name/shots, its log in games/$name/explore.log"
else
  echo "skipped"
fi

echo "== [5/6] vector unit programs"
if compgen -G "$game/vu1/*.bin" > /dev/null || compgen -G "$game/vu0/*.bin" > /dev/null; then
  "$tools/vurecomp" "$game/vu1" "$generated" --vu0-dir "$game/vu0" | tail -1
else
  # vurecomp replaces the old files itself; with nothing recorded it never runs, so programs of an earlier run would stay in the build.
  rm -f "$generated"/vu1_*.cpp
  echo "none recorded, the native runner interprets them and the page compiles them on the fly"
fi

echo "== [6/6] builds"
build_native
if [ "$web" = 1 ]; then
  "$here/build_web.sh" "$name" "$EMSDK"
fi
echo
if [ "$web" = 1 ]; then
  printf 'Done. Serve it with:  %s scripts/serve.py %q %q\n' "$pyname" "build/web-$name" "games/$name/disc.iso"
  echo "and open http://localhost:8620/"
fi
play=$(printf '%q %q --disc %q --window' "build/native-$name/ps2run" "${elf#$ROOT/}" "${iso#$ROOT/}")
[ ${#wide[@]} -eq 0 ] || play="$play --widescreen"
[ ${#patches[@]} -eq 0 ] || play="$play --patches $(printf '%q' "games/$name/patches.txt")"
echo "Play it natively:  $play"
