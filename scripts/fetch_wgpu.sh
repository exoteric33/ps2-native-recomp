#!/bin/sh
# Downloads the wgpu-native release for this PC into third_party/wgpu-native and checks its SHA-256 before unpacking.
# Another version needs its SHA-256 as the second argument: scripts/fetch_wgpu.sh v30.0.0.1 <sha256>.
set -e
pinned=v29.0.1.1
version=${1:-$pinned}
sha256=${2:-}
here=$(cd "$(dirname "$0")/.." && pwd)
target="$here/third_party/wgpu-native"
os=$(uname -s | sed "s/Darwin/macos/;s/Linux/linux/;s/MINGW.*/windows/;s/MSYS.*/windows/")
arch=$(uname -m | sed "s/arm64/aarch64/")
[ "$os" = windows ] && arch=$arch-gnu
if [ -z "$sha256" ] && [ "$version" = "$pinned" ]; then
  # from the release page (GitHub's digest for each file), checked by downloading each once on 2026-10-31
  case "$os-$arch" in
    linux-x86_64)       sha256=95a4d90c071005a98d03eab348beaa6b07e16eb00d1dcdb9f8348f75eb97ec5a ;;
    linux-aarch64)      sha256=015fcdf1dbae82e614a783cc38017e5399ae0927a889fe9b69c9b664bc61b47a ;;
    macos-aarch64)      sha256=a5797a37b1adf720bcd5dcffb291edbbd5b7b14be0a3874c28e6393a655a7a3e ;;
    macos-x86_64)       sha256=8e2f7378548ddd0e2cf21e7d864dda46e953f0af724855a33778b85ead206d41 ;;
    windows-x86_64-gnu) sha256=d471e3614733c1d4ddd61bfd19868356477d0d37bf531bf8c6cb64a7f579bd2a ;;
    *) echo "wgpu-native: no $pinned build for $os-$arch (there are linux and macos x86_64/aarch64, windows x86_64)" >&2; exit 1 ;;
  esac
fi
[ -n "$sha256" ] || { echo "wgpu-native: no SHA-256 known for $version: pass it as the second argument" >&2; exit 1; }
mkdir -p "$target"
url="https://github.com/gfx-rs/wgpu-native/releases/download/$version/wgpu-$os-$arch-release.zip"
# curl from Git for Windows and tar.exe read their command line in the ANSI code page and cannot open a folder with letters
# outside it (Chinese on a Western Windows), so both run inside the folder with a plain file name. MinGit has no unzip.
(cd "$target" && curl -fL -o wgpu.zip "$url") || { rm -f "$target/wgpu.zip"; echo "wgpu-native: download failed: $url" >&2; exit 1; }
# sha256sum on Linux and in Git Bash, shasum on macOS
if command -v sha256sum > /dev/null; then got=$(cd "$target" && sha256sum wgpu.zip | cut -d' ' -f1)
else got=$(cd "$target" && shasum -a 256 wgpu.zip | cut -d' ' -f1); fi
if [ "$got" != "$sha256" ]; then rm -f "$target/wgpu.zip"; echo "wgpu-native: wrong SHA-256 $got (expected $sha256)" >&2; exit 1; fi
if command -v unzip > /dev/null; then unzip -o -q "$target/wgpu.zip" -d "$target"; else (cd "$target" && tar -xf wgpu.zip); fi
rm "$target/wgpu.zip"
echo "wgpu-native $version in $target"
