"""
Generic PlayStation 2 Graphics Synthesizer (GS) Texture Decoder & PNG Encoder.

Provides hardware-accurate PS2 GS VRAM page/block/column addressing (PSMCT32, PSMCT24,
PSMCT16, PSMT8, PSMT4), CSM1 CLUT palette unswizzling, GIF IMAGE/PACKED transfer stream
execution, Sony SDK .TIM2 (.TM2) decoding, Snowblind Engine .tex/.TEX decoding, and raw
16-bit (.16) framebuffer decoding, exporting directly to 32-bit RGBA .PNG using only the
Python standard library.
"""

import struct
import zlib

# PS2 GS Pixel Storage Modes (PSM)
PSMCT32 = 0x00
PSMCT24 = 0x01
PSMCT16 = 0x02
PSMCT16S = 0x0A
PSMT8 = 0x13
PSMT4 = 0x14
PSMT8H = 0x1B
PSMT4HL = 0x24
PSMT4HH = 0x2C

_BLOCK32 = (
    (0, 1, 4, 5, 16, 17, 20, 21),
    (2, 3, 6, 7, 18, 19, 22, 23),
    (8, 9, 12, 13, 24, 25, 28, 29),
    (10, 11, 14, 15, 26, 27, 30, 31),
)
_COLUMN32 = (
    (0, 1, 4, 5, 8, 9, 12, 13),
    (2, 3, 6, 7, 10, 11, 14, 15),
)
_BLOCK16 = (
    (0, 2, 8, 10),
    (1, 3, 9, 11),
    (4, 6, 12, 14),
    (5, 7, 13, 15),
    (16, 18, 24, 26),
    (17, 19, 25, 27),
    (20, 22, 28, 30),
    (21, 23, 29, 31),
)
_COLUMN16 = (
    (0, 2, 8, 10, 16, 18, 24, 26, 1, 3, 9, 11, 17, 19, 25, 27),
    (4, 6, 12, 14, 20, 22, 28, 30, 5, 7, 13, 15, 21, 23, 29, 31),
)
_COLUMN_WORD = (
    (
        (0, 1, 4, 5, 8, 9, 12, 13),
        (2, 3, 6, 7, 10, 11, 14, 15),
    ),
    (
        (8, 9, 12, 13, 0, 1, 4, 5),
        (10, 11, 14, 15, 2, 3, 6, 7),
    ),
)

_OFF32 = [[0] * 64 for _ in range(32)]
_OFF16 = [[0] * 64 for _ in range(64)]
_OFF8 = [[0] * 128 for _ in range(64)]
_OFF4 = [[0] * 128 for _ in range(128)]


