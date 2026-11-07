"""
Generic & Pluggable 2nd-Level PS2 Archive Unpacker and Signature Carver.

Supported archive families:
  1. CRI Middleware .AFS ('AFS\\x00') — standard container across hundreds of PS2 games
  2. Snowblind Engine .LMP (v1 64-byte inline records & v2 12-byte string-table records)
  3. Snowblind Engine .GOB (level container holding nested .LMP archives)
  4. Snowblind Engine .TEX (level texture chunk container)
  5. Universal PS2 Signature Carver (--carve) — scans any unknown proprietary container
     for standard Sony SDK assets ('TIM2' textures, 'VAGp' SPU2 audio, '\\x7fELF' modules)
"""

import os
import struct
from dataclasses import dataclass

from .ps2_textures import (
    decode_raw16_to_rgba,
    decode_snowblind_tex_to_rgba,
    decode_tim2_to_rgba,
    encode_rgba_png,
)
from .ps2_audio import decode_vag_to_wav_bytes
from .codecs import decompress_capcom_lz16, parse_mwo3_header


@dataclass
class ArchiveEntry:
    name: str
    offset: int
    size: int
    kind: str


def _classify_asset_name(name, header_bytes=b''):
    low = name.lower()
    if header_bytes[:4] == b'TIM2' or low.endswith(('.tm2', '.tim2')):
        return 'texture:tim2'
    if header_bytes[:4] == b'VAGp' or low.endswith('.vag'):
        return 'audio:vag'
    if header_bytes[:4] == b'\x7fELF' or low.endswith(('.elf', '.irx')):
        return 'exec:elf'
    if header_bytes[:4] == b'MWo3' or low.endswith('.mwo3'):
        return 'exec:mwo3_overlay'
    if low.endswith('.tex'):
        return 'texture:snowblind_tex'
    if low.endswith('.16'):
        return 'texture:raw16'
    if low.endswith('.lmp'):
        return 'archive:snowblind_lmp'
    if low.endswith('.gob'):
        return 'archive:snowblind_gob'
    if low.endswith('.afs') or header_bytes[:4] == b'AFS\x00':
        return 'archive:cri_afs'
    if low.endswith('.vif'):
        return 'mesh:ps2_vif'
    if low.endswith('.anm'):
        return 'anim:snowblind_anm'
    return 'data:raw'


class BaseArchiveDriver:
    format_name = 'UNKNOWN'

    def __init__(self, path, data):
        self.path = path
        self.data = data
        self.entries = []

    def read_entry(self, entry: ArchiveEntry) -> bytes:
        return self.data[entry.offset:entry.offset + entry.size]


class AfsArchiveDriver(BaseArchiveDriver):
    """CRI Middleware .AFS archive reader ('AFS\\x00')."""

    format_name = 'CRI-AFS'

    @classmethod
    def probe(cls, data, path=''):
        if len(data) < 16 or data[:4] != b'AFS\x00':
            return False
        count = struct.unpack_from('<I', data, 4)[0]
        return 0 < count <= 65535 and 8 + count * 8 <= len(data)

    def __init__(self, path, data):
        super().__init__(path, data)
        count = struct.unpack_from('<I', data, 4)[0]
        raw_pairs = [struct.unpack_from('<II', data, 8 + i * 8) for i in range(count)]

        # Optional 48-byte filename directory pointer stored right after the entry table
        meta_off = 0
        meta_size = 0
        if 8 + (count + 1) * 8 <= len(data):
            meta_off, meta_size = struct.unpack_from('<II', data, 8 + count * 8)

        has_meta = (
            meta_off > 0
            and meta_size >= count * 48
            and meta_off + count * 48 <= len(data)
        )

        for i, (off, size) in enumerate(raw_pairs):
            if off == 0 and size == 0:
                continue
            name = ''
            if has_meta:
                rec = data[meta_off + i * 48:meta_off + i * 48 + 32]
                raw_name = rec.split(b'\x00', 1)[0].decode('ascii', 'replace').strip()
                if raw_name and all(32 <= ord(c) < 127 for c in raw_name):
                    name = raw_name
            hdr = data[off:off + 16]
            if not name:
                ext = '.bin'
                if hdr[:4] == b'TIM2':
                    ext = '.tm2'
                elif hdr[:4] == b'VAGp':
                    ext = '.vag'
                elif hdr[:4] == b'AFS\x00':
                    ext = '.afs'
                name = f'file_{i:04d}{ext}'
            self.entries.append(ArchiveEntry(name=name, offset=off, size=size, kind=_classify_asset_name(name, hdr)))


