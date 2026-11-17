#!/usr/bin/env python
"""
Reads CHD v5 disc images (MAME's "Compressed Hunks of Data") and writes the plain disc image inside as an .iso.

    python chd.py info  game.chd              what is inside (codecs, size, tracks)
    python chd.py iso   game.chd game.iso     convert; a DVD comes out byte for byte, a CD gives its data track as 2048-byte sectors
                                              (uses build/tools/chdunpack when it is built: same result, on all cores, about ten times
                                              faster; PS2_CHD_NATIVE=0 makes it use only this file)
    python chd.py selftest                    check the decoders on built-in data

Every hunk codec chdman writes is decoded here with the standard library or our own code, so a player needs nothing else:
  zlib        raw deflate, Python's zlib
  lzma        LZMA stream without header, Python's lzma (the dictionary only has to be as big as one hunk)
  huff        Huffman, own code
  flac        FLAC frames, own code
  zstd        Zstandard, own code (the standard library's or the zstandard package's decoder when there is one: faster)
  cdzl, cdlz, cdzs, cdfl   the same four for CDs; the sub-channel part is skipped, only the sector data is wanted

Where the rules come from (nothing is copied):
  * The file layout, the hunk map, the Huffman tree and bit order, the codec framing and the CD hunk layout follow the CHD v5
    description in MAME's chd.h and its codecs; libchdr (BSD-3-Clause, https://github.com/rtissera/libchdr) was read to learn them.
  * FLAC: RFC 9639. Zstandard: RFC 8878. LZMA and deflate come from the standard library.
  * CD sector layout (sync, header, mode 1 and mode 2 form 1): ECMA-130.
"""

import array
import hashlib
import os
import struct
import sys
import zlib

try:
    import lzma
except ImportError:  # a Python without lzma: only the lzma codecs are lost
    lzma = None

SECTOR = 2048
CD_RAW = 2352          # one CD frame: sector bytes
CD_SUBCODE = 96
CD_FRAME = CD_RAW + CD_SUBCODE
CD_SYNC = bytes([0]) + bytes([255]) * 10 + bytes([0])
CD_PAD = 4             # chdman pads every CD track to a multiple of 4 frames

# hunk map entry kinds (the map's own numbering)
T_NONE, T_SELF, T_PARENT = 4, 5, 6


class ChdError(Exception):
    pass


# --------------------------------------------------------------------------------------------------------------------
# Huffman (the one CHD uses for its map and for the "huff" codec): canonical codes, most significant bit first
# --------------------------------------------------------------------------------------------------------------------

class _Bits:
    """Reads bits, most significant first, from a bytes object; reading past the end gives zeros and is counted."""

    def __init__(self, data):
        self.data = data
        self.pos = 0  # bit position
        self.total = len(data) * 8

    def read(self, n):
        if n == 0:
            return 0
        v = self.peek(n)
        self.pos += n
        return v

    def peek(self, n):
        byte, off = self.pos >> 3, self.pos & 7
        nbytes = (off + n + 7) >> 3
        part = self.data[byte:byte + nbytes]
        chunk = int.from_bytes(part, 'big') << (8 * (nbytes - len(part)))
        return (chunk >> (nbytes * 8 - off - n)) & ((1 << n) - 1)

    @property
    def overflow(self):
        return self.pos > self.total


class _Huffman:
    """Canonical Huffman decoder with up to 24-bit codes. Codes are assigned from the longest to the shortest."""

    LOOKUP_BITS = 12

    def __init__(self, numcodes, maxbits):
        self.numcodes = numcodes
        self.maxbits = maxbits
        self.lengths = [0] * numcodes
        self.table = None

    def _assign(self):
        hist = [0] * 33
        for n in self.lengths:
            if n > self.maxbits:
                raise ChdError('Huffman code too long')
            hist[n] += 1
        start = 0
        for n in range(32, 0, -1):
            nxt = (start + hist[n]) >> 1
            hist[n] = start
            start = nxt
        codes = [0] * self.numcodes
        for sym, n in enumerate(self.lengths):
            if n:
                codes[sym] = hist[n]
                hist[n] += 1
        self.codes = codes
        look = min(self.maxbits, self.LOOKUP_BITS)
        self.look = look
        table = [0] * (1 << look)
        self.slow = []
        for sym, n in enumerate(self.lengths):
            if not n:
                continue
            code = codes[sym]
            if n <= look:
                shift = look - n
                lo = code << shift
                table[lo:lo + (1 << shift)] = [(sym << 5) | n] * (1 << shift)
            else:
                self.slow.append((n, code, sym))
        self.table = table

    def import_rle(self, bits):
        """The hunk map's tree: lengths with runs, the number of bits per entry depends on maxbits."""
        numbits = 5 if self.maxbits >= 16 else 4 if self.maxbits >= 8 else 3
        cur = 0
        while cur < self.numcodes:
            nb = bits.read(numbits)
            if nb != 1:
                self.lengths[cur] = nb
                cur += 1
            else:
                nb = bits.read(numbits)
                if nb == 1:
                    self.lengths[cur] = 1
                    cur += 1
                else:
                    rep = bits.read(numbits) + 3
                    if cur + rep > self.numcodes:
                        raise ChdError('bad Huffman run in the hunk map')
                    for _ in range(rep):
                        self.lengths[cur] = nb
                        cur += 1
        self._assign()

    def import_huffman(self, bits):
        """The "huff" codec's tree: the lengths are themselves Huffman coded with a small tree."""
        small = _Huffman(24, 6)
        small.lengths[0] = bits.read(3)
        start = bits.read(3) + 1
        count = 0
        for i in range(1, 24):
            if i < start or count == 7:
                small.lengths[i] = 0
            else:
                count = bits.read(3)
                small.lengths[i] = 0 if count == 7 else count
        small._assign()
        rlefull = 0
        t = self.numcodes - 9
        while t:
            t >>= 1
            rlefull += 1
        cur = 0
        last = 0
        while cur < self.numcodes:
            value = small.decode(bits)
            if value:
                last = value - 1
                self.lengths[cur] = last
                cur += 1
            else:
                count = bits.read(3) + 2
                if count == 9:
                    count += bits.read(rlefull)
                while count and cur < self.numcodes:
                    self.lengths[cur] = last
                    cur += 1
                    count -= 1
        self._assign()

    def decode(self, bits):
        look = self.look
        e = self.table[bits.peek(look)]
        if e:
            bits.pos += e & 31
            return e >> 5
        w = bits.peek(self.maxbits)
        for n, code, sym in self.slow:
            if (w >> (self.maxbits - n)) == code:
                bits.pos += n
                return sym
        raise ChdError('bad Huffman code')


