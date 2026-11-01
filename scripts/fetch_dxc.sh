#!/bin/sh
# Downloads Microsoft's DirectX Shader Compiler (dxcompiler.dll + dxil.dll, x64) into third_party/dxc. wgpu uses it for Direct3D 12
# when the two DLLs sit next to ps2run.exe; without them it falls back to the old FXC compiler. Windows only. The files are never
# committed. Licence: Microsoft's terms (LICENSE-MS.txt, saved next to the DLLs) allow installing and using it on Windows; passing the
# DLLs on to others has conditions (docs/speed.md, section DXC), so we download them on each PC instead of shipping them.
set -e
version=${1:-v1.9.2609}
file=${2:-dxc_2026_09_29.zip}
sha256=${3:-ad31b1fc8443175d204f77a611fdb3ef2ec42759bdc2f1167368de24a4a7e7f1}
here=$(cd "$(dirname "$0")/.." && pwd)
target="$here/third_party/dxc"
mkdir -p "$target"
(cd "$target" && curl -sSL -f -o dxc.zip "https://github.com/microsoft/DirectXShaderCompiler/releases/download/$version/$file")
if command -v sha256sum > /dev/null; then got=$(sha256sum "$target/dxc.zip" | cut -d' ' -f1)
else got=$(python -I -c "import hashlib,sys;print(hashlib.sha256(open(sys.argv[1],'rb').read()).hexdigest())" "$target/dxc.zip"); fi
if [ "$got" != "$sha256" ]; then rm -f "$target/dxc.zip"; echo "dxc: wrong SHA-256 $got (expected $sha256)" >&2; exit 1; fi
# curl from Git for Windows and tar.exe read their command line in the ANSI code page, so tar runs inside the folder with a plain
# file name (see fetch_wgpu.sh). MinGit has no unzip.
# The zip stores its paths with backslashes; unzip turns them into folders, bsdtar keeps them, so the DLLs are looked up by name.
if command -v unzip > /dev/null; then unzip -o -q "$target/dxc.zip" -d "$target/unpacked" || true; else (mkdir -p "$target/unpacked" && cd "$target/unpacked" && tar -xf ../dxc.zip); fi
for f in dxcompiler.dll dxil.dll; do
  src=$(find "$target/unpacked" -path '*x64*' -name "$f" | head -n 1)
  [ -n "$src" ] || { echo "dxc: $f not found in the download" >&2; exit 1; }
  cp "$src" "$target/$f"
done
cp "$target/unpacked/"LICEN[CS]E*.txt "$target/"
rm -rf "$target/unpacked" "$target/dxc.zip"
echo "DXC $version in $target"
