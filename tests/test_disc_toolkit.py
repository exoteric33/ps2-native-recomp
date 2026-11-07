#!/usr/bin/env python3
"""
Automated unit tests for the independent PS2 Disc Extraction Toolkit (tools/ps2_disc_toolkit):
1. DVD-9 Dual-Layer ISO-2048 with PVD root_size=0 on Layer 1 and SYSTEM.CNF auto-detection
2. CD-ROM BIN/CUE-2352 (Mode 2 XA Form 1) extraction and verification
3. CSO (CISO zlib compressed) image extraction and verification
4. Hidden-LBA unindexed region splitting (unindexed/region_*.bin) and custom profile regions
"""

import lzma
import os
import struct
import sys
import tempfile
import unittest
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))

from ps2_disc_toolkit.containers import (  # noqa: E402
    SECTOR_SIZE,
    RAW_CD_SECTOR_SIZE,
    CD_SYNC_PATTERN,
    CSO_MAGIC,
    CHD_MAGIC,
    _lzma_aligned_dict_size,
    open_container,
)
from ps2_disc_toolkit.cli import (  # noqa: E402
    extract_disc,
    verify_disc,
    detect_extracted_boot_elf,
)


def make_dir_record(lba, size, name_bytes, is_dir=False):
    name_len = len(name_bytes)
    rec_len = 33 + name_len
    if rec_len % 2 == 1:
        rec_len += 1
    buf = bytearray(rec_len)
    buf[0] = rec_len
    struct.pack_into('<I', buf, 2, lba)
    struct.pack_into('>I', buf, 6, lba)
    struct.pack_into('<I', buf, 10, size)
    struct.pack_into('>I', buf, 14, size)
    buf[25] = 2 if is_dir else 0
    struct.pack_into('<H', buf, 28, 1)
    struct.pack_into('>H', buf, 30, 1)
    buf[32] = name_len
    buf[33:33 + name_len] = name_bytes
    return bytes(buf)


def make_pvd(volume_id, total_sectors, root_lba, root_size):
    pvd = bytearray(SECTOR_SIZE)
    pvd[0:6] = b'\x01CD001'
    pvd[6] = 1
    vol_bytes = volume_id.encode('ascii')[:32].ljust(32, b' ')
    pvd[40:72] = vol_bytes
    struct.pack_into('<I', pvd, 80, total_sectors)
    struct.pack_into('>I', pvd, 84, total_sectors)
    struct.pack_into('<H', pvd, 128, SECTOR_SIZE)
    struct.pack_into('>H', pvd, 130, SECTOR_SIZE)
    root_rec = make_dir_record(root_lba, root_size, b'\x00', is_dir=True)
    pvd[156:156 + len(root_rec)] = root_rec
    return bytes(pvd)