class SnowblindLmpDriver(BaseArchiveDriver):
    """
    Snowblind Engine .LMP archive reader.
    Supports both:
      - v2 (Champions of Norrath / Return to Arms): count + count * 12B (name_off, data_off, size)
      - v1 (Baldur's Gate: Dark Alliance): count + count * 64B (char name[56], data_off, size)
    """

    format_name = 'Snowblind-LMP'

    @classmethod
    def _detect_layout(cls, data):
        if len(data) < 16:
            return None
        count = struct.unpack_from('<I', data, 0)[0]
        if count == 0 or count > 8192:
            return None

        # Check v2 (12-byte entries: name_offset, data_offset, data_size)
        if 4 + count * 12 <= len(data):
            valid_v2 = True
            for i in range(min(count, 8)):
                noff, doff, dsize = struct.unpack_from('<III', data, 4 + i * 12)
                if noff < 4 + count * 12 or noff >= len(data):
                    valid_v2 = False
                    break
                if doff + dsize > len(data):
                    valid_v2 = False
                    break
                end = data.find(b'\x00', noff, min(len(data), noff + 128))
                if end <= noff:
                    valid_v2 = False
                    break
                s = data[noff:end]
                if not all(32 <= b < 127 for b in s):
                    valid_v2 = False
                    break
            if valid_v2:
                return 'v2'

        # Check v1 (64-byte entries: char name[56], data_offset, data_size)
        if 4 + count * 64 <= len(data):
            valid_v1 = True
            for i in range(min(count, 8)):
                rec = data[4 + i * 64:4 + (i + 1) * 64]
                raw_name = rec[:56].split(b'\x00', 1)[0]
                doff, dsize = struct.unpack_from('<II', rec, 56)
                if not raw_name or not all(32 <= b < 127 for b in raw_name):
                    valid_v1 = False
                    break
                if doff < 4 + count * 64 or doff + dsize > len(data):
                    valid_v1 = False
                    break
            if valid_v1:
                return 'v1'

        return None

    @classmethod
    def probe(cls, data, path=''):
        return cls._detect_layout(data) is not None

    def __init__(self, path, data):
        super().__init__(path, data)
        layout = self._detect_layout(data)
        if not layout:
            raise ValueError(f'{path}: not a valid Snowblind .LMP archive')
        self.format_name = f'Snowblind-LMP-{layout}'
        count = struct.unpack_from('<I', data, 0)[0]

        for i in range(count):
            if layout == 'v2':
                noff, doff, dsize = struct.unpack_from('<III', data, 4 + i * 12)
                end = data.find(b'\x00', noff, min(len(data), noff + 128))
                name = data[noff:end].decode('ascii', 'replace') if end > noff else f'entry_{i:04d}.bin'
            else:
                rec = data[4 + i * 64:4 + (i + 1) * 64]
                name = rec[:56].split(b'\x00', 1)[0].decode('ascii', 'replace')
                doff, dsize = struct.unpack_from('<II', rec, 56)
            hdr = data[doff:doff + 16]
            self.entries.append(ArchiveEntry(name=name, offset=doff, size=dsize, kind=_classify_asset_name(name, hdr)))


class SnowblindGobDriver(BaseArchiveDriver):
    """Snowblind Engine .GOB level container (40-byte entries: char name[32], offset, size)."""

    format_name = 'Snowblind-GOB'

    @classmethod
    def probe(cls, data, path=''):
        if len(data) < 80:
            return False
        pos = 0
        found = 0
        first_off = len(data)
        while pos + 40 <= min(len(data), first_off):
            rec = data[pos:pos + 40]
            if rec[0] == 0:
                break
            raw_name = rec[:32].split(b'\x00', 1)[0]
            off, size = struct.unpack_from('<II', rec, 32)
            if not raw_name or not all(32 <= b < 127 for b in raw_name):
                return False
            if off < 40 or off + size > len(data) or size == 0:
                return False
            first_off = min(first_off, off)
            found += 1
            pos += 40
        return found > 0

    def __init__(self, path, data):
        super().__init__(path, data)
        pos = 0
        first_off = len(data)
        while pos + 40 <= min(len(data), first_off):
            rec = data[pos:pos + 40]
            if rec[0] == 0:
                break
            name = rec[:32].split(b'\x00', 1)[0].decode('ascii', 'replace')
            off, size = struct.unpack_from('<II', rec, 32)
            first_off = min(first_off, off)
            hdr = data[off:off + 16]
            self.entries.append(ArchiveEntry(name=name, offset=off, size=size, kind=_classify_asset_name(name, hdr)))
            pos += 40


