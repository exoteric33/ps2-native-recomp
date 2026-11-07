"""
Generic & Pluggable 2nd-Level PS2 Archive Unpacker, 3D/Texture/Audio Converter, and Signature Carver.

Supported archive families:
  1. CRI Middleware .AFS ('AFS\\x00') & 'MOMO' offset-table containers
  2. PS2 Headerless Offset-Table Archives (.PAC: count + count * (offset, size))
  3. Snowblind Engine .LMP (v1 64-byte inline records & v2 12-byte string-table records)
  4. Snowblind Engine .GOB (level container holding nested .LMP archives)
  5. Snowblind Engine .TEX (level texture chunk container)
  6. Sector-Aligned Container Scanner (0x800 DVD sector boundaries for monolithic containers)
  7. Universal PS2 Signature Carver (--carve) — scans any unknown proprietary container
     for standard Sony SDK & PS2 assets ('TIM2' textures, 'VAGp' SPU2 audio, 'FMOD' 3D meshes,
     '\\x7fELF' modules, 'MWo3' overlays)
"""

import json
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
from .ps2_meshes import (
    decode_fmod_to_meshes,
    decode_vif_to_meshes,
    export_mesh_to_obj,
    export_meshes_to_obj,
    is_fmod_bytes,
)
from .codecs import decompress_capcom_lz16, parse_mwo3_header

SECTOR_SIZE = 0x800


@dataclass
class ArchiveEntry:
    name: str
    offset: int
    size: int
    kind: str


def validate_pac_header(buf: bytes, max_entries: int = 512) -> tuple[bool, int, int]:
    """
    Validate whether `buf` begins with a coherent PS2 .PAC offset/size allocation table:
      - uint32 count (1 .. max_entries)
      - count * (uint32 offset, uint32 size)
    Returns (is_valid, count, max_end_offset).
    """
    if len(buf) < 12:
        return (False, 0, 0)
    count = struct.unpack_from('<I', buf, 0)[0]
    if count == 0 or count > max_entries:
        return (False, 0, 0)

    header_end = 4 + count * 8
    if header_end > len(buf):
        return (False, 0, 0)

    max_end = 0
    prev_offset = header_end
    non_empty = 0
    for i in range(count):
        offset, size = struct.unpack_from('<II', buf, 4 + i * 8)
        if offset == 0 and size == 0:
            continue
        if size == 0:
            continue
        if offset < header_end or offset < prev_offset or size > 64 * 1024 * 1024:
            return (False, 0, 0)
        max_end = max(max_end, offset + size)
        prev_offset = offset
        non_empty += 1

    if non_empty == 0 or max_end <= header_end:
        return (False, 0, 0)
    return (True, count, max_end)


def _sniff_payload_ext_and_kind(payload: bytes, fallback_name: str = '') -> tuple[str, str]:
    """
    Inspect a binary payload buffer and return (recommended_extension, asset_kind).
    """
    low = fallback_name.lower()
    hdr = payload[:16]
    if hdr[:4] == b'TIM2' or low.endswith(('.tm2', '.tim2')):
        return '.tm2', 'texture:tim2'
    if hdr[:4] == b'VAGp' or low.endswith('.vag'):
        return '.vag', 'audio:vag'
    if hdr[:4] == b'\x7fELF' or low.endswith(('.elf', '.irx')):
        return '.elf', 'exec:elf'
    if hdr[:4] == b'MWo3' or low.endswith('.mwo3'):
        return '.mwo3', 'exec:mwo3_overlay'
    if hdr[:4] in (b'AFS\x00', b'MOMO') or low.endswith('.afs'):
        return '.afs', 'archive:cri_afs'
    if is_fmod_bytes(payload) or low.endswith('.fmod'):
        return '.fmod', 'mesh:ps2_fmod'
    # Check if payload is itself a valid .PAC container with recognizable children
    if PacArchiveDriver.probe(payload):
        return '.pac', 'archive:ps2_pac'
    if low.endswith('.tex'):
        return '.tex', 'texture:snowblind_tex'
    if low.endswith('.16'):
        return '.16', 'texture:raw16'
    if low.endswith('.lmp'):
        return '.lmp', 'archive:snowblind_lmp'
    if low.endswith('.gob'):
        return '.gob', 'archive:snowblind_gob'
    if low.endswith('.vif'):
        return '.vif', 'mesh:ps2_vif'
    if low.endswith('.anm'):
        return '.anm', 'anim:snowblind_anm'
    return '.bin', 'data:raw'


