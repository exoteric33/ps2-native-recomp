# Virtual Disc Folder (`disc_folder`) & Asset Extraction Toolkit (`ps2_disc_toolkit`)

`ps2-native-recomp` can run a game directly from an **extracted virtual disc directory** (`--disc games/<name>/disc_files`) without keeping the original `.iso` / `.bin` / `.cso` / `.chd` disc image around.

Unlike a plain file-only extraction, `tools/ps2_disc_toolkit` preserves the exact 2048-byte sector layout (`disc_manifest.tsv` + `disc_meta.bin` + `unindexed/`), so games that stream assets by raw LBA sector numbers (`sceCdRead`) work identically to a raw disc image while keeping all files on disk open for inspection and modding.

---

## 1. Extracting a Disc Image (`scripts/extract_disc.py`)

Supported input containers:
- **ISO-2048** (`.iso`, single-layer DVD-5 and dual-layer DVD-9 / PTP)
- **BIN/CUE-2352** (`.bin` / `.cue`, CD-ROM Mode 1 & Mode 2 Form 1 XA)
- **CSO** (`.cso`, CISO zlib-compressed images)
- **CHD v5** (`.chd`, MAME Compressed Hunks of Data: `zlib`, `lzma`, `zstd`, `cdzl`, `cdlz`, `cdzs`)

```bash
# 1. Inspect disc metadata, dual-layer PVD, and hidden LBA gaps
python3 scripts/extract_disc.py inspect path/to/game.iso --scan-gaps

# 2. Extract all files + sector manifest into games/<name>/disc_files
python3 scripts/extract_disc.py extract path/to/game.iso games/<name>/disc_files --skip-existing

# 3. Verify sector-for-sector reconstruction against the original image
python3 scripts/extract_disc.py verify path/to/game.iso games/<name>/disc_files
```

Running with the extracted directory:
```bash
./build/native-<name>/ps2run games/<name>/disc_files/layer0/<BOOT_ELF> --disc games/<name>/disc_files
```

---

## 2. Unpacking 2nd-Level Game Archives & Converting PS2 3D/Texture/Audio Assets

Many PS2 games pack their 3D models, textures, audio streams, and relocatable overlays inside container files (`.AFS`, `.PAC`, `.MOMO`, `.LMP`, `.GOB`, `.TEX`, `DATA.BIN`, etc.). `extract_disc.py` includes a built-in 2nd-level unpacker and asset converter (`tools/ps2_disc_toolkit/archives.py`, `ps2_textures.py`, `ps2_audio.py`, `ps2_meshes.py`):

- **Archive containers**: CRI `.AFS` (`AFS\x00`), `.MOMO`, headerless offset-table `.PAC` archives (`count + [offset, size]`), sector-aligned (`0x800`) `.PAC` containers, Snowblind `.LMP`/`.GOB`/`.TEX`, Capcom 16-bit LZ16 streams, and a fallback universal signature carver (`--carve`).
- **Textures**: Sony SDK `.TIM2` (`.tm2`), `.tex`, and `.16` $\rightarrow$ **32-bit RGBA `.png`** (with hardware GS VRAM unswizzling, CSM1 CLUT palette unswizzling, and PS2 $0..128 \rightarrow 0..255$ alpha scaling).
- **Audio**: Sony SDK SPU2 ADPCM `.vag` (`VAGp`) $\rightarrow$ **16-bit PCM `.wav`**.
- **3D Meshes**: Recursive chunk-based `.FMOD` 3D models and VIF1/VU1 geometry packets (`.vif`, honoring the GS `ADC 0x8000` strip-restart flag) $\rightarrow$ **Wavefront `.obj`** (with UVs, normals, per-vertex RGB shading, automatic sub-mesh splitting `_partXX.obj`, and bounding-box role classification in `assets_catalog.json`).

```bash
# List subfiles inside an archive (.AFS, .PAC, .MOMO, .LMP, .GOB, .TEX, DATA.BIN)
python3 scripts/extract_disc.py unpack-list games/<name>/disc_files/layer0/DATA.BIN --max-entries 20

# Extract and convert PS2 textures (.tm2, .tex, .16 -> .png), audio (.vag -> .wav),
# 3D models (.fmod, .vif -> .obj + submesh parts + assets_catalog.json),
# Metrowerks MWo3 overlays, and Capcom LZ16 containers
python3 scripts/extract_disc.py unpack-extract \
  games/<name>/disc_files/layer0/DATA.BIN \
  games/<name>/assets_unpacked/data_bin \
  --convert --recursive --max-entries 50

# Carve TIM2 textures and VAGp audio from arbitrary binary containers
python3 scripts/extract_disc.py unpack-extract \
  games/<name>/disc_files/layer0/DATA.BIN \
  games/<name>/assets_unpacked/data_carved \
  --carve --convert
```

---

## 3. Single-File Executable Bundler (`PS2PACK1`, Linux & Windows)

Once a game has been extracted into `games/<name>/disc_files` and compiled, you can package the entire recompiled game—including the host `ps2run` binary, the guest ELF, precompiled `ee-live` caches (`.so` on Linux, `.dll` on Windows), optional JSON assets, and the complete virtual disc (`disc_files/`)—into a **single self-contained executable** per target OS (`<Game>-Linux-x86_64` and `<Game>-Windows-x86_64.exe`).

At launch (0 command-line arguments / double-click), the executable inspects the last 64 bytes of its own binary (`/proc/self/exe` on Linux, `GetModuleFileNameW(NULL)` on Windows) for the `PS2PACK1` trailer. If present:
- It serves all disc sector reads (`sceCdRead`) directly from its own file descriptor via `pread64` / `ReadFile(OVERLAPPED)` (`0 bytes` of temporary disc extraction to `/tmp`).
- It loads the embedded guest ELF directly into RAM.
- It automatically creates a `saves/` folder (`saves/memcard.ps2`, `saves/states/`, `saves/cache/ee-live/`) next to the executable on first launch and seeds any embedded `ee-live` caches.
- If a file inside `games/<name>/disc_files/layer0/` was modified (e.g. modded textures or patched scripts) before bundling, the bundler automatically patches its ISO-9660 directory entry LBA/size inside `disc_meta.bin`.

```bash
# 1. (Optional, fetched automatically when building for Windows)
#    Fetch llvm-mingw UCRT toolchain, SDL2 mingw, and wgpu-native-win into third_party/
sh scripts/fetch_win_cross.sh

# 2. Build a single-file executable for Linux, Windows, or both (--os linux | windows | all)
python3 scripts/extract_disc.py bundle \
  --game <name> \
  --os all \
  --out dist/<GameTitle>
```

Output:
- `dist/<GameTitle>-Linux-x86_64`
- `dist/<GameTitle>-Windows-x86_64.exe` (statically linked against `libc++`, `libunwind`, `winpthread`, `SDL2`, and `wgpu_native`; zero external runtime DLL dependencies)

