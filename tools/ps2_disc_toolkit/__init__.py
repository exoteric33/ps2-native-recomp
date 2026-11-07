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
from .archives import (  # noqa: F401
    open_archive,
    open_archive_bytes,
    extract_archive,
    convert_asset_bytes,
)
from .ps2_textures import (  # noqa: F401
    GsVramCanvas,
    decode_tim2_to_rgba,
    decode_snowblind_tex_to_rgba,
    decode_raw16_to_rgba,
    encode_rgba_png,
    unswizzle_clut_csm1,
)
from .ps2_audio import (  # noqa: F401
    decode_vag_to_wav_bytes,
    decode_spu2_adpcm_blocks,
)
from .ps2_meshes import (  # noqa: F401
    Ps2SubMesh,
    is_fmod_bytes,
    decode_fmod_to_meshes,
    decode_vif_to_meshes,
    export_mesh_to_obj,
    export_meshes_to_obj,
)
from .bundler import build_single_file_bundle  # noqa: F401