def _classify_asset_name(name, header_bytes=b'', full_bytes=b''):
    probe_buf = full_bytes if full_bytes else header_bytes
    _ext, kind = _sniff_payload_ext_and_kind(probe_buf, fallback_name=name)
    return kind


class BaseArchiveDriver:
    format_name = 'UNKNOWN'

    def __init__(self, path, data):
        self.path = path
        self.data = data
        self.entries = []

    def read_entry(self, entry: ArchiveEntry) -> bytes:
        return self.data[entry.offset:entry.offset + entry.size]


class AfsArchiveDriver(BaseArchiveDriver):
    """CRI Middleware .AFS ('AFS\\x00') and 'MOMO' offset-table archive reader."""

    format_name = 'CRI-AFS'

    @classmethod
    def probe(cls, data, path=''):
        if len(data) < 16 or data[:4] not in (b'AFS\x00', b'MOMO'):
            return False
        count = struct.unpack_from('<I', data, 4)[0]
        if not (0 < count <= 65535 and 8 + count * 8 <= len(data)):
            return False
        off0, sz0 = struct.unpack_from('<II', data, 8)
        return off0 >= 8 + count * 8 and off0 + sz0 <= len(data)

    def __init__(self, path, data):
        super().__init__(path, data)
        if data[:4] == b'MOMO':
            self.format_name = 'PS2-MOMO'
        count = struct.unpack_from('<I', data, 4)[0]
        raw_pairs = [struct.unpack_from('<II', data, 8 + i * 8) for i in range(count)]

        # Optional 48-byte filename directory pointer stored right after the entry table in CRI AFS
        meta_off = 0
        meta_size = 0
        if data[:4] == b'AFS\x00' and 8 + (count + 1) * 8 <= len(data):
            meta_off, meta_size = struct.unpack_from('<II', data, 8 + count * 8)

        has_meta = (
            meta_off > 0
            and meta_size >= count * 48
            and meta_off + count * 48 <= len(data)
        )

        for i, (off, size) in enumerate(raw_pairs):
            if off == 0 and size == 0:
                continue
            if off + size > len(data):
                continue
            name = ''
            if has_meta:
                rec = data[meta_off + i * 48:meta_off + i * 48 + 32]
                raw_name = rec.split(b'\x00', 1)[0].decode('ascii', 'replace').strip()
                if raw_name and all(32 <= ord(c) < 127 for c in raw_name):
                    name = raw_name
            chunk = data[off:off + min(size, 65536)]
            ext, kind = _sniff_payload_ext_and_kind(chunk, fallback_name=name)
            if not name:
                name = f'file_{i:04d}{ext}'
            self.entries.append(ArchiveEntry(name=name, offset=off, size=size, kind=kind))