def _init_gs_tables():
    for y in range(32):
        for x in range(64):
            col = (y & 7) // 2
            cy = y & 1
            cx = x & 7
            _OFF32[y][x] = _BLOCK32[y // 8][x // 8] * 256 + col * 64 + _COLUMN32[cy][cx] * 4
    for y in range(64):
        for x in range(64):
            col = (y & 7) // 2
            cy = y & 1
            cx = x & 15
            _OFF16[y][x] = _BLOCK16[y // 8][x // 16] * 256 + col * 64 + _COLUMN16[cy][cx] * 2
    for y in range(64):
        for x in range(128):
            blk = _BLOCK32[(y // 16) & 3][(x // 16) & 7]
            wx = x % 16
            wy = y % 16
            col = wy // 4
            wy %= 4
            tbl = ((wy & 2) >> 1) ^ ((y // 4) & 1)
            byte = ((wx & 8) >> 2) + ((wy & 2) >> 1)
            _OFF8[y][x] = blk * 256 + col * 64 + _COLUMN_WORD[tbl][wy & 1][wx & 7] * 4 + byte
    for y in range(128):
        for x in range(128):
            blk = _BLOCK16[(y // 16) & 7][(x // 32) & 3]
            wx = x % 32
            wy = y % 16
            col = wy // 4
            wy %= 4
            shift = (wx & 0x18) + ((wy & 2) << 1)
            nibble = shift // 4
            tbl = ((wy & 2) >> 1) ^ (col & 1)
            _OFF4[y][x] = (col * 64 + blk * 256 + _COLUMN_WORD[tbl][wy & 1][wx & 7] * 4) * 2 + nibble


_init_gs_tables()


def _page_address(bp, bw, x, y, pw, ph):
    per_row = max(1, (bw * 64) // pw)
    return bp * 256 + ((x // pw) + (y // ph) * per_row) * 8192


def scale_ps2_alpha(a):
    """Convert PS2 GS alpha (0x00..0x80 = 0..100% opacity) to standard 8-bit alpha (0..255)."""
    if a >= 0x80:
        return 255
    return (a * 255 + 64) // 128


def bgr555_to_rgba(pixel16, force_opaque=False):
    """Convert a 16-bit PS2 pixel (ABGR1555) to (R, G, B, A)."""
    r = (pixel16 & 0x1F) << 3
    g = ((pixel16 >> 5) & 0x1F) << 3
    b = ((pixel16 >> 10) & 0x1F) << 3
    a = 255 if (force_opaque or (pixel16 & 0x8000)) else 255
    return r | (r >> 5), g | (g >> 5), b | (b >> 5), a


def unswizzle_clut_csm1(palette_bytes, bpp=32):
    """
    Unswizzle a 256-entry PS2 GS CSM1 palette into linear RGBA32 (256 * 4 = 1024 bytes).
    In CSM1 mode, every 32 entries swap indices [8..15] with [16..23].
    """
    entry_size = 4 if bpp == 32 else 2
    num_entries = len(palette_bytes) // entry_size

    # Detect TCC=0 / opaque diffuse CLUTs where most non-black colors have alpha == 0
    force_opaque_nonblack = False
    if bpp == 32 and num_entries > 0:
        nonblack = 0
        zero_alpha_nonblack = 0
        for i in range(num_entries):
            r, g, b, a = palette_bytes[i * 4:i * 4 + 4]
            if (r | g | b) > 0:
                nonblack += 1
                if a == 0:
                    zero_alpha_nonblack += 1
        if nonblack > 0 and zero_alpha_nonblack * 6 >= nonblack:
            force_opaque_nonblack = True

    rgba_entries = []
    for i in range(num_entries):
        if bpp == 32:
            r, g, b, a = palette_bytes[i * 4:i * 4 + 4]
            if force_opaque_nonblack:
                alpha = 0 if (i == 0 and (r | g | b) == 0) else 255
            else:
                alpha = scale_ps2_alpha(a)
            rgba_entries.append(bytes((r, g, b, alpha)))
        else:
            val16 = struct.unpack_from('<H', palette_bytes, i * 2)[0]
            r, g, b, a = bgr555_to_rgba(val16, force_opaque=False)
            rgba_entries.append(bytes((r, g, b, a)))

    if num_entries < 256:
        return b''.join(rgba_entries).ljust(num_entries * 4, b'\x00')

    unswizzled = [b'\x00\x00\x00\x00'] * 256
    for blk in range(0, 256, 32):
        for j in range(8):
            unswizzled[blk + j] = rgba_entries[blk + j]
            unswizzled[blk + 8 + j] = rgba_entries[blk + 16 + j]
            unswizzled[blk + 16 + j] = rgba_entries[blk + 8 + j]
            unswizzled[blk + 24 + j] = rgba_entries[blk + 24 + j]
    return b''.join(unswizzled)


def encode_rgba_png(width, height, rgba_bytes):
    """Encode raw RGBA32 bytes (width * height * 4) into a standard PNG image."""
    if len(rgba_bytes) < width * height * 4:
        rgba_bytes = rgba_bytes.ljust(width * height * 4, b'\x00')

    def make_chunk(chunk_type, payload):
        crc = zlib.crc32(chunk_type)
        crc = zlib.crc32(payload, crc) & 0xFFFFFFFF
        return struct.pack('>I', len(payload)) + chunk_type + payload + struct.pack('>I', crc)

    ihdr = struct.pack('>IIBBBBB', width, height, 8, 6, 0, 0, 0)  # 8-bit RGBA (color_type=6)
    stride = width * 4
    raw_rows = bytearray((stride + 1) * height)
    for y in range(height):
        row_start = y * (stride + 1)
        raw_rows[row_start] = 0  # Filter type 0 (None)
        src_off = y * stride
        raw_rows[row_start + 1:row_start + 1 + stride] = rgba_bytes[src_off:src_off + stride]

    idat = zlib.compress(bytes(raw_rows), level=6)
    return (
        b'\x89PNG\r\n\x1a\n'
        + make_chunk(b'IHDR', ihdr)
        + make_chunk(b'IDAT', idat)
        + make_chunk(b'IEND', b'')
    )


class GsVramCanvas:
    """
    Hardware-accurate 4 MB PlayStation 2 GS VRAM simulator.
    Executes GIF BITBLTBUF / TRXPOS / TRXREG / TRXDIR / IMAGE transfers and reads back
    swizzled PSMT8 / PSMT4 / PSMCT32 / PSMCT16 textures as linear RGBA32 pixels.
    """

    VRAM_SIZE = 4 * 1024 * 1024

    def __init__(self):
        self.vram = bytearray(self.VRAM_SIZE)
        self.dbp = 0
        self.dbw = 1
        self.dpsm = PSMCT32
        self.dsax = 0
        self.dsay = 0
        self.rrw = 0
        self.rrh = 0
        self.transfers = []

    def write_rect(self, dbp, dbw, dpsm, dsax, dsay, rrw, rrh, data):
        """Write a host-to-local rectangular image transfer into GS VRAM."""
        bw = max(1, dbw)
        if dpsm == PSMCT32:
            pos = 0
            for ry in range(rrh):
                y = dsay + ry
                for rx in range(rrw):
                    if pos + 4 > len(data):
                        return
                    x = dsax + rx
                    addr = (_page_address(dbp, bw, x, y, 64, 32) + _OFF32[y & 31][x & 63]) & (self.VRAM_SIZE - 1)
                    self.vram[addr:addr + 4] = data[pos:pos + 4]
                    pos += 4
        elif dpsm in (PSMCT16, PSMCT16S):
            pos = 0
            for ry in range(rrh):
                y = dsay + ry
                for rx in range(rrw):
                    if pos + 2 > len(data):
                        return
                    x = dsax + rx
                    addr = (_page_address(dbp, bw, x, y, 64, 64) + _OFF16[y & 63][x & 63]) & (self.VRAM_SIZE - 1)
                    self.vram[addr:addr + 2] = data[pos:pos + 2]
                    pos += 2
        elif dpsm == PSMT8:
            pos = 0
            for ry in range(rrh):
                y = dsay + ry
                for rx in range(rrw):
                    if pos >= len(data):
                        return
                    x = dsax + rx
                    addr = (_page_address(dbp, bw, x, y, 128, 64) + _OFF8[y & 63][x & 127]) & (self.VRAM_SIZE - 1)
                    self.vram[addr] = data[pos]
                    pos += 1
        elif dpsm == PSMT4:
            pos = 0
            for ry in range(rrh):
                y = dsay + ry
                for rx in range(0, rrw, 2):
                    if pos >= len(data):
                        return
                    bval = data[pos]
                    pos += 1
                    for sub in range(2):
                        x = dsax + rx + sub
                        if rx + sub >= rrw:
                            break
                        nibble = (bval >> 4) if sub else (bval & 0xF)
                        nibble_addr = _page_address(dbp, bw, x, y, 128, 128) * 2 + _OFF4[y & 127][x & 127]
                        byte_idx = (nibble_addr >> 1) & (self.VRAM_SIZE - 1)
                        if nibble_addr & 1:
                            self.vram[byte_idx] = (self.vram[byte_idx] & 0x0F) | (nibble << 4)
                        else:
                            self.vram[byte_idx] = (self.vram[byte_idx] & 0xF0) | nibble

    def read_clut_psmct32(self, cbp, count=256):
        """Read a CSM1 CLUT stored in PSMCT32 (16x16 for 256 colors, 8x2 for 16 colors) and scale PS2 alpha."""
        out = bytearray(count * 4)
        idx = 0
        # In CSM1, a 256-color CLUT occupies 16x16 pixels in PSMCT32 in two 8x16 halves (blocks 0 and 2 or linear 16x16)
        for y in range(16 if count == 256 else 2):
            for x in range(16 if count == 256 else 8):
                if idx >= count:
                    break
                addr = (_page_address(cbp, 1, x, y, 64, 32) + _OFF32[y & 31][x & 63]) & (self.VRAM_SIZE - 1)
                r, g, b, a = self.vram[addr:addr + 4]
                out[idx * 4:idx * 4 + 4] = bytes((r, g, b, scale_ps2_alpha(a)))
                idx += 1
        if count == 256:
            # Note: reading via _OFF32[y][x] for x=0..15, y=0..15 reads block 0 (x=0..7, y=0..7), block 1 (x=8..15, y=0..7),
            # block 2 (x=0..7, y=8..15), block 3 (x=8..15, y=8..15) — which already matches CSM1's 16x16 2D coordinate grid!
            pass
        return bytes(out)

    def read_texture_rgba(self, tbp0, tbw, psm, width, height, clut_rgba=None):
        """Read a texture of (width, height) in format psm from GS VRAM at (tbp0, tbw) into linear RGBA32."""
        bw = max(1, tbw)
        out = bytearray(width * height * 4)
        if psm == PSMT8:
            if not clut_rgba or len(clut_rgba) < 1024:
                clut_rgba = bytes(i for i in range(256) for _ in range(4))
            for y in range(height):
                row_off = y * width * 4
                for x in range(width):
                    addr = (_page_address(tbp0, bw, x, y, 128, 64) + _OFF8[y & 63][x & 127]) & (self.VRAM_SIZE - 1)
                    cidx = self.vram[addr] * 4
                    out[row_off + x * 4:row_off + (x + 1) * 4] = clut_rgba[cidx:cidx + 4]
        elif psm == PSMT4:
            if not clut_rgba or len(clut_rgba) < 64:
                clut_rgba = bytes((i * 17) for i in range(16) for _ in range(4))
            for y in range(height):
                row_off = y * width * 4
                for x in range(width):
                    nibble_addr = _page_address(tbp0, bw, x, y, 128, 128) * 2 + _OFF4[y & 127][x & 127]
                    byte_val = self.vram[(nibble_addr >> 1) & (self.VRAM_SIZE - 1)]
                    cidx = ((byte_val >> 4) if (nibble_addr & 1) else (byte_val & 0xF)) * 4
                    out[row_off + x * 4:row_off + (x + 1) * 4] = clut_rgba[cidx:cidx + 4]
        elif psm in (PSMCT16, PSMCT16S):
            for y in range(height):
                row_off = y * width * 4
                for x in range(width):
                    addr = (_page_address(tbp0, bw, x, y, 64, 64) + _OFF16[y & 63][x & 63]) & (self.VRAM_SIZE - 1)
                    val16 = struct.unpack_from('<H', self.vram, addr)[0]
                    out[row_off + x * 4:row_off + (x + 1) * 4] = bytes(bgr555_to_rgba(val16))
        else:  # PSMCT32 / PSMCT24
            for y in range(height):
                row_off = y * width * 4
                for x in range(width):
                    addr = (_page_address(tbp0, bw, x, y, 64, 32) + _OFF32[y & 31][x & 63]) & (self.VRAM_SIZE - 1)
                    r, g, b, a = self.vram[addr:addr + 4]
                    out[row_off + x * 4:row_off + (x + 1) * 4] = bytes((r, g, b, scale_ps2_alpha(a)))
        return bytes(out)

    def parse_gif_transfers(self, gif_bytes):
        """Parse a raw GIF packet stream and return a list of rectangular IMAGE transfers."""
        pos = 0
        transfers = []
        while pos + 16 <= len(gif_bytes):
            lo, hi = struct.unpack_from('<QQ', gif_bytes, pos)
            nloop = lo & 0x7FFF
            flg = (lo >> 58) & 3
            nreg = (lo >> 60) & 0xF
            if nreg == 0:
                nreg = 16
            pos += 16
            if nloop == 0:
                break
            if flg == 0:  # PACKED
                for _ in range(nloop):
                    for r in range(nreg):
                        if pos + 16 > len(gif_bytes):
                            break
                        reg_id = (hi >> (4 * r)) & 0xF
                        val_lo, val_hi = struct.unpack_from('<QQ', gif_bytes, pos)
                        if reg_id == 0xE:  # A+D
                            addr = val_hi & 0xFF
                            if addr == 0x50:  # BITBLTBUF
                                self.dbp = (val_lo >> 32) & 0x3FFF
                                self.dbw = (val_lo >> 48) & 0x3F
                                self.dpsm = (val_lo >> 56) & 0x3F
                            elif addr == 0x51:  # TRXPOS
                                self.dsax = (val_lo >> 32) & 0x7FF
                                self.dsay = (val_lo >> 48) & 0x7FF
                            elif addr == 0x52:  # TRXREG
                                self.rrw = val_lo & 0xFFF
                                self.rrh = (val_lo >> 32) & 0xFFF
                        pos += 16
            elif flg in (2, 3):  # IMAGE
                byte_len = nloop * 16
                img_slice = gif_bytes[pos:pos + byte_len]
                transfers.append({
                    'dbp': self.dbp,
                    'dbw': self.dbw,
                    'dpsm': self.dpsm,
                    'dsax': self.dsax,
                    'dsay': self.dsay,
                    'rrw': self.rrw,
                    'rrh': self.rrh,
                    'data': img_slice,
                })
                pos += byte_len
            else:  # REGLIST
                byte_len = ((nloop * nreg + 1) // 2) * 16
                pos += byte_len
        self.transfers = transfers
        return transfers


def decode_snowblind_tex_to_rgba(data):
    """
    Decode a Snowblind Engine .tex texture (used in Champions of Norrath, Return to Arms,
    Baldur's Gate: Dark Alliance) into (width, height, rgba_bytes).
    """
    if len(data) < 0x80:
        raise ValueError('Snowblind .tex buffer too small (< 128 bytes)')
    width, height, _unk4, _qwc = struct.unpack_from('<HHHH', data, 0)
    _unk8, _tex0_lo, gif_off = struct.unpack_from('<III', data, 8)
    if width == 0 or height == 0 or width > 2048 or height > 2048:
        raise ValueError(f'Invalid Snowblind .tex dimensions: {width}x{height}')
    if gif_off == 0 or gif_off >= len(data):
        gif_off = 0x80

    canvas = GsVramCanvas()
    transfers = canvas.parse_gif_transfers(data[gif_off:])
    if not transfers:
        raise ValueError('No GIF IMAGE transfers found in Snowblind .tex')

    # Check if the first transfer is a 16x16 (256-color) or 8x2 (16-color) CLUT upload
    clut_rgba = None
    pixel_transfers = transfers
    first = transfers[0]
    if len(transfers) >= 2 and first['rrw'] == 16 and first['rrh'] == 16 and len(first['data']) == 1024:
        clut_rgba = unswizzle_clut_csm1(first['data'], bpp=32)
        pixel_transfers = transfers[1:]
        psm = PSMT8
    elif len(transfers) >= 2 and first['rrw'] == 8 and first['rrh'] == 2 and len(first['data']) == 64:
        clut_rgba = unswizzle_clut_csm1(first['data'], bpp=32)
        pixel_transfers = transfers[1:]
        psm = PSMT4
    else:
        psm = PSMCT32

    pixel_dbw = max(1, pixel_transfers[0]['dbw'])
    pixel_dbp = pixel_transfers[0]['dbp']
    for tr in pixel_transfers:
        canvas.write_rect(tr['dbp'], tr['dbw'], tr['dpsm'], tr['dsax'], tr['dsay'], tr['rrw'], tr['rrh'], tr['data'])

    if psm in (PSMT8, PSMT4):
        # When 8-bit/4-bit textures are uploaded via PSMCT32, TBW in 64-texel units is 2x the PSMCT32 DBW
        tbw = max((width + 63) // 64, pixel_dbw * 2)
    else:
        tbw = max((width + 63) // 64, pixel_dbw)

    rgba = canvas.read_texture_rgba(pixel_dbp, tbw, psm, width, height, clut_rgba=clut_rgba)
    if (_unk8 & 0x40) and height >= 4:
        # 2D UI field-split texture: top half holds even scanlines, bottom half holds odd scanlines
        half = height // 2
        stride = width * 4
        deinterleaved = bytearray(len(rgba))
        for y in range(half):
            deinterleaved[(2 * y) * stride:(2 * y + 1) * stride] = rgba[y * stride:(y + 1) * stride]
            deinterleaved[(2 * y + 1) * stride:(2 * y + 2) * stride] = rgba[(y + half) * stride:(y + half + 1) * stride]
        if height & 1:
            deinterleaved[(height - 1) * stride:height * stride] = rgba[(height - 1) * stride:height * stride]
        rgba = bytes(deinterleaved)
    return width, height, rgba


def decode_tim2_to_rgba(data):
    """
    Decode a standard Sony PS2 SDK .TIM2 (.TM2) texture file into (width, height, rgba_bytes).
    Supports 32-bit RGBA (type 3), 24-bit RGB (type 2), 16-bit ABGR1555 (type 1),
    8-bit indexed PSMT8 (type 5, CSM1/CSM2), and 4-bit indexed PSMT4 (type 4).
    """
    if len(data) < 64 or data[:4] != b'TIM2':
        raise ValueError('Not a valid Sony TIM2 texture (missing TIM2 magic)')

    _version, alignment, _pic_count = struct.unpack_from('<BBH', data, 4)
    pic_offset = 128 if alignment == 1 else 16
    if len(data) < pic_offset + 48:
        raise ValueError('Truncated TIM2 picture header')

    (
        _total_size,
        clut_size,
        image_size,
        header_size,
        clut_colors,
        _pict_format,
        mipmap_textures,
        clut_type,
        image_type,
        width,
        height,
    ) = struct.unpack_from('<IIIHHBBBBHH', data, pic_offset)

    img_start = pic_offset + header_size
    clut_start = img_start + image_size
    img_bytes = data[img_start:img_start + image_size]
    clut_bytes = data[clut_start:clut_start + clut_size]

    # Decode CLUT if indexed (image_type 4 = 4-bit, 5 = 8-bit)
    clut_rgba = b''
    if image_type in (4, 5) and clut_size > 0:
        csm2 = (clut_type & 0x80) != 0
        clut_low = clut_type & 0x3F
        bpp = 16 if clut_low == 1 else (24 if clut_low == 2 else 32)
        if image_type == 5 and not csm2 and clut_colors >= 256:
            clut_rgba = unswizzle_clut_csm1(clut_bytes, bpp=bpp)
        else:
            entries = []
            step = bpp // 8
            for i in range(min(clut_colors, len(clut_bytes) // step)):
                if bpp == 32:
                    r, g, b, a = clut_bytes[i * 4:i * 4 + 4]
                    entries.append(bytes((r, g, b, scale_ps2_alpha(a))))
                elif bpp == 24:
                    r, g, b = clut_bytes[i * 3:i * 3 + 3]
                    entries.append(bytes((r, g, b, 255)))
                else:
                    v16 = struct.unpack_from('<H', clut_bytes, i * 2)[0]
                    entries.append(bytes(bgr555_to_rgba(v16)))
            clut_rgba = b''.join(entries)

    out = bytearray(width * height * 4)
    if image_type == 3:  # 32-bit RGBA
        for i in range(min(width * height, len(img_bytes) // 4)):
            r, g, b, a = img_bytes[i * 4:i * 4 + 4]
            out[i * 4:i * 4 + 4] = bytes((r, g, b, scale_ps2_alpha(a)))
    elif image_type == 2:  # 24-bit RGB
        for i in range(min(width * height, len(img_bytes) // 3)):
            r, g, b = img_bytes[i * 3:i * 3 + 3]
            out[i * 4:i * 4 + 4] = bytes((r, g, b, 255))
    elif image_type == 1:  # 16-bit ABGR1555
        for i in range(min(width * height, len(img_bytes) // 2)):
            v16 = struct.unpack_from('<H', img_bytes, i * 2)[0]
            out[i * 4:i * 4 + 4] = bytes(bgr555_to_rgba(v16))
    elif image_type == 5:  # 8-bit indexed
        for i in range(min(width * height, len(img_bytes))):
            cidx = img_bytes[i] * 4
            if cidx + 4 <= len(clut_rgba):
                out[i * 4:i * 4 + 4] = clut_rgba[cidx:cidx + 4]
    elif image_type == 4:  # 4-bit indexed (low nibble first)
        npix = width * height
        for i in range(npix):
            bval = img_bytes[i >> 1] if (i >> 1) < len(img_bytes) else 0
            idx = (bval >> 4) if (i & 1) else (bval & 0xF)
            cidx = idx * 4
            if cidx + 4 <= len(clut_rgba):
                out[i * 4:i * 4 + 4] = clut_rgba[cidx:cidx + 4]
    else:
        raise ValueError(f'Unsupported TIM2 image_type={image_type}')

    return width, height, bytes(out)


def decode_raw16_to_rgba(data, width=640, height=448):
    """Decode a raw 16-bit PS2 framebuffer image (such as Snowblind .16 loading screens) to RGBA32."""
    expected = width * height * 2
    if len(data) < expected:
        raise ValueError(f'Raw 16-bit buffer too small ({len(data)} < {expected})')
    out = bytearray(width * height * 4)
    for i in range(width * height):
        v16 = struct.unpack_from('<H', data, i * 2)[0]
        out[i * 4:i * 4 + 4] = bytes(bgr555_to_rgba(v16, force_opaque=True))
    return width, height, bytes(out)


def gpu_content_key(data: bytes) -> int:
    """
    Compute the exact 64-bit hash used by `content_key(const uint8_t *p, size_t n)`
    in `runtime/src/gs/gpu/gpu.cpp` during GS VRAM texture uploads.
    """
    n = len(data)
    mask64 = 0xFFFFFFFFFFFFFFFF
    c1 = 0x9E3779B97F4A7C15
    c2 = 0xBF58476D1CE4E5B9
    h = (n * c1) & mask64

    def mix(v: int) -> None:
        nonlocal h
        term = (v + c1 + ((h << 6) & mask64) + (h >> 2)) & mask64
        h = ((h ^ term) * c2) & mask64

    step = max(8, (n // 64) & ~7)
    i = 0
    while i + 8 <= n:
        v = struct.unpack_from('<Q', data, i)[0]
        mix(v)
        i += step

    tail_len = min(n, 8)
    tail_bytes = data[n - tail_len : n].ljust(8, b'\x00') if tail_len > 0 else b'\x00' * 8
    v = struct.unpack_from('<Q', tail_bytes, 0)[0]
    mix(v)
    return h


def compute_tim2_gpu_key(data: bytes) -> int:
    """
    Extract the raw pixel payload (`img_bytes`) of a Sony .TIM2 file and compute its
    64-bit `gpu_content_key` so the C++ GPU runtime can match live .PNG overrides.
    """
    if len(data) < 64 or data[:4] != b'TIM2':
        return gpu_content_key(data)
    _version, alignment, _pic_count = struct.unpack_from('<BBH', data, 4)
    pic_offset = 128 if alignment == 1 else 16
    if len(data) < pic_offset + 48:
        return gpu_content_key(data)
    _total_size, _clut_size, image_size, header_size = struct.unpack_from('<IIIH', data, pic_offset)
    img_start = pic_offset + header_size
    img_bytes = data[img_start : img_start + image_size]
    return gpu_content_key(img_bytes)


def decode_png_to_rgba_bytes(png_bytes: bytes) -> tuple[int, int, bytes]:
    """
    Decode a standard 8-bit PNG image (RGBA color_type=6, RGB color_type=2,
    Paletted color_type=3, or Grayscale color_type=0/4) into (width, height, rgba_bytes)
    using only the Python standard library.
    """
    if len(png_bytes) < 33 or png_bytes[:8] != b'\x89PNG\r\n\x1a\n':
        raise ValueError('Not a valid PNG file')

    pos = 8
    width = 0
    height = 0
    bit_depth = 8
    color_type = 6
    palette: bytes = b''
    trns: bytes = b''
    idat_chunks: list[bytes] = []

    while pos + 8 <= len(png_bytes):
        length = struct.unpack_from('>I', png_bytes, pos)[0]
        ctype = png_bytes[pos + 4 : pos + 8]
        chunk_data = png_bytes[pos + 8 : pos + 8 + length]
        pos += 12 + length
        if ctype == b'IHDR':
            width, height, bit_depth, color_type, _comp, _filt, _interlace = struct.unpack_from(
                '>IIBBBBB', chunk_data, 0
            )
        elif ctype == b'PLTE':
            palette = chunk_data
        elif ctype == b'tRNS':
            trns = chunk_data
        elif ctype == b'IDAT':
            idat_chunks.append(chunk_data)
        elif ctype == b'IEND':
            break

    if width <= 0 or height <= 0 or bit_depth != 8:
        raise ValueError(f'Unsupported PNG format: {width}x{height} bit_depth={bit_depth} color_type={color_type}')

    bpp_map = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}
    if color_type not in bpp_map:
        raise ValueError(f'Unsupported PNG color_type={color_type}')

    bpp = bpp_map[color_type]
    stride = width * bpp
    raw = zlib.decompress(b''.join(idat_chunks))

    recon = bytearray(height * stride)
    in_pos = 0
    for y in range(height):
        if in_pos >= len(raw):
            break
        ftype = raw[in_pos]
        in_pos += 1
        row_out = y * stride
        prev_out = (y - 1) * stride
        for x in range(stride):
            val = raw[in_pos + x] if (in_pos + x) < len(raw) else 0
            a = recon[row_out + x - bpp] if x >= bpp else 0
            b = recon[prev_out + x] if y > 0 else 0
            c = recon[prev_out + x - bpp] if (y > 0 and x >= bpp) else 0
            if ftype == 1:  # Sub
                val = (val + a) & 0xFF
            elif ftype == 2:  # Up
                val = (val + b) & 0xFF
            elif ftype == 3:  # Average
                val = (val + ((a + b) >> 1)) & 0xFF
            elif ftype == 4:  # Paeth
                p = a + b - c
                pa = abs(p - a)
                pb = abs(p - b)
                pc = abs(p - c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                val = (val + pr) & 0xFF
            recon[row_out + x] = val
        in_pos += stride

    if color_type == 6:
        return width, height, bytes(recon)

    out = bytearray(width * height * 4)
    npix = width * height
    if color_type == 2:  # RGB
        for i in range(npix):
            out[i * 4 : i * 4 + 3] = recon[i * 3 : i * 3 + 3]
            out[i * 4 + 3] = 255
    elif color_type == 3:  # Paletted
        for i in range(npix):
            idx = recon[i]
            r = palette[idx * 3] if idx * 3 + 2 < len(palette) else 0
            g = palette[idx * 3 + 1] if idx * 3 + 2 < len(palette) else 0
            b = palette[idx * 3 + 2] if idx * 3 + 2 < len(palette) else 0
            a = trns[idx] if idx < len(trns) else 255
            out[i * 4 : i * 4 + 4] = bytes((r, g, b, a))
    elif color_type == 4:  # Grayscale + Alpha
        for i in range(npix):
            v = recon[i * 2]
            a = recon[i * 2 + 1]
            out[i * 4 : i * 4 + 4] = bytes((v, v, v, a))
    elif color_type == 0:  # Grayscale
        for i in range(npix):
            v = recon[i]
            out[i * 4 : i * 4 + 4] = bytes((v, v, v, 255))

    return width, height, bytes(out)


def _unscale_ps2_alpha(a8: int) -> int:
    """Convert standard 8-bit alpha (0..255) back to PS2 GS alpha (0x00..0x80)."""
    if a8 >= 254:
        return 0x80
    return (a8 * 128 + 127) // 255


def _rgba_to_bgr555(r: int, g: int, b: int, a: int) -> int:
    return ((1 if a >= 128 else 0) << 15) | ((b >> 3) << 10) | ((g >> 3) << 5) | (r >> 3)


def _swizzle_clut_csm1(linear_clut_bytes: bytes, bpp: int = 32) -> bytes:
    """Swizzle a 256-entry linear palette into PS2 GS CSM1 order (swap [8..15] with [16..23] per 32 entries)."""
    step = 4 if bpp == 32 else 2
    if len(linear_clut_bytes) < 256 * step:
        return linear_clut_bytes
    out = bytearray(len(linear_clut_bytes))
    for blk in range(0, 256, 32):
        for j in range(8):
            i0 = (blk + j) * step
            i1 = (blk + 8 + j) * step
            i2 = (blk + 16 + j) * step
            i3 = (blk + 24 + j) * step
            out[i0 : i0 + step] = linear_clut_bytes[i0 : i0 + step]
            out[i1 : i1 + step] = linear_clut_bytes[i2 : i2 + step]
            out[i2 : i2 + step] = linear_clut_bytes[i1 : i1 + step]
            out[i3 : i3 + step] = linear_clut_bytes[i3 : i3 + step]
    return bytes(out)


def encode_png_to_tim2_bytes(png_bytes: bytes, reference_tim2: bytes) -> bytes:
    """
    Encode a modified 1x .PNG image back into its original Sony .TIM2 container (`reference_tim2`),
    preserving the exact TIM2 header, GS TEX0/TEX1 registers, pixel format (PSMT4, PSMT8,
    PSMCT16, PSMCT24, PSMCT32), and CSM1 palette swizzling.
    If the PNG is a higher-resolution HD texture (width != orig_width or height != orig_height),
    returns `reference_tim2` unchanged so `DATA.BIN` layout remains safe while the C++ GPU
    runtime loads the HD .PNG live!
    """
    if len(reference_tim2) < 64 or reference_tim2[:4] != b'TIM2':
        return reference_tim2

    png_w, png_h, rgba = decode_png_to_rgba_bytes(png_bytes)

    _version, alignment, _pic_count = struct.unpack_from('<BBH', reference_tim2, 4)
    pic_offset = 128 if alignment == 1 else 16
    if len(reference_tim2) < pic_offset + 48:
        return reference_tim2

    (
        _total_size,
        clut_size,
        image_size,
        header_size,
        clut_colors,
        _pict_format,
        _mipmap_textures,
        clut_type,
        image_type,
        orig_w,
        orig_h,
    ) = struct.unpack_from('<IIIHHBBBBHH', reference_tim2, pic_offset)

    # HD textures (>1x) are handled live by the C++ WebGPU runtime; keep 1x TIM2 in disc container
    if png_w != orig_w or png_h != orig_h:
        return reference_tim2

    img_start = pic_offset + header_size
    clut_start = img_start + image_size
    npix = orig_w * orig_h
    new_img = bytearray(image_size)
    new_clut = bytearray(reference_tim2[clut_start : clut_start + clut_size])

    if image_type == 3:  # 32-bit RGBA
        for i in range(min(npix, image_size // 4)):
            r, g, b, a = rgba[i * 4 : i * 4 + 4]
            new_img[i * 4 : i * 4 + 4] = bytes((r, g, b, _unscale_ps2_alpha(a)))
    elif image_type == 2:  # 24-bit RGB
        for i in range(min(npix, image_size // 3)):
            new_img[i * 3 : i * 3 + 3] = rgba[i * 4 : i * 4 + 3]
    elif image_type == 1:  # 16-bit ABGR1555
        for i in range(min(npix, image_size // 2)):
            r, g, b, a = rgba[i * 4 : i * 4 + 4]
            struct.pack_into('<H', new_img, i * 2, _rgba_to_bgr555(r, g, b, a))
    elif image_type in (4, 5) and clut_size > 0:
        max_colors = 16 if image_type == 4 else 256
        csm2 = (clut_type & 0x80) != 0
        clut_low = clut_type & 0x3F
        bpp = 16 if clut_low == 1 else (24 if clut_low == 2 else 32)

        # Build an adaptive palette from the modified PNG (up to `max_colors` unique colors,
        # quantizing if the PNG uses more than `max_colors` colors)
        unique_colors: dict[tuple[int, int, int, int], int] = {}
        pixel_indices = [0] * npix
        for i in range(npix):
            c = (rgba[i * 4], rgba[i * 4 + 1], rgba[i * 4 + 2], rgba[i * 4 + 3])
            idx = unique_colors.get(c)
            if idx is None and len(unique_colors) < max_colors:
                idx = len(unique_colors)
                unique_colors[c] = idx
            if idx is not None:
                pixel_indices[i] = idx
            else:
                # Nearest color in palette
                best_idx = 0
                best_d = 1 << 30
                cr, cg, cb, ca = c
                for (pr, pg, pb, pa), pidx in unique_colors.items():
                    d = (cr - pr) ** 2 + (cg - pg) ** 2 + (cb - pb) ** 2 + (ca - pa) ** 2
                    if d < best_d:
                        best_d = d
                        best_idx = pidx
                pixel_indices[i] = best_idx

        palette_list = [(0, 0, 0, 0)] * max_colors
        for col, idx in unique_colors.items():
            palette_list[idx] = col

        step = bpp // 8
        linear_clut = bytearray(max_colors * step)
        for i, (r, g, b, a) in enumerate(palette_list):
            if bpp == 32:
                linear_clut[i * 4 : i * 4 + 4] = bytes((r, g, b, _unscale_ps2_alpha(a)))
            elif bpp == 24:
                linear_clut[i * 3 : i * 3 + 3] = bytes((r, g, b))
            else:
                struct.pack_into('<H', linear_clut, i * 2, _rgba_to_bgr555(r, g, b, a))

        if image_type == 5 and not csm2 and clut_colors >= 256:
            swizzled = _swizzle_clut_csm1(bytes(linear_clut), bpp=bpp)
            new_clut[: min(len(new_clut), len(swizzled))] = swizzled[: len(new_clut)]
        else:
            new_clut[: min(len(new_clut), len(linear_clut))] = linear_clut[: len(new_clut)]

        if image_type == 5:
            for i in range(min(npix, len(new_img))):
                new_img[i] = pixel_indices[i]
        else:
            for i in range(0, npix, 2):
                lo = pixel_indices[i] & 0x0F
                hi = (pixel_indices[i + 1] & 0x0F) if (i + 1 < npix) else 0
                if (i >> 1) < len(new_img):
                    new_img[i >> 1] = lo | (hi << 4)
    else:
        return reference_tim2

    out = bytearray(reference_tim2)
    out[img_start : img_start + image_size] = new_img
    out[clut_start : clut_start + clut_size] = new_clut
    return bytes(out)