def build_synthetic_dvd9_iso():
    """
    Create a synthetic 80-sector dual-layer DVD-9 ISO:
    - Layer 0: sectors 0..39 (PVD at 16, root dir at 20, SYSTEM.CNF at 21, SLES_999.01 at 22, hidden LBA data at 30..33)
    - Layer 1: base=40, sectors 40..79 (PVD at 56 with root_size=0, root dir at 60, ACT2.LMP at 61..62)
    """
    total_sectors = 80
    l0_sectors = 40
    l1_sectors = 40
    img = bytearray(total_sectors * SECTOR_SIZE)

    syscnf_bytes = b"BOOT2 = cdrom0:\\SLES_999.01;1\r\nVER = 1.02\r\nVMODE = PAL\r\n"
    elf_bytes = b"\x7fELF" + b"SYNTHETIC_PS2_BOOT_EXECUTABLE" * 20
    l1_file_bytes = b"LAYER1_SNOWBLIND_LMP_ARCHIVE_" * 100

    # Layer 0 root directory at sector 20
    l0_root = bytearray()
    dot_pos = 0
    l0_root += make_dir_record(20, 0, b'\x00', is_dir=True)
    l0_root += make_dir_record(20, 0, b'\x01', is_dir=True)
    l0_root += make_dir_record(21, len(syscnf_bytes), b'SYSTEM.CNF;1', is_dir=False)
    l0_root += make_dir_record(22, len(elf_bytes), b'SLES_999.01;1', is_dir=False)
    l0_root_size = len(l0_root)
    struct.pack_into('<I', l0_root, dot_pos + 10, l0_root_size)

    img[16 * SECTOR_SIZE:17 * SECTOR_SIZE] = make_pvd('TEST_DVD9_L0', l0_sectors, 20, l0_root_size)
    img[20 * SECTOR_SIZE:20 * SECTOR_SIZE + l0_root_size] = l0_root
    img[21 * SECTOR_SIZE:21 * SECTOR_SIZE + len(syscnf_bytes)] = syscnf_bytes
    img[22 * SECTOR_SIZE:22 * SECTOR_SIZE + len(elf_bytes)] = elf_bytes

    # Hidden LBA data at sectors 30..33 (4 sectors = 8192 bytes outside ISO 9660 directory tree)
    hidden_data = (b"HIDDEN_LBA_STREAM_BLOCK_" * 342)[:4 * SECTOR_SIZE]
    img[30 * SECTOR_SIZE:34 * SECTOR_SIZE] = hidden_data

    # Layer 1 root directory at relative LBA 20 (absolute sector 40 + 20 = 60)
    l1_root = bytearray()
    l1_root += make_dir_record(20, 0, b'\x00', is_dir=True)
    l1_root += make_dir_record(20, 0, b'\x01', is_dir=True)
    l1_root += make_dir_record(21, len(l1_file_bytes), b'ACT2.LMP;1', is_dir=False)
    l1_root_size = len(l1_root)
    struct.pack_into('<I', l1_root, 10, l1_root_size)

    # Layer 1 PVD at sector 56 (l1_base + 16) intentionally sets root_size=0 to test DVD-9 fallback
    img[56 * SECTOR_SIZE:57 * SECTOR_SIZE] = make_pvd('TEST_DVD9_L1', l1_sectors, 20, 0)
    img[60 * SECTOR_SIZE:60 * SECTOR_SIZE + l1_root_size] = l1_root
    img[61 * SECTOR_SIZE:61 * SECTOR_SIZE + len(l1_file_bytes)] = l1_file_bytes

    return bytes(img)


def convert_iso_to_bin2352(iso_bytes):
    """Wrap 2048-byte ISO sectors into raw 2352-byte Mode 2 XA Form 1 CD-ROM sectors."""
    num_sectors = len(iso_bytes) // SECTOR_SIZE
    out = bytearray(num_sectors * RAW_CD_SECTOR_SIZE)
    for s in range(num_sectors):
        base = s * RAW_CD_SECTOR_SIZE
        out[base:base + 12] = CD_SYNC_PATTERN
        out[base + 15] = 2  # Mode 2 XA
        user = iso_bytes[s * SECTOR_SIZE:(s + 1) * SECTOR_SIZE]
        out[base + 24:base + 24 + SECTOR_SIZE] = user
        out[base + 24 + SECTOR_SIZE:base + RAW_CD_SECTOR_SIZE] = b'\xAA' * (RAW_CD_SECTOR_SIZE - 24 - SECTOR_SIZE)
    return bytes(out)


def convert_iso_to_cso(iso_bytes):
    """Compress 2048-byte ISO bytes into a CISO v1 (.cso) container."""
    num_blocks = len(iso_bytes) // SECTOR_SIZE
    header_size = 24
    index_table = [0] * (num_blocks + 1)
    data_chunks = []
    cur_offset = header_size + (num_blocks + 1) * 4

    for b in range(num_blocks):
        block = iso_bytes[b * SECTOR_SIZE:(b + 1) * SECTOR_SIZE]
        comp_obj = zlib.compressobj(level=6, wbits=-zlib.MAX_WBITS)
        compressed = comp_obj.compress(block) + comp_obj.flush()
        if len(compressed) < SECTOR_SIZE:
            index_table[b] = cur_offset
            data_chunks.append(compressed)
            cur_offset += len(compressed)
        else:
            index_table[b] = cur_offset | 0x80000000
            data_chunks.append(block)
            cur_offset += len(block)
    index_table[num_blocks] = cur_offset

    hdr = struct.pack('<4sIQIBB2x', CSO_MAGIC, header_size, len(iso_bytes), SECTOR_SIZE, 1, 0)
    idx_bytes = struct.pack(f'<{num_blocks + 1}I', *index_table)
    return hdr + idx_bytes + b''.join(data_chunks)


