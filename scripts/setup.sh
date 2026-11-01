#!/usr/bin/env bash
# bash 4 or newer (port.sh, which runs this, needs it too); macOS ships bash 3.2 as /bin/bash
if [ -z "${BASH_VERSION:-}" ] || [ "${BASH_VERSINFO:-0}" -lt 4 ]; then
  echo "this script needs bash 4 or newer, this is ${BASH_VERSION:-not bash} (macOS: brew install bash, then run it again)" >&2; exit 1
fi
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
EMSDK="${EMSDK:-$ROOT/tools/emsdk}"
EMSDK_VERSION="6.0.11"
web=1
[ "${1:-}" = "--native-only" ] && web=0

# emsdk looks for python3, which on Windows is the Microsoft Store stub; hand it a Python that runs.
if [ -z "${EMSDK_PYTHON:-}" ]; then
  case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*)
    for candidate in python py; do
      if "$candidate" -c 'import sys; sys.exit(sys.version_info < (3,))' > /dev/null 2>&1; then
        EMSDK_PYTHON="$(command -v "$candidate")"; export EMSDK_PYTHON; break
      fi
    done;;
  esac
fi

if [ "$web" = 1 ] && [ ! -f "$EMSDK/emsdk_env.sh" ]; then
  echo "== installing the Emscripten SDK $EMSDK_VERSION into $EMSDK"
  mkdir -p "$(dirname "$EMSDK")"
  git clone https://github.com/emscripten-core/emsdk.git "$EMSDK"
  "$EMSDK/emsdk" install "$EMSDK_VERSION"
  "$EMSDK/emsdk" activate "$EMSDK_VERSION"
fi

if [ ! -f "$ROOT/third_party/wgpu-native/lib/libwgpu_native.a" ]; then
  echo "== fetching wgpu-native for the native runner"
  "$ROOT/scripts/fetch_wgpu.sh"
fi

case "$(uname -s)" in MINGW*|MSYS*)
  if [ ! -f "$ROOT/third_party/sdl2/lib/libSDL2.dll.a" ]; then
    echo "== fetching SDL2 for the native runner"
    "$ROOT/scripts/fetch_sdl2.sh"
  fi
  # Microsoft's DXC shader compiler for Direct3D 12 (faster pictures than the built-in FXC); a failed download only means FXC.
  if [ ! -f "$ROOT/third_party/dxc/dxcompiler.dll" ]; then
    echo "== fetching the DXC shader compiler for the native runner"
    "$ROOT/scripts/fetch_dxc.sh" || echo "DXC not available: Direct3D 12 will use FXC"
  fi;;
esac

cxx_tools=1  # the tools get no AVX2 flags
. "$ROOT/scripts/cxx.sh"
cmake -S "$ROOT" -B "$ROOT/build/tools" -G Ninja -DCMAKE_BUILD_TYPE=Release "${cxx_flags[@]}" > /dev/null
ninja -C "$ROOT/build/tools" eerecomp vurecomp
echo "== ready"
