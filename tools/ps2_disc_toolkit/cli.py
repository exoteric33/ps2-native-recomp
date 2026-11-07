#!/usr/bin/env python3
"""
Unified CLI and API for the independent PS2 Disc Extraction & Packaging Toolkit.
Commands:
  inspect <image>
  extract <image> <out_dir>
  verify  <image> <out_dir>
  package --binary <bin> --disc <image_or_dir> --out <dist_dir>
  port    <name> <image_or_dir> [options]
"""

import argparse
from collections import Counter
import glob
import json
import os
import re
import shutil
import stat
import struct
import subprocess
import sys
import time

if __package__ in (None, ''):
    sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    from ps2_disc_toolkit.containers import open_container, SECTOR_SIZE
    from ps2_disc_toolkit.iso9660 import IsoFileSystem, FileEntry, is_inside_directory, parse_system_cnf
    from ps2_disc_toolkit.profiles import (
        load_profile,
        match_profile,
        collect_non_file_gaps,
        scan_non_zero_runs,
    )
else:
    from .containers import open_container, SECTOR_SIZE
    from .iso9660 import IsoFileSystem, FileEntry, is_inside_directory, parse_system_cnf
    from .profiles import (
        load_profile,
        match_profile,
        collect_non_file_gaps,
        scan_non_zero_runs,
    )


def resolve_profile(profile_arg, syscnf, files):
    if profile_arg and profile_arg != 'auto':
        return load_profile(profile_arg)
    return match_profile(syscnf.disc_id, files)


def inspect_disc(image_path, profile_arg='auto', scan_gaps=False):
    with open_container(image_path) as dev:
        initial_prof = load_profile(profile_arg) if (profile_arg and profile_arg != 'auto') else None
        l1_override = initial_prof.layer1_pvd_sector if initial_prof else None
        fs = IsoFileSystem(dev, layer1_pvd_sector=l1_override)
        syscnf = fs.read_system_cnf()
        files = fs.list_files()
        prof = initial_prof or match_profile(syscnf.disc_id, files)

        gaps = collect_non_file_gaps(dev.total_sectors, files, prof.custom_regions)
        total_gap_sectors = sum(e - s for s, e in gaps)

        print(f"=== PS2 Disc Inspection : {os.path.basename(image_path)} ===")
        print(f"Container Format : {dev.format_name} (raw sector = {dev.raw_sector_size} B, logical = {SECTOR_SIZE} B)")
        print(f"Total Sectors    : {dev.total_sectors} ({dev.logical_size / (1 << 20):.2f} MiB logical)")
        if syscnf.boot_elf:
            print(f"Disc ID          : {syscnf.disc_id}")
            print(f"Boot Executable  : {syscnf.boot_elf} ({syscnf.boot_raw})")
            print(f"Video Mode / Ver : {syscnf.vmode or 'UNKNOWN'} / {syscnf.version or '1.00'}")
        else:
            print("SYSTEM.CNF       : Not found")

        print(f"Matched Profile  : {prof.name} ({prof.description})")
        print(f"Layers Detected  : {len(fs.layers)}")
        for layer in fs.layers:
            l_files = [f for f in files if f.layer == layer.index]
            l_bytes = sum(f.size for f in l_files)
            repair_note = " [PVD root_size=0 auto-repaired from '.' entry]" if layer.pvd_root_size_was_zero else ""
            print(
                f"  - Layer {layer.index}: base={layer.base_sector}, sectors={layer.volume_sectors}, "
                f"volume='{layer.volume_id}', files={len(l_files)} ({l_bytes / (1 << 20):.2f} MiB){repair_note}"
            )

        ext_counts = Counter(os.path.splitext(f.path)[1].upper() or '<NONE>' for f in files)
        top_exts = ', '.join(f"{ext}:{cnt}" for ext, cnt in ext_counts.most_common(6))
        print(f"Total ISO Files  : {len(files)} ({top_exts})")
        print(f"Non-File Sectors : {total_gap_sectors} sectors ({total_gap_sectors * SECTOR_SIZE / (1 << 20):.2f} MiB across {len(gaps)} gap(s))")

        if scan_gaps:
            runs = scan_non_zero_runs(dev, gaps)
            thresh = prof.raw_region_threshold_sectors
            large = [(s, c) for s, c in runs if c >= thresh]
            small = [(s, c) for s, c in runs if c < thresh]
            print(f"  - System/Dir Metadata Runs (< {prof.raw_region_threshold_mb:.1f} MiB): {len(small)} run(s), {sum(c for _, c in small)} sectors")
            print(f"  - Unindexed Hidden-LBA Regions (>= {prof.raw_region_threshold_mb:.1f} MiB): {len(large)} region(s), {sum(c for _, c in large)} sectors")


