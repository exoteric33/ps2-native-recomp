"""
Physical container drivers for PS2 disc images:
- Iso2048Device : Standard 2048-byte sector .iso images (DVD-5 and DVD-9).
- Bin2352Device : Raw 2352-byte sector CD-ROM .bin / .cue images (Mode 1 and Mode 2 XA Form 1/Form 2).
- CsoDevice     : Compressed .cso (CISO v1/v2 zlib) images read on the fly without full decompression.
- ChdDevice     : MAME Compressed Hunks of Data (.chd v5) DVD and CD images (zlib, lzma, zstd, cdzl, cdlz, cdzs, raw).
"""

from collections import OrderedDict
import ctypes
import ctypes.util
import lzma
import os
import re
import struct
import zlib

SECTOR_SIZE = 2048
RAW_CD_SECTOR_SIZE = 2352
CD_FRAME_SIZE = 2448
CD_SYNC_PATTERN = b'\x00\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\x00'
CSO_MAGIC = b'CISO'
CHD_MAGIC = b'MComprHD'


class SectorDevice:
    """Abstract 2048-byte logical sector reader over any physical disc image container."""

    format_name = 'UNKNOWN'
    raw_sector_size = SECTOR_SIZE

    def __init__(self, path, total_sectors):
        self.path = path
        self.total_sectors = total_sectors

    @property
    def logical_size(self):
        return self.total_sectors * SECTOR_SIZE

    def read_sectors(self, sector, count):
        raise NotImplementedError

    def close(self):
        pass

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, tb):
        self.close()


