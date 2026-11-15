"""
Standalone ISO 9660 (ECMA-119) single/dual-layer filesystem parser and PS2 SYSTEM.CNF analyzer.
Zero dependency on scripts/disc.py.
"""

from dataclasses import dataclass
import os
import re
import struct
import sys

from .containers import SECTOR_SIZE

MAX_DIRECTORY_BYTES = 16 << 20


@dataclass
class LayerInfo:
    index: int
    base_sector: int
    pvd_sector: int
    volume_id: str
    volume_sectors: int
    root_lba: int
    root_size: int
    pvd_root_size_was_zero: bool = False


@dataclass
class FileEntry:
    path: str
    layer: int
    sector: int
    size: int


@dataclass
class SystemCnf:
    boot_raw: str = ''
    boot_elf: str = ''
    disc_id: str = ''
    version: str = ''
    vmode: str = ''
    raw_text: str = ''


def is_safe_iso_name(name):
    return name not in ('', '.', '..') and not any(c in '/\\:' or ord(c) < 32 for c in name)


def is_inside_directory(destination, rel_path):
    base = os.path.realpath(destination)
    target = os.path.realpath(os.path.join(base, rel_path))
    return os.path.commonpath([base, target]) == base


def parse_system_cnf(text):
    info = SystemCnf(raw_text=text.strip())
    for raw_line in text.splitlines():
        line = raw_line.strip()
        if '=' not in line:
            continue
        k, v = [p.strip() for p in line.split('=', 1)]
        ku = k.upper()
        if ku == 'BOOT2':
            info.boot_raw = v
            cleaned = v.split(';')[0].replace('/', '\\')
            prog = cleaned.split('\\')[-1].strip()
            info.boot_elf = prog
            m = re.match(r'^([A-Z]{4})_(\d{3})\.(\d{2})$', prog.upper())
            if m:
                info.disc_id = f"{m.group(1)}-{m.group(2)}{m.group(3)}"
            else:
                info.disc_id = prog.upper()
        elif ku == 'VER':
            info.version = v
        elif ku == 'VMODE':
            info.vmode = v.upper()
    return info