def extract_disc(image_path, out_dir, profile_arg='auto', raw_threshold_mb=None, skip_existing=False):
    t0 = time.time()
    with open_container(image_path) as dev:
        initial_prof = load_profile(profile_arg) if (profile_arg and profile_arg != 'auto') else None
        l1_override = initial_prof.layer1_pvd_sector if initial_prof else None
        fs = IsoFileSystem(dev, layer1_pvd_sector=l1_override)
        syscnf = fs.read_system_cnf()
        files = fs.list_files()
        prof = initial_prof or match_profile(syscnf.disc_id, files)
        if raw_threshold_mb is not None:
            prof.raw_region_threshold_mb = float(raw_threshold_mb)

        total_sectors = dev.total_sectors
        layer1_start = fs.layers[1].base_sector if len(fs.layers) > 1 else 0

        print(
            f"[ps2-disc-toolkit] {image_path} [{dev.format_name}]: "
            f"{len(files)} ISO files, {len(fs.layers)} layer(s), {total_sectors} sectors "
            f"(profile: {prof.name})"
        )

        os.makedirs(out_dir, exist_ok=True)
        meta_path = os.path.join(out_dir, 'disc_meta.bin')
        manifest_path = os.path.join(out_dir, 'disc_manifest.tsv')
        info_path = os.path.join(out_dir, 'disc_info.json')

        gaps = collect_non_file_gaps(total_sectors, files, prof.custom_regions)
        non_zero_runs = scan_non_zero_runs(dev, gaps)
        thresh_sectors = prof.raw_region_threshold_sectors

        meta_spans = []
        unindexed_entries = []
        meta_offset = 0

        # Extract custom profile-defined hidden LBA regions first
        for reg in prof.custom_regions:
            rel_path = f"unindexed/{reg.name}"
            if not is_inside_directory(out_dir, rel_path):
                sys.exit(f"Refusing to write outside {out_dir}: {rel_path!r}")
            dest_path = os.path.join(out_dir, rel_path)
            entry = FileEntry(path=rel_path, layer=0, sector=reg.start_sector, size=reg.size_bytes)
            if not (skip_existing and os.path.isfile(dest_path) and os.path.getsize(dest_path) == reg.size_bytes):
                fs.copy_file_to_disk(entry, dest_path)
            unindexed_entries.append((0, reg.start_sector, reg.size_bytes, f"__RAW__{reg.name}", rel_path))

        # Partition non-zero non-file runs into small metadata spans vs large unindexed hidden-LBA regions
        with open(meta_path, 'wb') as meta_out:
            for run_start, run_count in non_zero_runs:
                if run_count >= thresh_sectors:
                    size_bytes = run_count * SECTOR_SIZE
                    reg_name = f"region_{run_start}_{run_count}.bin"
                    rel_path = f"unindexed/{reg_name}"
                    dest_path = os.path.join(out_dir, rel_path)
                    entry = FileEntry(path=rel_path, layer=0, sector=run_start, size=size_bytes)
                    if not (skip_existing and os.path.isfile(dest_path) and os.path.getsize(dest_path) == size_bytes):
                        fs.copy_file_to_disk(entry, dest_path)
                    unindexed_entries.append((0, run_start, size_bytes, f"__UNINDEXED_{run_start}__", rel_path))
                else:
                    s = run_start
                    rem = run_count
                    while rem > 0:
                        c = min(rem, 2048)
                        meta_out.write(dev.read_sectors(s, c))
                        s += c
                        rem -= c
                    meta_spans.append((run_start, run_count, meta_offset))
                    meta_offset += run_count * SECTOR_SIZE

        print(
            f"[ps2-disc-toolkit] wrote {meta_offset / 1024:.1f} KiB of system/directory metadata "
            f"({len(meta_spans)} spans) and {len(unindexed_entries)} unindexed region(s)"
        )

        extracted_count = 0
        reused_count = 0
        total_file_bytes = 0

        with open(manifest_path, 'w', encoding='utf-8', newline='\n') as mf:
            mf.write('# ps2-disc-manifest v1\n')
            mf.write(f'# container={dev.format_name} disc_id={syscnf.disc_id} boot_elf={syscnf.boot_elf} profile={prof.name}\n')
            mf.write(f'TOTAL_SECTORS\t{total_sectors}\tLAYER1_START\t{layer1_start}\n')
            for start_sec, count_sec, m_off in meta_spans:
                mf.write(f'META\t{start_sec}\t{count_sec}\t{m_off}\n')
            for layer_idx, start_sec, size_b, tag_name, rel_path in unindexed_entries:
                mf.write(f'FILE\t{layer_idx}\t{start_sec}\t{size_b}\t{tag_name}\t{rel_path}\n')

            for idx, entry in enumerate(files, 1):
                local_rel = f"layer{entry.layer}/{entry.path}"
                if not is_inside_directory(out_dir, local_rel):
                    sys.exit(f"Refusing to write outside {out_dir}: {local_rel!r}")
                dest_path = os.path.join(out_dir, local_rel)
                if skip_existing and os.path.isfile(dest_path) and os.path.getsize(dest_path) == entry.size:
                    reused_count += 1
                else:
                    fs.copy_file_to_disk(entry, dest_path)
                    extracted_count += 1
                total_file_bytes += entry.size
                mf.write(f'FILE\t{entry.layer}\t{entry.sector}\t{entry.size}\t{entry.path}\t{local_rel}\n')
                if idx % 100 == 0 or idx == len(files):
                    elapsed = max(time.time() - t0, 0.001)
                    print(f"[ps2-disc-toolkit] {idx}/{len(files)} files ({total_file_bytes / (1 << 30):.2f} GiB) in {elapsed:.1f}s")

        disc_info = {
            'container_format': dev.format_name,
            'raw_sector_size': dev.raw_sector_size,
            'total_sectors': total_sectors,
            'layer1_start': layer1_start,
            'disc_id': syscnf.disc_id,
            'boot_elf': syscnf.boot_elf,
            'boot_raw': syscnf.boot_raw,
            'vmode': syscnf.vmode,
            'version': syscnf.version,
            'profile': prof.name,
            'iso_files': len(files),
            'unindexed_regions': len(unindexed_entries),
        }
        with open(info_path, 'w', encoding='utf-8', newline='\n') as jf:
            json.dump(disc_info, jf, indent=2)
            jf.write('\n')

        print(f"[ps2-disc-toolkit] complete: {extracted_count} extracted, {reused_count} reused -> {out_dir}")


