#!/bin/sh
# Downloads the SDL2 mingw development package (Windows) into third_party/sdl2 and checks its SHA-256 before unpacking.
# Another version needs its SHA-256 as the second argument: scripts/fetch_sdl2.sh 2.32.12 <sha256>.
set -e
pinned=2.32.10
version=${1:-$pinned}
sha256=${2:-}
# from the release page (GitHub's digest for the file), checked by downloading it on 2026-10-31
[ -n "$sha256" ] || [ "$version" != "$pinned" ] || sha256=f15cff5fca62ec9381a016ef1d42a95c638cd72d2f226ba5781c76fe43dbd1ac
[ -n "$sha256" ] || { echo "SDL2: no SHA-256 known for $version: pass it as the second argument" >&2; exit 1; }
here=$(cd "$(dirname "$0")/.." && pwd)
target="$here/third_party/sdl2"
mkdir -p "$target"
url="https://github.com/libsdl-org/SDL/releases/download/release-$version/SDL2-devel-$version-mingw.zip"
# curl from Git for Windows and tar.exe read their command line in the ANSI code page and cannot open a folder with letters
# outside it (Chinese on a Western Windows), so both run inside the folder with a plain file name. MinGit has no unzip.
(cd "$target" && curl -fL -o sdl2.zip "$url") || { rm -f "$target/sdl2.zip"; echo "SDL2: download failed: $url" >&2; exit 1; }
# sha256sum on Linux and in Git Bash, shasum on macOS
if command -v sha256sum > /dev/null; then got=$(cd "$target" && sha256sum sdl2.zip | cut -d' ' -f1)
else got=$(cd "$target" && shasum -a 256 sdl2.zip | cut -d' ' -f1); fi
if [ "$got" != "$sha256" ]; then rm -f "$target/sdl2.zip"; echo "SDL2: wrong SHA-256 $got (expected $sha256)" >&2; exit 1; fi
if command -v unzip > /dev/null; then unzip -o -q "$target/sdl2.zip" -d "$target"; else (cd "$target" && tar -xf sdl2.zip); fi
cp -R "$target/SDL2-$version/x86_64-w64-mingw32/." "$target"
rm -rf "$target/SDL2-$version" "$target/sdl2.zip"
echo "SDL2 $version in $target"