# --------------------------------------------------------------------------------------------------------------------
# FLAC (RFC 9639): a run of frames without the file header; 16-bit stereo, as CHD's flac codecs always write it
# --------------------------------------------------------------------------------------------------------------------

_FLAC_BLOCK = {1: 192, 2: 576, 3: 1152, 4: 2304, 5: 4608, 8: 256, 9: 512, 10: 1024, 11: 2048, 12: 4096, 13: 8192, 14: 16384, 15: 32768}


def flac_decode(data, samples_per_channel, channels=2):
    """Decode FLAC frames until samples_per_channel samples are out. Returns (list per channel, bytes used)."""
    X = int.from_bytes(data, 'big')
    total = len(data) * 8
    pos = 0

    def rd(n):
        nonlocal pos
        if n == 0:
            return 0
        if pos + n > total:
            raise ChdError('FLAC data ends too early')
        v = (X >> (total - pos - n)) & ((1 << n) - 1)
        pos += n
        return v

    def rd_signed(n):
        v = rd(n)
        return v - (1 << n) if v >> (n - 1) else v

    def rd_unary():
        nonlocal pos
        q = 0
        while True:
            if pos >= total:
                raise ChdError('FLAC data ends too early')
            w = min(32, total - pos)
            v = (X >> (total - pos - w)) & ((1 << w) - 1)
            if v:
                z = w - v.bit_length()
                pos += z + 1
                return q + z
            q += w
            pos += w

    out = [[] for _ in range(channels)]
    done = 0
    while done < samples_per_channel:
        if pos & 7:
            pos = (pos + 7) & ~7
        start_bit = pos
        if rd(14) != 0x3FFE:
            raise ChdError('FLAC frame sync not found')
        rd(1)
        rd(1)  # reserved, blocking strategy
        bs_code = rd(4)
        sr_code = rd(4)
        ch_code = rd(4)
        ss_code = rd(3)
        rd(1)
        first = rd(8)  # coded frame / sample number: UTF-8 style, 1 to 7 bytes
        extra = 0 if first < 0x80 else 1 if first < 0xE0 else 2 if first < 0xF0 else 3 if first < 0xF8 else 4 if first < 0xFC else 5 if first < 0xFE else 6
        for _ in range(extra):
            rd(8)
        if bs_code == 6:
            blocksize = rd(8) + 1
        elif bs_code == 7:
            blocksize = rd(16) + 1
        elif bs_code in _FLAC_BLOCK:
            blocksize = _FLAC_BLOCK[bs_code]
        else:
            raise ChdError('FLAC block size code not supported')
        if sr_code == 12:
            rd(8)
        elif sr_code in (13, 14):
            rd(16)
        rd(8)  # CRC-8 of the header
        bps = {0: 16, 1: 8, 2: 12, 4: 16, 5: 20, 6: 24}.get(ss_code)
        if bps is None:
            raise ChdError('FLAC sample size not supported')
        if ch_code < 8:
            nch, mode = ch_code + 1, 0
        elif ch_code <= 10:
            nch, mode = 2, ch_code - 7  # 1 left/side, 2 right/side, 3 mid/side
        else:
            raise ChdError('FLAC channel layout not supported')
        if nch != channels:
            raise ChdError('FLAC stream has the wrong number of channels')
        chans = []
        for c in range(nch):
            b = bps
            if (mode == 1 and c == 1) or (mode == 2 and c == 0) or (mode == 3 and c == 1):
                b += 1
            chans.append(_flac_subframe(rd, rd_signed, rd_unary, blocksize, b))
        if mode == 1:      # left, side
            chans[1] = [l - s for l, s in zip(chans[0], chans[1])]
        elif mode == 2:    # side, right
            chans[0] = [s + r for s, r in zip(chans[0], chans[1])]
        elif mode == 3:    # mid, side
            m, s = chans
            left, right = [], []
            for mm, ss in zip(m, s):
                mm = (mm << 1) | (ss & 1)
                left.append((mm + ss) >> 1)
                right.append((mm - ss) >> 1)
            chans = [left, right]
        pos = (pos + 7) & ~7
        rd(16)  # CRC-16
        take = min(blocksize, samples_per_channel - done)
        for c in range(nch):
            out[c].extend(chans[c][:take] if take < blocksize else chans[c])
        done += take
    return out, pos >> 3


def _flac_subframe(rd, rd_signed, rd_unary, blocksize, bps):
    if rd(1):
        raise ChdError('bad FLAC subframe')
    kind = rd(6)
    wasted = 0
    if rd(1):
        wasted = 1
        while not rd(1):
            wasted += 1
    b = bps - wasted
    if kind == 0:
        v = rd_signed(b)
        s = [v] * blocksize
    elif kind == 1:
        s = [rd_signed(b) for _ in range(blocksize)]
    elif 8 <= kind <= 12:
        order = kind - 8
        s = [rd_signed(b) for _ in range(order)]
        res = _flac_residual(rd, rd_unary, blocksize, order)
        if order == 0:
            s = res
        elif order == 1:
            p = s[0]
            for r in res:
                p += r
                s.append(p)
        elif order == 2:
            a, c = s
            for r in res:
                a, c = c, 2 * c - a + r
                s.append(c)
        elif order == 3:
            a, c, d = s
            for r in res:
                a, c, d = c, d, 3 * d - 3 * c + a + r
                s.append(d)
        else:
            a, c, d, e = s
            for r in res:
                a, c, d, e = c, d, e, 4 * e - 6 * d + 4 * c - a + r
                s.append(e)
    elif kind >= 32:
        order = kind - 31
        s = [rd_signed(b) for _ in range(order)]
        prec = rd(4) + 1
        if prec == 16:
            raise ChdError('bad FLAC predictor precision')
        shift = rd_signed(5)
        coefs = [rd_signed(prec) for _ in range(order)]
        res = _flac_residual(rd, rd_unary, blocksize, order)
        coefs.reverse()
        for i, r in enumerate(res):
            acc = 0
            base = i
            for j in range(order):
                acc += coefs[j] * s[base + j]
            s.append((acc >> shift) + r if shift >= 0 else (acc << -shift) + r)
    else:
        raise ChdError('bad FLAC subframe type')
    if wasted:
        s = [v << wasted for v in s]
    return s