def verify_disc(image_path, out_dir):
    with open_container(image_path) as dev:
        manifest_path = os.path.join(out_dir, 'disc_manifest.tsv')
        meta_path = os.path.join(out_dir, 'disc_meta.bin')

        spans = []
        total_sectors = 0
        layer1_start = 0
        file_sectors = []

        with open(manifest_path, 'r', encoding='utf-8') as mf:
            for line in mf:
                line = line.strip()
                if not line or line.startswith('#'):
                    continue
                parts = line.split('\t')
                if parts[0] == 'TOTAL_SECTORS':
                    total_sectors = int(parts[1])
                    if len(parts) >= 4:
                        layer1_start = int(parts[3])
                elif parts[0] == 'META':
                    sec, cnt, off = int(parts[1]), int(parts[2]), int(parts[3])
                    spans.append((sec * SECTOR_SIZE, cnt * SECTOR_SIZE, meta_path, off))
                elif parts[0] == 'FILE':
                    sec, size, local_rel = int(parts[2]), int(parts[3]), parts[5]
                    if size > 0:
                        spans.append((sec * SECTOR_SIZE, size, os.path.join(out_dir, local_rel), 0))
                        file_sectors.append(sec)
                        last_s = sec + (size - 1) // SECTOR_SIZE
                        if last_s != sec:
                            file_sectors.append(last_s)
        spans.sort(key=lambda x: x[0])

        if total_sectors != dev.total_sectors:
            sys.exit(f"[verify] total_sectors mismatch: manifest={total_sectors} vs container={dev.total_sectors}")

        def read_virtual(sector, count):
            req_start = sector * SECTOR_SIZE
            req_end = req_start + count * SECTOR_SIZE
            buf = bytearray(count * SECTOR_SIZE)
            for d_off, length, fpath, f_off in spans:
                if d_off + length <= req_start:
                    continue
                if d_off >= req_end:
                    break
                ov_s = max(req_start, d_off)
                ov_e = min(req_end, d_off + length)
                if ov_e > ov_s:
                    with open(fpath, 'rb') as fp:
                        fp.seek(f_off + (ov_s - d_off))
                        buf[ov_s - req_start:ov_e - req_start] = fp.read(ov_e - ov_s)
            return bytes(buf)

        test_sectors = {0, 16, total_sectors - 1}
        if layer1_start > 0:
            test_sectors.update({layer1_start, layer1_start + 16})
        if total_sectors > (1 << 21):
            test_sectors.update({(1 << 21) - 1, 1 << 21, (1 << 21) + 1})
        # Sample up to 16 file boundary sectors across the disc
        if file_sectors:
            step = max(1, len(file_sectors) // 16)
            test_sectors.update(file_sectors[::step][:16])

        sorted_test = sorted(s for s in test_sectors if 0 <= s < total_sectors)
        for s in sorted_test:
            orig_bytes = dev.read_sectors(s, 1)
            virt_bytes = read_virtual(s, 1)
            if orig_bytes != virt_bytes:
                sys.exit(f"[verify] MISMATCH at sector {s}!")

        print(f"[verify] SUCCESS: all {len(sorted_test)} sampled sectors match {dev.format_name} container 100% bit-for-bit.")


def detect_extracted_boot_elf(data_dir):
    """Find the guest boot ELF path inside an extracted virtual disc directory via disc_info.json or SYSTEM.CNF."""
    info_path = os.path.join(data_dir, 'disc_info.json')
    boot_elf_name = None
    if os.path.isfile(info_path):
        with open(info_path, 'r', encoding='utf-8') as jf:
            info = json.load(jf)
            boot_elf_name = info.get('boot_elf')

    if not boot_elf_name:
        syscnf_candidates = [
            os.path.join(data_dir, 'layer0', 'SYSTEM.CNF'),
            os.path.join(data_dir, 'SYSTEM.CNF'),
        ]
        for cand in syscnf_candidates:
            if os.path.isfile(cand):
                with open(cand, 'r', encoding='utf-8', errors='replace') as sf:
                    boot_elf_name = parse_system_cnf(sf.read()).boot_elf
                break

    if not boot_elf_name:
        return None

    for root_cand in (os.path.join(data_dir, 'layer0'), data_dir):
        if not os.path.isdir(root_cand):
            continue
        for entry in os.listdir(root_cand):
            if entry.upper() == boot_elf_name.upper():
                return os.path.join(root_cand, entry)
    return None


def package_game(binary_path, iso_or_disc_dir, output_dir, elf_path=None, keys_path=None, memcard_path=None, link_assets=False, profile_arg='auto'):
    if not os.path.isfile(binary_path):
        sys.exit(f"Binary not found: {binary_path}")

    os.makedirs(output_dir, exist_ok=True)
    data_dir = os.path.join(output_dir, 'data')
    saves_dir = os.path.join(output_dir, 'saves')
    os.makedirs(saves_dir, exist_ok=True)

    if os.path.isdir(iso_or_disc_dir):
        if link_assets:
            if os.path.lexists(data_dir):
                os.unlink(data_dir) if os.path.islink(data_dir) else shutil.rmtree(data_dir)
            os.symlink(os.path.abspath(iso_or_disc_dir), data_dir)
            print(f"[package_game] linked extracted data directory -> {data_dir}")
        else:
            shutil.copytree(iso_or_disc_dir, data_dir, dirs_exist_ok=True)
            print(f"[package_game] copied extracted data directory -> {data_dir}")
    else:
        extract_disc(iso_or_disc_dir, data_dir, profile_arg=profile_arg, skip_existing=True)

    if not elf_path:
        elf_path = detect_extracted_boot_elf(data_dir)
        if not elf_path:
            sys.exit("Could not auto-detect boot ELF from SYSTEM.CNF; please pass --elf explicitly.")
        print(f"[package_game] auto-detected boot ELF from SYSTEM.CNF: {os.path.basename(elf_path)}")
    elif not os.path.isfile(elf_path):
        sys.exit(f"Guest ELF not found: {elf_path}")

    bin_name = os.path.basename(binary_path)
    elf_name = os.path.basename(elf_path)
    dst_bin = os.path.join(output_dir, bin_name)
    dst_elf = os.path.join(output_dir, elf_name)

    shutil.copy2(binary_path, dst_bin)
    os.chmod(dst_bin, os.stat(dst_bin).st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
    shutil.copy2(elf_path, dst_elf)

    if keys_path and os.path.isfile(keys_path):
        shutil.copy2(keys_path, os.path.join(output_dir, 'keys.ini'))
        shutil.copy2(keys_path, os.path.join(saves_dir, 'keys.ini'))

    if memcard_path and os.path.isfile(memcard_path) and not os.path.isfile(os.path.join(saves_dir, 'memcard.ps2')):
        shutil.copy2(memcard_path, os.path.join(saves_dir, 'memcard.ps2'))

    launch_sh = os.path.join(output_dir, 'launch.sh')
    with open(launch_sh, 'w', encoding='utf-8', newline='\n') as f:
        f.write('#!/usr/bin/env bash\n')
        f.write('set -euo pipefail\n')
        f.write('DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"\n')
        f.write('cd "$DIR"\n')
        f.write('mkdir -p "$DIR/saves"\n')
        f.write('export PS2_KEYS="${PS2_KEYS:-$DIR/keys.ini}"\n')
        f.write('EXTRA_ARGS=(--widescreen)\n')
        f.write('if [[ " $* " != *" --frames "* && " $* " != *" --window "* && " $* " != *" --fullscreen "* ]]; then\n')
        f.write('  EXTRA_ARGS+=(--window)\n')
        f.write('fi\n')
        f.write(f'exec "$DIR/{bin_name}" "$DIR/{elf_name}" --disc "$DIR/data" --memcard "$DIR/saves/memcard.ps2" "${{EXTRA_ARGS[@]}}" "$@"\n')
    os.chmod(launch_sh, os.stat(launch_sh).st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)

    print(f"[package_game] standalone ISO-free package ready in: {output_dir}")
    print(f"[package_game] launch with: {launch_sh}")


def extract_relocatable_modules(disc_files_dir, modules_out_dir):
    """Scan an extracted virtual disc folder for relocatable EE modules (SNR2, xff2, AFS\\0) without needing a .iso."""
    found = []
    for layer_dir in ('layer0', 'layer1'):
        base_dir = os.path.join(disc_files_dir, layer_dir)
        if not os.path.isdir(base_dir):
            continue
        for root, _, filenames in os.walk(base_dir):
            for fname in sorted(filenames):
                full_path = os.path.join(root, fname)
                rel_path = os.path.relpath(full_path, base_dir).replace('\\', '/')
                size = os.path.getsize(full_path)
                if size < 8:
                    continue
                with open(full_path, 'rb') as fp:
                    head = fp.read(8)
                    if head[:4] in (b'SNR2', b'xff2'):
                        found.append((rel_path.replace('/', '_'), full_path, 0, size))
                    elif head[:4] == b'AFS\x00':
                        count = struct.unpack_from('<I', head, 4)[0]
                        table = fp.read(min(count * 8, max(size - 8, 0)))
                        for idx in range(len(table) // 8):
                            offset, length = struct.unpack_from('<II', table, idx * 8)
                            if offset + 4 <= size:
                                fp.seek(offset)
                                if fp.read(4) == b'SNR2':
                                    found.append((f"{rel_path.replace('/', '_')}_{idx:04d}", full_path, offset, length))

    os.makedirs(modules_out_dir, exist_ok=True)
    for mod_name, src_path, offset, length in found:
        if not is_inside_directory(modules_out_dir, mod_name + '.snr'):
            sys.exit(f"Refusing to write module outside {modules_out_dir}: {mod_name!r}")
        with open(src_path, 'rb') as src_fp, open(os.path.join(modules_out_dir, mod_name + '.snr'), 'wb') as out_fp:
            src_fp.seek(offset)
            out_fp.write(src_fp.read(length))
    print(f"[port] {len(found)} relocatable module(s) (SNR2, xff2) extracted to {modules_out_dir}")


def _build_native_runner(repo_root, generated_dir, native_build_dir):
    """Build build/native-<name>/ps2run using the repository's CMake + shared_runtime setup."""
    cmd = (
        f'set -euo pipefail; '
        f'ROOT={subprocess.list2cmdline([repo_root])}; '
        f'here="$ROOT/scripts"; '
        f'generated={subprocess.list2cmdline([generated_dir])}; '
        f'native={subprocess.list2cmdline([native_build_dir])}; '
        f'ninja_jobs=""; '
        f'[ -z "${{PS2_JOBS:-}}" ] || ninja_jobs="-j$PS2_JOBS"; '
        f'. "$here/runtime.sh"; '
        f'. "$here/cxx.sh"; '
        f'shared_runtime; '
        f'config="${{cxx_flags[*]}} ${{runtime_lib:-own runtime}}"; '
        f'if [ ! -f "$native/build.ninja" ] || [ "$(cat "$native/.port-config" 2>/dev/null)" != "$config" ]; then '
        f'  cmake -S "$ROOT" -B "$native" -G Ninja -DCMAKE_BUILD_TYPE=$build_type "${{cxx_flags[@]}}" '
        f'    -DPS2_GENERATED="$generated" -DPS2_RUNTIME_LIB="$runtime_lib" > /dev/null; '
        f'  echo "$config" > "$native/.port-config"; '
        f'fi; '
        f'ninja -C "$native" $ninja_jobs ps2run > "$native/ninja.log" 2>&1 || {{ tail -30 "$native/ninja.log"; exit 1; }}'
    )
    subprocess.run(['bash', '-c', cmd], check=True)


def port_standalone(
    name,
    source_path,
    out_dist_dir=None,
    profile_arg='auto',
    frames=3600,
    rounds=8,
    explore=True,
    presses=None,
    pnach_files=None,
    pnach_sections=None,
    widescreen=True,
    link_assets=True,
    keys_path=None,
    memcard_path=None,
):
    """
    End-to-end ISO-free porting pipeline:
      1. Extract disc image (.iso, .bin/.cue, .cso, .chd) -> games/<name>/disc_files/
      2. Locate guest boot ELF from SYSTEM.CNF & apply optional .pnach patches
      3. Translate EE machine code -> C++ (eerecomp)
      4. Run headless exploring rounds against games/<name>/disc_files/ & compile VU1 (vurecomp)
      5. Build native runner & package standalone ISO-free distribution into dist/<name>/
    """
    if not re.match(r'^[A-Za-z0-9._-]+$', name) or name.startswith('.'):
        sys.exit(f"Invalid game name '{name}': use letters, digits, ., _, and - only.")

    repo_root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    scripts_dir = os.path.join(repo_root, 'scripts')
    game_dir = os.path.join(repo_root, 'games', name)
    generated_dir = os.path.join(repo_root, 'generated', name)
    native_dir = os.path.join(repo_root, 'build', f'native-{name}')
    tools_dir = os.path.join(repo_root, 'build', 'tools')
    dist_dir = out_dist_dir or os.path.join(repo_root, 'dist', name)

    os.makedirs(os.path.join(game_dir, 'vu1'), exist_ok=True)
    os.makedirs(os.path.join(game_dir, 'shots'), exist_ok=True)
    os.makedirs(generated_dir, exist_ok=True)

    eerecomp_bin = os.path.join(tools_dir, 'eerecomp')
    vurecomp_bin = os.path.join(tools_dir, 'vurecomp')
    if not (os.path.isfile(eerecomp_bin) and os.path.isfile(vurecomp_bin)):
        subprocess.run([os.path.join(scripts_dir, 'setup.sh'), '--native-only'], check=True)

    print("== [1/5] disc extraction (ISO-free virtual disc) ==")
    if os.path.isdir(source_path) and os.path.isfile(os.path.join(source_path, 'disc_manifest.tsv')):
        disc_files_dir = os.path.abspath(source_path)
        print(f"[port] using existing extracted virtual disc directory: {disc_files_dir}")
    else:
        disc_files_dir = os.path.join(game_dir, 'disc_files')
        extract_disc(source_path, disc_files_dir, profile_arg=profile_arg, skip_existing=True)

    boot_elf = detect_extracted_boot_elf(disc_files_dir)
    if not boot_elf:
        sys.exit(f"[port] Could not detect boot ELF inside {disc_files_dir}")
    print(f"[port] boot executable: {os.path.relpath(boot_elf, repo_root)}")

    print("== [2/5] executable & patches ==")
    elf = boot_elf
    core_elf = os.path.join(game_dir, 'core.elf')
    core_gt4 = os.path.join(disc_files_dir, 'layer0', 'CORE.GT4')
    if os.path.isfile(core_elf):
        elf = core_elf
    elif os.path.isfile(core_gt4):
        subprocess.run(
            [sys.executable, '-I', os.path.join(scripts_dir, 'gt4_core.py'), core_gt4, core_elf],
            check=True,
        )
        elf = core_elf

    crc = subprocess.check_output(
        [sys.executable, '-I', os.path.join(scripts_dir, 'pnach.py'), 'crc', boot_elf],
        text=True,
    ).strip()
    print(f"[port] PCSX2 ELF CRC: {crc}")

    patches_arg = []
    if pnach_files:
        patched_dir = os.path.join(game_dir, 'patched')
        os.makedirs(patched_dir, exist_ok=True)
        patched_elf = os.path.join(patched_dir, os.path.basename(elf))
        patches_txt = os.path.join(game_dir, 'patches.txt')
        cmd = [sys.executable, '-I', os.path.join(scripts_dir, 'pnach.py'), 'apply', elf, patched_elf, patches_txt, '--crc', crc]
        for sec in (pnach_sections or []):
            cmd.extend(['--section', sec])
        cmd.extend(os.path.abspath(p) for p in pnach_files)
        applied = subprocess.check_output(cmd, text=True)
        print(applied.strip())
        if 'meant for a 16:9 screen' in applied:
            widescreen = True
        elf = patched_elf
        patches_arg = ['--patches', patches_txt]

    toggles_arg = []
    toggles_txt = os.path.join(game_dir, 'toggles.txt')
    if os.path.isfile(toggles_txt):
        toggles_arg = ['--toggles', toggles_txt]

    modules_arg = []
    with open(elf, 'rb') as ef:
        elf_bytes = ef.read()
    if b'SNR2' in elf_bytes or b'xff2' in elf_bytes:
        modules_dir = os.path.join(game_dir, 'modules')
        extract_relocatable_modules(disc_files_dir, modules_dir)
        modules_arg = ['--modules', modules_dir]

    overlays_arg = []
    win_res = subprocess.run(
        [sys.executable, '-I', os.path.join(scripts_dir, 'windows.py'), 'list', elf],
        capture_output=True,
        text=True,
        check=False,
    )
    if win_res.returncode == 0 and win_res.stdout.strip():
        overlays_dir = os.path.join(game_dir, 'overlays')
        os.makedirs(overlays_dir, exist_ok=True)
        overlays_arg = ['--overlays', overlays_dir]

    entries_txt = os.path.join(game_dir, 'entries.txt')
    if not os.path.isfile(entries_txt):
        open(entries_txt, 'a', encoding='utf-8').close()

    def run_translate():
        cmd = [eerecomp_bin, elf, '--entries', entries_txt] + modules_arg + overlays_arg + toggles_arg + ['--out', generated_dir]
        out = subprocess.check_output(cmd, text=True).strip().splitlines()
        if out:
            print(out[-1])

    print("== [3/5] machine code to C++ (eerecomp) ==")
    if not explore and os.path.isfile(os.path.join(generated_dir, 'table.cpp')):
        print(f"Preserving existing generated C++ units in {generated_dir} (--no-explore)")
    else:
        run_translate()

    print("== [4/5] exploring & vector unit programs ==")
    if explore and rounds > 0:
        if not presses:
            press_args = ['--press', '300:start']
            for f in range(600, frames, 300):
                press_args.extend(['--press', f'{f}:cross'])
        else:
            press_args = []
            for p in presses:
                press_args.extend(['--press', p])

        for rnd in range(1, rounds + 1):
            _build_native_runner(repo_root, generated_dir, native_dir)
            misses_txt = os.path.join(game_dir, 'misses.txt')
            explore_mc = os.path.join(game_dir, 'explore.ps2')
            explore_ram = os.path.join(game_dir, 'explore_ram.bin')
            explore_log = os.path.join(game_dir, 'explore.log')
            for tmp in (misses_txt, explore_mc, explore_ram):
                if os.path.isfile(tmp):
                    os.remove(tmp)

            env = os.environ.copy()
            env['PS2_DUMP'] = explore_ram
            env['PS2_GPU_LOW_POWER'] = '1'
            env['PS2_VU_RECORD'] = os.path.join(game_dir, 'vu1')

            run_cmd = [
                os.path.join(native_dir, 'ps2run'),
                elf,
                '--disc', disc_files_dir,
                '--misses', misses_txt,
                '--memcard', explore_mc,
                '--frames', str(frames),
                '--frames-dir', os.path.join(game_dir, 'shots'),
                '--frame-every', '300',
            ]
            if widescreen:
                run_cmd.append('--widescreen')
            run_cmd.extend(patches_arg)
            run_cmd.extend(press_args)

            with open(explore_log, 'w', encoding='utf-8') as lf:
                subprocess.run(run_cmd, env=env, stdout=lf, stderr=subprocess.STDOUT, check=False)

            known = set()
            if os.path.isfile(entries_txt):
                with open(entries_txt, 'r', encoding='utf-8') as ef:
                    for t in ef.read().split():
                        nm, _, at = t.rpartition(':')
                        known.add((nm + ':' if nm else '') + f"{int(at, 16):x}")

            found_new = []
            if os.path.isfile(misses_txt):
                with open(misses_txt, 'r', encoding='utf-8') as mf:
                    for t in mf.read().split():
                        nm, _, at = t.rpartition(':')
                        tok = (nm + ':' if nm else '') + f"{int(at, 16):x}"
                        if tok not in known:
                            known.add(tok)
                            found_new.append(tok)
            found_new.sort()
            if found_new:
                with open(entries_txt, 'a', encoding='utf-8', newline='\n') as ef:
                    for tok in found_new:
                        ef.write(tok + '\n')
            print(f"[port] round {rnd}: {len(found_new)} new entry point(s)")
            if not found_new:
                break
            run_translate()
    else:
        print("[port] headless exploration skipped (--no-explore)")

    vu_bins = glob.glob(os.path.join(game_dir, 'vu1', '*.bin'))
    if vu_bins:
        out = subprocess.check_output([vurecomp_bin, os.path.join(game_dir, 'vu1'), generated_dir], text=True).strip().splitlines()
        if out:
            print(out[-1])
    else:
        for old_vu in glob.glob(os.path.join(generated_dir, 'vu1_*.cpp')):
            os.remove(old_vu)

    print("== [5/5] native build & standalone ISO-free packaging ==")
    _build_native_runner(repo_root, generated_dir, native_dir)

    default_keys = keys_path or (os.path.join(game_dir, 'keys.ini') if os.path.isfile(os.path.join(game_dir, 'keys.ini')) else None)
    default_mc = memcard_path or (os.path.join(game_dir, 'memcard.ps2') if os.path.isfile(os.path.join(game_dir, 'memcard.ps2')) else None)

    package_game(
        binary_path=os.path.join(native_dir, 'ps2run'),
        iso_or_disc_dir=disc_files_dir,
        output_dir=dist_dir,
        elf_path=elf,
        keys_path=default_keys,
        memcard_path=default_mc,
        link_assets=link_assets,
        profile_arg=profile_arg,
    )


def unpack_list_cmd(archive_path, force_carve=False, max_entries=None):
    from ps2_disc_toolkit.archives import open_archive
    arc = open_archive(archive_path, force_carve=force_carve)
    entries = arc.entries[:max_entries] if max_entries is not None else arc.entries
    print(f"[unpack-list] {archive_path} [{arc.format_name}]: showing {len(entries)}/{len(arc.entries)} entry(ies)")
    for idx, entry in enumerate(entries):
        print(f"  [{idx:4d}] {entry.name:36s}  offset=0x{entry.offset:08x}  size={entry.size:9d} B  ({entry.kind})")


def unpack_extract_cmd(archive_path, out_dir, convert=False, recursive=False, force_carve=False, max_entries=None):
    from ps2_disc_toolkit.archives import extract_archive
    summary = extract_archive(
        archive_path,
        out_dir,
        convert=convert,
        recursive=recursive,
        force_carve=force_carve,
        max_entries=max_entries,
    )
    cat_note = f", catalog -> {summary['catalog']}" if summary.get('catalog') else ""
    print(
        f"[unpack-extract] {archive_path} [{summary['format']}]: "
        f"{summary['extracted']} extracted, {summary['converted']} converted -> {out_dir}{cat_note}"
    )
    return summary


def main(argv=None):
    parser = argparse.ArgumentParser(
        prog='ps2-disc-toolkit',
        description='Independent, multi-format PS2 Disc Extraction & Virtual Disc Packaging Toolkit.',
    )
    sub = parser.add_subparsers(dest='cmd', required=True)

    p_ins = sub.add_parser('inspect', help='Inspect a PS2 disc image (.iso, .bin/.cue, .cso, .chd)')
    p_ins.add_argument('image', help='Path to input disc image (.iso, .bin, .cue, .cso, .chd)')
    p_ins.add_argument('--profile', default='auto', help='Profile name or JSON path (default: auto)')
    p_ins.add_argument('--scan-gaps', action='store_true', help='Scan non-file sectors for hidden LBA regions')

    p_ext = sub.add_parser('extract', help='Extract a PS2 disc image into a standalone Virtual Disc folder')
    p_ext.add_argument('iso', help='Path to input disc image (.iso, .bin, .cue, .cso, .chd)')
    p_ext.add_argument('out_dir', help='Destination virtual disc folder')
    p_ext.add_argument('--profile', default='auto', help='Profile name or JSON path (default: auto)')
    p_ext.add_argument('--raw-threshold-mb', type=float, default=None, help='Override threshold (MiB) for unindexed LBA region extraction')
    p_ext.add_argument('--skip-existing', action='store_true', help='Skip files already extracted with matching size')

    p_ver = sub.add_parser('verify', help='Verify extracted Virtual Disc folder against original disc image')
    p_ver.add_argument('iso', help='Path to original disc image (.iso, .bin, .cue, .cso, .chd)')
    p_ver.add_argument('out_dir', help='Path to extracted virtual disc folder')

    p_pkg = sub.add_parser('package', help='Package a recompiled PS2 game into a standalone ISO-free folder')
    p_pkg.add_argument('--binary', required=True, help='Path to compiled native game binary')
    p_pkg.add_argument('--disc', required=True, help='Path to disc image OR already-extracted virtual disc folder')
    p_pkg.add_argument('--out', required=True, help='Output standalone package directory')
    p_pkg.add_argument('--elf', default=None, help='Optional guest boot ELF path (auto-detected from SYSTEM.CNF if omitted)')
    p_pkg.add_argument('--keys', default=None, help='Optional keys.ini file to bundle')
    p_pkg.add_argument('--memcard', default=None, help='Optional existing .ps2 memory card file to bundle')
    p_pkg.add_argument('--profile', default='auto', help='Optional extraction profile if --disc is an image')
    p_pkg.add_argument('--link-assets', action='store_true', help='Symlink data/ to existing extracted disc folder instead of copying')

    p_prt = sub.add_parser('port', help='End-to-end pipeline: Disc Image (.iso/.bin/.cue/.cso/.chd) -> Recompile -> Standalone ISO-free Game')
    p_prt.add_argument('name', help='Short identifier for the game (e.g. norrath)')
    p_prt.add_argument('source', help='Path to input disc image (.iso, .bin, .cue, .cso, .chd) or extracted folder')
    p_prt.add_argument('--out', default=None, help='Output standalone package directory (default: dist/<name>)')
    p_prt.add_argument('--profile', default='auto', help='Extraction profile name or JSON path (default: auto)')
    p_prt.add_argument('--frames', type=int, default=3600, help='Frames per exploring round (default: 3600)')
    p_prt.add_argument('--rounds', type=int, default=8, help='Maximum exploring rounds (default: 8)')
    p_prt.add_argument('--no-explore', action='store_true', help='Skip headless exploring runs')
    p_prt.add_argument('--press', action='append', default=None, help='Input press F:BUTTON for exploring runs (repeatable)')
    p_prt.add_argument('--pnach', action='append', default=None, help='Apply PCSX2 .pnach patch file (repeatable)')
    p_prt.add_argument('--pnach-section', action='append', default=None, help='Enable section [S] in .pnach files')
    p_prt.add_argument('--no-widescreen', action='store_true', help='Disable default 16:9 widescreen flag')
    p_prt.add_argument('--copy-assets', action='store_true', help='Copy extracted disc files into dist/<name>/data instead of symlinking')
    p_prt.add_argument('--keys', default=None, help='Optional keys.ini file to bundle')
    p_prt.add_argument('--memcard', default=None, help='Optional existing .ps2 memory card file to bundle')

    p_uls = sub.add_parser('unpack-list', help='List subfiles inside a 2nd-level PS2 archive (.AFS, .PAC, .MOMO, .LMP, .GOB, .TEX, or --carve)')
    p_uls.add_argument('archive', help='Path to 2nd-level archive file')
    p_uls.add_argument('--carve', action='store_true', help='Force universal signature carver (TIM2/VAGp)')
    p_uls.add_argument('--max-entries', type=int, default=None, help='Maximum number of entries to display')

    p_uex = sub.add_parser('unpack-extract', help='Extract and optionally convert assets (.png, .wav, .obj + assets_catalog.json) from a PS2 archive')
    p_uex.add_argument('archive', help='Path to 2nd-level archive file (.AFS, .PAC, .LMP, .GOB, .TEX, DATA.BIN) or standalone asset (.fmod, .vif, .tex, .tm2, .vag, .16)')
    p_uex.add_argument('out_dir', help='Output directory for extracted/converted files')
    p_uex.add_argument('--convert', action='store_true', help='Convert PS2 textures (.tex, .tm2, .16) to .png, audio (.vag) to .wav, and 3D models (.fmod, .vif) to .obj + assets_catalog.json')
    p_uex.add_argument('--recursive', action='store_true', help='Recursively unpack nested archives (e.g. .PAC / .AFS / .LMP inside containers)')
    p_uex.add_argument('--carve', action='store_true', help='Force universal signature carver (TIM2/VAGp) for unknown containers')
    p_uex.add_argument('--max-entries', type=int, default=None, help='Maximum number of top-level entries to extract')

    args = parser.parse_args(argv)
    if args.cmd == 'inspect':
        inspect_disc(args.image, profile_arg=args.profile, scan_gaps=args.scan_gaps)
    elif args.cmd == 'extract':
        extract_disc(
            args.iso,
            args.out_dir,
            profile_arg=args.profile,
            raw_threshold_mb=args.raw_threshold_mb,
            skip_existing=args.skip_existing,
        )
    elif args.cmd == 'verify':
        verify_disc(args.iso, args.out_dir)
    elif args.cmd == 'package':
        package_game(
            args.binary,
            args.disc,
            args.out,
            elf_path=args.elf,
            keys_path=args.keys,
            memcard_path=args.memcard,
            link_assets=args.link_assets,
            profile_arg=args.profile,
        )
    elif args.cmd == 'port':
        port_standalone(
            name=args.name,
            source_path=args.source,
            out_dist_dir=args.out,
            profile_arg=args.profile,
            frames=args.frames,
            rounds=args.rounds,
            explore=not args.no_explore,
            presses=args.press,
            pnach_files=args.pnach,
            pnach_sections=args.pnach_section,
            widescreen=not args.no_widescreen,
            link_assets=not args.copy_assets,
            keys_path=args.keys,
            memcard_path=args.memcard,
        )
    elif args.cmd == 'unpack-list':
        unpack_list_cmd(args.archive, force_carve=args.carve, max_entries=args.max_entries)
    elif args.cmd == 'unpack-extract':
        unpack_extract_cmd(
            args.archive,
            args.out_dir,
            convert=args.convert,
            recursive=args.recursive,
            force_carve=args.carve,
            max_entries=args.max_entries,
        )


if __name__ == '__main__':
    main()