class _MsbBitWriter:
    """Helper to pack MSB-first bitstreams for synthetic CHD v5 maps."""
    def __init__(self):
        self.buf = bytearray()
        self.acc = 0
        self.bits = 0

    def write(self, val: int, count: int):
        for i in range(count - 1, -1, -1):
            bit = (val >> i) & 1
            self.acc = (self.acc << 1) | bit
            self.bits += 1
            if self.bits == 8:
                self.buf.append(self.acc)
                self.acc = 0
                self.bits = 0

    def to_bytes(self) -> bytes:
        if self.bits > 0:
            self.buf.append(self.acc << (8 - self.bits))
            self.acc = 0
            self.bits = 0
        return bytes(self.buf)


def convert_iso_to_chd_v5_dvd(iso_bytes):
    """
    Build a synthetic CHD v5 DVD container (unit_bytes=2048, hunk_bytes=8192)
    exercising zlib (type 0), lzma (type 1), COMPRESSION_NONE (type 4),
    COMPRESSION_SELF (type 5), and a Huffman-RLE compressed v5 map.
    """
    unit_bytes = SECTOR_SIZE
    sectors_per_hunk = 4
    hunk_bytes = sectors_per_hunk * unit_bytes
    total_hunks = len(iso_bytes) // hunk_bytes
    header_size = 124

    data_chunks = []
    entries = []
    cur_offset = header_size
    first_zero_hunk = None

    for h in range(total_hunks):
        hunk = iso_bytes[h * hunk_bytes:(h + 1) * hunk_bytes]
        crc16 = zlib.crc32(hunk) & 0xFFFF  # placeholder; we use cbits=0 in map header so CRC check is optional, or compute real CRC-16
        if hunk == b'\x00' * hunk_bytes:
            if first_zero_hunk is None:
                # Encode first all-zero hunk with zlib (type 0)
                comp_obj = zlib.compressobj(level=6, wbits=-zlib.MAX_WBITS)
                compressed = comp_obj.compress(hunk) + comp_obj.flush()
                entries.append((0, len(compressed), cur_offset, crc16))
                data_chunks.append(compressed)
                cur_offset += len(compressed)
                first_zero_hunk = h
            else:
                # Subsequent all-zero hunks reference first_zero_hunk via COMPRESSION_SELF (type 5)
                entries.append((5, 0, first_zero_hunk, crc16))
        elif h % 3 == 0:
            # Encode with raw LZMA1 (type 1) matching MAME CHD LZMA parameters
            dict_size = _lzma_aligned_dict_size(hunk_bytes)
            alone = lzma.compress(
                hunk,
                format=lzma.FORMAT_ALONE,
                filters=[{'id': lzma.FILTER_LZMA1, 'lc': 3, 'lp': 0, 'pb': 2, 'dict_size': dict_size}],
            )
            raw_lzma = alone[13:]  # strip 13-byte FORMAT_ALONE header
            entries.append((1, len(raw_lzma), cur_offset, crc16))
            data_chunks.append(raw_lzma)
            cur_offset += len(raw_lzma)
        elif h % 3 == 1:
            # Encode with raw deflate (type 0)
            comp_obj = zlib.compressobj(level=6, wbits=-zlib.MAX_WBITS)
            compressed = comp_obj.compress(hunk) + comp_obj.flush()
            entries.append((0, len(compressed), cur_offset, crc16))
            data_chunks.append(compressed)
            cur_offset += len(compressed)
        else:
            # Store uncompressed (type 4 = COMPRESSION_NONE)
            entries.append((4, hunk_bytes, cur_offset, crc16))
            data_chunks.append(hunk)
            cur_offset += len(hunk)

    # Build a CHD v5 RLE-Huffman compressed map:
    # 1. 16-symbol Huffman tree (4 bits per length):
    #    Give symbols 0..7 bitlength=3 (canonical 3-bit codes 000..111) and symbols 8..15 bitlength=0.
    bw = _MsbBitWriter()
    # Emit 8 copies of bitlength=3 using RLE: code 1 (4 bits), value 3 (4 bits), count=8 -> 8-3=5 (4 bits)
    bw.write(1, 4)
    bw.write(3, 4)
    bw.write(8 - 3, 4)
    # Emit 8 copies of bitlength=0 using RLE: code 1 (4 bits), value 0 (4 bits), count=8 -> 8-3=5 (4 bits)
    bw.write(1, 4)
    bw.write(0, 4)
    bw.write(8 - 3, 4)

    # 2. Pass 1: emit 3-bit canonical code for each hunk's compression type (0, 1, 4, or 5)
    for comp_type, _, _, _ in entries:
        bw.write(comp_type, 3)

    # 3. Pass 2: emit length/offset and 16-bit CRC for each hunk
    length_bits = 16
    self_bits = 16
    parent_bits = 0
    firstoffs = header_size
    for comp_type, comp_len, off_or_self, crc16 in entries:
        if comp_type in (0, 1):
            bw.write(comp_len, length_bits)
            bw.write(crc16, 16)
        elif comp_type == 4:
            bw.write(crc16, 16)
        elif comp_type == 5:
            bw.write(off_or_self, self_bits)

    map_stream = bw.to_bytes()
    map_hdr = bytearray(16)
    struct.pack_into('>I', map_hdr, 0, len(map_stream))
    map_hdr[4:10] = firstoffs.to_bytes(6, 'big')
    struct.pack_into('>H', map_hdr, 10, 0)
    map_hdr[12] = length_bits
    map_hdr[13] = self_bits
    map_hdr[14] = parent_bits
    map_hdr[15] = 0

    map_offset = cur_offset
    chd_hdr = bytearray(header_size)
    chd_hdr[0:8] = CHD_MAGIC
    struct.pack_into('>I', chd_hdr, 8, header_size)
    struct.pack_into('>I', chd_hdr, 12, 5)  # version 5
    chd_hdr[16:20] = b'zlib'
    chd_hdr[20:24] = b'lzma'
    chd_hdr[24:28] = b'\x00\x00\x00\x00'
    chd_hdr[28:32] = b'\x00\x00\x00\x00'
    struct.pack_into('>Q', chd_hdr, 32, total_hunks * hunk_bytes)
    struct.pack_into('>Q', chd_hdr, 40, map_offset)
    struct.pack_into('>Q', chd_hdr, 48, 0)
    struct.pack_into('>I', chd_hdr, 56, hunk_bytes)
    struct.pack_into('>I', chd_hdr, 60, unit_bytes)

    return bytes(chd_hdr) + b''.join(data_chunks) + bytes(map_hdr) + map_stream