class IsoFileSystem:
    """Reads Layer 0 and optional Layer 1 (DVD-9) ISO 9660 directory structures from a SectorDevice."""

    def __init__(self, device, layer1_pvd_sector=None):
        self.device = device
        self.layers = []
        self._init_layers(layer1_pvd_sector)

    def _parse_layer(self, index, base_sector, pvd_sector):
        pvd = self.device.read_sectors(pvd_sector, 1)
        if len(pvd) < SECTOR_SIZE or pvd[:6] != b'\x01CD001':
            raise ValueError(f'No valid ISO 9660 Primary Volume Descriptor at sector {pvd_sector}')
        volume_id = pvd[40:72].decode('ascii', 'replace').strip()
        vol_sectors = struct.unpack_from('<I', pvd, 80)[0]
        root_lba = struct.unpack_from('<I', pvd, 158)[0]
        root_size = struct.unpack_from('<I', pvd, 166)[0]
        was_zero = False
        if root_size < 34:
            root_sec = self.device.read_sectors(base_sector + root_lba, 1)
            if len(root_sec) >= 34 and root_sec[0] >= 34 and struct.unpack_from('<I', root_sec, 2)[0] == root_lba:
                root_size = struct.unpack_from('<I', root_sec, 10)[0]
                was_zero = True
        return LayerInfo(
            index=index,
            base_sector=base_sector,
            pvd_sector=pvd_sector,
            volume_id=volume_id,
            volume_sectors=vol_sectors,
            root_lba=root_lba,
            root_size=root_size,
            pvd_root_size_was_zero=was_zero,
        )

    def _init_layers(self, layer1_pvd_override=None):
        l0 = self._parse_layer(0, 0, 16)
        self.layers.append(l0)

        if layer1_pvd_override is not None:
            if layer1_pvd_override < self.device.total_sectors:
                probe = self.device.read_sectors(layer1_pvd_override, 1)
                if probe[:6] == b'\x01CD001':
                    self.layers.append(self._parse_layer(1, layer1_pvd_override - 16, layer1_pvd_override))
            return

        # Standard DVD-9 Layer 1 discovery around the end of Layer 0
        start = l0.volume_sectors
        limit = min(start + 64, self.device.total_sectors)
        for sec in range(start, limit):
            probe = self.device.read_sectors(sec, 1)
            if probe[:6] == b'\x01CD001':
                self.layers.append(self._parse_layer(1, sec - 16, sec))
                break

    def list_files(self):
        entries = []
        for layer in self.layers:
            seen = set()
            self._walk_dir(layer, layer.root_lba, layer.root_size, '', 0, seen, entries)
        return entries

    def _walk_dir(self, layer, lba, size, prefix, depth, seen, out_entries):
        abs_sector = layer.base_sector + lba
        if depth > 16 or abs_sector in seen or size == 0:
            return
        seen.add(abs_sector)
        if size > MAX_DIRECTORY_BYTES:
            print(f"[iso9660] warning: directory {prefix!r} at sector {abs_sector} exceeds max size, skipped", file=sys.stderr)
            return

        num_sectors = (size + SECTOR_SIZE - 1) // SECTOR_SIZE
        data = self.device.read_sectors(abs_sector, num_sectors)
        if len(data) < size:
            raise ValueError(f"Truncated disc image reading directory {prefix!r} at sector {abs_sector}")

        offset = 0
        while offset < size:
            length = data[offset]
            if length == 0:
                offset = (offset // SECTOR_SIZE + 1) * SECTOR_SIZE
                continue
            if length < 34 or offset + length > len(data) or 33 + data[offset + 32] > length:
                print(f"[iso9660] warning: malformed directory record in {prefix!r} at sector {abs_sector}", file=sys.stderr)
                return

            rec = data[offset:offset + length]
            offset += length
            extent_lba = struct.unpack_from('<I', rec, 2)[0]
            extent_size = struct.unpack_from('<I', rec, 10)[0]
            raw_name = rec[33:33 + rec[32]]
            if raw_name in (b'\x00', b'\x01'):
                continue

            name = raw_name.decode('ascii', 'replace').split(';')[0]
            if not is_safe_iso_name(name):
                continue

            full_path = prefix + name
            is_dir = (rec[25] & 2) != 0
            if is_dir:
                self._walk_dir(layer, extent_lba, extent_size, full_path + '/', depth + 1, seen, out_entries)
            else:
                out_entries.append(FileEntry(
                    path=full_path,
                    layer=layer.index,
                    sector=layer.base_sector + extent_lba,
                    size=extent_size,
                ))

    def find_file(self, target_path):
        wanted = target_path.replace('\\', '/').strip('/').upper()
        for entry in self.list_files():
            if entry.path.upper() == wanted:
                return entry
        return None

    def read_file_bytes(self, entry):
        if entry.size == 0:
            return b''
        sectors = (entry.size + SECTOR_SIZE - 1) // SECTOR_SIZE
        raw = self.device.read_sectors(entry.sector, sectors)
        return raw[:entry.size]

    def copy_file_to_disk(self, entry, dest_path, chunk_sectors=2048):
        os.makedirs(os.path.dirname(dest_path) or '.', exist_ok=True)
        remaining = entry.size
        cur_sector = entry.sector
        with open(dest_path, 'wb') as out:
            while remaining > 0:
                want_sectors = min(chunk_sectors, (remaining + SECTOR_SIZE - 1) // SECTOR_SIZE)
                raw = self.device.read_sectors(cur_sector, want_sectors)
                if not raw:
                    raise IOError(f"Unexpected end of disc image while extracting {entry.path} at sector {cur_sector}")
                write_bytes = min(remaining, len(raw))
                out.write(raw[:write_bytes])
                remaining -= write_bytes
                cur_sector += want_sectors

    def read_system_cnf(self):
        entry = self.find_file('SYSTEM.CNF')
        if not entry:
            return SystemCnf()
        text = self.read_file_bytes(entry).decode('ascii', 'replace')
        return parse_system_cnf(text)
