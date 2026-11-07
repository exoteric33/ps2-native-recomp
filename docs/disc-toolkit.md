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

## 2. Unpacking 2nd-Level Game Archives & Converting PS2 3D/Texture/Audio/Video Assets

Many PS2 games pack their 3D models, textures, audio streams, FMV videos, and relocatable overlays inside container files (`.AFS`, `.PAC`, `.MOMO`, `.LMP`, `.GOB`, `.TEX`, `DATA.BIN`, etc.). `extract_disc.py` includes a built-in 2nd-level unpacker and bidirectional asset converter (`tools/ps2_disc_toolkit/archives.py`, `ps2_textures.py`, `ps2_audio.py`, `ps2_video.py`, `ps2_meshes.py`, `repacker.py`):

- **Archive containers**: CRI `.AFS` (`AFS\x00`), `.MOMO`, headerless offset-table `.PAC` archives (`count + [offset, size]`), sector-aligned (`0x800`) `.PAC` containers, Snowblind `.LMP`/`.GOB`/`.TEX`, Capcom 16-bit LZ16 streams, and a fallback universal signature carver (`--carve`).
- **Textures**: Sony SDK `.TIM2` (`.tm2`), `.tex`, and `.16` $\leftrightarrow$ **32-bit RGBA `.png`** (with hardware GS VRAM unswizzling/swizzling, CSM1 CLUT palette unswizzling, PS2 $0..128 \leftrightarrow 0..255$ alpha scaling, and `textures_index.tsv` GPU hash indexing).
- **Audio & Sound Banks**: CRI Middleware `.ADX` (`\x80\x00...(c)CRI`), Sony SDK SPU2 ADPCM `.vag` (`VAGp`), `.ads` (`SShd`/`SSbd`), and raw SPU2 `.bd` sound banks $\leftrightarrow$ **16-bit PCM `.wav`**.
- **Videos**: CRI Sofdec `.SFD` (MPEG-2 + CRI ADX audio) and Sony `.PSS` (MPEG-2 + SPU2/PCM audio) (`0x000001BA`) $\leftrightarrow$ **`.mp4` (H.264 + synchronized stereo AAC audio)**.
- **3D Meshes**: Recursive chunk-based `.FMOD` 3D models (including `0x000C0000` skeletal skinning weights) and VIF1/VU1 geometry packets (`.vif`, honoring the GS `ADC 0x8000` strip-restart flag) $\leftrightarrow$ **Wavefront `.obj`** (with UVs, normals, per-vertex RGB shading, automatic sub-mesh splitting `_partXX.obj`, nearest-vertex bone weight transfer on re-encode, and bounding-box role classification in `assets_catalog.json`).