def convert_iso_to_chd_v5_cdrom_cdzl(iso_bytes):
    """
    Build a synthetic CHD v5 CD-ROM container (unit_bytes=2448, frames_per_hunk=4)
    using the 'cdzl' codec and a direct 12-byte v5 map table.
    """
    bin2352 = convert_iso_to_bin2352(iso_bytes)
    num_sectors = len(iso_bytes) // SECTOR_SIZE
    unit_bytes = 2448
    frames_per_hunk = 4
    hunk_bytes = frames_per_hunk * unit_bytes
    total_hunks = num_sectors // frames_per_hunk
    header_size = 124

    data_chunks = []
    map_entries = bytearray(total_hunks * 12)
    cur_offset = header_size
    ecc_bytes = (frames_per_hunk + 7) // 8

    for h in range(total_hunks):
        # Build stripped base stream (frames * 2352 with 12-byte sync stripped -> frames * 2340) + subcode stream
        ecc_header = bytearray(ecc_bytes)
        stripped_frames = bytearray()
        subcode_frames = bytearray(frames_per_hunk * 96)
        for f_idx in range(frames_per_hunk):
            s_idx = h * frames_per_hunk + f_idx
            sec = bin2352[s_idx * RAW_CD_SECTOR_SIZE:(s_idx + 1) * RAW_CD_SECTOR_SIZE]
            # Zero 12-byte CD_SYNC_PATTERN and set bit in ecc_header (matching MAME cd_compressor)
            ecc_header[f_idx // 8] |= (1 << (7 - (f_idx % 8)))
            stripped_frames += b'\x00' * 12 + sec[12:]

        comp_base_obj = zlib.compressobj(level=6, wbits=-zlib.MAX_WBITS)
        comp_base = comp_base_obj.compress(bytes(stripped_frames)) + comp_base_obj.flush()

        comp_sub_obj = zlib.compressobj(level=6, wbits=-zlib.MAX_WBITS)
        comp_sub = comp_sub_obj.compress(bytes(subcode_frames)) + comp_sub_obj.flush()

        # hunk_bytes = 9792 < 65536 -> 2-byte big-endian base length header
        hunk_payload = bytes(ecc_header) + struct.pack('>H', len(comp_base)) + comp_base + comp_sub
        data_chunks.append(hunk_payload)

        # Uncompressed 12-byte v5 map entry: [type(1)=0][len(3)][offset(6)][crc16(2)]
        pos = h * 12
        map_entries[pos] = 0  # compressor 0 ('cdzl')
        map_entries[pos + 1:pos + 4] = len(hunk_payload).to_bytes(3, 'big')
        map_entries[pos + 4:pos + 10] = cur_offset.to_bytes(6, 'big')
        cur_offset += len(hunk_payload)

    map_offset = cur_offset
    map_hdr = bytearray(16)
    struct.pack_into('>I', map_hdr, 0, len(map_entries))
    map_hdr[4:10] = header_size.to_bytes(6, 'big')
    map_hdr[12] = 0     # length_bits = 0
    map_hdr[15] = 0xFF  # direct 12-byte map marker

    chd_hdr = bytearray(header_size)
    chd_hdr[0:8] = CHD_MAGIC
    struct.pack_into('>I', chd_hdr, 8, header_size)
    struct.pack_into('>I', chd_hdr, 12, 5)  # version 5
    chd_hdr[16:20] = b'cdzl'
    struct.pack_into('>Q', chd_hdr, 32, total_hunks * hunk_bytes)
    struct.pack_into('>Q', chd_hdr, 40, map_offset)
    struct.pack_into('>I', chd_hdr, 56, hunk_bytes)
    struct.pack_into('>I', chd_hdr, 60, unit_bytes)

    return bytes(chd_hdr) + b''.join(data_chunks) + bytes(map_hdr) + bytes(map_entries)


class TestPs2DiscToolkit(unittest.TestCase):
    def setUp(self):
        self.tmpdir = tempfile.TemporaryDirectory()
        self.iso_bytes = build_synthetic_dvd9_iso()

    def tearDown(self):
        self.tmpdir.cleanup()

    def test_1_dvd9_iso2048_and_hidden_lba(self):
        iso_path = os.path.join(self.tmpdir.name, 'dvd9.iso')
        out_dir = os.path.join(self.tmpdir.name, 'extracted_dvd9')
        with open(iso_path, 'wb') as f:
            f.write(self.iso_bytes)

        # Use raw_threshold_mb=0.005 (~2 sectors) so the 4-sector hidden LBA block at 30..33 is split to unindexed/
        extract_disc(iso_path, out_dir, raw_threshold_mb=0.005)
        verify_disc(iso_path, out_dir)

        self.assertTrue(os.path.isfile(os.path.join(out_dir, 'layer0', 'SYSTEM.CNF')))
        self.assertTrue(os.path.isfile(os.path.join(out_dir, 'layer0', 'SLES_999.01')))
        self.assertTrue(os.path.isfile(os.path.join(out_dir, 'layer1', 'ACT2.LMP')))
        self.assertTrue(os.path.isfile(os.path.join(out_dir, 'unindexed', 'region_30_4.bin')))

        boot_elf = detect_extracted_boot_elf(out_dir)
        self.assertIsNotNone(boot_elf)
        self.assertEqual(os.path.basename(boot_elf), 'SLES_999.01')

    def test_2_bin_cue_2352_cdrom(self):
        bin_path = os.path.join(self.tmpdir.name, 'cdrom.bin')
        cue_path = os.path.join(self.tmpdir.name, 'cdrom.cue')
        out_dir = os.path.join(self.tmpdir.name, 'extracted_cdrom')

        with open(bin_path, 'wb') as f:
            f.write(convert_iso_to_bin2352(self.iso_bytes))
        with open(cue_path, 'w', encoding='utf-8') as f:
            f.write('FILE "cdrom.bin" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n')

        with open_container(cue_path) as dev:
            self.assertEqual(dev.format_name, 'BIN-2352')
            self.assertEqual(dev.total_sectors, 80)

        extract_disc(cue_path, out_dir)
        verify_disc(cue_path, out_dir)
        self.assertTrue(os.path.isfile(os.path.join(out_dir, 'layer1', 'ACT2.LMP')))

    def test_3_cso_compressed_container(self):
        cso_path = os.path.join(self.tmpdir.name, 'compressed.cso')
        out_dir = os.path.join(self.tmpdir.name, 'extracted_cso')

        with open(cso_path, 'wb') as f:
            f.write(convert_iso_to_cso(self.iso_bytes))

        with open_container(cso_path) as dev:
            self.assertEqual(dev.format_name, 'CSO-ZLIB')
            self.assertEqual(dev.total_sectors, 80)

        extract_disc(cso_path, out_dir)
        verify_disc(cso_path, out_dir)
        self.assertTrue(os.path.isfile(os.path.join(out_dir, 'layer0', 'SLES_999.01')))
        self.assertTrue(os.path.isfile(os.path.join(out_dir, 'layer1', 'ACT2.LMP')))

    def test_4_chd_v5_dvd_mixed_codecs(self):
        chd_path = os.path.join(self.tmpdir.name, 'dvd9.chd')
        out_dir = os.path.join(self.tmpdir.name, 'extracted_chd_dvd')

        with open(chd_path, 'wb') as f:
            f.write(convert_iso_to_chd_v5_dvd(self.iso_bytes))

        with open_container(chd_path) as dev:
            self.assertEqual(dev.format_name, 'CHD-v5(zlib+lzma)')
            self.assertEqual(dev.total_sectors, 80)

        extract_disc(chd_path, out_dir, raw_threshold_mb=0.005)
        verify_disc(chd_path, out_dir)
        self.assertTrue(os.path.isfile(os.path.join(out_dir, 'layer0', 'SLES_999.01')))
        self.assertTrue(os.path.isfile(os.path.join(out_dir, 'layer1', 'ACT2.LMP')))
        self.assertTrue(os.path.isfile(os.path.join(out_dir, 'unindexed', 'region_30_4.bin')))

    def test_5_chd_v5_cdrom_cdzl(self):
        chd_path = os.path.join(self.tmpdir.name, 'cdrom.chd')
        out_dir = os.path.join(self.tmpdir.name, 'extracted_chd_cdrom')

        with open(chd_path, 'wb') as f:
            f.write(convert_iso_to_chd_v5_cdrom_cdzl(self.iso_bytes))

        with open_container(chd_path) as dev:
            self.assertEqual(dev.format_name, 'CHD-v5(cdzl)')
            self.assertEqual(dev.total_sectors, 80)

        extract_disc(chd_path, out_dir)
        verify_disc(chd_path, out_dir)
        self.assertTrue(os.path.isfile(os.path.join(out_dir, 'layer0', 'SYSTEM.CNF')))
        self.assertTrue(os.path.isfile(os.path.join(out_dir, 'layer1', 'ACT2.LMP')))

    def _build_synthetic_tim2_and_vag(self):
        # 1. Build a valid 16x16 8-bit indexed Sony TIM2 texture (version 4, 16B alignment)
        width, height = 16, 16
        image_size = width * height
        clut_size = 256 * 4
        header_size = 48
        total_pic_size = header_size + image_size + clut_size
        tim2_hdr = struct.pack('<4sBBH8x', b'TIM2', 4, 0, 1)
        pic_hdr = struct.pack(
            '<IIIHHBBBBHH24x',
            total_pic_size,
            clut_size,
            image_size,
            header_size,
            256,
            0,
            1,
            3,  # 32-bit RGBA CLUT, CSM1
            5,  # 8-bit IDTEX8
            width,
            height,
        )
        pixels = bytes((i % 256) for i in range(image_size))
        clut_flat = b''.join(bytes(((i & 0xFF), ((i * 3) & 0xFF), ((i * 7) & 0xFF), 0x80)) for i in range(256))
        tim2_bytes = tim2_hdr + pic_hdr + pixels + clut_flat

        # 2. Build a valid Sony VAG ('VAGp') audio file with 4 ADPCM blocks (112 samples @ 16000 Hz)
        adpcm_blocks = (
            b'\x12\x00' + b'\x12\x34' * 7
            + b'\x12\x00' + b'\x23\x45' * 7
            + b'\x12\x00' + b'\x34\x56' * 7
            + b'\x12\x01' + b'\x00\x00' * 7
        )
        vag_hdr = struct.pack('>4sIIII12x16s', b'VAGp', 4, 0, len(adpcm_blocks), 16000, b'SYNTH_SFX\x00\x00\x00\x00\x00\x00\x00')
        vag_bytes = vag_hdr + adpcm_blocks
        return tim2_bytes, vag_bytes

    def test_6_cri_afs_archive_and_tim2_vag_conversion(self):
        from ps2_disc_toolkit.archives import extract_archive, open_archive

        tim2_bytes, vag_bytes = self._build_synthetic_tim2_and_vag()
        # Build a CRI Middleware .AFS container with 2 files + metadata directory at 0x1000
        f0_off = 0x800
        f1_off = 0x1000
        meta_off = 0x1800
        afs = bytearray(0x2000)
        afs[0:4] = b'AFS\x00'
        struct.pack_into('<I', afs, 4, 2)
        struct.pack_into('<II', afs, 8, f0_off, len(tim2_bytes))
        struct.pack_into('<II', afs, 16, f1_off, len(vag_bytes))
        struct.pack_into('<II', afs, 24, meta_off, 2 * 48)
        afs[f0_off:f0_off + len(tim2_bytes)] = tim2_bytes
        afs[f1_off:f1_off + len(vag_bytes)] = vag_bytes
        afs[meta_off:meta_off + 32] = b'hud_icon.tm2'.ljust(32, b'\x00')
        afs[meta_off + 48:meta_off + 80] = b'attack_hit.vag'.ljust(32, b'\x00')

        afs_path = os.path.join(self.tmpdir.name, 'test.afs')
        out_dir = os.path.join(self.tmpdir.name, 'afs_out')
        with open(afs_path, 'wb') as f:
            f.write(afs)

        arc = open_archive(afs_path)
        self.assertEqual(arc.format_name, 'CRI-AFS')
        self.assertEqual(len(arc.entries), 2)

        res = extract_archive(afs_path, out_dir, convert=True)
        self.assertEqual(res['extracted'], 2)
        self.assertEqual(res['converted'], 2)
        self.assertTrue(os.path.isfile(os.path.join(out_dir, 'hud_icon.png')))
        self.assertTrue(os.path.isfile(os.path.join(out_dir, 'attack_hit.wav')))

    def test_7_snowblind_lmp_gob_and_gs_tex_conversion(self):
        from ps2_disc_toolkit.archives import extract_archive, open_archive

        # Build a synthetic Snowblind .tex (16x16 PSMCT32 GIF transfer) inside a Snowblind .LMP v2 inside a .GOB
        w, h = 16, 16
        tex_hdr = bytearray(0x80)
        struct.pack_into('<HHHHIII', tex_hdr, 0, w, h, 0, 70, 0x14, 0, 0x80)
        # GIFTag 1: A+D (4 regs: BITBLTBUF, TRXPOS, TRXREG, TRXDIR)
        gif_ad = bytearray()
        gif_ad += struct.pack('<QQ', 4 | (1 << 15) | (0 << 58) | (1 << 60), 0x0E)
        gif_ad += struct.pack('<QQ', (0 << 32) | (1 << 48) | (0 << 56), 0x50)  # BITBLTBUF: DBP=0, DBW=1, DPSM=PSMCT32
        gif_ad += struct.pack('<QQ', 0, 0x51)                                  # TRXPOS: 0, 0
        gif_ad += struct.pack('<QQ', w | (h << 32), 0x52)                      # TRXREG: 16x16
        gif_ad += struct.pack('<QQ', 0, 0x53)                                  # TRXDIR: host->local
        # GIFTag 2: IMAGE (64 QW = 1024 bytes = 16x16 RGBA32)
        gif_img_tag = struct.pack('<QQ', 64 | (1 << 15) | (2 << 58), 0)
        gif_pixels = b'\x20\x80\xe0\x80' * (w * h)
        tex_bytes = bytes(tex_hdr) + bytes(gif_ad) + gif_img_tag + gif_pixels

        _, vag_bytes = self._build_synthetic_tim2_and_vag()

        # Pack tex_bytes and vag_bytes into a Snowblind .LMP v2
        names_blob = b'creature.tex\x00roar.vag\x00'
        lmp_hdr_size = 4 + 2 * 12
        noff0 = lmp_hdr_size
        noff1 = lmp_hdr_size + len(b'creature.tex\x00')
        doff0 = 0x80
        doff1 = doff0 + len(tex_bytes)
        lmp = bytearray(doff1 + len(vag_bytes))
        struct.pack_into('<I', lmp, 0, 2)
        struct.pack_into('<III', lmp, 4, noff0, doff0, len(tex_bytes))
        struct.pack_into('<III', lmp, 16, noff1, doff1, len(vag_bytes))
        lmp[lmp_hdr_size:lmp_hdr_size + len(names_blob)] = names_blob
        lmp[doff0:doff0 + len(tex_bytes)] = tex_bytes
        lmp[doff1:doff1 + len(vag_bytes)] = vag_bytes

        # Wrap the .LMP inside a Snowblind .GOB
        gob = bytearray(0x80 + len(lmp))
        gob[0:12] = b'monster.lmp\x00'
        struct.pack_into('<II', gob, 32, 0x80, len(lmp))
        gob[0x80:0x80 + len(lmp)] = lmp

        gob_path = os.path.join(self.tmpdir.name, 'test.gob')
        out_dir = os.path.join(self.tmpdir.name, 'gob_out')
        with open(gob_path, 'wb') as f:
            f.write(gob)

        arc = open_archive(gob_path)
        self.assertEqual(arc.format_name, 'Snowblind-GOB')
        res = extract_archive(gob_path, out_dir, convert=True, recursive=True)
        self.assertEqual(res['converted'], 2)
        self.assertTrue(os.path.isfile(os.path.join(out_dir, 'monster_unpacked', 'creature.png')))
        self.assertTrue(os.path.isfile(os.path.join(out_dir, 'monster_unpacked', 'roar.wav')))

    def test_8_signature_carver_unknown_container(self):
        from ps2_disc_toolkit.archives import extract_archive

        tim2_bytes, vag_bytes = self._build_synthetic_tim2_and_vag()
        blob = (
            b'PROPRIETARY_UNKNOWN_HEADER_V99' + b'\xAA' * 200
            + tim2_bytes
            + b'\x55' * 128
            + vag_bytes
            + b'\x00' * 64
        )
        blob_path = os.path.join(self.tmpdir.name, 'mystery.dat')
        out_dir = os.path.join(self.tmpdir.name, 'carved_out')
        with open(blob_path, 'wb') as f:
            f.write(blob)

        res = extract_archive(blob_path, out_dir, convert=True, force_carve=True)
        self.assertEqual(res['format'], 'Signature-Carver')
        self.assertEqual(res['extracted'], 2)
        self.assertEqual(res['converted'], 2)

    def test_9_bidirectional_audio_texture_and_mesh_roundtrip(self):
        from ps2_disc_toolkit.ps2_audio import (
            decode_adx_to_wav_bytes,
            decode_vag_to_wav_bytes,
            encode_wav_to_adx_bytes,
            encode_wav_to_vag_bytes,
            is_adx_bytes,
        )
        from ps2_disc_toolkit.ps2_textures import (
            compute_tim2_gpu_key,
            decode_tim2_to_rgba,
            encode_png_to_tim2_bytes,
            encode_rgba_png,
        )

        tim2_bytes, vag_bytes = self._build_synthetic_tim2_and_vag()
        sr, ns, wav_bytes = decode_vag_to_wav_bytes(vag_bytes)
        self.assertEqual(sr, 16000)
        self.assertGreater(ns, 0)

        # VAG <-> WAV round-trip
        vag_rt = encode_wav_to_vag_bytes(wav_bytes, vag_bytes)
        self.assertEqual(vag_rt[:4], b'VAGp')
        sr2, ns2, _ = decode_vag_to_wav_bytes(vag_rt)
        self.assertEqual(sr2, sr)
        self.assertEqual(ns2, ns)

        # ADX <-> WAV round-trip
        adx_bytes = encode_wav_to_adx_bytes(wav_bytes)
        self.assertTrue(is_adx_bytes(adx_bytes))
        adx_sr, adx_ns, adx_wav = decode_adx_to_wav_bytes(adx_bytes)
        self.assertEqual(adx_sr, 16000)
        self.assertGreaterEqual(adx_ns, ns)
        self.assertEqual(adx_wav[:4], b'RIFF')

        # TIM2 <-> PNG round-trip + GPU key
        w, h, rgba = decode_tim2_to_rgba(tim2_bytes)
        png_bytes = encode_rgba_png(w, h, rgba)
        tim2_rt = encode_png_to_tim2_bytes(png_bytes, tim2_bytes)
        w2, h2, rgba2 = decode_tim2_to_rgba(tim2_rt)
        self.assertEqual((w2, h2), (w, h))
        self.assertEqual(rgba2, rgba)
        self.assertGreater(compute_tim2_gpu_key(tim2_rt), 0)


if __name__ == '__main__':
    unittest.main(verbosity=2)



