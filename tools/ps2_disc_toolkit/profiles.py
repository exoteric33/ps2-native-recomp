"""
Configurable JSON profile loader, automatic game/engine matcher, and hidden-LBA
unindexed region splitter for PS2 discs.
"""

from dataclasses import dataclass, field
import fnmatch
import json
import os

from .containers import SECTOR_SIZE

PROFILES_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'profiles')


@dataclass
class CustomRegion:
    start_sector: int
    size_bytes: int
    name: str


@dataclass
class ProfileConfig:
    name: str = 'auto-iso9660'
    description: str = 'Standard ISO 9660 (DVD-5 / DVD-9 / CD-ROM) with automatic unindexed LBA region detection'
    disc_ids: list = field(default_factory=list)
    signature_files: list = field(default_factory=list)
    layer1_pvd_sector: int = None
    raw_region_threshold_mb: float = 8.0
    index_files: list = field(default_factory=list)
    archive_extensions: list = field(default_factory=list)
    custom_regions: list = field(default_factory=list)

    @property
    def raw_region_threshold_sectors(self):
        return max(1, int((self.raw_region_threshold_mb * (1 << 20)) // SECTOR_SIZE))


def load_profile(path_or_name):
    if not path_or_name or path_or_name == 'auto':
        return ProfileConfig()
    candidate = path_or_name
    if not os.path.isfile(candidate):
        candidate = os.path.join(PROFILES_DIR, path_or_name if path_or_name.endswith('.json') else f'{path_or_name}.json')
    if not os.path.isfile(candidate):
        raise FileNotFoundError(f'Profile not found: {path_or_name}')
    with open(candidate, 'r', encoding='utf-8') as f:
        raw = json.load(f)
    regions = [
        CustomRegion(
            start_sector=int(r['start_sector']),
            size_bytes=int(r['size_bytes']),
            name=str(r['name']),
        )
        for r in raw.get('custom_regions', [])
    ]
    return ProfileConfig(
        name=raw.get('name', os.path.splitext(os.path.basename(candidate))[0]),
        description=raw.get('description', ''),
        disc_ids=[d.upper() for d in raw.get('disc_ids', [])],
        signature_files=raw.get('signature_files', []),
        layer1_pvd_sector=raw.get('layer1_pvd_sector'),
        raw_region_threshold_mb=float(raw.get('raw_region_threshold_mb', 8.0)),
        index_files=raw.get('index_files', []),
        archive_extensions=raw.get('archive_extensions', []),
        custom_regions=regions,
    )


def match_profile(disc_id, file_entries):
    """Auto-detect if a built-in profile in profiles/*.json matches this disc's ID or file signatures."""
    if not os.path.isdir(PROFILES_DIR):
        return ProfileConfig()

    upper_paths = [e.path.upper() for e in file_entries]
    upper_set = set(upper_paths)
    disc_id_u = (disc_id or '').upper()

    for fname in sorted(os.listdir(PROFILES_DIR)):
        if not fname.endswith('.json'):
            continue
        prof = load_profile(os.path.join(PROFILES_DIR, fname))
        if disc_id_u and disc_id_u in prof.disc_ids:
            return prof
        if prof.signature_files:
            matched_all = True
            for pattern in prof.signature_files:
                pat_u = pattern.upper()
                if any(ch in pat_u for ch in '*?['):
                    if not any(fnmatch.fnmatch(p, pat_u) for p in upper_paths):
                        matched_all = False
                        break
                elif pat_u not in upper_set:
                    matched_all = False
                    break
            if matched_all:
                return prof

    return ProfileConfig()


def collect_non_file_gaps(total_sectors, file_entries, custom_regions=None):
    """Return sorted disjoint [start_sector, end_sector) intervals not covered by ISO files or custom regions."""
    occupied = []
    for entry in file_entries:
        count = (entry.size + SECTOR_SIZE - 1) // SECTOR_SIZE
        if count > 0:
            occupied.append((entry.sector, entry.sector + count))
    for reg in (custom_regions or []):
        count = (reg.size_bytes + SECTOR_SIZE - 1) // SECTOR_SIZE
        if count > 0:
            occupied.append((reg.start_sector, reg.start_sector + count))
    occupied.sort()

    gaps = []
    cursor = 0
    for start, end in occupied:
        if start > cursor:
            gaps.append((cursor, start))
        if end > cursor:
            cursor = end
    if cursor < total_sectors:
        gaps.append((cursor, total_sectors))
    return gaps


def scan_non_zero_runs(device, gaps):
    """Scan non-file sector gaps and return contiguous non-zero sector runs [(start_sector, sector_count)]."""
    runs = []
    zero_sector = b'\x00' * SECTOR_SIZE
    for gap_start, gap_end in gaps:
        run_start = None
        run_count = 0
        s = gap_start
        while s < gap_end:
            chunk = min(gap_end - s, 2048)
            raw = device.read_sectors(s, chunk)
            for i in range(chunk):
                sec = raw[i * SECTOR_SIZE:(i + 1) * SECTOR_SIZE]
                if sec != zero_sector:
                    if run_start is None:
                        run_start = s + i
                        run_count = 1
                    else:
                        run_count += 1
                else:
                    if run_start is not None:
                        runs.append((run_start, run_count))
                        run_start = None
                        run_count = 0
            s += chunk
        if run_start is not None:
            runs.append((run_start, run_count))
    return runs