class SnowblindLevelTexDriver(BaseArchiveDriver):
    """Snowblind Engine level .TEX chunk container (4-byte zero + 12-byte chunk records)."""

    format_name = 'Snowblind-Level-TEX'

    @classmethod
    def probe(cls, data, path=''):
        if not path.lower().endswith('.tex') or len(data) < 0x800:
            return False
        if struct.unpack_from('<I', data, 0)[0] != 0:
            return False
        cid, off, size = struct.unpack_from('<III', data, 4)
        return off == 0x800 and 0 < size <= len(data) - 0x800 and cid > 0

    def __init__(self, path, data):
        super().__init__(path, data)
        pos = 4
        idx = 0
        while pos + 12 <= 0x800:
            cid, off, size = struct.unpack_from('<III', data, pos)
            if off == 0 and size == 0:
                break
            if off >= 0x800 and off + size <= len(data):
                name = f'chunk_{idx:03d}_id{cid:04x}.texchunk'
                self.entries.append(ArchiveEntry(name=name, offset=off, size=size, kind='data:snowblind_texchunk'))
                idx += 1
            pos += 12


class SignatureCarverDriver(BaseArchiveDriver):
    """
    Universal PS2 File Carver (--carve).
    Scans any unknown binary container for standard Sony PS2 SDK signatures:
      - 'TIM2' (.tm2 textures)
      - 'VAGp' (.vag SPU2 ADPCM audio)
      - '\\x7fELF' (.elf / .irx executables and IOP modules)
    """

    format_name = 'Signature-Carver'

    def __init__(self, path, data):
        super().__init__(path, data)
        self._scan_tim2()
        self._scan_vag()
        self.entries.sort(key=lambda e: e.offset)

    def _scan_tim2(self):
        pos = 0
        idx = 0
        while True:
            hit = self.data.find(b'TIM2', pos)
            if hit < 0 or hit + 64 > len(self.data):
                break
            ver, align, pic_cnt = struct.unpack_from('<BBH', self.data, hit + 4)
            hdr_pad = self.data[hit + 8:hit + 16]
            pic_off = 128 if align == 1 else 16
            if ver in (3, 4) and align in (0, 1) and 1 <= pic_cnt <= 64 and hdr_pad == b'\x00' * 8:
                if hit + pic_off + 48 <= len(self.data):
                    total_pic_size = struct.unpack_from('<I', self.data, hit + pic_off)[0]
                    total_size = pic_off + total_pic_size
                    if 64 <= total_size <= len(self.data) - hit:
                        name = f'carved_{idx:04d}_{hit:08x}.tm2'
                        self.entries.append(ArchiveEntry(name=name, offset=hit, size=total_size, kind='texture:tim2'))
                        idx += 1
                        pos = hit + total_size
                        continue
            pos = hit + 4

    def _scan_vag(self):
        pos = 0
        idx = 0
        while True:
            hit = self.data.find(b'VAGp', pos)
            if hit < 0 or hit + 64 > len(self.data):
                break
            ver, _res, data_size, srate = struct.unpack_from('>IIII', self.data, hit + 4)
            if ver in (0, 2, 3, 4, 0x20) and 16 <= data_size <= len(self.data) - hit - 48 and 4000 <= srate <= 96000:
                raw_name = self.data[hit + 32:hit + 48].split(b'\x00', 1)[0].decode('ascii', 'ignore').strip()
                safe_name = ''.join(c if (c.isalnum() or c in '_-') else '_' for c in raw_name)
                fname = f'{safe_name}_{hit:08x}.vag' if safe_name else f'carved_{idx:04d}_{hit:08x}.vag'
                total_size = 48 + data_size
                self.entries.append(ArchiveEntry(name=fname, offset=hit, size=total_size, kind='audio:vag'))
                idx += 1
                pos = hit + total_size
                continue
            pos = hit + 4


_DRIVERS = (
    AfsArchiveDriver,
    SnowblindLmpDriver,
    SnowblindGobDriver,
    SnowblindLevelTexDriver,
)


def open_archive_bytes(data, path='', force_carve=False):
    """Detect and open a 2nd-level PS2 archive from an in-memory buffer."""
    if not force_carve:
        for drv_cls in _DRIVERS:
            if drv_cls.probe(data, path):
                return drv_cls(path, data)
    carver = SignatureCarverDriver(path, data)
    if carver.entries or force_carve:
        return carver
    raise ValueError(f'{path}: unrecognized archive format (and no standard TIM2/VAG signatures found)')


