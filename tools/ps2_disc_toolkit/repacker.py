"""
Generic Multi-Core Incremental Asset Repacker & Live Texture Indexer for PS2 Disc Toolkit.

Scans `games/<name>/assets_unpacked/` for modified PC-format assets (.obj, .png, .wav, .mp4),
converts ONLY modified files in parallel using `ProcessPoolExecutor` back into their native
PS2 formats (.fmod, .tm2, .vag, .adx, .sfd/.pss), re-packs nested .PAC / .MOMO archives,
re-compresses Capcom LZ16 streams (.dec.* -> .bin), and patches the parent .AFS / DATA.BIN
containers inside `games/<name>/disc_files/layer0/`.
"""

from __future__ import annotations

from concurrent.futures import ProcessPoolExecutor
import json
import os
import re
import struct
import time
from pathlib import Path

from .codecs import compress_capcom_lz16
from .ps2_audio import encode_wav_to_adx_bytes, encode_wav_to_vag_bytes
from .ps2_meshes import encode_obj_to_fmod
from .ps2_textures import (
    compute_tim2_gpu_key,
    decode_png_to_rgba_bytes,
    decode_tim2_to_rgba,
    encode_png_to_tim2_bytes,
)
from .ps2_video import encode_mp4_to_sfd_pss

_ENTRY_INDEX_RE = re.compile(r"(?:entry|file|sfx)_(\d+)", re.IGNORECASE)


def _convert_single_modified_task(task: dict) -> dict:
    """
    Worker function executed in parallel via ProcessPoolExecutor.
    Converts one modified PC asset (.obj, .png, .wav, .mp4) back into its PS2 binary file.
    """
    conv_path = Path(task["conv_path"])
    orig_path = Path(task["orig_path"])
    kind = task["kind"]

    if not conv_path.is_file() or not orig_path.is_file():
        return {"ok": False, "conv_path": str(conv_path), "reason": "missing_file"}

    ref_bytes = orig_path.read_bytes()
    if kind == "fmod":
        obj_text = conv_path.read_text(encoding="utf-8", errors="ignore")
        new_bytes = encode_obj_to_fmod(obj_text, ref_bytes)
        orig_path.write_bytes(new_bytes)
        return {"ok": True, "kind": kind, "conv_path": str(conv_path), "orig_path": str(orig_path)}

    if kind == "tim2":
        png_bytes = conv_path.read_bytes()
        png_w, png_h, _rgba = decode_png_to_rgba_bytes(png_bytes)
        orig_w = task.get("width", png_w)
        orig_h = task.get("height", png_h)
        if png_w != orig_w or png_h != orig_h:
            # HD texture (>1x): handled live by the C++ WebGPU runtime!
            return {
                "ok": True,
                "kind": "tim2_hd_live",
                "conv_path": str(conv_path),
                "orig_path": str(orig_path),
                "hd_dims": (png_w, png_h),
            }
        new_bytes = encode_png_to_tim2_bytes(png_bytes, ref_bytes)
        orig_path.write_bytes(new_bytes)
        return {"ok": True, "kind": kind, "conv_path": str(conv_path), "orig_path": str(orig_path)}

    if kind in ("vag", "bd_vag"):
        wav_bytes = conv_path.read_bytes()
        vag_target = Path(task["vag_path"]) if task.get("vag_path") else orig_path
        ref_vag = vag_target.read_bytes() if vag_target.is_file() else ref_bytes
        new_vag = encode_wav_to_vag_bytes(wav_bytes, ref_vag)
        vag_target.write_bytes(new_vag)
        if kind == "bd_vag" and orig_path != vag_target:
            # Patch the ADPCM slice inside the parent .bd sound bank
            bd_off = int(task.get("bdOffset", 0))
            bd_len = int(task.get("bdLength", 0))
            adpcm_slice = new_vag[48:]
            bd_buf = bytearray(orig_path.read_bytes())
            if bd_len > 0 and bd_off + bd_len <= len(bd_buf):
                fitted = adpcm_slice[:bd_len].ljust(bd_len, b"\x00")
                # Ensure end-of-sample flag is set on the last non-zero 16-byte block
                if len(fitted) >= 16:
                    fitted = bytearray(fitted)
                    fitted[-15] = 0x07
                bd_buf[bd_off : bd_off + bd_len] = fitted
                orig_path.write_bytes(bytes(bd_buf))
        return {"ok": True, "kind": kind, "conv_path": str(conv_path), "orig_path": str(orig_path)}

    if kind == "adx":
        wav_bytes = conv_path.read_bytes()
        new_adx = encode_wav_to_adx_bytes(wav_bytes, ref_bytes)
        orig_path.write_bytes(new_adx)
        return {"ok": True, "kind": kind, "conv_path": str(conv_path), "orig_path": str(orig_path)}

    if kind in ("video_sfd", "video_pss"):
        target_ext = ".sfd" if kind == "video_sfd" else ".pss"
        new_vid = encode_mp4_to_sfd_pss(conv_path, ref_bytes, target_ext=target_ext)
        orig_path.write_bytes(new_vid)
        return {"ok": True, "kind": kind, "conv_path": str(conv_path), "orig_path": str(orig_path)}

    return {"ok": False, "conv_path": str(conv_path), "reason": f"unsupported_kind:{kind}"}