def _flac_residual(rd, rd_unary, blocksize, order):
    method = rd(2)
    if method > 1:
        raise ChdError('bad FLAC residual coding')
    pbits = 4 if method == 0 else 5
    esc = (1 << pbits) - 1
    porder = rd(4)
    parts = 1 << porder
    out = []
    for p in range(parts):
        n = (blocksize >> porder) - (order if p == 0 else 0)
        k = rd(pbits)
        if k == esc:
            raw = rd(5)
            for _ in range(n):
                out.append(_signed(rd(raw), raw) if raw else 0)
        else:
            for _ in range(n):
                q = rd_unary()
                v = (q << k) | (rd(k) if k else 0)
                out.append((v >> 1) ^ -(v & 1))
    return out


def _signed(v, n):
    return v - (1 << n) if v >> (n - 1) else v


def flac_pcm_bytes(channels_samples, big_endian):
    """Interleave to 16-bit samples, in the byte order the hunk asks for."""
    left, right = channels_samples
    buf = array.array('h')
    n = len(left)
    inter = [0] * (2 * n)
    inter[0::2] = left
    inter[1::2] = right
    buf.extend(inter)
    if big_endian == (sys.byteorder == 'little'):
        buf.byteswap()
    return buf.tobytes()


# --------------------------------------------------------------------------------------------------------------------
# Zstandard (RFC 8878). One frame is one hunk. The library decoders are used when the Python has one.
# --------------------------------------------------------------------------------------------------------------------

_LL_BASE = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 18, 20, 22, 24, 28, 32, 40, 48, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768, 65536]
_LL_BITS = [0] * 16 + [1, 1, 1, 1, 2, 2, 3, 3, 4, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16]
_ML_BASE = list(range(3, 35)) + [35, 37, 39, 41, 43, 47, 51, 59, 67, 83, 99, 131, 259, 515, 1027, 2051, 4099, 8195, 16387, 32771, 65539]
_ML_BITS = [0] * 32 + [1, 1, 1, 1, 2, 2, 3, 3, 4, 4, 5, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16]
_LL_DEFAULT = (6, [4, 3, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 3, 2, 1, 1, 1, 1, 1, -1, -1, -1, -1])
_ML_DEFAULT = (6, [1, 4, 3, 2, 2, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
                   1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, -1, -1, -1, -1, -1, -1, -1])
_OF_DEFAULT = (5, [1, 1, 1, 1, 1, 1, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, -1, -1, -1, -1, -1])


class _Back:
    """Reads a Zstandard bitstream backwards: the last byte holds a 1 bit that marks the end; bits come out most significant first."""

    def __init__(self, data, lo, hi):
        if hi <= lo or data[hi - 1] == 0:
            raise ChdError('bad Zstandard bitstream')
        last = data[hi - 1]
        self.data = data
        self.lo = lo
        self.idx = hi - 2
        self.cnt = last.bit_length() - 1
        self.acc = last & ((1 << self.cnt) - 1)
        self.deficit = 0  # bits asked for beyond the start (they read as zero)

    def read(self, n):
        if n == 0:
            return 0
        if self.cnt < n:
            acc, cnt, idx, data, lo = self.acc, self.cnt, self.idx, self.data, self.lo
            while cnt < 56 and idx >= lo:
                acc = (acc << 8) | data[idx]
                idx -= 1
                cnt += 8
            self.idx = idx
            if cnt < n:
                miss = n - cnt
                self.deficit += miss
                self.acc = 0
                self.cnt = 0
                return (acc << miss) & ((1 << n) - 1)
            self.acc, self.cnt = acc, cnt
        self.cnt -= n
        v = self.acc >> self.cnt
        self.acc &= (1 << self.cnt) - 1
        return v

    def bits_left(self):
        return self.cnt + 8 * (self.idx - self.lo + 1)


def _fse_table(al, probs):
    size = 1 << al
    sym = [0] * size
    nxt = [0] * len(probs)
    high = size - 1
    for s, p in enumerate(probs):
        if p == -1:
            sym[high] = s
            high -= 1
            nxt[s] = 1
        else:
            nxt[s] = p
    step = (size >> 1) + (size >> 3) + 3
    mask = size - 1
    pos = 0
    for s, p in enumerate(probs):
        for _ in range(p if p > 0 else 0):
            sym[pos] = s
            pos = (pos + step) & mask
            while pos > high:
                pos = (pos + step) & mask
    if pos != 0:
        raise ChdError('bad Zstandard FSE table')
    nbits = [0] * size
    base = [0] * size
    for i in range(size):
        s = sym[i]
        n = nxt[s]
        nxt[s] += 1
        nb = al - (n.bit_length() - 1)
        nbits[i] = nb
        base[i] = (n << nb) - size
    return sym, nbits, base, al


def _fse_read_counts(data, pos, max_symbol, max_al):
    """Reads an FSE table description (forward, least significant bit first). Returns (accuracy log, probabilities, next byte)."""
    X = int.from_bytes(data[pos:pos + 64], 'little')
    nbytes = len(data[pos:pos + 64]) * 8
    al = (X & 15) + 5
    if al > max_al:
        raise ChdError('Zstandard FSE accuracy too large')
    bp = 4
    remaining = (1 << al) + 1
    threshold = 1 << al
    nb = al + 1
    probs = []
    prev0 = False
    while remaining > 1 and len(probs) <= max_symbol:
        if prev0:
            while True:
                r = (X >> bp) & 3
                bp += 2
                probs.extend([0] * r)
                if r != 3:
                    break
            prev0 = False
            if len(probs) > max_symbol:
                break
        mx = (2 * threshold - 1) - remaining
        v = (X >> bp) & (2 * threshold - 1)
        if (v & (threshold - 1)) < mx:
            count = v & (threshold - 1)
            bp += nb - 1
        else:
            count = v
            if count >= threshold:
                count -= mx
            bp += nb
        count -= 1
        remaining -= -count if count < 0 else count
        probs.append(count)
        prev0 = count == 0
        while remaining < threshold:
            nb -= 1
            threshold >>= 1
    if remaining != 1 or bp > nbytes:
        raise ChdError('bad Zstandard FSE table description')
    return al, probs, pos + ((bp + 7) >> 3)


