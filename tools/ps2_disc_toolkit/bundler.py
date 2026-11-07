"""
Single-File Executable Bundler (PS2PACK1) for `ps2-native-recomp`.

Appends a 64-bit indexed `PS2PACK1` archive + 64-byte `Ps2BundleFooter` to the end
of a compiled Linux ELF (`ps2run`) or Windows PE (`ps2run.exe`) binary, and provides
multi-OS (`linux`, `windows`, `all`) build & bundling orchestration for any game.
"""

from __future__ import annotations

import concurrent.futures
import json
import os
import re
import shutil
import stat
import struct
import subprocess
import time
from typing import Dict, List, Optional, Tuple

BUNDLE_MAGIC = b"PS2PACK1"

KIND_ASSET = 0
KIND_DISC_SPAN = 1

# Matches `struct Ps2BundleFooter` in `runtime/include/ps2/disc_bundle.hpp` (64 bytes)
FOOTER_STRUCT = struct.Struct("<8sQQQQII16s")
# Matches `struct Ps2BundleTocEntryHeader` in `runtime/include/ps2/disc_bundle.hpp` (32 bytes)
TOC_ENTRY_STRUCT = struct.Struct("<BBHIQQQ")

assert FOOTER_STRUCT.size == 64
assert TOC_ENTRY_STRUCT.size == 32


def _detect_binary_os(binary_path: str) -> str:
    """Detect target OS ('windows', 'linux', or 'macos') from executable header or extension."""
    try:
        with open(binary_path, "rb") as f:
            magic = f.read(4)
        if magic.startswith(b"MZ"):
            return "windows"
        if magic == b"\x7fELF":
            return "linux"
        if magic in (
            b"\xfe\xed\xfa\xce",
            b"\xfe\xed\xfa\xcf",
            b"\xce\xfa\xed\xfe",
            b"\xcf\xfa\xed\xfe",
        ):
            return "macos"
    except OSError:
        pass
    if binary_path.lower().endswith(".exe"):
        return "windows"
    return "linux"


def _align_up_file(f, align: int) -> int:
    pos = f.tell()
    rem = pos % align
    if rem != 0:
        pad = align - rem
        f.write(b"\x00" * pad)
        pos += pad
    return pos


def _copy_file_into_bundle(src_path: str, dst_file, align: int = 64) -> Tuple[int, int]:
    offset = _align_up_file(dst_file, align)
    size = 0
    with open(src_path, "rb") as sf:
        while True:
            chunk = sf.read(4 * 1024 * 1024)
            if not chunk:
                break
            dst_file.write(chunk)
            size += len(chunk)
    return offset, size


def _write_bytes_into_bundle(payload: bytes, dst_file, align: int = 64) -> Tuple[int, int]:
    offset = _align_up_file(dst_file, align)
    dst_file.write(payload)
    return offset, len(payload)


def _patch_iso9660_dir_record_in_meta(
    meta_buf: bytearray,
    iso_path: str,
    old_sec: int,
    old_size: int,
    new_sec: int,
    new_size: int,
) -> bool:
    """
    Locate and patch an ISO-9660 directory record inside `disc_meta.bin` (`meta_buf`)
    when an extracted file in `disc_files/layer0/` has been modified in size/LBA.
    """
    base_name = os.path.basename(iso_path).upper()
    needle = struct.pack("<I", old_sec) + struct.pack(">I", old_sec) + struct.pack(
        "<I", old_size
    ) + struct.pack(">I", old_size)
    replacement = struct.pack("<I", new_sec) + struct.pack(">I", new_sec) + struct.pack(
        "<I", new_size
    ) + struct.pack(">I", new_size)

    patched = False
    search_pos = 0
    while True:
        idx = meta_buf.find(needle, search_pos)
        if idx == -1:
            break
        rec_start = idx - 2
        if rec_start >= 0 and rec_start + 34 <= len(meta_buf):
            rec_len = meta_buf[rec_start]
            name_len = meta_buf[rec_start + 32]
            if rec_len >= 33 + name_len and rec_start + rec_len <= len(meta_buf):
                raw_name = bytes(
                    meta_buf[rec_start + 33 : rec_start + 33 + name_len]
                ).decode("latin1", errors="ignore").upper()
                clean_name = raw_name.split(";")[0]
                if clean_name == base_name:
                    meta_buf[idx : idx + 16] = replacement
                    patched = True
        search_pos = idx + 1
    return patched