def bootstrap_manifest_for_dir(unpacked_dir: Path, source_archive: Path | None = None) -> dict:
    """
    Build `.unpack_manifest.json` and `textures_index.tsv` for an already-extracted directory
    (such as `games/mhdos/assets_unpacked/data_3d_kit/`) by pairing existing native files
    (.tm2, .fmod, .vag, .adx, .sfd, .pss) with their converted counterparts (.png, .obj, .wav, .mp4).
    """
    assets: dict[str, dict] = {}
    textures_tsv_rows: list[str] = []

    for root, _dirs, files in os.walk(unpacked_dir):
        file_set = set(files)
        root_path = Path(root)
        for fname in sorted(files):
            stem, ext = os.path.splitext(fname)
            low_ext = ext.lower()
            orig_abs = root_path / fname

            if low_ext in (".tm2", ".tim2") and f"{stem}.png" in file_set:
                png_abs = root_path / f"{stem}.png"
                try:
                    raw = orig_abs.read_bytes()
                    w, h, _rgba = decode_tim2_to_rgba(raw)
                    gpu_key = compute_tim2_gpu_key(raw)
                    st = png_abs.stat()
                    rel_png = os.path.relpath(png_abs, unpacked_dir)
                    rel_orig = os.path.relpath(orig_abs, unpacked_dir)
                    assets[rel_png] = {
                        "original": rel_orig,
                        "kind": "tim2",
                        "mtime_ns": st.st_mtime_ns,
                        "size": st.st_size,
                        "width": w,
                        "height": h,
                        "gpuKey": f"{gpu_key:016x}",
                    }
                    textures_tsv_rows.append(
                        f"{gpu_key:016x}\t{w}\t{h}\t{st.st_mtime_ns}\t{st.st_size}\t{rel_png}"
                    )
                except Exception:
                    pass

            elif low_ext == ".fmod" and f"{stem}.obj" in file_set:
                obj_abs = root_path / f"{stem}.obj"
                st = obj_abs.stat()
                assets[os.path.relpath(obj_abs, unpacked_dir)] = {
                    "original": os.path.relpath(orig_abs, unpacked_dir),
                    "kind": "fmod",
                    "mtime_ns": st.st_mtime_ns,
                    "size": st.st_size,
                }

            elif low_ext == ".vag" and f"{stem}.wav" in file_set:
                wav_abs = root_path / f"{stem}.wav"
                st = wav_abs.stat()
                assets[os.path.relpath(wav_abs, unpacked_dir)] = {
                    "original": os.path.relpath(orig_abs, unpacked_dir),
                    "kind": "vag",
                    "mtime_ns": st.st_mtime_ns,
                    "size": st.st_size,
                }

            elif low_ext == ".adx" and f"{stem}.wav" in file_set:
                wav_abs = root_path / f"{stem}.wav"
                st = wav_abs.stat()
                assets[os.path.relpath(wav_abs, unpacked_dir)] = {
                    "original": os.path.relpath(orig_abs, unpacked_dir),
                    "kind": "adx",
                    "mtime_ns": st.st_mtime_ns,
                    "size": st.st_size,
                }

            elif low_ext in (".sfd", ".pss") and f"{stem}.mp4" in file_set:
                mp4_abs = root_path / f"{stem}.mp4"
                st = mp4_abs.stat()
                assets[os.path.relpath(mp4_abs, unpacked_dir)] = {
                    "original": os.path.relpath(orig_abs, unpacked_dir),
                    "kind": "video_sfd" if low_ext == ".sfd" else "video_pss",
                    "mtime_ns": st.st_mtime_ns,
                    "size": st.st_size,
                }

    if textures_tsv_rows:
        (unpacked_dir / "textures_index.tsv").write_text(
            "\n".join(textures_tsv_rows) + "\n", encoding="utf-8"
        )

    manifest = {
        "sourceArchive": str(source_archive.resolve()) if source_archive else "",
        "format": "BOOTSTRAPPED",
        "assets": assets,
    }
    manifest_path = unpacked_dir / ".unpack_manifest.json"
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    return manifest


