#!/usr/bin/env bash
# Builds the program a single-file pack of a game needs: the game's runner linked fully static (no SDL2.dll or other DLL beside it),
# build/pack-<name>/ps2run.exe. It is the same runner as build/native-<name> (same flags, same generated code, same shared runtime
# library) configured with -DPS2_WIN_STATIC=ON in a folder of its own, so the normal build and Play are never touched.
# The game has to be built first (generated/<name> is made by port.sh). Run it again after a rebuild: ninja only compiles what changed.
# Used by the app's "Make one file" button; by hand:  bash scripts/pack_runner.sh <name>
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
here="$ROOT/scripts"
[ $# -eq 1 ] || { echo "usage: pack_runner.sh <game name>" >&2; exit 2; }
name="$1"
if ! [[ "$name" =~ ^[A-Za-z0-9._-]+$ ]] || [[ "$name" == .* ]]; then
  echo "game name '$name': use letters, digits, . _ and - only, not starting with a dot" >&2; exit 2
fi
generated="$ROOT/generated/$name"
pack="$ROOT/build/pack-$name"
[ -d "$generated" ] || { echo "$name is not built yet (no generated/$name): build it first" >&2; exit 1; }
[ -x "$ROOT/build/tools/eerecomp" ] || [ -x "$ROOT/build/tools/eerecomp.exe" ] || { echo "the translator tools are missing: build a game once first" >&2; exit 1; }

py=()
for candidate in python3 python "py -3"; do
  if $candidate -c 'import sys; sys.exit(sys.version_info < (3,))' > /dev/null 2>&1; then py=($candidate); break; fi
done
[ ${#py[@]} -gt 0 ] || { echo "no Python 3 found: install one and put it on the PATH" >&2; exit 1; }
py+=(-X utf8)

ninja_jobs=
if [ -n "${PS2_JOBS:-}" ]; then
  case "$PS2_JOBS" in ''|*[!0-9]*|0*) echo "PS2_JOBS=$PS2_JOBS: expected a positive number" >&2; exit 2 ;; esac
  ninja_jobs="-j$PS2_JOBS"
fi
. "$here/runtime.sh"
. "$here/build_units.sh"
. "$here/cxx.sh"
shared_runtime

echo "== one-file runner for $name"
config="static ${cxx_flags[*]} ${runtime_lib:-own runtime}"
if [ ! -f "$pack/build.ninja" ] || [ "$(cat "$pack/.pack-config" 2> /dev/null)" != "$config" ]; then
  mkdir -p "$pack"
  cmake -S "$ROOT" -B "$pack" -G Ninja -DCMAKE_BUILD_TYPE=$build_type "${cxx_flags[@]}" -DPS2_GENERATED="$generated" -DPS2_RUNTIME_LIB="$runtime_lib" -DPS2_WIN_STATIC=ON > /dev/null
  echo "$config" > "$pack/.pack-config"
fi
ps2_ninja "$pack" ps2run || exit 1
[ -f "$pack/ps2run.exe" ] || [ -f "$pack/ps2run" ] || { echo "the runner was not made" >&2; exit 1; }
echo "== runner ready: build/pack-$name/ps2run.exe"