def _fnv1a64(h: int, data: bytes) -> int:
    for b in data:
        h = ((h ^ b) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def compute_ee_live_stamp(
    repo_root: str,
    elf_path: str,
    target_os: str = "windows",
    avx2: bool = True,
    sizeof_cpu: int = 1456,
) -> int:
    """
    Compute the 64-bit FNV-1a `ee-live` stamp matching `runtime/src/ee_live.cpp`.
    """
    h = 0xCBF29CE484222325
    h = _fnv1a64(h, b"ee-live 3")
    for hdr_name in ("cpu", "fpu", "vu", "memory", "mmi", "generated", "ee_live_abi"):
        hdr_path = os.path.join(repo_root, "runtime", "include", "ps2", f"{hdr_name}.hpp")
        with open(hdr_path, "rb") as hf:
            h = _fnv1a64(h, hf.read())
    flags = ["-std=c++20", "-O1", "-DNDEBUG", "-ffp-contract=off", "-w"]
    if avx2:
        flags.append("-mavx2")
    if target_os != "windows":
        flags.extend(["-fPIC", "-fvisibility=hidden"])
    for f in flags:
        if not f.startswith("-O"):
            h = _fnv1a64(h, f.encode("utf-8") + b"\x00")
    with open(elf_path, "rb") as ef:
        h = _fnv1a64(h, ef.read())
    h = _fnv1a64(h, struct.pack("<Q", sizeof_cpu))
    return h


def cross_compile_ee_live_for_windows(
    repo_root: str,
    elf_path: str,
    linux_ee_live_dir: str,
    win_ee_live_dir: str,
) -> int:
    """
    Cross-compile preserved `work-<id>/src/*.cpp` directories from `linux_ee_live_dir`
    into self-contained Windows x86_64 `.dll`s in `win_ee_live_dir`.
    """
    if not os.path.isdir(linux_ee_live_dir):
        return 0
    cxx = os.path.join(
        repo_root, "third_party", "llvm-mingw", "bin", "x86_64-w64-mingw32-clang++"
    )
    if not os.path.isfile(cxx):
        return 0

    win_stamp = compute_ee_live_stamp(repo_root, elf_path, target_os="windows", avx2=True)
    win_stamp_hex = f"0x{win_stamp:016X}ull"
    win_prefix = f"{win_stamp >> 32:08x}"

    os.makedirs(win_ee_live_dir, exist_ok=True)
    inc_dir = os.path.join(linux_ee_live_dir, "include")
    if not os.path.isdir(inc_dir):
        inc_dir = os.path.join(repo_root, "runtime", "include")

    # Copy any .mark files with the Windows stamp prefix
    for fname in sorted(os.listdir(linux_ee_live_dir)):
        if fname.endswith(".mark") and "-whole-" in fname:
            rest = fname.split("-", 1)[1]
            dst_mark = os.path.join(win_ee_live_dir, f"{win_prefix}-{rest}")
            if not os.path.isfile(dst_mark):
                shutil.copy2(os.path.join(linux_ee_live_dir, fname), dst_mark)

    candidate_lib_ids: list[str] = []
    seen_lib_ids: set[str] = set()
    for fname in sorted(os.listdir(linux_ee_live_dir)):
        if fname.endswith(".so") and "-" in fname[:-3]:
            lid = fname[:-3].split("-", 1)[1]
            if lid not in seen_lib_ids:
                seen_lib_ids.add(lid)
                candidate_lib_ids.append(lid)
        elif fname.startswith("work-"):
            lid = fname[5:]
            if lid not in seen_lib_ids and os.path.isfile(
                os.path.join(linux_ee_live_dir, fname, "src", "table.cpp")
            ):
                seen_lib_ids.add(lid)
                candidate_lib_ids.append(lid)

    built_count = 0
    for lib_id in candidate_lib_ids:
        dll_path = os.path.join(win_ee_live_dir, f"{win_prefix}-{lib_id}.dll")
        if os.path.isfile(dll_path):
            built_count += 1
            continue
        src_dir = os.path.join(linux_ee_live_dir, f"work-{lib_id}", "src")
        if not os.path.isdir(src_dir):
            continue

        tmp_work = os.path.join(win_ee_live_dir, f".tmp-work-{lib_id}")
        if os.path.exists(tmp_work):
            shutil.rmtree(tmp_work)
        shutil.copytree(src_dir, tmp_work)

        table_cpp = os.path.join(tmp_work, "table.cpp")
        if os.path.isfile(table_cpp):
            with open(table_cpp, "r", encoding="utf-8") as tf:
                txt = tf.read()
            txt = re.sub(
                r"ps2_ee_live_stamp\(\)\s*\{\s*return\s+0x[0-9A-Fa-f]+ull;\s*\}",
                f"ps2_ee_live_stamp() {{ return {win_stamp_hex}; }}",
                txt,
            )
            with open(table_cpp, "w", encoding="utf-8") as tf:
                tf.write(txt)

        opt = "-O0" if lib_id.endswith("q") else "-O1"
        cpp_files = sorted(
            [
                os.path.join(tmp_work, f)
                for f in os.listdir(tmp_work)
                if f.endswith(".cpp")
            ],
            key=lambda p: os.path.getsize(p),
            reverse=True,
        )
        flags = [
            "-std=c++20",
            opt,
            "-DNDEBUG",
            "-ffp-contract=off",
            "-w",
            "-mavx2",
            f"-I{inc_dir}",
        ]

        def _compile_unit(cpp_path: str) -> str:
            obj_path = cpp_path[:-4] + ".obj"
            subprocess.run(
                [cxx, *flags, "-c", cpp_path, "-o", obj_path],
                check=True,
            )
            return obj_path

        with concurrent.futures.ThreadPoolExecutor(max_workers=8) as ex:
            objs = list(ex.map(_compile_unit, cpp_files))

        subprocess.run(
            [
                cxx,
                "-shared",
                "-DNDEBUG",
                "-ffp-contract=off",
                "-w",
                "-mavx2",
                "-static",
                "-static-libgcc",
                "-static-libstdc++",
                *objs,
                "-o",
                dll_path,
            ],
            check=True,
        )
        shutil.rmtree(tmp_work, ignore_errors=True)
        print(
            f"[ps2-bundler] Cross-compiled Windows ee-live DLL: {os.path.basename(dll_path)} "
            f"({os.path.getsize(dll_path):,} bytes)"
        )
        built_count += 1
    return built_count


def ensure_windows_binary(repo_root: str, game_name: str) -> str:
    """
    Ensure `build/win-<game_name>/ps2run.exe` is compiled using `cmake/win-x86_64-mingw.cmake`.
    """
    win_build_dir = os.path.join(repo_root, "build", f"win-{game_name}")
    exe_path = os.path.join(win_build_dir, "ps2run.exe")
    toolchain_file = os.path.join(repo_root, "cmake", "win-x86_64-mingw.cmake")
    generated_dir = os.path.join(repo_root, "generated", game_name)
    cxx = os.path.join(
        repo_root, "third_party", "llvm-mingw", "bin", "x86_64-w64-mingw32-clang++"
    )
    if not os.path.isfile(cxx):
        fetch_script = os.path.join(repo_root, "scripts", "fetch_win_cross.sh")
        if os.path.isfile(fetch_script):
            print("[ps2-bundler] Fetching Windows cross-compilation toolchain...")
            subprocess.run(["sh", fetch_script], cwd=repo_root, check=True)
    if not os.path.isfile( os.path.join(win_build_dir, "build.ninja")):
        print(f"[ps2-bundler] Configuring Windows x86_64 build in {win_build_dir}...")
        subprocess.run(
            [
                "cmake",
                "-B",
                win_build_dir,
                "-G",
                "Ninja",
                "-DCMAKE_BUILD_TYPE=Release",
                f"-DCMAKE_TOOLCHAIN_FILE={toolchain_file}",
                "-DCMAKE_CXX_FLAGS=-mavx2 -ffp-contract=off",
                f"-DPS2_GENERATED={generated_dir}",
            ],
            cwd=repo_root,
            check=True,
        )
    print(f"[ps2-bundler] Building Windows executable {exe_path}...")
    subprocess.run(["ninja", "-C", win_build_dir, "ps2run"], cwd=repo_root, check=True)
    if not os.path.isfile(exe_path):
        raise FileNotFoundError(f"Failed to produce Windows binary: {exe_path}")
    return exe_path


def build_single_file_bundle(
    binary_path: str,
    disc_dir: str,
    output_path: str,
    elf_path: Optional[str] = None,
    translations_dir: Optional[str] = None,
    ee_live_dir: Optional[str] = None,
    vu1_live_dir: Optional[str] = None,
    game_id: Optional[str] = None,
) -> Dict[str, object]:
    """
    Create a self-contained single-file executable (`output_path`) by concatenating
    `binary_path` with a `PS2PACK1` payload containing:
      - `boot.elf` and `boot_name.txt`
      - `translations/*.json` (optional)
      - `ee-live/*` and `vu1-live/*` pre-compiled cache libraries matching the binary's OS
      - `disc_meta.bin` and all virtual disc files from `disc_dir` (with automatic
        ISO-9660 size/LBA patching if any extracted file was modified on disk)
    """
    t0 = time.time()
    if not os.path.isfile(binary_path):
        raise FileNotFoundError(f"Runner binary not found: {binary_path}")
    target_os = _detect_binary_os(binary_path)
    allowed_lib_exts = (
        (".dll", ".mark")
        if target_os == "windows"
        else ((".dylib", ".mark") if target_os == "macos" else (".so", ".mark"))
    )

    manifest_path = os.path.join(disc_dir, "disc_manifest.tsv")
    meta_path = os.path.join(disc_dir, "disc_meta.bin")
    info_path = os.path.join(disc_dir, "disc_info.json")
    if not os.path.isfile(manifest_path) or not os.path.isfile(meta_path):
        raise FileNotFoundError(
            f"Virtual disc folder is missing disc_manifest.tsv or disc_meta.bin: {disc_dir}"
        )

    disc_info: Dict[str, object] = {}
    if os.path.isfile(info_path):
        with open(info_path, "r", encoding="utf-8") as jf:
            disc_info = json.load(jf)

    boot_elf_rel = str(disc_info.get("boot_elf") or "")
    boot_raw = str(disc_info.get("boot_raw") or "")
    detected_game_id = str(game_id or disc_info.get("disc_id") or "ps2game")

    if not elf_path:
        if boot_elf_rel:
            candidate = os.path.join(disc_dir, "layer0", boot_elf_rel)
            if os.path.isfile(candidate):
                elf_path = candidate
    if not elf_path or not os.path.isfile(elf_path):
        raise FileNotFoundError(
            f"Could not locate guest boot ELF (pass --elf explicitly or ensure layer0/{boot_elf_rel} exists)"
        )
    if not boot_raw:
        boot_name = boot_elf_rel or os.path.basename(elf_path)
        boot_raw = f"cdrom0:\\{boot_name};1"

    os.makedirs(os.path.dirname(os.path.abspath(output_path)) or ".", exist_ok=True)
    shutil.copyfile(binary_path, output_path)

    # Tuple layout: (kind, disk_offset, bundle_offset, byte_length, vpath)
    toc_entries: List[Tuple[int, int, int, int, str]] = []

    total_sectors = 0
    layer1_start = 0
    meta_spans: List[Tuple[int, int, int]] = []
    file_rows: List[Tuple[int, int, int, str, str]] = []

    with open(manifest_path, "r", encoding="utf-8") as mf:
        for raw_line in mf:
            line = raw_line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split("\t")
            if parts[0] == "TOTAL_SECTORS":
                total_sectors = int(parts[1])
                if len(parts) >= 4:
                    layer1_start = int(parts[3])
            elif parts[0] == "META":
                meta_spans.append((int(parts[1]), int(parts[2]), int(parts[3])))
            elif parts[0] == "FILE":
                file_rows.append(
                    (int(parts[1]), int(parts[2]), int(parts[3]), parts[4], parts[5])
                )

    with open(meta_path, "rb") as mf_bin:
        meta_buf = bytearray(mf_bin.read())

    # Check if any extracted file in disc_dir was modified in size by the user (modding)
    resolved_file_rows: List[Tuple[int, int, int, str, str]] = []
    modified_files_count = 0
    for layer_idx, sec, manifest_size, iso_path, local_rel in file_rows:
        src_file = os.path.join(disc_dir, local_rel)
        if not os.path.isfile(src_file):
            if manifest_size == 0:
                continue
            raise FileNotFoundError(
                f"Missing virtual disc file referenced by manifest: {src_file}"
            )
        actual_size = os.path.getsize(src_file)
        new_sec = sec
        if actual_size != manifest_size:
            modified_files_count += 1
            old_sectors = (manifest_size + 2047) // 2048
            new_sectors = (actual_size + 2047) // 2048
            if new_sectors > old_sectors:
                new_sec = total_sectors
                total_sectors += new_sectors
            _patch_iso9660_dir_record_in_meta(
                meta_buf, iso_path, sec, manifest_size, new_sec, actual_size
            )
        resolved_file_rows.append((layer_idx, new_sec, actual_size, iso_path, local_rel))

    with open(output_path, "ab") as out:
        exe_size = out.tell()
        bundle_start = _align_up_file(out, 4096)

        # 1. Embed boot.elf & boot_name.txt
        boot_off, boot_sz = _copy_file_into_bundle(elf_path, out, align=64)
        toc_entries.append((KIND_ASSET, 0, boot_off, boot_sz, "boot.elf"))

        boot_arg_off, boot_arg_sz = _write_bytes_into_bundle(
            boot_raw.encode("utf-8"), out, align=16
        )
        toc_entries.append((KIND_ASSET, 0, boot_arg_off, boot_arg_sz, "boot_name.txt"))

        # 2. Embed translation JSON dictionaries if provided
        trans_count = 0
        if translations_dir and os.path.isdir(translations_dir):
            for fname in sorted(os.listdir(translations_dir)):
                if not fname.endswith(".json"):
                    continue
                fpath = os.path.join(translations_dir, fname)
                if not os.path.isfile(fpath):
                    continue
                toff, tsz = _copy_file_into_bundle(fpath, out, align=16)
                toc_entries.append(
                    (KIND_ASSET, 0, toff, tsz, f"translations/{fname}")
                )
                trans_count += 1

        # 3. Embed pre-compiled ee-live and vu1-live cache files matching the binary's target OS
        ee_live_count = 0
        if ee_live_dir and os.path.isdir(ee_live_dir):
            for fname in sorted(os.listdir(ee_live_dir)):
                if not fname.endswith(allowed_lib_exts):
                    continue
                fpath = os.path.join(ee_live_dir, fname)
                if not os.path.isfile(fpath):
                    continue
                loff, lsz = _copy_file_into_bundle(fpath, out, align=64)
                toc_entries.append((KIND_ASSET, 0, loff, lsz, f"ee-live/{fname}"))
                ee_live_count += 1

        vu1_live_count = 0
        if vu1_live_dir and os.path.isdir(vu1_live_dir):
            for fname in sorted(os.listdir(vu1_live_dir)):
                if not fname.endswith(allowed_lib_exts[0]):
                    continue
                fpath = os.path.join(vu1_live_dir, fname)
                if not os.path.isfile(fpath):
                    continue
                voff, vsz = _copy_file_into_bundle(fpath, out, align=64)
                toc_entries.append((KIND_ASSET, 0, voff, vsz, f"vu1-live/{fname}"))
                vu1_live_count += 1

        # 4. Embed disc_meta.bin (aligned to 2048-byte sector boundary, with any modded file patches)
        meta_bundle_off, _ = _write_bytes_into_bundle(bytes(meta_buf), out, align=2048)
        for sec, cnt, m_off in meta_spans:
            toc_entries.append(
                (
                    KIND_DISC_SPAN,
                    sec * 2048,
                    meta_bundle_off + m_off,
                    cnt * 2048,
                    "__META__",
                )
            )

        # 5. Embed all virtual disc files (sector-aligned to 2048 bytes)
        written_files: Dict[str, Tuple[int, int]] = {}
        disc_file_count = 0
        for layer_idx, sec, size_b, iso_path, local_rel in resolved_file_rows:
            if size_b <= 0:
                continue
            if local_rel in written_files:
                foff, fsz = written_files[local_rel]
            else:
                src_file = os.path.join(disc_dir, local_rel)
                foff, fsz = _copy_file_into_bundle(src_file, out, align=2048)
                written_files[local_rel] = (foff, fsz)
            toc_entries.append(
                (KIND_DISC_SPAN, sec * 2048, foff, size_b, iso_path)
            )
            disc_file_count += 1

        # 6. Serialize TOC
        toc_offset = _align_up_file(out, 64)
        toc_chunks: List[bytes] = []
        for kind, disk_off, b_off, byte_len, vpath in toc_entries:
            vpath_bytes = vpath.encode("utf-8")
            hdr = TOC_ENTRY_STRUCT.pack(
                kind,
                0,
                0,
                len(vpath_bytes),
                disk_off,
                b_off,
                byte_len,
            )
            toc_chunks.append(hdr + vpath_bytes)
        toc_blob = b"".join(toc_chunks)
        out.write(toc_blob)
        toc_size = len(toc_blob)

        # 7. Write 64-byte Ps2BundleFooter
        game_id_bytes = detected_game_id.encode("ascii", errors="ignore")[:15].ljust(
            16, b"\x00"
        )
        footer = FOOTER_STRUCT.pack(
            BUNDLE_MAGIC,
            bundle_start,
            toc_offset,
            toc_size,
            total_sectors,
            layer1_start,
            len(toc_entries),
            game_id_bytes,
        )
        out.write(footer)
        final_size = out.tell()

    # Ensure executable permission bit is set on POSIX
    st = os.stat(output_path)
    os.chmod(output_path, st.st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)

    elapsed = time.time() - t0
    summary = {
        "output_path": output_path,
        "target_os": target_os,
        "exe_size_bytes": exe_size,
        "final_size_bytes": final_size,
        "game_id": detected_game_id,
        "boot_raw": boot_raw,
        "total_sectors": total_sectors,
        "toc_count": len(toc_entries),
        "disc_files": disc_file_count,
        "modified_disc_files": modified_files_count,
        "translation_files": trans_count,
        "ee_live_files": ee_live_count,
        "vu1_live_files": vu1_live_count,
        "elapsed_sec": elapsed,
    }
    mod_note = f" ({modified_files_count} modded size/LBA patched)" if modified_files_count else ""
    print(
        f"[ps2-bundler] Built single-file executable ({target_os}): {output_path}\n"
        f"  - Base runner : {exe_size / (1 << 20):.2f} MiB\n"
        f"  - Total size  : {final_size / (1 << 30):.2f} GiB ({final_size:,} bytes)\n"
        f"  - Game ID     : {detected_game_id} ({boot_raw}, {total_sectors} sectors)\n"
        f"  - Embedded    : {disc_file_count} disc files{mod_note}, {trans_count} translation JSONs, "
        f"{ee_live_count} ee-live files, {vu1_live_count} vu1-live files\n"
        f"  - Completed in {elapsed:.1f}s"
    )
    return summary


def bundle_game_targets(
    repo_root: str,
    game_name: Optional[str] = None,
    target_os: str = "auto",
    binary_path: Optional[str] = None,
    disc_dir: Optional[str] = None,
    output_path: Optional[str] = None,
    elf_path: Optional[str] = None,
    translations_dir: Optional[str] = None,
    ee_live_dir: Optional[str] = None,
    vu1_live_dir: Optional[str] = None,
    game_id: Optional[str] = None,
) -> List[Dict[str, object]]:
    """
    High-level multi-OS bundler:
      - If `--binary` is explicitly provided (and `--os` is `auto`), bundles that single binary into `--out`.
      - If `--game <name>` is provided (with `--os linux|windows|all`), resolves the game's extracted
        `disc_files/`, optional `extensions/<name>/translations/`, and per-OS binaries & `ee-live` caches,
        automatically cross-compiling the Windows runner and `ee-live` DLLs if needed.
    """
    if binary_path and target_os == "auto":
        if not disc_dir and game_name:
            disc_dir = os.path.join(repo_root, "games", game_name, "disc_files")
        if not disc_dir or not output_path:
            raise ValueError("--disc and --out are required when bundling an explicit --binary")
        return [
            build_single_file_bundle(
                binary_path=binary_path,
                disc_dir=disc_dir,
                output_path=output_path,
                elf_path=elf_path,
                translations_dir=translations_dir,
                ee_live_dir=ee_live_dir,
                vu1_live_dir=vu1_live_dir,
                game_id=game_id or game_name,
            )
        ]

    if not game_name:
        raise ValueError("Provide either --game <name> [--os linux|windows|all] or explicit --binary/--disc/--out")

    resolved_disc = disc_dir or os.path.join(repo_root, "games", game_name, "disc_files")
    resolved_trans = translations_dir
    if resolved_trans is None:
        cand_trans = os.path.join(repo_root, "extensions", game_name, "translations")
        if os.path.isdir(cand_trans):
            resolved_trans = cand_trans

    info_path = os.path.join(resolved_disc, "disc_info.json")
    boot_elf_rel = ""
    if os.path.isfile(info_path):
        with open(info_path, "r", encoding="utf-8") as jf:
            boot_elf_rel = str(json.load(jf).get("boot_elf") or "")
    resolved_elf = elf_path or (
        os.path.join(resolved_disc, "layer0", boot_elf_rel) if boot_elf_rel else None
    )

    os_list = ["linux", "windows"] if target_os in ("all", "auto") else [target_os.lower()]
    base_prefix = output_path or os.path.join(repo_root, "dist", game_name)
    # Strip trailing OS suffix if user passed e.g. dist/MonsterHunter2-Windows-x86_64.exe
    for suffix in ("-Linux-x86_64", "-Windows-x86_64.exe", ".exe"):
        if len(os_list) > 1 and base_prefix.endswith(suffix):
            base_prefix = base_prefix[: -len(suffix)]
            break

    results: List[Dict[str, object]] = []
    for os_target in os_list:
        if os_target == "linux":
            bin_p = binary_path or os.path.join(repo_root, "build", f"native-{game_name}", "ps2run")
            ee_dir = ee_live_dir or os.path.join(repo_root, "games", game_name, "ee-live")
            vu1_dir = vu1_live_dir or os.path.join(repo_root, "games", game_name, "vu1-live")
            if len(os_list) == 1 and output_path and not os.path.isdir(output_path):
                out_p = output_path
            else:
                out_p = f"{base_prefix}-Linux-x86_64"
        elif os_target == "windows":
            bin_p = binary_path or ensure_windows_binary(repo_root, game_name)
            linux_ee = os.path.join(repo_root, "games", game_name, "ee-live")
            win_ee = ee_live_dir or os.path.join(repo_root, "games", game_name, "ee-live-win")
            if resolved_elf and os.path.isfile(resolved_elf) and os.path.isdir(linux_ee):
                cross_compile_ee_live_for_windows(repo_root, resolved_elf, linux_ee, win_ee)
            ee_dir = win_ee
            vu1_dir = vu1_live_dir or os.path.join(repo_root, "games", game_name, "vu1-live-win")
            if len(os_list) == 1 and output_path and not os.path.isdir(output_path):
                out_p = output_path if output_path.lower().endswith(".exe") else f"{output_path}.exe"
            else:
                out_p = f"{base_prefix}-Windows-x86_64.exe"
        else:
            raise ValueError(f"Unsupported --os target: {os_target!r} (expected linux, windows, or all)")

        results.append(
            build_single_file_bundle(
                binary_path=bin_p,
                disc_dir=resolved_disc,
                output_path=out_p,
                elf_path=resolved_elf,
                translations_dir=resolved_trans,
                ee_live_dir=ee_dir,
                vu1_live_dir=vu1_dir,
                game_id=game_id or game_name,
            )
        )
    return results