class PacArchiveDriver(BaseArchiveDriver):
    """
    Generic PS2 .PAC headerless offset-table archive reader.
    Layout:
      - 0x00: uint32 entry_count (1..512)
      - 0x04: entry_count * (uint32 offset, uint32 size)
    """

    format_name = 'PS2-PAC'

    @classmethod
    def _has_recognized_child(cls, data: bytes, count: int, depth: int = 0) -> bool:
        for i in range(min(count, 32)):
            off, size = struct.unpack_from('<II', data, 4 + i * 8)
            if size < 12 or off + size > len(data):
                continue
            chunk = data[off:off + size]
            magic = chunk[:4]
            if magic in (b'TIM2', b'VAGp', b'FMOD', b'AFS\x00', b'MOMO', b'\x7fELF', b'MWo3'):
                return True
            if is_fmod_bytes(chunk):
                return True
            if depth < 2:
                sub_ok, sub_cnt, sub_end = validate_pac_header(chunk[:4096])
                if sub_ok and sub_end <= len(chunk) and (len(chunk) - sub_end) <= 2048:
                    if cls._has_recognized_child(chunk, sub_cnt, depth + 1):
                        return True
        return False

    @classmethod
    def probe(cls, data: bytes, path: str = '') -> bool:
        if len(data) < 20 or data[:4] in (b'TIM2', b'VAGp', b'FMOD', b'AFS\x00', b'MOMO', b'\x7fELF', b'MWo3'):
            return False
        if is_fmod_bytes(data):
            return False
        ok, count, max_end = validate_pac_header(data[:4096])
        if not ok or max_end > len(data) or (len(data) - max_end) > 4096:
            return False
        return cls._has_recognized_child(data, count)

    def __init__(self, path, data):
        super().__init__(path, data)
        ok, count, _max_end = validate_pac_header(data[:4096])
        if not ok:
            raise ValueError(f'{path}: not a valid PS2 .PAC archive')

        for i in range(count):
            off, size = struct.unpack_from('<II', data, 4 + i * 8)
            if size == 0 or off + size > len(data):
                continue
            chunk = data[off:off + size]
            ext, kind = _sniff_payload_ext_and_kind(chunk)
            name = f'entry_{i:04d}{ext}'
            self.entries.append(ArchiveEntry(name=name, offset=off, size=size, kind=kind))


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


def _count_pac_signatures(pac_bytes: bytes, count: int, depth: int = 0) -> dict[str, int]:
    """Recursively count FMOD (3D), TIM2 (texture), and VAGp (audio) chunks inside a .PAC buffer."""
    counts = {'fmod': 0, 'tm2': 0, 'vag': 0}
    for i in range(count):
        offset, size = struct.unpack_from('<II', pac_bytes, 4 + i * 8)
        if size < 12 or offset + size > len(pac_bytes):
            continue
        chunk = pac_bytes[offset:offset + size]
        magic = chunk[:4]
        if magic == b'TIM2':
            counts['tm2'] += 1
        elif magic == b'VAGp':
            counts['vag'] += 1
        elif magic == b'FMOD' or is_fmod_bytes(chunk):
            counts['fmod'] += 1
        elif depth < 2:
            is_sub, sub_count, sub_end = validate_pac_header(chunk[:4096])
            if is_sub and sub_end <= len(chunk):
                sub = _count_pac_signatures(chunk, sub_count, depth + 1)
                counts['fmod'] += sub['fmod']
                counts['tm2'] += sub['tm2']
                counts['vag'] += sub['vag']
    return counts


class SectorAlignedPacScannerDriver(BaseArchiveDriver):
    """
    Scans 2048-byte (0x800) DVD sector boundaries of a monolithic container for embedded
    .PAC archives containing 3D models (.FMOD) and/or textures (.TIM2).
    """

    format_name = 'PS2-Sector-PAC-Scanner'

    @classmethod
    def probe(cls, data: bytes, path: str = '') -> bool:
        if len(data) < SECTOR_SIZE * 4:
            return False
        total_sectors = len(data) // SECTOR_SIZE
        sector = 0
        while sector < min(total_sectors, 65536):
            off = sector * SECTOR_SIZE
            ok, count, byte_len = validate_pac_header(data[off:off + 4096])
            if ok and byte_len <= 32 * 1024 * 1024 and off + byte_len <= len(data):
                sigs = _count_pac_signatures(data[off:off + byte_len], count)
                if sigs['fmod'] >= 1 or sigs['tm2'] >= 1 or sigs['vag'] >= 1:
                    return True
            sector += 1
        return False

    def __init__(self, path, data):
        super().__init__(path, data)
        total_sectors = len(data) // SECTOR_SIZE
        sector = 0
        while sector < total_sectors:
            off = sector * SECTOR_SIZE
            ok, count, byte_len = validate_pac_header(data[off:off + 4096])
            if ok and byte_len <= 32 * 1024 * 1024 and off + byte_len <= len(data):
                sigs = _count_pac_signatures(data[off:off + byte_len], count)
                if sigs['fmod'] >= 1 or sigs['tm2'] >= 1 or sigs['vag'] >= 1:
                    fname = f"pac_sec{sector:06d}_fmod{sigs['fmod']}_tm2{sigs['tm2']}.pac"
                    self.entries.append(ArchiveEntry(name=fname, offset=off, size=byte_len, kind='archive:ps2_pac'))
                    sector += max(1, (byte_len + SECTOR_SIZE - 1) // SECTOR_SIZE)
                    continue
            sector += 1


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
    PacArchiveDriver,
    SectorAlignedPacScannerDriver,
)


