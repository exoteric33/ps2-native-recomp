#!/usr/bin/env python3
"""
Wrapper CLI around the independent PS2 Disc Extraction Toolkit (tools/ps2_disc_toolkit).
Supports inspect, extract, verify, and package across ISO-2048 (DVD-5/DVD-9),
BIN/CUE-2352 (CD-ROM Mode 1 / Mode 2 XA), CSO and CHD (v5) compressed images.

CHD files are read by scripts/chd.py (every codec chdman writes: zlib, lzma, huff, flac, zstd and the CD ones), which replaces the
toolkit's own CHD reader here. `chd-iso <in.chd> <out.iso>` writes the plain image (what port.sh uses for a .chd file).
"""

import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
sys.path.insert(0, os.path.join(ROOT, 'scripts'))

import chd as chd_reader  # noqa: E402
from ps2_disc_toolkit import containers  # noqa: E402
from ps2_disc_toolkit.cli import main, extract_disc, verify_disc, inspect_disc  # noqa: E402,F401


class ChdDevice(containers.SectorDevice):
    """The toolkit's view of a CHD: 2048-byte sectors, read through scripts/chd.py."""

    def __init__(self, path):
        try:
            self._image = chd_reader.ChdImage(path)
        except chd_reader.ChdError as e:
            raise ValueError('%s: %s' % (path, e))
        codecs = '+'.join(c.decode('ascii') for c in self._image.chd.codecs if c != bytes(4)) or 'RAW'
        self.format_name = 'CHD-v5(%s)' % codecs
        super().__init__(path, self._image.total_sectors)

    def read_sectors(self, sector, count):
        try:
            data = self._image.read_sectors(sector, count) if sector >= 0 else b''
        except chd_reader.ChdError as e:
            raise ValueError('%s: %s' % (self.path, e))
        want = max(0, min(count, self.total_sectors - sector)) * containers.SECTOR_SIZE
        return data.ljust(want, b'\x00') if want else b''

    def close(self):
        if getattr(self, '_image', None):
            self._image.close()
            self._image = None


containers.ChdDevice = ChdDevice  # open_container() looks the class up by this name

if __name__ == '__main__':
    if len(sys.argv) == 4 and sys.argv[1] == 'chd-iso':
        sys.exit(chd_reader.main(['chd.py', 'iso', sys.argv[2], sys.argv[3]]))
    main()