```bash
# List subfiles inside an archive (.AFS, .PAC, .MOMO, .LMP, .GOB, .TEX, DATA.BIN)
python3 scripts/extract_disc.py unpack-list games/<name>/disc_files/layer0/DATA.BIN --max-entries 20

# Extract and convert PS2 textures (.tm2, .tex, .16 -> .png), audio (.vag, .adx, .ads, .bd -> .wav),
# videos (.sfd, .pss -> .mp4), 3D models (.fmod, .vif -> .obj + submesh parts + assets_catalog.json),
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

## 3. Bidirectional Modding: Texture Indexing & Multi-Core Incremental `sync`

- **Texture Hash Index (`textures_index.tsv`)**:
  During `unpack-extract --convert`, every decoded `.png` texture is indexed by its 64-bit GS upload hash (`content_key`) in `games/<name>/assets_unpacked/textures_index.tsv`.
- **Multi-Core Incremental Asset Repacker (`sync`)**:
  Modified 3D meshes (`.obj` $\rightarrow$ `.fmod`), audio tracks/SFX (`.wav` $\rightarrow$ `.adx`/`.vag`/`.bd`), FMV videos (`.mp4` $\rightarrow$ `.sfd`/`.pss`), and 1x textures (`.png` $\rightarrow$ `.tm2`) are incrementally repacked in parallel (`ProcessPoolExecutor`) into their parent `.PAC` / `.MOMO` / `LZ16` / `.AFS` / `DATA.BIN` containers in `games/<name>/disc_files/layer0/`:
  ```bash
  python3 scripts/extract_disc.py sync --game <name>
  ```
  *(Can also be invoked before bundling by passing `--sync` to `extract_disc.py bundle`).*

---

## 3.5 Single-File Executable Bundler (`PS2PACK1`, Linux & Windows)

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

---

## 4. Format Provenance & License Compatibility

All container parsers, archive unpackers, and asset codecs in `tools/ps2_disc_toolkit/` are implemented from public standards, MIT/BSD/ISC-compatible specifications, or clean-room binary/MIPS inspection:

| Format / Component | Module | Specification / Provenance | License Compatibility |
| :--- | :--- | :--- | :--- |
| **ISO 9660 (`.iso`, single/dual-layer)** | `containers.py`, `iso9660.py` | Public ECMA-119 standard (sector 16 PVD, 33-byte directory records) & plain-text `SYSTEM.CNF` | Public standard (MIT-compatible) |
| **Raw CD-ROM (`.bin` / `.cue`)** | `containers.py` | Public ECMA-130 (Yellow Book Mode 1) & Sony/Philips CD-ROM XA (Mode 2 Form 1/2, 2352B sectors) | Public standard (MIT-compatible) |
| **Compressed ISO (`.cso` v1/v2)** | `containers.py` | Public CISO header/block-table layout (Booster 2006; Unknown Brackets `maxcso`, ISC License) | ISC / Public spec (MIT-compatible) |
| **MAME CHD v5 (`.chd`)** | `containers.py` | MAME `src/lib/util/chd.h` & `libchdr` (Aaron Giles / MAMEdev, BSD-3-Clause notice included in `containers.py`) | BSD-3-Clause (MIT-compatible) |
| **CRI `.AFS` & `.MOMO` archives** | `archives.py` | Clean-room inspection of 8-byte `(offset, size)` tables and optional 48-byte filename footers in PS2 disc files | Clean-room (MIT) |
| **Offset-table `.PAC` archives** | `archives.py` | Clean-room inspection of 32-bit little-endian `count + count * (offset, size)` tables in PS2 archives | Clean-room (MIT) |
| **Snowblind `.LMP`, `.GOB`, `.TEX`** | `archives.py`, `ps2_textures.py` | Clean-room reverse-engineering of binary headers and MIPS loaders in *Champions of Norrath* (also documented in MIT-licensed `bgda-explorer`) | Clean-room / MIT |
| **Capcom 16-bit LZ16 & `MWo3`** | `codecs.py` | Clean-room reverse-engineering of MIPS R5900 decompression routine and 32-byte `MWo3` overlay header in *Monster Hunter 2* (`SLPM_662.80`) | Clean-room (MIT) |
| **GS VRAM swizzling & `.TIM2`** | `ps2_textures.py` | Sony *GS Users Manual* (Ch. 8 Local Memory), public Sony PS2 SDK `TIM2` specification, and PS2Dev Wiki | Public hardware/SDK docs (MIT) |
| **`.FMOD` & `.VIF` 3D meshes** | `ps2_meshes.py` | Clean-room reverse-engineering of `.FMOD` chunk hierarchy (`SLPM_662.80`) and Sony *EE Users Manual* (Ch. 6 VIF1 `UNPACK` & GS `ADC` bit 15) | Clean-room / Public hardware docs (MIT) |
| **Sony SPU2 ADPCM (`.vag`, `.ads`, `.bd`)** | `ps2_audio.py` | Public PS1/PS2 SPU ADPCM 16-byte block format and 5 fixed IIR filter pairs (PS2Dev Wiki / nocash PSX/PS2 specs) | Public hardware docs (MIT) |
| **CRI `.ADX` audio** | `ps2_audio.py` | Clean-room implementation from public MultimediaWiki 18-byte ADX frame specification (`wiki.multimedia.cx/index.php/CRI_ADX_file`) | Clean-room (MIT) |
| **`.PSS` & `.SFD` video streams** | `ps2_video.py` | Public ISO/IEC 13818-1 (MPEG-2 Program Stream `0x000001BA`) & clean-room PES header inspection (`0xE0`/`0xC0`/`0xBD`); transcoding via external `ffmpeg` | Public standard / Clean-room (MIT) |