def open_archive_bytes(data, path='', force_carve=False):
    """Detect and open a 2nd-level PS2 archive from an in-memory buffer (transparently unwrapping LZ16 if compressed)."""
    if not force_carve:
        for drv_cls in _DRIVERS:
            if drv_cls.probe(data, path):
                return drv_cls(path, data)
        if data[:4] not in (b'TIM2', b'VAGp', b'AFS\x00', b'MOMO', b'\x7fELF', b'MWo3') and 32 <= len(data) <= 16 * 1024 * 1024:
            dec = decompress_capcom_lz16(data)
            if dec is not None and len(dec) > len(data):
                for drv_cls in _DRIVERS:
                    if drv_cls.probe(dec, path):
                        drv = drv_cls(path, dec)
                        drv.format_name = f'{drv.format_name}+LZ16'
                        return drv
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
    Attempt to convert a single PS2 asset buffer (.tex, .tm2, .16, .vag, .fmod, .vif) into a
    standard PC format (.png, .wav, .obj). Returns (new_filename, converted_bytes) or (None, None).
    """
    stem, ext = os.path.splitext(name)
    low_ext = ext.lower()

    if data[:4] == b'TIM2' or low_ext in ('.tm2', '.tim2'):
        w, h, rgba = decode_tim2_to_rgba(data)
        return f'{stem}.png', encode_rgba_png(w, h, rgba)

    if data[:4] == b'VAGp' or low_ext == '.vag':
        _sr, _ns, wav_bytes = decode_vag_to_wav_bytes(data)
        return f'{stem}.wav', wav_bytes

    if is_fmod_bytes(data) or low_ext == '.fmod':
        meshes = decode_fmod_to_meshes(data)
        if meshes:
            obj_txt = export_meshes_to_obj(meshes, header_comment=f'PS2 FMOD 3D Mesh ({name})')
            return f'{stem}.obj', obj_txt.encode('utf-8')

    if low_ext == '.vif':
        meshes = decode_vif_to_meshes(data)
        if meshes:
            obj_txt = export_meshes_to_obj(meshes, header_comment=f'PS2 VIF/VU1 3D Mesh ({name})')
            return f'{stem}.obj', obj_txt.encode('utf-8')

    if low_ext == '.tex':
        w, h, rgba = decode_snowblind_tex_to_rgba(data)
        return f'{stem}.png', encode_rgba_png(w, h, rgba)

    if low_ext == '.16' and len(data) >= 640 * 448 * 2:
        w, h, rgba = decode_raw16_to_rgba(data, 640, 448)
        return f'{stem}.png', encode_rgba_png(w, h, rgba)

    if data[:4] not in (b'TIM2', b'VAGp', b'AFS\x00', b'MOMO', b'\x7fELF', b'MWo3') and 32 <= len(data) <= 4 * 1024 * 1024:
        dec = decompress_capcom_lz16(data)
        if dec is not None and len(dec) > len(data):
            dec_ext, _dec_kind = _sniff_payload_ext_and_kind(dec)
            return f'{stem}.dec{dec_ext}', dec

    return None, None


def extract_archive(path, out_dir, convert=False, recursive=False, force_carve=False, max_entries=None):
    """
    Extract a 2nd-level PS2 archive (or convert a standalone PS2 asset file) into out_dir.
    When `convert=True`, converts textures (.tm2, .tex, .16 -> .png), audio (.vag -> .wav),
    and 3D models (.fmod, .vif -> .obj + submesh parts), and writes `assets_catalog.json`.
    Returns a summary dict with counts of extracted and converted files.
    """
    os.makedirs(out_dir, exist_ok=True)
    with open(path, 'rb') as f:
        data = f.read()

    base_name = os.path.basename(path)
    try:
        arc = open_archive_bytes(data, path=path, force_carve=force_carve)
    except ValueError:
        arc = None

    if arc is None:
        if convert:
            conv_name, conv_bytes = convert_asset_bytes(base_name, data)
            if conv_name and conv_bytes:
                dst = os.path.join(out_dir, conv_name)
                with open(dst, 'wb') as out_f:
                    out_f.write(conv_bytes)
                return {'format': 'Standalone-Asset', 'extracted': 0, 'converted': 1, 'files': [dst]}
        raise ValueError(f'{path}: unrecognized archive format (and no standard TIM2/VAG signatures found)')
    summary = {'format': arc.format_name, 'extracted': 0, 'converted': 0, 'files': []}
    catalog = {
        'source': os.path.basename(path),
        'format': arc.format_name,
        'textures': [],
        'audio': [],
        'meshes': [],
    }

    def _convert_and_record(rel_prefix: str, payload: bytes, target_dir: str) -> None:
        stem, ext = os.path.splitext(rel_prefix)
        low_ext = ext.lower()

        # 1. TIM2 Texture -> .png
        if payload[:4] == b'TIM2' or low_ext in ('.tm2', '.tim2'):
            w, h, rgba = decode_tim2_to_rgba(payload)
            png_bytes = encode_rgba_png(w, h, rgba)
            png_path = os.path.join(target_dir, f'{stem}.png')
            os.makedirs(os.path.dirname(png_path), exist_ok=True)
            with open(png_path, 'wb') as cf:
                cf.write(png_bytes)
            summary['converted'] += 1
            summary['files'].append(png_path)
            catalog['textures'].append({
                'id': stem,
                'file': os.path.relpath(png_path, out_dir),
                'width': w,
                'height': h,
            })
            return

        # 2. VAG Audio -> .wav
        if payload[:4] == b'VAGp' or low_ext == '.vag':
            sr, ns, wav_bytes = decode_vag_to_wav_bytes(payload)
            wav_path = os.path.join(target_dir, f'{stem}.wav')
            os.makedirs(os.path.dirname(wav_path), exist_ok=True)
            with open(wav_path, 'wb') as cf:
                cf.write(wav_bytes)
            summary['converted'] += 1
            summary['files'].append(wav_path)
            catalog['audio'].append({
                'id': stem,
                'file': os.path.relpath(wav_path, out_dir),
                'sampleRate': sr,
                'samples': ns,
            })
            return

        # 3. FMOD 3D Model -> combined .obj + split _partXX.obj submeshes
        if is_fmod_bytes(payload) or low_ext == '.fmod':
            meshes = decode_fmod_to_meshes(payload)
            if meshes:
                full_obj_path = os.path.join(target_dir, f'{stem}.obj')
                os.makedirs(os.path.dirname(full_obj_path), exist_ok=True)
                with open(full_obj_path, 'w', encoding='utf-8') as cf:
                    cf.write(export_meshes_to_obj(meshes, header_comment=f'PS2 FMOD 3D Mesh ({stem})'))
                summary['converted'] += 1
                summary['files'].append(full_obj_path)

                if len(meshes) > 1:
                    for sub in meshes:
                        part_id = f'{stem}_part{sub.index:02d}'
                        part_path = os.path.join(target_dir, f'{part_id}.obj')
                        with open(part_path, 'w', encoding='utf-8') as pf:
                            pf.write(export_mesh_to_obj(sub, header_comment=f'PS2 FMOD SubMesh {sub.index} ({stem})'))
                        summary['converted'] += 1
                        summary['files'].append(part_path)
                        catalog['meshes'].append({
                            'id': part_id,
                            'file': os.path.relpath(part_path, out_dir),
                            'vertexCount': len(sub.vertices),
                            'triangleCount': len(sub.faces),
                            'bounds': sub.compute_bounds(),
                        })
                else:
                    sub = meshes[0]
                    catalog['meshes'].append({
                        'id': stem,
                        'file': os.path.relpath(full_obj_path, out_dir),
                        'vertexCount': len(sub.vertices),
                        'triangleCount': len(sub.faces),
                        'bounds': sub.compute_bounds(),
                    })
            return

        # 4. Other convertible assets (.tex, .16, .vif)
        conv_name, conv_bytes = convert_asset_bytes(rel_prefix, payload)
        if conv_name and conv_bytes and not conv_name.endswith(('.dec.bin', '.dec.pac', '.dec.afs', '.dec.fmod', '.dec.tm2')):
            conv_path = os.path.join(target_dir, conv_name)
            os.makedirs(os.path.dirname(conv_path), exist_ok=True)
            with open(conv_path, 'wb') as cf:
                cf.write(conv_bytes)
            summary['converted'] += 1
            summary['files'].append(conv_path)

    def _process_archive(driver, target_dir, limit=None):
        os.makedirs(target_dir, exist_ok=True)
        entries = driver.entries[:limit] if limit is not None else driver.entries
        for entry in entries:
            safe_rel = entry.name.replace('\\', '/').lstrip('/')
            raw_bytes = driver.read_entry(entry)
            dst_path = os.path.join(target_dir, safe_rel)
            os.makedirs(os.path.dirname(dst_path), exist_ok=True)
            with open(dst_path, 'wb') as out_f:
                out_f.write(raw_bytes)
            summary['extracted'] += 1
            summary['files'].append(dst_path)

            # Check if the entry is Capcom LZ16-compressed
            active_bytes = raw_bytes
            active_rel = safe_rel
            active_kind = entry.kind
            if (convert or recursive) and raw_bytes[:4] not in (b'TIM2', b'VAGp', b'AFS\x00', b'MOMO', b'\x7fELF', b'MWo3') and 32 <= len(raw_bytes) <= 4 * 1024 * 1024:
                dec = decompress_capcom_lz16(raw_bytes)
                if dec is not None and len(dec) > len(raw_bytes):
                    dec_ext, dec_kind = _sniff_payload_ext_and_kind(dec)
                    stem = os.path.splitext(safe_rel)[0]
                    active_rel = f'{stem}.dec{dec_ext}'
                    active_bytes = dec
                    active_kind = dec_kind
                    dec_path = os.path.join(target_dir, active_rel)
                    with open(dec_path, 'wb') as df:
                        df.write(dec)
                    summary['converted'] += 1
                    summary['files'].append(dec_path)

            if recursive and active_kind.startswith('archive:'):
                sub_dir = os.path.join(target_dir, os.path.splitext(active_rel)[0] + '_unpacked')
                try:
                    sub_drv = open_archive_bytes(active_bytes, path=active_rel)
                    _process_archive(sub_drv, sub_dir, limit=None)
                except ValueError:
                    pass

            if convert:
                try:
                    _convert_and_record(active_rel, active_bytes, target_dir)
                except Exception:
                    pass

    _process_archive(arc, out_dir, limit=max_entries)

    if convert and (catalog['textures'] or catalog['audio'] or catalog['meshes']):
        catalog_path = os.path.join(out_dir, 'assets_catalog.json')
        with open(catalog_path, 'w', encoding='utf-8') as jf:
            json.dump(catalog, jf, indent=2)
            jf.write('\n')
        summary['catalog'] = catalog_path

    return summary
