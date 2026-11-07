"""
Independent, multi-format PS2 Disc Extraction, Bidirectional Modding & Packaging Toolkit.
Supports ISO-2048 (DVD-5 / DVD-9), BIN/CUE-2352 (CD-ROM Mode 1 / Mode 2 XA),
CSO (CISO zlib compressed images), and CHD v5 (MAME Compressed Hunks of Data:
zlib, lzma, zstd, cdzl, cdlz, cdzs), plus configurable engine profiles,
hidden-LBA unindexed region extraction, 3D/Texture/Audio/Video conversion,
and multi-core incremental asset repacking.
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
    compute_tim2_gpu_key,
    decode_png_to_rgba_bytes,
    decode_tim2_to_rgba,
    decode_snowblind_tex_to_rgba,
    decode_raw16_to_rgba,
    encode_png_to_tim2_bytes,
    encode_rgba_png,
    gpu_content_key,
    unswizzle_clut_csm1,
)
from .ps2_audio import (  # noqa: F401
    decode_adx_to_wav_bytes,
    decode_sshd_to_wav_bytes,
    decode_vag_to_wav_bytes,
    decode_spu2_adpcm_blocks,
    encode_wav_to_adx_bytes,
    encode_wav_to_vag_bytes,
    is_adx_bytes,
    is_spu2_bd_bank,
    is_sshd_bytes,
    is_vag_bytes,
    split_spu2_bd_bank_to_vag,
)
from .ps2_video import (  # noqa: F401
    convert_sfd_pss_to_mp4,
    demux_ps2_mpeg_ps,
    detect_ps2_video_subtype,
    encode_mp4_to_sfd_pss,
    is_ps2_mpeg_ps_bytes,
)
from .ps2_meshes import (  # noqa: F401
    Ps2SubMesh,
    is_fmod_bytes,
    decode_fmod_to_meshes,
    decode_vif_to_meshes,
    encode_obj_to_fmod,
    export_mesh_to_obj,
    export_meshes_to_obj,
    parse_obj_to_submeshes,
)
from .repacker import (  # noqa: F401
    bootstrap_manifest_for_dir,
    sync_game_assets,
    sync_unpacked_directory,
)
from .bundler import build_single_file_bundle  # noqa: F401