def repack_pac_or_momo_container(container_path: Path, unpacked_dir: Path) -> bool:
    """
    Rebuild a PS2 .PAC or MOMO offset-table container (`container_path`) using the
    updated entry files inside `unpacked_dir` (`entry_XXXX.*` or `file_XXXX.*`).
    """
    if not container_path.is_file() or not unpacked_dir.is_dir():
        return False

    orig = container_path.read_bytes()
    is_momo = orig[:4] == b"MOMO"
    is_afs = orig[:4] == b"AFS\x00"
    if is_momo or is_afs:
        count = struct.unpack_from("<I", orig, 4)[0]
        table_base = 8
        align = 64 if is_momo else 2048
    else:
        count = struct.unpack_from("<I", orig, 0)[0]
        table_base = 4
        align = 16

    if count <= 0 or count > 65535 or table_base + count * 8 > len(orig):
        return False

    # Map entry index -> file in unpacked_dir (excluding .png, .obj, .wav, .mp4, .dec.*)
    SKIP_EXTS = {".png", ".obj", ".mtl", ".wav", ".mp4", ".json", ".tsv"}
    index_to_file: dict[int, Path] = {}
    for p in unpacked_dir.iterdir():
        if not p.is_file() or p.suffix.lower() in SKIP_EXTS or ".dec." in p.name.lower():
            continue
        m = _ENTRY_INDEX_RE.match(p.stem)
        if m:
            index_to_file[int(m.group(1))] = p

    orig_pairs = [struct.unpack_from("<II", orig, table_base + i * 8) for i in range(count)]
    first_non_zero = next((off for off, sz in orig_pairs if off > 0 and sz > 0), table_base + count * 8)
    header_end = max(table_base + count * 8, first_non_zero)

    out = bytearray(orig[:header_end])
    for i, (orig_off, orig_sz) in enumerate(orig_pairs):
        if orig_off == 0 and orig_sz == 0:
            struct.pack_into("<II", out, table_base + i * 8, 0, 0)
            continue
        if i in index_to_file:
            payload = index_to_file[i].read_bytes()
        else:
            payload = orig[orig_off : orig_off + orig_sz]

        rem = len(out) % align
        if rem != 0:
            out.extend(b"\x00" * (align - rem))
        new_off = len(out)
        new_sz = len(payload)
        struct.pack_into("<II", out, table_base + i * 8, new_off, new_sz)
        out.extend(payload)

    container_path.write_bytes(bytes(out))
    return True