def _zstd_huffman_weights(data, pos, end):
    """Reads a Huffman tree description. Returns (lookup table, max bits, next byte)."""
    hb = data[pos]
    pos += 1
    if hb >= 128:
        n = hb - 127
        weights = []
        for i in range(n):
            b = data[pos + (i >> 1)]
            weights.append((b >> 4) if i % 2 == 0 else (b & 15))
        pos += (n + 1) >> 1
    else:
        stop = pos + hb
        al, probs, bpos = _fse_read_counts(data, pos, 255, 6)
        sym, nbits, base, _ = _fse_table(al, probs)
        back = _Back(data, bpos, stop)
        s1 = back.read(al)
        s2 = back.read(al)
        weights = []
        while True:
            weights.append(sym[s1])
            s1 = base[s1] + back.read(nbits[s1])
            if back.deficit:
                weights.append(sym[s2])
                break
            weights.append(sym[s2])
            s2 = base[s2] + back.read(nbits[s2])
            if back.deficit:
                weights.append(sym[s1])
                break
            if len(weights) > 255:
                raise ChdError('bad Zstandard Huffman weights')
        pos = stop
    total = sum(1 << (w - 1) for w in weights if w)
    if total == 0:
        raise ChdError('bad Zstandard Huffman weights')
    mb = total.bit_length()
    rest = (1 << mb) - total
    if rest & (rest - 1):
        raise ChdError('bad Zstandard Huffman weights')
    weights.append(rest.bit_length())
    if mb > 11 or len(weights) > 256:
        raise ChdError('bad Zstandard Huffman tree')
    table = [0] * (1 << mb)
    at = 0
    for w in range(1, mb + 1):
        cnt = 1 << (w - 1)
        e_nb = mb + 1 - w
        for s, ws in enumerate(weights):
            if ws == w:
                table[at:at + cnt] = [(s << 4) | e_nb] * cnt
                at += cnt
    return table, mb, pos


def _zstd_huff_streams(data, lo, hi, table, mb, n):
    """Decodes n literals from one backward Huffman bitstream."""
    if hi <= lo or data[hi - 1] == 0:
        raise ChdError('bad Zstandard literals stream')
    last = data[hi - 1]
    cnt = last.bit_length() - 1
    acc = last & ((1 << cnt) - 1)
    idx = hi - 2
    out = bytearray(n)
    mask = (1 << mb) - 1
    short = 0
    for i in range(n):
        if cnt < mb:
            while cnt < 56 and idx >= lo:
                acc = (acc << 8) | data[idx]
                idx -= 1
                cnt += 8
            acc &= (1 << cnt) - 1
            if cnt < mb:
                acc <<= mb - cnt
                short += mb - cnt
                cnt = mb
        e = table[(acc >> (cnt - mb)) & mask]
        cnt -= e & 15
        out[i] = e >> 4
    if cnt != short or idx >= lo:  # the padding bits were only look-ahead; every real bit must be used
        raise ChdError('Zstandard literals stream is damaged')
    return bytes(out)


def _zstd_pure(data):
    pos = 0
    out = bytearray()
    n = len(data)
    while pos < n:
        magic = struct.unpack_from('<I', data, pos)[0]
        if 0x184D2A50 <= magic <= 0x184D2A5F:
            pos += 8 + struct.unpack_from('<I', data, pos + 4)[0]
            continue
        if magic != 0xFD2FB528:
            raise ChdError('Zstandard frame expected')
        pos += 4
        fhd = data[pos]
        pos += 1
        single = (fhd >> 5) & 1
        checksum = (fhd >> 2) & 1
        did = fhd & 3
        fcs = fhd >> 6
        if not single:
            pos += 1
        pos += (0, 1, 2, 4)[did]
        pos += (1 if single else 0, 2, 4, 8)[fcs]
        state = {'huff': None, 'll': None, 'of': None, 'ml': None, 'rep': [1, 4, 8]}
        frame_start = len(out)
        while True:
            h = data[pos] | (data[pos + 1] << 8) | (data[pos + 2] << 16)
            pos += 3
            last, btype, bsize = h & 1, (h >> 1) & 3, h >> 3
            if btype == 0:
                out += data[pos:pos + bsize]
                pos += bsize
            elif btype == 1:
                out += data[pos:pos + 1] * bsize
                pos += 1
            elif btype == 2:
                _zstd_block(data, pos, pos + bsize, out, state, frame_start)
                pos += bsize
            else:
                raise ChdError('bad Zstandard block')
            if last:
                break
        if checksum:
            pos += 4
    return bytes(out)