class Iso2048Device(SectorDevice):
    """Standard 2048-byte cooked ISO image reader (DVD-5 / DVD-9 / cooked CD)."""

    format_name = 'ISO-2048'
    raw_sector_size = SECTOR_SIZE

    def __init__(self, path):
        self._fp = open(path, 'rb')
        size = os.path.getsize(path)
        super().__init__(path, size // SECTOR_SIZE)

    def read_sectors(self, sector, count):
        if count <= 0 or sector >= self.total_sectors:
            return b''
        clamped = min(count, self.total_sectors - sector)
        self._fp.seek(sector * SECTOR_SIZE)
        data = self._fp.read(clamped * SECTOR_SIZE)
        if len(data) < clamped * SECTOR_SIZE:
            data += b'\x00' * (clamped * SECTOR_SIZE - len(data))
        return data

    def close(self):
        if getattr(self, '_fp', None):
            self._fp.close()
            self._fp = None


class Bin2352Device(SectorDevice):
    """
    Raw 2352-byte CD-ROM image reader (.bin or .cue).
    Supports Mode 1 (payload at +16) and Mode 2 XA Form 1/2 (payload at +24).
    """

    format_name = 'BIN-2352'
    raw_sector_size = RAW_CD_SECTOR_SIZE

    def __init__(self, path, bin_path=None, track_sector_offset=0):
        self.cue_path = path if path != bin_path and bin_path else None
        actual_bin = bin_path or path
        self._fp = open(actual_bin, 'rb')
        self.bin_path = actual_bin
        self.track_sector_offset = track_sector_offset
        size = os.path.getsize(actual_bin)
        total = max(0, (size // RAW_CD_SECTOR_SIZE) - track_sector_offset)
        super().__init__(path, total)
        self.cd_mode = self._detect_mode()

    def _detect_mode(self):
        self._fp.seek(self.track_sector_offset * RAW_CD_SECTOR_SIZE)
        raw = self._fp.read(RAW_CD_SECTOR_SIZE)
        if len(raw) >= 16 and raw[:12] == CD_SYNC_PATTERN:
            mode = raw[15]
            if mode in (1, 2):
                return mode
        return 2  # PS2 CD-ROMs are almost always Mode 2 XA

    @staticmethod
    def extract_user_data(raw_sector, default_mode=2):
        if len(raw_sector) < RAW_CD_SECTOR_SIZE:
            raw_sector = raw_sector.ljust(RAW_CD_SECTOR_SIZE, b'\x00')
        mode = raw_sector[15] if raw_sector[:12] == CD_SYNC_PATTERN else default_mode
        if mode == 1:
            return raw_sector[16:16 + SECTOR_SIZE]
        # Mode 2 XA: 12B sync + 4B header + 8B subheader = 24B offset
        return raw_sector[24:24 + SECTOR_SIZE]

    def read_sectors(self, sector, count):
        if count <= 0 or sector >= self.total_sectors:
            return b''
        clamped = min(count, self.total_sectors - sector)
        phys_sector = self.track_sector_offset + sector
        self._fp.seek(phys_sector * RAW_CD_SECTOR_SIZE)
        raw = self._fp.read(clamped * RAW_CD_SECTOR_SIZE)
        out = bytearray(clamped * SECTOR_SIZE)
        for i in range(clamped):
            chunk = raw[i * RAW_CD_SECTOR_SIZE:(i + 1) * RAW_CD_SECTOR_SIZE]
            if not chunk:
                break
            out[i * SECTOR_SIZE:(i + 1) * SECTOR_SIZE] = self.extract_user_data(chunk, self.cd_mode)
        return bytes(out)

    def close(self):
        if getattr(self, '_fp', None):
            self._fp.close()
            self._fp = None


class CsoDevice(SectorDevice):
    """
    Compressed ISO (.cso) reader supporting standard CISO v1 zlib block streams.
    Header layout (24 bytes):
      0..4   : 'CISO'
      4..8   : header_size (uint32 LE, usually 24)
      8..16  : total_bytes (uint64 LE, uncompressed ISO size)
      16..20 : block_size  (uint32 LE, usually 2048)
      20     : version     (uint8)
      21     : align       (uint8, index shift)
      22..24 : reserved
    """

    format_name = 'CSO-ZLIB'
    raw_sector_size = SECTOR_SIZE

    def __init__(self, path):
        self._fp = open(path, 'rb')
        hdr = self._fp.read(24)
        if len(hdr) < 24 or hdr[:4] != CSO_MAGIC:
            self._fp.close()
            raise ValueError(f'{path}: invalid CISO header')
        _, _, total_bytes, block_size, version, align = struct.unpack('<4sIQIBB2x', hdr)
        if block_size == 0 or block_size % SECTOR_SIZE != 0:
            self._fp.close()
            raise ValueError(f'{path}: unsupported CSO block size {block_size}')
        self.block_size = block_size
        self.sectors_per_block = block_size // SECTOR_SIZE
        self.version = version
        self.align = align
        self.num_blocks = (total_bytes + block_size - 1) // block_size
        idx_bytes = (self.num_blocks + 1) * 4
        idx_raw = self._fp.read(idx_bytes)
        if len(idx_raw) < idx_bytes:
            self._fp.close()
            raise ValueError(f'{path}: truncated CSO block index table')
        self.index = struct.unpack(f'<{self.num_blocks + 1}I', idx_raw)
        self._cache = OrderedDict()
        self._max_cache_blocks = 64
        super().__init__(path, total_bytes // SECTOR_SIZE)

    def _read_block(self, block_idx):
        if block_idx in self._cache:
            self._cache.move_to_end(block_idx)
            return self._cache[block_idx]
        if block_idx < 0 or block_idx >= self.num_blocks:
            return b'\x00' * self.block_size

        entry = self.index[block_idx]
        plain = (entry & 0x80000000) != 0
        next_entry = self.index[block_idx + 1] & 0x7FFFFFFF
        offset = (entry & 0x7FFFFFFF) << self.align
        end_offset = next_entry << self.align
        read_len = max(0, end_offset - offset)

        self._fp.seek(offset)
        raw = self._fp.read(read_len)
        if plain:
            data = raw.ljust(self.block_size, b'\x00')[:self.block_size]
        else:
            # CISO uses raw deflate (-zlib.MAX_WBITS) or standard zlib
            try:
                data = zlib.decompress(raw, -zlib.MAX_WBITS)
            except zlib.error:
                data = zlib.decompress(raw)
            if len(data) < self.block_size:
                data = data.ljust(self.block_size, b'\x00')

        self._cache[block_idx] = data
        if len(self._cache) > self._max_cache_blocks:
            self._cache.popitem(last=False)
        return data

    def read_sectors(self, sector, count):
        if count <= 0 or sector >= self.total_sectors:
            return b''
        clamped = min(count, self.total_sectors - sector)
        out = bytearray(clamped * SECTOR_SIZE)
        for i in range(clamped):
            s = sector + i
            blk = s // self.sectors_per_block
            sub = (s % self.sectors_per_block) * SECTOR_SIZE
            blk_data = self._read_block(blk)
            out[i * SECTOR_SIZE:(i + 1) * SECTOR_SIZE] = blk_data[sub:sub + SECTOR_SIZE]
        return bytes(out)

    def close(self):
        if getattr(self, '_fp', None):
            self._fp.close()
            self._fp = None


class _MsbBitReader:
    """MSB-first bitstream reader used to decode CHD v5 Huffman-compressed hunk maps."""

    def __init__(self, data):
        self.data = data
        self.bit_pos = 0
        self.total_bits = len(data) * 8

    def peek(self, nbits):
        if nbits <= 0:
            return 0
        byte_idx = self.bit_pos >> 3
        bit_off = self.bit_pos & 7
        acc = 0
        avail = 0
        while avail < nbits + bit_off:
            b = self.data[byte_idx] if byte_idx < len(self.data) else 0
            acc = (acc << 8) | b
            avail += 8
            byte_idx += 1
        shift = avail - bit_off - nbits
        return (acc >> shift) & ((1 << nbits) - 1)

    def remove(self, nbits):
        self.bit_pos += nbits

    def read(self, nbits):
        val = self.peek(nbits)
        self.bit_pos += nbits
        return val


def _zstd_decompress(raw, uncompressed_len):
    """Decompress a raw Zstandard frame using Python zstandard module or system libzstd via ctypes."""
    try:
        import zstandard
        return zstandard.ZstdDecompressor().decompress(raw, max_output_size=uncompressed_len)
    except ImportError:
        pass

    lib_name = ctypes.util.find_library('zstd')
    if not lib_name:
        raise RuntimeError('Zstandard decompression requires libzstd or python-zstandard')
    if not hasattr(_zstd_decompress, '_lib'):
        lib = ctypes.CDLL(lib_name)
        lib.ZSTD_decompress.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_size_t]
        lib.ZSTD_decompress.restype = ctypes.c_size_t
        lib.ZSTD_isError.argtypes = [ctypes.c_size_t]
        lib.ZSTD_isError.restype = ctypes.c_uint
        _zstd_decompress._lib = lib

    lib = _zstd_decompress._lib
    out_buf = ctypes.create_string_buffer(uncompressed_len)
    res = lib.ZSTD_decompress(out_buf, uncompressed_len, raw, len(raw))
    if lib.ZSTD_isError(res) or res != uncompressed_len:
        raise ValueError(f'ZSTD_decompress failed (ret={res}, expected={uncompressed_len})')
    return out_buf.raw


def _lzma_aligned_dict_size(hunk_bytes):
    """Re-derive MAME/libchdr aligned LZMA dictionary size for a given hunk_bytes (level=6)."""
    dict_size = 1 << 26
    if dict_size > hunk_bytes:
        dict_size = min(dict_size, max(hunk_bytes, 1 << 12))
    if dict_size >= (1 << 21):
        mask = (1 << 20) - 1
        aligned = (dict_size + mask) & ~mask
        return max(aligned, dict_size)
    i = 22
    while True:
        aligned = (2 + (i & 1)) << (i >> 1)
        if aligned >= dict_size:
            return aligned
        i += 1


class ChdDevice(SectorDevice):
    """
    MAME Compressed Hunks of Data (.chd v5) reader for PS2 DVD (2048B units) and CD (2448B units).
    Supports uncompressed and compressed v5 maps with codecs:
      - DVD / general : zlib, lzma, zstd
      - CD-ROM        : cdzl (CD Deflate), cdlz (CD LZMA), cdzs (CD Zstandard)
    """

    CHD_V5_HEADER_SIZE = 124

    def __init__(self, path):
        self._fp = open(path, 'rb')
        hdr = self._fp.read(self.CHD_V5_HEADER_SIZE)
        if len(hdr) < self.CHD_V5_HEADER_SIZE or hdr[:8] != CHD_MAGIC:
            self._fp.close()
            raise ValueError(f'{path}: invalid CHD header')

        header_len, version = struct.unpack_from('>II', hdr, 8)
        if version != 5 or header_len != self.CHD_V5_HEADER_SIZE:
            self._fp.close()
            raise ValueError(f'{path}: unsupported CHD version v{version} (only CHD v5 is supported)')

        self.codecs = [hdr[16 + i * 4:20 + i * 4] for i in range(4)]
        self.logical_bytes, self.map_offset, self.meta_offset, self.hunk_bytes, self.unit_bytes = struct.unpack_from(
            '>QQQII', hdr, 32
        )
        parent_sha1 = hdr[104:124]
        if parent_sha1 != b'\x00' * 20:
            self._fp.close()
            raise ValueError(f'{path}: child CHD requiring external parent is not supported')

        if self.unit_bytes not in (SECTOR_SIZE, RAW_CD_SECTOR_SIZE, CD_FRAME_SIZE):
            self._fp.close()
            raise ValueError(f'{path}: unsupported CHD unit_bytes={self.unit_bytes}')
        if self.hunk_bytes == 0 or self.hunk_bytes % self.unit_bytes != 0:
            self._fp.close()
            raise ValueError(f'{path}: invalid CHD hunk_bytes={self.hunk_bytes} for unit_bytes={self.unit_bytes}')

        self.units_per_hunk = self.hunk_bytes // self.unit_bytes
        self.hunk_count = (self.logical_bytes + self.hunk_bytes - 1) // self.hunk_bytes
        self.is_compressed = self.codecs[0] != b'\x00\x00\x00\x00'
        self._lzma_dict_raw = _lzma_aligned_dict_size(self.hunk_bytes)
        self._lzma_dict_cd = _lzma_aligned_dict_size(self.units_per_hunk * RAW_CD_SECTOR_SIZE)

        active_codecs = '+'.join(c.decode('ascii', 'replace') for c in self.codecs if c != b'\x00\x00\x00\x00') or 'RAW'
        self.format_name = f'CHD-v5({active_codecs})'
        self.raw_sector_size = self.unit_bytes

        # Map entries: list of tuples (comp_type, comp_len, offset)
        self.map_entries = self._load_v5_map()
        self._cache = OrderedDict()
        self._max_cache_hunks = 32
        total_units = self.logical_bytes // self.unit_bytes
        super().__init__(path, total_units)

    def _load_v5_map(self):
        self._fp.seek(self.map_offset)
        if not self.is_compressed:
            raw = self._fp.read(self.hunk_count * 4)
            if len(raw) < self.hunk_count * 4:
                raise ValueError(f'{self.path}: truncated uncompressed CHD v5 map')
            indices = struct.unpack(f'>{self.hunk_count}I', raw)
            entries = []
            for blk_idx in indices:
                if blk_idx == 0:
                    entries.append((4, 0, 0))  # zero-filled hunk
                else:
                    entries.append((4, self.hunk_bytes, blk_idx * self.hunk_bytes))
            return entries

        map_hdr = self._fp.read(16)
        if len(map_hdr) < 16:
            raise ValueError(f'{self.path}: truncated CHD v5 compressed map header')
        map_bytes = struct.unpack_from('>I', map_hdr, 0)[0]
        first_hi, first_lo, _map_crc, length_bits, self_bits, parent_bits, reserved = struct.unpack_from(
            '>HIHBBBB', map_hdr, 4
        )
        first_offs = (first_hi << 32) | first_lo

        # Support direct 12-byte uncompressed map table when reserved == 0xFF and length_bits == 0
        if reserved == 0xFF and length_bits == 0 and map_bytes == self.hunk_count * 12:
            raw = self._fp.read(map_bytes)
            entries = []
            for i in range(self.hunk_count):
                rec = raw[i * 12:(i + 1) * 12]
                ctype = rec[0]
                clen = (rec[1] << 16) | (rec[2] << 8) | rec[3]
                off_hi, off_lo = struct.unpack_from('>HI', rec, 4)
                entries.append((ctype, clen, (off_hi << 32) | off_lo))
            return entries

        comp_blob = self._fp.read(map_bytes)
        if len(comp_blob) < map_bytes:
            raise ValueError(f'{self.path}: truncated CHD v5 map bitstream')

        bs = _MsbBitReader(comp_blob)
        # 1. Import 16-symbol RLE Huffman tree (maxbits=8 -> 4 bits per length value)
        lengths = [0] * 16
        curnode = 0
        while curnode < 16:
            nb = bs.read(4)
            if nb != 1:
                lengths[curnode] = nb
                curnode += 1
            else:
                nb = bs.read(4)
                if nb == 1:
                    lengths[curnode] = 1
                    curnode += 1
                else:
                    rep = bs.read(4) + 3
                    if curnode + rep > 16:
                        raise ValueError(f'{self.path}: invalid CHD v5 Huffman tree RLE count')
                    for _ in range(rep):
                        lengths[curnode] = nb
                        curnode += 1

        # 2. Assign canonical codes and build 256-entry lookup table
        bithisto = [0] * 33
        for nb in lengths:
            bithisto[nb] += 1
        curstart = 0
        for codelen in range(32, 0, -1):
            nextstart = (curstart + bithisto[codelen]) >> 1
            bithisto[codelen] = curstart
            curstart = nextstart

        lookup = [(0, 1)] * 256
        for sym, nb in enumerate(lengths):
            if nb > 0:
                code = bithisto[nb]
                bithisto[nb] += 1
                shift = 8 - nb
                for idx in range(code << shift, (code + 1) << shift):
                    lookup[idx] = (sym, nb)

        def decode_sym():
            sym, nb = lookup[bs.peek(8)]
            bs.remove(nb)
            return sym

        # 3. Pass 1: decode hunk compression types
        types = [0] * self.hunk_count
        repcount = 0
        lastcomp = 0
        for h in range(self.hunk_count):
            if repcount > 0:
                types[h] = lastcomp
                repcount -= 1
            else:
                val = decode_sym()
                if val == 7:  # COMPRESSION_RLE_SMALL
                    types[h] = lastcomp
                    repcount = 2 + decode_sym()
                elif val == 8:  # COMPRESSION_RLE_LARGE
                    types[h] = lastcomp
                    repcount = 2 + 16 + (decode_sym() << 4)
                    repcount += decode_sym()
                else:
                    types[h] = lastcomp = val

        # 4. Pass 2: decode hunk lengths and offsets
        entries = []
        curoffset = first_offs
        last_self = 0
        last_parent = 0
        for h, t in enumerate(types):
            offset = curoffset
            length = 0
            if t in (0, 1, 2, 3):
                length = bs.read(length_bits)
                curoffset += length
                bs.remove(16)  # skip CRC16
            elif t == 4:  # COMPRESSION_NONE
                length = self.hunk_bytes
                curoffset += length
                bs.remove(16)
            elif t == 5:  # COMPRESSION_SELF
                last_self = offset = bs.read(self_bits)
            elif t == 6:  # COMPRESSION_PARENT
                last_parent = offset = bs.read(parent_bits)
            elif t in (9, 10):  # COMPRESSION_SELF_0 / _1
                if t == 10:
                    last_self += 1
                t = 5
                offset = last_self
            elif t == 11:  # COMPRESSION_PARENT_SELF
                t = 6
                last_parent = offset = (h * self.hunk_bytes) // self.unit_bytes
            elif t in (12, 13):  # COMPRESSION_PARENT_0 / _1
                if t == 13:
                    last_parent += self.units_per_hunk
                t = 6
                offset = last_parent
            else:
                raise ValueError(f'{self.path}: unknown CHD v5 hunk compression type {t}')
            entries.append((t, length, offset))
        return entries

    def _decompress_base_codec(self, codec_tag, raw, dest_len, is_cd=False):
        if codec_tag == b'zlib':
            try:
                data = zlib.decompress(raw, -zlib.MAX_WBITS)
            except zlib.error:
                data = zlib.decompress(raw)
        elif codec_tag == b'lzma':
            dict_size = self._lzma_dict_cd if is_cd else self._lzma_dict_raw
            alone_hdr = struct.pack('<BIQ', 93, dict_size, dest_len)
            data = lzma.decompress(alone_hdr + raw, format=lzma.FORMAT_ALONE)
        elif codec_tag == b'zstd':
            data = _zstd_decompress(raw, dest_len)
        else:
            raise ValueError(f'{self.path}: unsupported CHD codec {codec_tag!r}')
        if len(data) < dest_len:
            data = data.ljust(dest_len, b'\x00')
        return data[:dest_len]

    def _decompress_cd_codec(self, base_tag, raw):
        frames = self.units_per_hunk
        complen_bytes = 2 if self.hunk_bytes < 65536 else 3
        ecc_bytes = (frames + 7) // 8
        header_bytes = ecc_bytes + complen_bytes
        if len(raw) < header_bytes:
            raise ValueError(f'{self.path}: truncated CD codec header')
        if complen_bytes == 2:
            complen_base = (raw[ecc_bytes] << 8) | raw[ecc_bytes + 1]
        else:
            complen_base = (raw[ecc_bytes] << 16) | (raw[ecc_bytes + 1] << 8) | raw[ecc_bytes + 2]
        base_stream = raw[header_bytes:header_bytes + complen_base]
        sector_bytes = bytearray(self._decompress_base_codec(base_tag, base_stream, frames * RAW_CD_SECTOR_SIZE, is_cd=True))

        # Reconstruct 2048-byte logical sectors for all frames in the hunk
        logical_hunk = bytearray(frames * SECTOR_SIZE)
        for f in range(frames):
            sec_off = f * RAW_CD_SECTOR_SIZE
            if (raw[f >> 3] & (1 << (7 - (f & 7)))) != 0:
                sector_bytes[sec_off:sec_off + 12] = CD_SYNC_PATTERN
            raw_sec = sector_bytes[sec_off:sec_off + RAW_CD_SECTOR_SIZE]
            logical_hunk[f * SECTOR_SIZE:(f + 1) * SECTOR_SIZE] = Bin2352Device.extract_user_data(raw_sec)
        return bytes(logical_hunk)

    def _read_logical_hunk(self, hunk_idx):
        if hunk_idx in self._cache:
            self._cache.move_to_end(hunk_idx)
            return self._cache[hunk_idx]
        if hunk_idx < 0 or hunk_idx >= self.hunk_count:
            return b'\x00' * (self.units_per_hunk * SECTOR_SIZE)

        ctype, clen, offset = self.map_entries[hunk_idx]
        if ctype == 5:  # COMPRESSION_SELF
            if offset >= hunk_idx:
                raise ValueError(f'{self.path}: invalid self-reference in hunk {hunk_idx} -> {offset}')
            data = self._read_logical_hunk(offset)
        elif ctype == 4:  # COMPRESSION_NONE
            if clen == 0 and offset == 0:
                raw_hunk = b'\x00' * self.hunk_bytes
            else:
                self._fp.seek(offset)
                raw_hunk = self._fp.read(self.hunk_bytes).ljust(self.hunk_bytes, b'\x00')
            if self.unit_bytes == SECTOR_SIZE:
                data = raw_hunk
            else:
                logical = bytearray(self.units_per_hunk * SECTOR_SIZE)
                for f in range(self.units_per_hunk):
                    raw_sec = raw_hunk[f * self.unit_bytes:f * self.unit_bytes + RAW_CD_SECTOR_SIZE]
                    logical[f * SECTOR_SIZE:(f + 1) * SECTOR_SIZE] = Bin2352Device.extract_user_data(raw_sec)
                data = bytes(logical)
        elif ctype in (0, 1, 2, 3):
            codec_tag = self.codecs[ctype]
            self._fp.seek(offset)
            raw = self._fp.read(clen)
            if codec_tag in (b'cdzl', b'cdlz', b'cdzs'):
                base_map = {b'cdzl': b'zlib', b'cdlz': b'lzma', b'cdzs': b'zstd'}
                data = self._decompress_cd_codec(base_map[codec_tag], raw)
            else:
                raw_hunk = self._decompress_base_codec(codec_tag, raw, self.hunk_bytes, is_cd=False)
                if self.unit_bytes == SECTOR_SIZE:
                    data = raw_hunk
                else:
                    logical = bytearray(self.units_per_hunk * SECTOR_SIZE)
                    for f in range(self.units_per_hunk):
                        raw_sec = raw_hunk[f * self.unit_bytes:f * self.unit_bytes + RAW_CD_SECTOR_SIZE]
                        logical[f * SECTOR_SIZE:(f + 1) * SECTOR_SIZE] = Bin2352Device.extract_user_data(raw_sec)
                    data = bytes(logical)
        else:
            raise ValueError(f'{self.path}: unsupported hunk type {ctype}')

        self._cache[hunk_idx] = data
        if len(self._cache) > self._max_cache_hunks:
            self._cache.popitem(last=False)
        return data

    def read_sectors(self, sector, count):
        if count <= 0 or sector >= self.total_sectors:
            return b''
        clamped = min(count, self.total_sectors - sector)
        out = bytearray(clamped * SECTOR_SIZE)
        for i in range(clamped):
            s = sector + i
            hunk_idx = s // self.units_per_hunk
            sub_off = (s % self.units_per_hunk) * SECTOR_SIZE
            hunk_data = self._read_logical_hunk(hunk_idx)
            out[i * SECTOR_SIZE:(i + 1) * SECTOR_SIZE] = hunk_data[sub_off:sub_off + SECTOR_SIZE]
        return bytes(out)

    def close(self):
        if getattr(self, '_fp', None):
            self._fp.close()
            self._fp = None


def parse_cue_sheet(cue_path):
    """Parse a .cue file to locate its primary data .bin file and sector size."""
    cue_dir = os.path.dirname(os.path.abspath(cue_path))
    bin_file = None
    sector_size = RAW_CD_SECTOR_SIZE
    with open(cue_path, 'r', encoding='utf-8', errors='replace') as f:
        for line in f:
            line = line.strip()
            m_file = re.match(r'^FILE\s+"([^"]+)"', line, re.IGNORECASE) or re.match(r'^FILE\s+(\S+)', line, re.IGNORECASE)
            if m_file and bin_file is None:
                candidate = os.path.join(cue_dir, m_file.group(1))
                if os.path.isfile(candidate):
                    bin_file = candidate
            m_track = re.match(r'^TRACK\s+\d+\s+MODE\d/(\d+)', line, re.IGNORECASE)
            if m_track:
                sector_size = int(m_track.group(1))
    if not bin_file:
        raise ValueError(f'{cue_path}: could not find referenced .bin file')
    return bin_file, sector_size


def open_container(path):
    """
    Auto-detect and open a PS2 disc image container (.iso, .bin, .cue, .cso, .chd).
    Checks magic bytes and sector sync patterns before falling back to extension/size.
    """
    if not os.path.isfile(path):
        raise FileNotFoundError(f'Disc image not found: {path}')

    lower = path.lower()
    if lower.endswith('.cue'):
        bin_path, sec_size = parse_cue_sheet(path)
        if sec_size == RAW_CD_SECTOR_SIZE:
            return Bin2352Device(path, bin_path=bin_path)
        return Iso2048Device(bin_path)

    with open(path, 'rb') as probe:
        head = probe.read(32)
        if head[:4] == CSO_MAGIC:
            return CsoDevice(path)
        if head[:8] == CHD_MAGIC:
            return ChdDevice(path)

        # Check if sector 0 has the 12-byte CD-ROM sync pattern AND sector 16 (at 16 * 2352) has CD001
        if head[:12] == CD_SYNC_PATTERN:
            probe.seek(16 * RAW_CD_SECTOR_SIZE)
            sec16_raw = probe.read(RAW_CD_SECTOR_SIZE)
            if len(sec16_raw) >= 32 and (sec16_raw[16:22] == b'\x01CD001' or sec16_raw[24:30] == b'\x01CD001'):
                return Bin2352Device(path)

        # Check standard 2048-byte ISO 9660 PVD at sector 16 (offset 32768)
        probe.seek(16 * SECTOR_SIZE)
        sec16_iso = probe.read(6)
        if sec16_iso == b'\x01CD001':
            return Iso2048Device(path)

        # Also check if a raw 2352-byte image lacked sync at sector 0 but has PVD at 16 * 2352
        probe.seek(16 * RAW_CD_SECTOR_SIZE)
        sec16_raw = probe.read(32)
        if len(sec16_raw) >= 30 and (sec16_raw[16:22] == b'\x01CD001' or sec16_raw[24:30] == b'\x01CD001'):
            return Bin2352Device(path)

    raise ValueError(f'{path}: unrecognized disc container format (neither ISO-2048, BIN-2352, CSO, nor CHD)')