def patch_afs_entries_in_place_or_rebuild(afs_path: Path, updated_entries: dict[int, bytes]) -> bool:
    """
    Update specific entry indices inside a CRI .AFS ('AFS\\x00') or nested DATA.BIN container.
    Uses fast in-place sector writes when every modified entry fits within its existing
    2048-byte sector allocation, or rebuilds the AFS layout if an entry grew beyond its slot.
    """
    if not updated_entries or not afs_path.is_file():
        return False

    with open(afs_path, "r+b") as f:
        hdr = f.read(8)
        if len(hdr) < 8 or hdr[:4] not in (b"AFS\x00", b"MOMO"):
            return False
        count = struct.unpack_from("<I", hdr, 4)[0]
        table_bytes = f.read(count * 8)
        pairs = [struct.unpack_from("<II", table_bytes, i * 8) for i in range(count)]

        # Check if this outer AFS has a nested inner AFS at entry 0 (case of MH2 DATA.BIN -> file_0000.afs)
        if 0 not in updated_entries and count <= 8 and pairs and pairs[0][1] > 64 * 1024 * 1024:
            f.seek(pairs[0][0])
            inner_magic = f.read(8)
            if inner_magic[:4] == b"AFS\x00":
                inner_base = pairs[0][0]
                inner_count = struct.unpack_from("<I", inner_magic, 4)[0]
                inner_tbl = f.read(inner_count * 8)
                inner_pairs = [struct.unpack_from("<II", inner_tbl, i * 8) for i in range(inner_count)]
                for idx, payload in updated_entries.items():
                    if 0 <= idx < inner_count:
                        off, orig_sz = inner_pairs[idx]
                        alloc_slots = ((orig_sz + 2047) // 2048) * 2048
                        if len(payload) <= alloc_slots:
                            f.seek(inner_base + off)
                            f.write(payload.ljust(alloc_slots, b"\x00"))
                            f.seek(inner_base + 8 + idx * 8 + 4)
                            f.write(struct.pack("<I", len(payload)))
                return True

        # Standard AFS update
        can_patch_in_place = True
        for idx, payload in updated_entries.items():
            if not (0 <= idx < count):
                continue
            off, orig_sz = pairs[idx]
            next_offs = [o for o, s in pairs if o > off and s > 0]
            slot_cap = (min(next_offs) - off) if next_offs else ((orig_sz + 2047) // 2048) * 2048
            if len(payload) > slot_cap:
                can_patch_in_place = False
                break

        if can_patch_in_place:
            for idx, payload in updated_entries.items():
                if 0 <= idx < count:
                    off, orig_sz = pairs[idx]
                    f.seek(off)
                    f.write(payload)
                    if len(payload) < orig_sz:
                        f.write(b"\x00" * (orig_sz - len(payload)))
                    f.seek(8 + idx * 8 + 4)
                    f.write(struct.pack("<I", len(payload)))
            return True

    # Full AFS rebuild when an entry exceeds its original sector slot
    raw_afs = afs_path.read_bytes()
    out = bytearray(raw_afs[: 8 + count * 8])
    first_off = min((o for o, s in pairs if o > 0 and s > 0), default=8 + count * 8)
    out.extend(raw_afs[len(out) : first_off])
    for idx, (off, orig_sz) in enumerate(pairs):
        if off == 0 and orig_sz == 0:
            continue
        rem = len(out) % 2048
        if rem != 0:
            out.extend(b"\x00" * (2048 - rem))
        new_off = len(out)
        payload = updated_entries[idx] if idx in updated_entries else raw_afs[off : off + orig_sz]
        struct.pack_into("<II", out, 8 + idx * 8, new_off, len(payload))
        out.extend(payload)
    afs_path.write_bytes(bytes(out))
    return True


def _propagate_hierarchy_upwards(modified_orig_paths: set[Path], root_unpacked_dir: Path) -> set[Path]:
    """
    Given a set of updated native PS2 files inside `root_unpacked_dir`, rebuild any parent
    `*_unpacked` containers (.pac, .momo) from deepest to shallowest, re-compressing
    Capcom LZ16 (`.dec.<ext>` -> `.bin`/`.pac`) along the way.
    Returns the set of top-level entry files in `root_unpacked_dir` that were updated.
    """
    active_paths = set(modified_orig_paths)

    # Collect all `*_unpacked` parent directories of active paths, sorted deepest first
    unpacked_parents: list[Path] = []
    for p in list(active_paths):
        cur = p.parent
        while cur != root_unpacked_dir and cur.is_relative_to(root_unpacked_dir):
            if cur.name.endswith("_unpacked") and cur not in unpacked_parents:
                unpacked_parents.append(cur)
            cur = cur.parent

    unpacked_parents.sort(key=lambda d: len(d.parts), reverse=True)

    for u_dir in unpacked_parents:
        stem_base = u_dir.name[: -len("_unpacked")]
        parent_dir = u_dir.parent
        # Find the archive file that produced `u_dir` (e.g. `file_0272.dec.pac` or `file_0000.momo`)
        candidates = sorted(
            p
            for p in parent_dir.glob(f"{stem_base}.*")
            if p.is_file() and p.suffix.lower() not in (".png", ".obj", ".mtl", ".wav", ".mp4")
        )
        if not candidates:
            continue
        arc_file = candidates[0]
        if repack_pac_or_momo_container(arc_file, u_dir):
            active_paths.add(arc_file)

    # Next, check if any updated file is a `.dec.<ext>` decompressed Capcom LZ16 stream
    top_updated: set[Path] = set()
    for p in list(active_paths):
        if ".dec." in p.name:
            raw_stem = p.name.split(".dec.", 1)[0]
            raw_candidates = [
                cand
                for cand in p.parent.glob(f"{raw_stem}.*")
                if cand.is_file()
                and ".dec." not in cand.name
                and cand.suffix.lower() not in (".png", ".obj", ".mtl", ".wav", ".mp4")
            ]
            if raw_candidates:
                raw_target = raw_candidates[0]
                comp_bytes = compress_capcom_lz16(p.read_bytes())
                raw_target.write_bytes(comp_bytes)
                if raw_target.parent == root_unpacked_dir:
                    top_updated.add(raw_target)
        elif p.parent == root_unpacked_dir:
            top_updated.add(p)

    return top_updated


def sync_unpacked_directory(
    unpacked_dir: Path,
    source_archive: Path | None = None,
    workers: int | None = None,
    force: bool = False,
) -> dict:
    """
    Incrementally synchronize a single `assets_unpacked/<sub_dir>` directory back into its
    source archive in `disc_files/layer0/`.
    Only files whose `(mtime_ns, size)` changed since extraction or last sync are re-encoded!
    """
    t0 = time.perf_counter()
    manifest_path = unpacked_dir / ".unpack_manifest.json"
    if not manifest_path.is_file():
        manifest = bootstrap_manifest_for_dir(unpacked_dir, source_archive=source_archive)
    else:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))

    if source_archive is None and manifest.get("sourceArchive"):
        cand = Path(manifest["sourceArchive"])
        if cand.is_file():
            source_archive = cand

    assets: dict[str, dict] = manifest.get("assets", {})
    tasks: list[dict] = []

    for rel_conv, meta in assets.items():
        conv_abs = unpacked_dir / rel_conv
        if not conv_abs.is_file():
            continue
        st = conv_abs.stat()
        if not force and st.st_mtime_ns == meta.get("mtime_ns") and st.st_size == meta.get("size"):
            continue
        orig_abs = unpacked_dir / meta["original"]
        task = {
            "rel_conv": rel_conv,
            "conv_path": str(conv_abs),
            "orig_path": str(orig_abs),
            "kind": meta["kind"],
            "width": meta.get("width"),
            "height": meta.get("height"),
            "bdOffset": meta.get("bdOffset"),
            "bdLength": meta.get("bdLength"),
            "vag_path": str(unpacked_dir / meta["vagFile"]) if meta.get("vagFile") else None,
            "new_mtime_ns": st.st_mtime_ns,
            "new_size": st.st_size,
        }
        tasks.append(task)

    if not tasks:
        return {
            "dir": str(unpacked_dir),
            "modified": 0,
            "repacked_containers": 0,
            "hd_textures": 0,
            "elapsed_ms": round((time.perf_counter() - t0) * 1000.0, 2),
        }

    max_workers = workers or min(os.cpu_count() or 4, len(tasks), 16)
    if len(tasks) == 1 or max_workers <= 1:
        results = [_convert_single_modified_task(t) for t in tasks]
    else:
        with ProcessPoolExecutor(max_workers=max_workers) as pool:
            results = list(pool.map(_convert_single_modified_task, tasks))

    modified_orig_paths: set[Path] = set()
    hd_textures = 0
    for task, res in zip(tasks, results):
        if res.get("ok"):
            assets[task["rel_conv"]]["mtime_ns"] = task["new_mtime_ns"]
            assets[task["rel_conv"]]["size"] = task["new_size"]
            if res.get("kind") == "tim2_hd_live":
                hd_textures += 1
            else:
                modified_orig_paths.add(Path(task["orig_path"]))

    top_updated = _propagate_hierarchy_upwards(modified_orig_paths, unpacked_dir)

    patched_archive = False
    if top_updated and source_archive and source_archive.is_file():
        updated_map: dict[int, bytes] = {}
        for top_p in top_updated:
            m = _ENTRY_INDEX_RE.match(top_p.stem)
            if m:
                updated_map[int(m.group(1))] = top_p.read_bytes()
        if updated_map:
            patched_archive = patch_afs_entries_in_place_or_rebuild(source_archive, updated_map)

    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    return {
        "dir": str(unpacked_dir),
        "modified": len(tasks),
        "repacked_top_entries": len(top_updated),
        "patched_archive": patched_archive,
        "hd_textures": hd_textures,
        "elapsed_ms": round((time.perf_counter() - t0) * 1000.0, 2),
    }