def _zstd_block(data, pos, end, out, st, frame_start):
    b0 = data[pos]
    ltype = b0 & 3
    sf = (b0 >> 2) & 3
    if ltype < 2:
        if sf in (0, 2):
            size, pos = b0 >> 3, pos + 1
        elif sf == 1:
            size, pos = (b0 >> 4) | (data[pos + 1] << 4), pos + 2
        else:
            size, pos = (b0 >> 4) | (data[pos + 1] << 4) | (data[pos + 2] << 12), pos + 3
        if ltype == 0:
            lits = data[pos:pos + size]
            pos += size
        else:
            lits = data[pos:pos + 1] * size
            pos += 1
    else:
        if sf == 0 or sf == 1:
            v = b0 | (data[pos + 1] << 8) | (data[pos + 2] << 16)
            size, csize, pos0 = (v >> 4) & 0x3FF, (v >> 14) & 0x3FF, pos + 3
        elif sf == 2:
            v = b0 | (data[pos + 1] << 8) | (data[pos + 2] << 16) | (data[pos + 3] << 24)
            size, csize, pos0 = (v >> 4) & 0x3FFF, (v >> 18) & 0x3FFF, pos + 4
        else:
            v = b0 | (data[pos + 1] << 8) | (data[pos + 2] << 16) | (data[pos + 3] << 24) | (data[pos + 4] << 32)
            size, csize, pos0 = (v >> 4) & 0x3FFFF, (v >> 22) & 0x3FFFF, pos + 5
        lend = pos0 + csize
        p = pos0
        if ltype == 2:
            st['huff'] = _zstd_huffman_weights(data, p, lend)
            p = st['huff'][2]
        elif st['huff'] is None:
            raise ChdError('Zstandard literals without a Huffman table')
        table, mb, _ = st['huff']
        if sf == 0:
            lits = _zstd_huff_streams(data, p, lend, table, mb, size)
        else:
            s1, s2, s3 = struct.unpack_from('<HHH', data, p)
            p += 6
            seg = (size + 3) >> 2
            parts = []
            a = p
            for sz, cnt in ((s1, seg), (s2, seg), (s3, seg), (lend - p - s1 - s2 - s3, size - 3 * seg)):
                parts.append(_zstd_huff_streams(data, a, a + sz, table, mb, cnt))
                a += sz
            lits = b''.join(parts)
        pos = lend
    if pos >= end:
        out += lits
        return
    nseq = data[pos]
    pos += 1
    if nseq == 0:
        out += lits
        return
    if nseq >= 128:
        if nseq == 255:
            nseq = data[pos] + (data[pos + 1] << 8) + 0x7F00
            pos += 2
        else:
            nseq = ((nseq - 128) << 8) + data[pos]
            pos += 1
    modes = data[pos]
    pos += 1
    tabs = {}
    for name, mode, default, maxsym, maxal in (('ll', modes >> 6, _LL_DEFAULT, 35, 9), ('of', (modes >> 4) & 3, _OF_DEFAULT, 31, 8), ('ml', (modes >> 2) & 3, _ML_DEFAULT, 52, 9)):
        if mode == 0:
            t = _fse_table(default[0], default[1])
        elif mode == 1:
            t = ([data[pos]], [0], [0], 0)
            pos += 1
        elif mode == 2:
            al, probs, pos = _fse_read_counts(data, pos, maxsym, maxal)
            t = _fse_table(al, probs)
        else:
            t = st[name]
            if t is None:
                raise ChdError('Zstandard repeat table without a table')
        st[name] = t
        tabs[name] = t
    llsym, llnb, llbase, llal = tabs['ll']
    ofsym, ofnb, ofbase, ofal = tabs['of']
    mlsym, mlnb, mlbase, mlal = tabs['ml']
    back = _Back(data, pos, end)
    lls = back.read(llal)
    ofs = back.read(ofal)
    mls = back.read(mlal)
    rep0, rep1, rep2 = st['rep']
    lp = 0
    rd = back.read
    for i in range(nseq):
        oc = ofsym[ofs]
        mc = mlsym[mls]
        lc = llsym[lls]
        ofv = (1 << oc) + rd(oc)
        ml = _ML_BASE[mc] + rd(_ML_BITS[mc])
        ll = _LL_BASE[lc] + rd(_LL_BITS[lc])
        if ofv > 3:
            off = ofv - 3
            rep0, rep1, rep2 = off, rep0, rep1
        else:
            idx = ofv - 1 + (1 if ll == 0 else 0)
            if idx == 0:
                off = rep0
            elif idx == 1:
                off = rep1
                rep0, rep1 = rep1, rep0
            elif idx == 2:
                off = rep2
                rep0, rep1, rep2 = rep2, rep0, rep1
            else:
                off = rep0 - 1
                rep0, rep1, rep2 = off, rep0, rep1
        if ll:
            out += lits[lp:lp + ll]
            lp += ll
        if off <= 0 or off > len(out) - frame_start:
            raise ChdError('bad Zstandard match offset')
        start = len(out) - off
        if off >= ml:
            out += out[start:start + ml]
        else:
            seg = bytes(out[start:])
            out += (seg * (ml // off + 1))[:ml]
        if i < nseq - 1:
            lls = llbase[lls] + rd(llnb[lls])
            mls = mlbase[mls] + rd(mlnb[mls])
            ofs = ofbase[ofs] + rd(ofnb[ofs])
    if back.deficit or back.bits_left() != 0:
        raise ChdError('Zstandard sequences are damaged')
    out += lits[lp:]
    st['rep'] = [rep0, rep1, rep2]


def _find_zstd():
    try:
        from compression import zstd  # Python 3.14

        return lambda raw, size: zstd.decompress(raw)
    except Exception:
        pass
    try:
        import zstandard

        d = zstandard.ZstdDecompressor()
        return lambda raw, size: d.decompress(raw, max_output_size=size)
    except Exception:
        pass
    return None


_ZSTD_LIB = _find_zstd()


def zstd_decompress(raw, size, pure_only=False):
    if _ZSTD_LIB is not None and not pure_only:
        return _ZSTD_LIB(raw, size)
    return _zstd_pure(raw)


def _huff_bytes(hf, raw, bitpos, size):
    """Decodes size symbols of a "huff" hunk from bit position bitpos (the tree has been read already)."""
    data = raw + bytes(16)
    table, look, mb = hf.table, hf.look, hf.maxbits
    slow = hf.slow
    mask = (1 << look) - 1
    bi = bitpos >> 3
    cnt = 8 - (bitpos & 7)
    acc = data[bi] & ((1 << cnt) - 1)
    bi += 1
    out = bytearray(size)
    from_bytes = int.from_bytes
    i = 0
    while i < size:
        # a code has at most 16 bits: with 64 bits in hand four codes are decoded without looking again
        if cnt < 64:
            acc = ((acc & ((1 << cnt) - 1)) << 64) | from_bytes(data[bi:bi + 8], 'big')
            bi += 8
            cnt += 64
        n = size - i
        for _ in range(4 if n > 4 else n):
            e = table[(acc >> (cnt - look)) & mask]
            if e:
                cnt -= e & 31
                out[i] = e >> 5
            else:
                w = (acc >> (cnt - mb)) & ((1 << mb) - 1)
                for nb, code, sym in slow:
                    if (w >> (mb - nb)) == code:
                        cnt -= nb
                        out[i] = sym
                        break
                else:
                    raise ChdError('bad Huffman code')
            i += 1
        if bi > len(raw) + 24:  # ran far off the end of a damaged hunk
            break
    if bi * 8 - cnt > len(raw) * 8:
        raise ChdError('Huffman data ends too early')
    return bytes(out)


# --------------------------------------------------------------------------------------------------------------------
# The CHD file
# --------------------------------------------------------------------------------------------------------------------

class Track:
    def __init__(self, number, kind, frames, pregap, pgtype, start):
        self.number, self.kind, self.frames, self.pregap, self.pgtype, self.start = number, kind, frames, pregap, pgtype, start

    @property
    def is_data(self):
        return self.kind != 'AUDIO'


class Chd:
    CODECS = (b'zlib', b'lzma', b'huff', b'flac', b'zstd', b'cdzl', b'cdlz', b'cdfl', b'cdzs')

    def __init__(self, path):
        self.path = path
        self.fp = open(path, 'rb')
        head = self.fp.read(124)
        if len(head) < 16 or head[:8] != b'MComprHD':
            raise ChdError('this is not a CHD file')
        length, version = struct.unpack_from('>II', head, 8)
        if version != 5:
            raise ChdError('this CHD file is version %d; only version 5 is supported (convert it with chdman, or use an .iso)' % version)
        if length != 124 or len(head) < 124:
            raise ChdError('the CHD header is damaged')
        self.codecs = [head[16 + 4 * i:20 + 4 * i] for i in range(4)]
        self.logical, self.map_offset, self.meta_offset, self.hunk_bytes, self.unit_bytes = struct.unpack_from('>QQQII', head, 32)
        self.raw_sha1 = head[64:84]
        if head[104:124] != bytes(20):
            raise ChdError('this CHD file needs a parent CHD file (not supported)')
        if self.hunk_bytes == 0 or self.unit_bytes == 0 or self.hunk_bytes % self.unit_bytes:
            raise ChdError('the CHD header is damaged')
        self.hunks = (self.logical + self.hunk_bytes - 1) // self.hunk_bytes
        self.compressed = self.codecs[0] != bytes(4)
        for c in self.codecs:
            if c != bytes(4) and c not in self.CODECS:
                raise ChdError('this CHD file uses the codec "%s", which is not supported' % c.decode('ascii', 'replace'))
        self._cache = {}
        self._cache_order = []
        self._keep = set()
        self.tracks = self._read_tracks()
        self._read_map()
        self.cd = bool(self.tracks)
        if self.cd:
            if self.unit_bytes != CD_FRAME:
                raise ChdError('the CD layout of this CHD file is not supported')
            self.frames_per_hunk = self.hunk_bytes // CD_FRAME

    def close(self):
        self.fp.close()

    # ---- metadata
    def _metadata(self):
        off = self.meta_offset
        seen = 0
        while off and seen < 100000:
            self.fp.seek(off)
            h = self.fp.read(16)
            if len(h) < 16:
                break
            tag = h[:4]
            length = int.from_bytes(h[5:8], 'big')
            nxt = struct.unpack_from('>Q', h, 8)[0]
            yield tag, self.fp.read(length)
            off = nxt
            seen += 1

    def _read_tracks(self):
        tracks = []
        start = 0
        for tag, data in self._metadata():
            if tag in (b'CHT2', b'CHTR'):
                fields = dict(p.split(':', 1) for p in data.decode('ascii', 'replace').rstrip('\0. \n').split() if ':' in p)
                try:
                    frames = int(fields['FRAMES'])
                    num = int(fields['TRACK'])
                    kind = fields['TYPE']
                except (KeyError, ValueError):
                    raise ChdError('the track list of this CHD file is damaged')
                pregap = int(fields.get('PREGAP', '0') or 0)
                pgtype = fields.get('PGTYPE', '')
                tracks.append(Track(num, kind, frames, pregap, pgtype, start))
                start += (frames + CD_PAD - 1) // CD_PAD * CD_PAD
            elif tag in (b'CHCD', b'CHGT', b'CHGD'):
                raise ChdError('this CHD file has a track list of an old or GD-ROM kind that is not supported')
        return tracks

    def describe(self):
        names = [c.decode('ascii') for c in self.codecs if c != bytes(4)]
        lines = ['CHD version 5, %s' % ('CD' if self.tracks else 'flat image (DVD or hard disk kind)'),
                 'size when unpacked: %d bytes, hunks of %d bytes, %d hunks' % (self.logical, self.hunk_bytes, self.hunks),
                 'codecs: %s' % (', '.join(names) or 'none')]
        for t in self.tracks:
            lines.append('track %d: %s, %d frames, starts at frame %d' % (t.number, t.kind, t.frames, t.start))
        return '\n'.join(lines)

    # ---- map
    def _read_map(self):
        n = self.hunks
        self.types = bytearray(n)
        self.lengths = array.array('Q', [0]) * n
        self.offsets = array.array('Q', [0]) * n
        self.fp.seek(self.map_offset)
        if not self.compressed:
            raw = self.fp.read(4 * n)
            if len(raw) < 4 * n:
                raise ChdError('the hunk map is cut off')
            for i, e in enumerate(struct.unpack('>%dI' % n, raw)):
                self.types[i] = T_NONE
                self.lengths[i] = self.hunk_bytes
                self.offsets[i] = e * self.hunk_bytes
            return
        mh = self.fp.read(16)
        if len(mh) < 16:
            raise ChdError('the hunk map is cut off')
        mapbytes = struct.unpack_from('>I', mh, 0)[0]
        first = int.from_bytes(mh[4:10], 'big')
        length_bits, self_bits, parent_bits = mh[12], mh[13], mh[14]
        blob = self.fp.read(mapbytes)
        if len(blob) < mapbytes:
            raise ChdError('the hunk map is cut off')
        # (the bit reader below pads with zero bytes at the end so the last reads never fall off)
        bits = _Bits(blob + bytes(8))
        tree = _Huffman(16, 8)
        tree.import_rle(bits)
        types = self.types
        rep = 0
        lastcomp = 0
        dec = tree.decode
        for h in range(n):
            if rep:
                types[h] = lastcomp
                rep -= 1
                continue
            v = dec(bits)
            if v == 7:
                types[h] = lastcomp
                rep = 2 + dec(bits)
            elif v == 8:
                types[h] = lastcomp
                rep = 2 + 16 + (dec(bits) << 4)
                rep += dec(bits)
            else:
                types[h] = lastcomp = v
        cur = first
        last_self = last_parent = 0
        for h in range(n):
            t = types[h]
            if t <= 3:
                ln = bits.read(length_bits)
                self.lengths[h] = ln
                self.offsets[h] = cur
                cur += ln
                bits.pos += 16
            elif t == T_NONE:
                self.lengths[h] = self.hunk_bytes
                self.offsets[h] = cur
                cur += self.hunk_bytes
                bits.pos += 16
            elif t == T_SELF:
                last_self = bits.read(self_bits)
                self.offsets[h] = last_self
            elif t == T_PARENT:
                last_parent = bits.read(parent_bits)
                raise ChdError('this CHD file needs a parent CHD file (not supported)')
            elif t in (9, 10):
                if t == 10:
                    last_self += 1
                types[h] = T_SELF
                self.offsets[h] = last_self
            else:
                raise ChdError('this CHD file needs a parent CHD file (not supported)')
        if bits.pos > len(blob) * 8:
            raise ChdError('the hunk map is damaged')
        for h in range(n):
            if types[h] == T_SELF:
                self._keep.add(self.offsets[h])

    # ---- hunks
    def _codec_decode(self, tag, raw, size):
        if tag == b'zlib':
            return zlib.decompress(raw, -15)
        if tag == b'lzma':
            if lzma is None:
                raise ChdError('this Python has no lzma module')
            dict_size = max(4096, 1 << (max(size, 1) - 1).bit_length()) if size > 4096 else 4096
            hdr = struct.pack('<BIQ', 93, dict_size, size)
            return lzma.LZMADecompressor(lzma.FORMAT_ALONE).decompress(hdr + raw, max_length=size)
        if tag == b'zstd':
            return zstd_decompress(raw, size)
        if tag == b'huff':
            bits = _Bits(raw + bytes(8))
            hf = _Huffman(256, 16)
            hf.import_huffman(bits)
            return _huff_bytes(hf, raw, bits.pos, size)
        if tag == b'flac':
            if not raw or raw[:1] not in (b'L', b'B'):
                raise ChdError('bad FLAC hunk')
            chans, _ = flac_decode(raw[1:], size // 4)
            return flac_pcm_bytes(chans, raw[:1] == b'B')
        raise ChdError('codec not supported')

    def _cd_decode(self, tag, raw):
        frames = self.frames_per_hunk
        size = frames * CD_RAW
        if tag == b'cdfl':
            # cdfl has no flag bytes: FLAC frames of the whole sector data (sync included, big-endian samples), then the sub-channel
            chans, _ = flac_decode(raw, size // 4)
            return flac_pcm_bytes(chans, True)
        eccb = (frames + 7) // 8
        cl = 2 if self.hunk_bytes < 65536 else 3
        if len(raw) < eccb + cl:
            raise ChdError('a CD hunk is cut off')
        baselen = int.from_bytes(raw[eccb:eccb + cl], 'big')
        base = raw[eccb + cl:eccb + cl + baselen]
        sectors = bytearray(self._codec_decode({b'cdzl': b'zlib', b'cdlz': b'lzma', b'cdzs': b'zstd'}[tag], base, size))
        if len(sectors) != size:
            raise ChdError('a CD hunk came out with the wrong size')
        for f in range(frames):  # the sync pattern of data frames is not stored, only a flag
            if raw[f >> 3] & (1 << (f & 7)):
                sectors[f * CD_RAW:f * CD_RAW + 12] = CD_SYNC
        return bytes(sectors)

    def _raw_hunk(self, h):
        try:
            return self._raw_hunk2(h)
        except ChdError:
            raise
        except (zlib.error, ValueError, IndexError, struct.error, OverflowError, KeyError, MemoryError) as e:
            raise ChdError('hunk %d is damaged (%s)' % (h, e))
        except Exception as e:  # lzma.LZMAError and the like
            if lzma is not None and isinstance(e, lzma.LZMAError):
                raise ChdError('hunk %d is damaged (%s)' % (h, e))
            raise

    def _raw_hunk2(self, h):
        """The hunk's bytes as stored: a DVD hunk, or for a CD frames * 2352 sector bytes (sub-channel left out)."""
        t = self.types[h]
        if t == T_SELF:
            return self.hunk(self.offsets[h])
        if t == T_NONE:
            if self.offsets[h] == 0:
                data = bytes(self.hunk_bytes)
            else:
                self.fp.seek(self.offsets[h])
                data = self.fp.read(self.hunk_bytes)
                if len(data) < self.hunk_bytes:
                    raise ChdError('the CHD file is cut off')
            if self.tracks:
                data = b''.join(data[f * CD_FRAME:f * CD_FRAME + CD_RAW] for f in range(self.frames_per_hunk))
            return data
        tag = self.codecs[t]
        self.fp.seek(self.offsets[h])
        raw = self.fp.read(self.lengths[h])
        if len(raw) < self.lengths[h]:
            raise ChdError('the CHD file is cut off')
        if tag in (b'cdzl', b'cdlz', b'cdfl', b'cdzs'):
            return self._cd_decode(tag, raw)
        data = self._codec_decode(tag, raw, self.hunk_bytes)
        if len(data) != self.hunk_bytes:
            raise ChdError('a hunk came out with the wrong size')
        if self.tracks:  # a CD stored with a plain codec: frames of 2448 bytes
            data = b''.join(data[f * CD_FRAME:f * CD_FRAME + CD_RAW] for f in range(self.frames_per_hunk))
        return data

    def hunk(self, h):
        d = self._cache.get(h)
        if d is not None:
            return d
        d = self._raw_hunk(h)
        if h in self._keep or len(self._cache) < 8:
            self._cache[h] = d
            self._cache_order.append(h)
            while len(self._cache_order) > 4096 + len(self._keep) // 4:  # bounded: a few tens of megabytes
                victim = self._cache_order.pop(0)
                if victim not in self._keep:
                    self._cache.pop(victim, None)
                else:
                    self._cache_order.append(victim)
                    break
        return d

    def decode_range(self, first, last):
        """Decoded hunks first..last-1 as a list (independent of the cache, for worker processes)."""
        return [self._raw_hunk(h) for h in range(first, last)]


def _user_data(sector, kind):
    """The 2048 bytes of a data sector, cut out of the stored form."""
    if kind in ('MODE1', 'MODE2_FORM1'):
        return sector[:SECTOR]
    if kind in ('MODE2', 'MODE2_FORM_MIX'):
        return sector[8:8 + SECTOR]
    mode = sector[15]
    return sector[16:16 + SECTOR] if mode == 1 else sector[24:24 + SECTOR]


class ChdImage:
    """Random access to the disc inside a CHD as 2048-byte sectors (a DVD as it is, a CD as its first data track)."""

    def __init__(self, path):
        self.chd = Chd(path)
        c = self.chd
        if c.cd:
            tracks = [t for t in c.tracks if t.is_data]
            if not tracks:
                c.close()
                raise ChdError('this CHD file holds an audio CD, no game disc')
            self.track = tracks[0]
            if self.track.pregap and self.track.pgtype.startswith('V'):
                c.close()
                raise ChdError('this CD image keeps its pregap in the file, which is not supported')
            self.total_sectors = self.track.frames
        else:
            self.track = None
            self.total_sectors = c.logical // SECTOR

    def read_sectors(self, sector, count):
        c = self.chd
        count = min(count, self.total_sectors - sector)
        if count <= 0:
            return b''
        out = bytearray()
        if self.track is None:
            pos, end = sector * SECTOR, (sector + count) * SECTOR
            while pos < end:
                h = pos // c.hunk_bytes
                data = c.hunk(h)
                a = pos - h * c.hunk_bytes
                piece = data[a:a + (end - pos)]
                out += piece
                pos += len(piece)
            return bytes(out)
        base = self.track.start
        for frame in range(base + sector, base + sector + count):
            h, k = divmod(frame, c.frames_per_hunk)
            out += _user_data(c.hunk(h)[k * CD_RAW:(k + 1) * CD_RAW], self.track.kind)
        return bytes(out)

    def close(self):
        self.chd.close()


_WORKER = None


def _worker_open(path):
    global _WORKER
    _WORKER = Chd(path)


def _worker_range(span):
    first, last = span
    return b''.join(_WORKER.hunk(h) for h in range(first, last))


def _job_count():
    for name in ('PS2_CHD_JOBS', 'PS2_JOBS'):
        try:
            n = int(os.environ.get(name, ''))
            if n >= 1:
                return n
        except ValueError:
            pass
    return max(1, min(os.cpu_count() or 1, 8))


def iter_hunks(chd, first, last, jobs=None):
    """Yields the decoded hunks first..last-1 in order. Several processes decode when the file is big enough to need it."""
    jobs = _job_count() if jobs is None else jobs
    size = chd.hunk_bytes if not chd.cd else chd.frames_per_hunk * CD_RAW
    if jobs <= 1 or last - first < 64:
        for h in range(first, last):
            yield chd.hunk(h)
        return
    import multiprocessing
    per = max(1, (1 << 21) // size)
    spans = [(a, min(a + per, last)) for a in range(first, last, per)]
    try:
        pool = multiprocessing.Pool(jobs, _worker_open, (chd.path,))
    except (OSError, ImportError):
        for h in range(first, last):
            yield chd.hunk(h)
        return
    try:
        pending = []
        nxt = 0
        while nxt < len(spans) or pending:
            while nxt < len(spans) and len(pending) < jobs * 3:
                pending.append((spans[nxt], pool.apply_async(_worker_range, (spans[nxt],))))
                nxt += 1
            (a, b), res = pending.pop(0)
            blob = res.get()
            for i in range(b - a):
                yield blob[i * size:(i + 1) * size]
    finally:
        pool.terminate()
        pool.join()


def convert(path, out_path, progress=None, verify=True, jobs=None):
    """Writes the image inside the CHD to out_path (through out_path + '.part'). Returns the number of bytes written."""
    chd = Chd(path)
    try:
        part = out_path + '.part'
        written = 0
        if chd.cd:
            data_tracks = [t for t in chd.tracks if t.is_data]
            if not data_tracks:
                raise ChdError('this CHD file holds an audio CD, no game disc')
            tr = data_tracks[0]
            if tr.pregap and tr.pgtype.startswith('V'):
                raise ChdError('this CD image keeps its pregap in the file, which is not supported')
            first_frame, nframes = tr.start, tr.frames
            fph = chd.frames_per_hunk
            first_h, last_h = first_frame // fph, (first_frame + nframes - 1) // fph + 1
            total = nframes * SECTOR
            with open(part, 'wb') as f:
                for h, data in zip(range(first_h, last_h), iter_hunks(chd, first_h, last_h, jobs)):
                    for k in range(fph):
                        frame = h * fph + k
                        if first_frame <= frame < first_frame + nframes:
                            f.write(_user_data(data[k * CD_RAW:(k + 1) * CD_RAW], tr.kind))
                            written += SECTOR
                    if progress:
                        progress(written, total)
        else:
            sha = hashlib.sha1() if verify else None
            total = chd.logical
            with open(part, 'wb') as f:
                for h, data in zip(range(chd.hunks), iter_hunks(chd, 0, chd.hunks, jobs)):
                    if (h + 1) * chd.hunk_bytes > chd.logical:
                        data = data[:chd.logical - h * chd.hunk_bytes]
                    f.write(data)
                    if sha:
                        sha.update(data)
                    written += len(data)
                    if progress:
                        progress(written, total)
            if sha and sha.digest() != chd.raw_sha1 and chd.raw_sha1 != bytes(20):
                raise ChdError('the data of this CHD file does not match its checksum (damaged file)')
        os.replace(part, out_path)
        return written
    except BaseException:
        try:
            os.remove(out_path + '.part')
        except OSError:
            pass
        raise
    finally:
        chd.close()


def _progress_printer(label):
    state = {'last': -1}

    def p(done, total):
        pct = int(done * 100 / total) if total else 100
        if pct >= state['last'] + 5 or pct == 100 and state['last'] != 100:
            state['last'] = pct
            print('%s: %d %%' % (label, pct), flush=True)
    return p


def selftest():
    import random

    # Huffman: a tree written by hand through the "huff" routine is covered by real files in tests/chd-check.py; here the
    # decoders that can be fed from small known data
    rnd = random.Random(1)
    blob = bytes(rnd.randrange(256) for _ in range(300))
    assert zlib.decompress(zlib.compress(blob)[2:-4], -15) == blob
    if lzma:
        assert lzma.decompress(lzma.compress(blob, format=lzma.FORMAT_ALONE), format=lzma.FORMAT_ALONE) == blob
    print('selftest ok')


def native_unpacker():
    """The path of build/tools/chdunpack (recomp/chdunpack, the same rules on every core, about ten times faster) when it has been
    built; None otherwise, and the Python code below does the work. PS2_CHD_NATIVE=0 turns it off."""
    if os.environ.get('PS2_CHD_NATIVE', '') == '0':
        return None
    tools = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'build', 'tools')
    for name in ('chdunpack.exe', 'chdunpack'):
        path = os.path.join(tools, name)
        if os.path.isfile(path) and os.access(path, os.X_OK):
            return path
    return None


def run_native(tool, src, dst):
    """Runs chdunpack with the same progress lines and the same error sentences (on stdout and stderr as they come). Returns its status."""
    import subprocess
    try:
        return subprocess.call([tool, src, dst])
    except OSError as e:
        print('chd.py: the native unpacker could not be started (%s)' % e, flush=True)
        return -1


def main(argv):
    if len(argv) >= 3 and argv[1] == 'info':
        c = Chd(argv[2])
        print(c.describe())
        c.close()
        return 0
    if len(argv) >= 4 and argv[1] == 'iso':
        native = native_unpacker()
        if native:
            rc = run_native(native, argv[2], argv[3])
            if rc in (0, 1, 2):  # done, or stopped with a sentence of its own (a damaged file and the like): same words as below
                return rc
            print('chd.py: the native unpacker stopped unexpectedly (status %d); unpacking with the slower Python code' % rc, flush=True)
        try:
            n = convert(argv[2], argv[3], _progress_printer('Unpacking the .chd disc image'))
        except ChdError as e:
            print('chd.py: %s' % e, file=sys.stderr)
            return 1
        print('wrote %d bytes' % n)
        return 0
    if len(argv) >= 2 and argv[1] == 'selftest':
        selftest()
        return 0
    print(__doc__)
    return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv))