def open_archive(path, force_carve=False):
    """Open a 2nd-level PS2 archive file from disk."""
    with open(path, 'rb') as f:
        data = f.read()
    return open_archive_bytes(data, path=path, force_carve=force_carve)


def convert_asset_bytes(name, data):
    """
    Attempt to convert a single PS2 asset buffer (.tex, .tm2, .16, .vag) into a standard PC
    format (.png or .wav). Returns (new_filename, converted_bytes) or (None, None).
    """
    stem, ext = os.path.splitext(name)
    low_ext = ext.lower()

    if data[:4] == b'TIM2' or low_ext in ('.tm2', '.tim2'):
        w, h, rgba = decode_tim2_to_rgba(data)
        return f'{stem}.png', encode_rgba_png(w, h, rgba)

    if data[:4] == b'VAGp' or low_ext == '.vag':
        _sr, _ns, wav_bytes = decode_vag_to_wav_bytes(data)
        return f'{stem}.wav', wav_bytes

    if low_ext == '.tex':
        w, h, rgba = decode_snowblind_tex_to_rgba(data)
        return f'{stem}.png', encode_rgba_png(w, h, rgba)

    if low_ext == '.16' and len(data) >= 640 * 448 * 2:
        w, h, rgba = decode_raw16_to_rgba(data, 640, 448)
        return f'{stem}.png', encode_rgba_png(w, h, rgba)

    if data[:4] not in (b'TIM2', b'VAGp', b'AFS\x00', b'\x7fELF', b'MWo3') and 32 <= len(data) <= 2 * 1024 * 1024:
        dec = decompress_capcom_lz16(data)
        if dec is not None and len(dec) > len(data):
            return f'{stem}.dec.bin', dec

    return None, None


def extract_archive(path, out_dir, convert=False, recursive=False, force_carve=False):
    """
    Extract a 2nd-level PS2 archive (or convert a standalone PS2 asset file) into out_dir.
    Returns a summary dict with counts of extracted and converted files.
    """
    os.makedirs(out_dir, exist_ok=True)
    with open(path, 'rb') as f:
        data = f.read()

    # Check if the input file itself is a standalone convertible asset (.16, .tm2, .vag, or single .tex)
    base_name = os.path.basename(path)
    if not force_carve and not any(d.probe(data, path) for d in _DRIVERS):
        if convert:
            conv_name, conv_bytes = convert_asset_bytes(base_name, data)
            if conv_name and conv_bytes:
                dst = os.path.join(out_dir, conv_name)
                with open(dst, 'wb') as out_f:
                    out_f.write(conv_bytes)
                return {'format': 'Standalone-Asset', 'extracted': 0, 'converted': 1, 'files': [dst]}

    arc = open_archive_bytes(data, path=path, force_carve=force_carve)
    summary = {'format': arc.format_name, 'extracted': 0, 'converted': 0, 'files': []}

    def _process_archive(driver, target_dir):
        os.makedirs(target_dir, exist_ok=True)
        for entry in driver.entries:
            safe_rel = entry.name.replace('\\', '/').lstrip('/')
            raw_bytes = driver.read_entry(entry)
            dst_path = os.path.join(target_dir, safe_rel)
            os.makedirs(os.path.dirname(dst_path), exist_ok=True)
            with open(dst_path, 'wb') as out_f:
                out_f.write(raw_bytes)
            summary['extracted'] += 1
            summary['files'].append(dst_path)

            if recursive and entry.kind.startswith('archive:'):
                sub_dir = os.path.join(target_dir, os.path.splitext(safe_rel)[0] + '_unpacked')
                try:
                    sub_drv = open_archive_bytes(raw_bytes, path=entry.name)
                    _process_archive(sub_drv, sub_dir)
                except ValueError:
                    pass

            if convert:
                try:
                    conv_name, conv_bytes = convert_asset_bytes(safe_rel, raw_bytes)
                    if conv_name and conv_bytes:
                        conv_path = os.path.join(target_dir, conv_name)
                        os.makedirs(os.path.dirname(conv_path), exist_ok=True)
                        with open(conv_path, 'wb') as cf:
                            cf.write(conv_bytes)
                        summary['converted'] += 1
                        summary['files'].append(conv_path)
                except Exception:
                    pass

    _process_archive(arc, out_dir)
    return summary