def _guess_source_archive_for_subdir(game_dir: Path, subdir_name: str) -> Path | None:
    """Resolve the corresponding `disc_files/layer0/` archive for a given `assets_unpacked/<subdir>`."""
    layer0 = game_dir / "disc_files" / "layer0"
    if not layer0.is_dir():
        return None
    low = subdir_name.lower()
    for cand in layer0.iterdir():
        if not cand.is_file():
            continue
        if cand.stem.lower() == low or cand.name.lower() == low:
            return cand
    if "data" in low and (layer0 / "DATA.BIN").is_file():
        return layer0 / "DATA.BIN"
    return None


def sync_game_assets(
    repo_root: Path,
    game: str,
    workers: int | None = None,
    force: bool = False,
) -> dict:
    """
    Scan all subdirectories inside `games/<game>/assets_unpacked/` and incrementally
    repack any modified 3D meshes (.obj), sounds (.wav), videos (.mp4), or 1x textures (.png)
    into `games/<game>/disc_files/layer0/`.
    Also aggregates a master `games/<game>/assets_unpacked/textures_index.tsv` for the C++ GPU runtime.
    """
    t0 = time.perf_counter()
    game_dir = repo_root / "games" / game
    unpacked_root = game_dir / "assets_unpacked"
    if not unpacked_root.is_dir():
        return {"game": game, "synced_dirs": [], "total_modified": 0, "elapsed_ms": 0.0}

    reports: list[dict] = []
    master_tsv_lines: list[str] = []

    for sub in sorted(unpacked_root.iterdir()):
        if not sub.is_dir() or sub.name.startswith("."):
            continue
        src_arc = _guess_source_archive_for_subdir(game_dir, sub.name)
        rep = sync_unpacked_directory(sub, source_archive=src_arc, workers=workers, force=force)
        reports.append(rep)

        sub_tsv = sub / "textures_index.tsv"
        if sub_tsv.is_file():
            for line in sub_tsv.read_text(encoding="utf-8").splitlines():
                parts = line.split("\t")
                if len(parts) >= 6:
                    rel_from_root = f"{sub.name}/{parts[5]}"
                    master_tsv_lines.append("\t".join(parts[:5] + [rel_from_root]))

    if master_tsv_lines:
        (unpacked_root / "textures_index.tsv").write_text(
            "\n".join(master_tsv_lines) + "\n", encoding="utf-8"
        )

    total_mod = sum(r["modified"] for r in reports)
    return {
        "game": game,
        "synced_dirs": reports,
        "total_modified": total_mod,
        "elapsed_ms": round((time.perf_counter() - t0) * 1000.0, 2),
    }
