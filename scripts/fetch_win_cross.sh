#!/bin/sh
# Downloads the Windows x86_64 cross-compilation toolchain (llvm-mingw UCRT) and Windows static dependencies
# (SDL2 2.32.10 mingw, wgpu-native v29.0.1.1 windows-x86_64-gnu) into third_party/ on a Linux x86_64 host.
set -e
here=$(cd "$(dirname "$0")/.." && pwd)

# 1. SDL2 2.32.10 (mingw x86_64)
if [ ! -f "$here/third_party/sdl2/lib/libSDL2.a" ]; then
  sh "$here/scripts/fetch_sdl2.sh"
else
  echo "SDL2 already present in $here/third_party/sdl2"
fi

# 2. wgpu-native v29.0.1.1 (windows-x86_64-gnu)
wgpu_win="$here/third_party/wgpu-native-win"
wgpu_ver=v29.0.1.1
wgpu_sha256=d471e3614733c1d4ddd61bfd19868356477d0d37bf531bf8c6cb64a7f579bd2a
if [ ! -f "$wgpu_win/lib/libwgpu_native.a" ]; then
  mkdir -p "$wgpu_win"
  url="https://github.com/gfx-rs/wgpu-native/releases/download/$wgpu_ver/wgpu-windows-x86_64-gnu-release.zip"
  echo "Downloading wgpu-native ($wgpu_ver, windows-x86_64-gnu)..."
  (cd "$wgpu_win" && curl -fL -o wgpu.zip "$url")
  got=$(cd "$wgpu_win" && sha256sum wgpu.zip | cut -d' ' -f1)
  if [ "$got" != "$wgpu_sha256" ]; then
    rm -f "$wgpu_win/wgpu.zip"
    echo "wgpu-native-win: wrong SHA-256 $got (expected $wgpu_sha256)" >&2
    exit 1
  fi
  unzip -o -q "$wgpu_win/wgpu.zip" -d "$wgpu_win"
  rm -f "$wgpu_win/wgpu.zip"
  echo "wgpu-native $wgpu_ver (Windows) in $wgpu_win"
else
  echo "wgpu-native (Windows) already present in $wgpu_win"
fi

# 3. llvm-mingw 20261104 UCRT (Linux x86_64 host -> Windows x86_64 target)
mingw_dir="$here/third_party/llvm-mingw"
mingw_ver=20261104
mingw_sha256=919380403de87dca3c581c2053230cd298af5e891286628ba0613b7fed370926
if [ ! -x "$mingw_dir/bin/x86_64-w64-mingw32-clang++" ]; then
  mkdir -p "$here/third_party"
  archive="$here/third_party/llvm-mingw.tar.xz"
  url="https://github.com/mstorsjo/llvm-mingw/releases/download/$mingw_ver/llvm-mingw-$mingw_ver-ucrt-ubuntu-22.04-x86_64.tar.xz"
  echo "Downloading llvm-mingw ($mingw_ver, UCRT x86_64)..."
  curl -fL -o "$archive" "$url"
  got=$(sha256sum "$archive" | cut -d' ' -f1)
  if [ "$got" != "$mingw_sha256" ]; then
    rm -f "$archive"
    echo "llvm-mingw: wrong SHA-256 $got (expected $mingw_sha256)" >&2
    exit 1
  fi
  rm -rf "$mingw_dir" "$here/third_party/llvm-mingw-$mingw_ver-ucrt-ubuntu-22.04-x86_64"
  tar -xf "$archive" -C "$here/third_party"
  mv "$here/third_party/llvm-mingw-$mingw_ver-ucrt-ubuntu-22.04-x86_64" "$mingw_dir"
  rm -f "$archive"
  echo "llvm-mingw $mingw_ver in $mingw_dir"
else
  echo "llvm-mingw already present in $mingw_dir"
fi
