"""
Independent, multi-format PS2 Disc Extraction & Virtual Disc Packaging Toolkit.
Supports ISO-2048 (DVD-5 / DVD-9), BIN/CUE-2352 (CD-ROM Mode 1 / Mode 2 XA),
CSO (CISO zlib compressed images), and CHD v5 (MAME Compressed Hunks of Data:
zlib, lzma, zstd, cdzl, cdlz, cdzs), plus configurable engine profiles and
hidden-LBA unindexed region extraction.
"""

from .containers import (  # noqa: F401
    open_container,
    SectorDevice,
    Iso2048Device,
    Bin2352Device,
    CsoDevice,
    ChdDevice,
    SECTOR_SIZE,
)
from .iso9660 import IsoFileSystem, SystemCnf, FileEntry, LayerInfo  # noqa: F401
from .profiles import ProfileConfig, load_profile, match_profile  # noqa: F401
from .bundler import build_single_file_bundle  # noqa: F401

