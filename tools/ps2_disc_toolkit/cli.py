#!/usr/bin/env python3
"""
Unified CLI and API for the independent PS2 Disc Extraction & Packaging Toolkit.
Commands:
  inspect <image>
  extract <image> <out_dir>
  verify  <image> <out_dir>
  bundle  --game <name> --os linux|windows|all --out <file>   (one file per game)
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
    from ps2_disc_toolkit.bundler import bundle_game_targets
    from ps2_disc_toolkit.containers import open_container, SECTOR_SIZE
    from ps2_disc_toolkit.iso9660 import IsoFileSystem, FileEntry, is_inside_directory, parse_system_cnf
    from ps2_disc_toolkit.profiles import (
        load_profile,
        match_profile,
        collect_non_file_gaps,
        scan_non_zero_runs,
    )
else:
    from .bundler import bundle_game_targets
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

    p_bnd = sub.add_parser('bundle', help='Build a self-contained Single-File Executable (PS2PACK1) for Linux, Windows, or both')
    p_bnd.add_argument('--game', default=None, help='Short game name (e.g. mhdos, norrath) to auto-resolve disc_files, translations, and per-OS binaries/caches')
    p_bnd.add_argument('--os', default='auto', choices=['auto', 'linux', 'windows', 'all'], help='Target OS when using --game (linux, windows, or all)')
    p_bnd.add_argument('--binary', default=None, help='Explicit path to compiled ps2run or ps2run.exe binary')
    p_bnd.add_argument('--disc', default=None, help='Path to extracted virtual disc folder (default: games/<game>/disc_files)')
    p_bnd.add_argument('--out', default=None, help='Output single-file executable path or prefix (e.g. dist/MonsterHunter2)')
    p_bnd.add_argument('--elf', default=None, help='Optional guest boot ELF path (auto-detected from disc_info.json if omitted)')
    p_bnd.add_argument('--translations', default=None, help='Optional translations folder containing *.json dictionaries')
    p_bnd.add_argument('--ee-live', default=None, help='Optional ee-live folder containing pre-compiled overlay libraries')
    p_bnd.add_argument('--vu1-live', default=None, help='Optional vu1-live folder containing pre-compiled VU1 libraries')
    p_bnd.add_argument('--game-id', default=None, help='Optional short game identifier (default: --game or disc_id from disc_info.json)')

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
    elif args.cmd == 'bundle':
        repo_root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
        bundle_game_targets(
            repo_root=repo_root,
            game_name=args.game,
            target_os=args.os,
            binary_path=args.binary,
            disc_dir=args.disc,
            output_path=args.out,
            elf_path=args.elf,
            translations_dir=args.translations,
            ee_live_dir=args.ee_live,
            vu1_live_dir=args.vu1_live,
            game_id=args.game_id,
        )


if __name__ == '__main__':
    main()

