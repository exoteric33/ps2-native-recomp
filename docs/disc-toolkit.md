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

## 2. Unpacking 2nd-Level Game Archives & Converting PS2 Assets

Many PS2 games pack their 3D models, textures, audio streams, and relocatable overlays inside container files (`.AFS`, `.LMP`, `.GOB`, `.TEX`, `DATA.BIN`, etc.). `extract_disc.py` includes a built-in 2nd-level unpacker and Sony SDK asset converter:

```bash
# List subfiles inside an archive (.AFS, .LMP, .GOB, .TEX)
python3 scripts/extract_disc.py unpack-list games/<name>/disc_files/layer0/DAT.AFS

# Extract and convert PS2 textures (.tm2, .tex, .16 -> .png), audio (.vag -> .wav),
# Metrowerks MWo3 overlays, and Capcom LZ16 containers (.dec.bin)
python3 scripts/extract_disc.py unpack-extract \
  games/<name>/disc_files/layer0/DAT.AFS \
  games/<name>/assets_unpacked/dat_afs \
  --convert --recursive

# Carve TIM2 textures and VAGp audio from proprietary binary containers
python3 scripts/extract_disc.py unpack-extract \
  games/<name>/disc_files/layer0/DATA.BIN \
  games/<name>/assets_unpacked/data_carved \
  --carve --convert
```
