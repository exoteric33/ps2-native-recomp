"""
Generic PS2 Binary Codecs & Executable Overlay Parsers.

Supported formats:
  1. Capcom 16-bit word-aligned LZ16 compression (decompress_capcom_lz16 / compress_capcom_lz16)
     Used in Monster Hunter, Monster Hunter G, Monster Hunter 2 (dos), Resident Evil Outbreak, etc.
  2. Metrowerks CodeWarrior PS2 'MWo3' Overlay Header parser (parse_mwo3_header)
"""

from __future__ import annotations

import struct
from dataclasses import dataclass
from typing import Optional


@dataclass
class Mwo3OverlayInfo:
    overlay_id: int
    vbase: int
    text_size: int
    data_size: int
    bss_size: int
    init_start: int
    init_end: int
    header_size: int = 0x20

    @property
    def text_offset(self) -> int:
        return self.header_size

    @property
    def data_offset(self) -> int:
        return self.header_size + self.text_size

    @property
    def total_file_payload(self) -> int:
        return self.header_size + self.text_size + self.data_size


def parse_mwo3_header(data: bytes) -> Optional[Mwo3OverlayInfo]:
    """Parse a 32-byte Metrowerks CodeWarrior PS2 'MWo3' overlay header."""
    if len(data) < 0x20 or data[:4] != b"MWo3":
        return None
    _magic, ovl_id, vbase, text_sz, data_sz, bss_sz, init_s, init_e = struct.unpack_from("<8I", data, 0)
    if vbase < 0x00100000 or vbase >= 0x02000000:
        return None
    if 0x20 + text_sz + data_sz > len(data):
        return None
    return Mwo3OverlayInfo(
        overlay_id=ovl_id,
        vbase=vbase,
        text_size=text_sz,
        data_size=data_sz,
        bss_size=bss_sz,
        init_start=init_s,
        init_end=init_e,
    )


def decompress_capcom_lz16(src: bytes, max_out_bytes: int = 8 * 1024 * 1024) -> Optional[bytes]:
    """
    Decompress a Capcom PS2 16-bit word-aligned LZ16 stream.
    Returns the decompressed bytes if `src` is a valid, properly terminated LZ16 stream,
    or None if `src` is not a valid Capcom LZ16 stream.
    """
    if len(src) < 8 or (len(src) & 1) != 0:
        return None

    out = bytearray()
    pos = 0
    n = len(src)
    mask = 0
    flags = 0
    ended = False

    while pos + 2 <= n:
        if mask == 0:
            flags = struct.unpack_from("<H", src, pos)[0]
            mask = 0x8000
            pos += 2
        if (flags & mask) == 0:
            if pos + 2 > n:
                return None
            out.extend(src[pos : pos + 2])
            pos += 2
        else:
            if pos + 2 > n:
                return None
            w = struct.unpack_from("<H", src, pos)[0]
            pos += 2
            count = w >> 11
            disp = w & 0x7FF
            if count == 0:
                if pos + 2 > n:
                    return None
                count = struct.unpack_from("<H", src, pos)[0]
                pos += 2
            if disp == 0:
                if count == 0:
                    ended = True
                    break
                if count > 0x20000:
                    return None
                out.extend(b"\x00\x00" * count)
            else:
                back_bytes = disp * 2
                if back_bytes > len(out) or count > 0x20000:
                    return None
                for _ in range(count):
                    idx = len(out) - back_bytes
                    out.append(out[idx])
                    out.append(out[idx + 1])
            if len(out) > max_out_bytes:
                return None
        mask >>= 1

    if not ended or abs(n - pos) > 16 or len(out) == 0:
        return None
    return bytes(out)


def compress_capcom_lz16(raw: bytes) -> bytes:
    """
    Compress a byte buffer into a Capcom PS2 16-bit word-aligned LZ16 stream.
    Supports:
      - Zero-word RLE runs (disp == 0, count >= 2)
      - 16-bit word LZ back-references (1 <= disp <= 0x7FF, count >= 2)
      - Literal 16-bit words
      - End-of-stream marker (disp == 0, count == 0)
    """
    if len(raw) & 1:
        raw = raw + b"\x00"

    words = [struct.unpack_from("<H", raw, i)[0] for i in range(0, len(raw), 2)]
    num_words = len(words)

    out = bytearray()
    flag_pos = 0
    flags = 0
    bit_idx = 0

    def start_flag_word() -> None:
        nonlocal flag_pos, flags, bit_idx
        flag_pos = len(out)
        out.extend(b"\x00\x00")
        flags = 0
        bit_idx = 0

    def flush_flag_word() -> None:
        struct.pack_into("<H", out, flag_pos, flags)

    def emit_bit(bit: int) -> None:
        nonlocal flags, bit_idx
        if bit_idx == 16:
            flush_flag_word()
            start_flag_word()
        if bit:
            flags |= 0x8000 >> bit_idx
        bit_idx += 1

    start_flag_word()
    i = 0
    while i < num_words:
        # 1. Check zero-word RLE run
        if words[i] == 0:
            z_len = 1
            while i + z_len < num_words and words[i + z_len] == 0 and z_len < 0xFFFF:
                z_len += 1
            if z_len >= 2:
                emit_bit(1)
                if z_len <= 0x1F:
                    out.extend(struct.pack("<H", z_len << 11))
                else:
                    out.extend(struct.pack("<HH", 0, z_len))
                i += z_len
                continue

        # 2. Check 16-bit sliding window back-reference (disp in 1..0x7FF)
        best_len = 0
        best_disp = 0
        win_start = max(0, i - 0x7FF)
        max_match = min(0xFFFF, num_words - i)
        w0 = words[i]
        for cand in range(i - 1, win_start - 1, -1):
            if words[cand] != w0:
                continue
            m_len = 1
            while m_len < max_match and words[cand + m_len] == words[i + m_len]:
                m_len += 1
            if m_len > best_len:
                best_len = m_len
                best_disp = i - cand
                if best_len >= 64:
                    break

        if best_len >= 3 or (best_len == 2 and best_len <= 0x1F):
            emit_bit(1)
            if best_len <= 0x1F:
                out.extend(struct.pack("<H", (best_len << 11) | best_disp))
            else:
                out.extend(struct.pack("<HH", best_disp, best_len))
            i += best_len
        else:
            emit_bit(0)
            out.extend(struct.pack("<H", words[i]))
            i += 1

    # End-of-stream marker: flag bit = 1, w = 0 (count = 0, disp = 0), ext_count = 0
    emit_bit(1)
    out.extend(struct.pack("<HH", 0, 0))
    flush_flag_word()
    return bytes(out)
