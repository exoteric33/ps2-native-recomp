#!/usr/bin/env python3
"""Check of scripts/vu_variants.ps1 (the diagnostic that shows why one entry address has hundreds of recorded vector programs).

The script is PowerShell with C# inside; this test holds an independent Python version of the same walk, hash and classification and
compares the two:
  1. made-up recordings with known answers (no game data): one entry address per kind of difference (a constant of an I-bit pair, the
     immediate of IADDIU / LQ / branch / FCSET / IADDI, flag bits of the upper word, the I bit, instructions, code that differs in a shared
     area, a program that runs past the end of memory), each checked for the exact categories and counts the script must print;
  2. if recorded vector programs are around (the main checkout's games\\<name>\\vu1, or --games), the busiest entries of each game: the script's
     numbers (variants, reachable pairs, differing pairs and their kinds, programs left when parts are ignored, runs, walk) must equal the
     Python ones, and every recomputed key must equal its file name.

Windows PowerShell 5.1 runs the script (`powershell.exe`); PowerShell 7 (`pwsh`) does too. Run from the repository root:
    python tests/vu_variants.py [--games <folder holding <name>\\vu1>] [--top 5]
"""
from pathlib import Path
import argparse
import re
import shutil
import struct
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
MASK = 16383
SLOTS = 2048
FNV = 0x100000001B3
M64 = (1 << 64) - 1
SCRIPT = root / 'scripts' / 'vu_variants.ps1'


# ---------------------------------------------------------------- the vector unit's code, as runtime/include/ps2/vu_ops.hpp reads it
def word(code, a):
    return struct.unpack_from('<I', code, a)[0]


def is_branch(lo):
    op = lo >> 25
    return op in (0x20, 0x21, 0x24, 0x25) or (0x28 <= op <= 0x2F and op not in (0x2A, 0x2B))


def reach(code, entry):
    """reachable(): the sorted pairs, how often the walk fell from 3FF8 to 0000, and how often a target wrapped"""
    seen, out, work = set(), [], [entry & MASK]
    fell_over = wrapped = 0
    while work:
        pc = work.pop()
        stop = -1
        stepped = False
        for _ in range(4096):
            if pc in seen:
                break
            seen.add(pc)
            out.append(pc)
            if stepped and pc == 0:
                fell_over += 1
            if stop == 0:
                break
            if stop > 0:
                stop -= 1
            lo, hi = word(code, pc), word(code, pc + 4)
            if hi & 0x40000000:
                stop = 0
            if not hi & 0x80000000 and is_branch(lo):
                op = lo >> 25
                if op not in (0x24, 0x25):
                    imm = lo & 0x7FF
                    imm -= 0x800 if imm & 0x400 else 0
                    raw = pc + 8 + imm * 8
                    wrapped += raw < 0 or raw > MASK
                    work.append(raw & MASK)
                if op in (0x21, 0x25):
                    wrapped += pc + 16 > MASK
                    work.append((pc + 16) & MASK)
                if op in (0x20, 0x21, 0x24, 0x25):
                    stop = 0
            pc = (pc + 8) & MASK
            stepped = True
    return sorted(out), fell_over, wrapped


def imm_mask(lo):
    """(mask, class name) of the immediate field of a lower instruction, or (0, None)"""
    op = lo >> 25
    if op in (0x00, 0x01):
        return 0x7FF, 'LQ/SQ offset'
    if op in (0x04, 0x05):
        return 0x7FF, 'ILW/ISW offset'
    if op in (0x08, 0x09):
        return 0x01E007FF, 'IADDIU/ISUBIU, base vi00 (a constant or an address)' if (lo >> 11) & 15 == 0 else 'IADDIU/ISUBIU'
    if op in (0x10, 0x11, 0x12, 0x13):
        return 0xFFFFFF, 'flag test (FCEQ/FCSET/FCAND/FCOR/FSEQ/FSSET/FSAND/FSOR)'
    if op in (0x14, 0x15, 0x16, 0x17):
        return 0x2007FF, 'flag test (FCEQ/FCSET/FCAND/FCOR/FSEQ/FSSET/FSAND/FSOR)'
    if op in (0x20, 0x21, 0x28, 0x29, 0x2C, 0x2D, 0x2E, 0x2F):
        return 0x7FF, 'branch offset'
    if op == 0x40 and lo & 0x3F == 0x32:
        return 0x7C0, 'IADDI'
    return 0, None


def key_of(code, pcs, entry, level=0, relative=False):
    h = 0xCBF29CE484222325 if relative else 0xCBF29CE484222325 ^ entry
    for pc in pcs:
        lo, hi = word(code, pc), word(code, pc + 4)
        pc = (pc - entry) & MASK if relative else pc
        if level < 4:
            if level >= 1:
                if hi & 0x80000000:
                    lo = 0
                elif level >= 2:
                    m, name = imm_mask(lo)
                    if m and (name != 'branch offset' or level >= 3):
                        lo &= ~m & 0xFFFFFFFF
        for v in (pc,) if level >= 4 else (pc, lo, hi):
            for i in range(4):
                h = ((h ^ ((v >> (8 * i)) & 0xFF)) * FNV) & M64
    return h


def build_pairs(code, pcs):
    """the pairs a build reads from a recording: reachable from the entry and from its jump targets (generate_files in vurecomp_gen.cpp)"""
    have = set(pcs)
    targets = set()
    for pc in pcs:
        lo, hi = word(code, pc), word(code, pc + 4)
        if hi & 0x80000000:
            continue
        if lo >> 25 in (0x21, 0x25):
            targets.add((pc + 16) & MASK)
        if lo >> 25 == 0x08 and (lo >> 11) & 15 == 0:
            targets.add(((((lo >> 10) & 0x7800) | (lo & 0x7FF)) * 8) & MASK)
    for t in targets:
        have.update(reach(code, t)[0])
    return len(have)


def quarters(ticks):
    """the text of Times() in the script: the span of the file times and the files in each quarter of it"""
    first, last = min(ticks), max(ticks)
    span = last - first
    if span < 600000000:
        return None
    q = [0, 0, 0, 0]
    for t in ticks:
        q[min(3, (t - first) * 4 // span)] += 1
    return q


def classify(vs, a):
    """the label of the pair at address a over the variants that reach it, or None when it does not differ"""
    here = [(word(c, a), word(c, a + 4)) for c, pcs in vs if a in pcs]
    values = set(here)
    partial = len(here) < len(vs)
    if len(values) == 1:
        return 'same words, reachable in some variants only' if partial else None
    iset = sum(1 for lo, hi in here if hi & 0x80000000)
    if 0 < iset < len(here):
        return 'I bit differs (lower word: constant in some variants, instruction in others)'
    lo0, hi0 = here[0]
    d_lo = d_hi = 0
    for lo, hi in here:
        d_lo |= lo ^ lo0
        d_hi |= hi ^ hi0
    lower = upper = None
    if d_lo:
        if iset == len(here):
            lower = 'LOI constant'
        elif any(lo >> 25 != lo0 >> 25 for lo, hi in here):
            lower = 'instruction (opcode)'
        else:
            m, name = imm_mask(lo0)
            lower = 'immediate only: ' + name if m and not d_lo & ~m else 'instruction (same opcode, other fields)'
    if d_hi:
        upper = 'flag bits E/M/D/T only' if not d_hi & ~0xF8000000 else 'instruction'
    label = ('upper %s + lower %s' % (upper, lower)) if upper and lower else 'upper ' + upper if upper else 'lower ' + lower
    return label + (' + reach differs' if partial else '')


def analyse(files, entry):
    """the numbers vu_variants.ps1 prints for one entry: files = [(path, key)]"""
    vs, keys_ok, runs, run_set = [], 0, 0, set()
    fell = wrap = far = 0
    far_max = zero_v = zero_max = sum_pairs = sum_build = 0
    ticks, rel0, rel2 = [], set(), set()
    for path, key in files:
        code = Path(path).read_bytes()
        pcs, f, w = reach(code, entry)
        keys_ok += key_of(code, pcs, entry) == key
        vs.append((code, set(pcs)))
        sum_pairs += len(pcs)
        sum_build += build_pairs(code, pcs)
        ticks.append(Path(path).stat().st_mtime_ns // 100)
        rel0.add(key_of(code, pcs, entry, 0, True))
        rel2.add(key_of(code, pcs, entry, 2, True))
        fell += f > 0
        wrap += w > 0
        d = max(abs(pc - entry) for pc in pcs)
        far += d > 0x1000
        zeros = sum(1 for pc in pcs if word(code, pc) == 0 and word(code, pc + 4) == 0)
        zero_v += zeros > 0
        zero_max = max(zero_max, zeros)
        far_max = max(far_max, d)
        i = 0
        while i < len(pcs):
            j = i
            while j + 1 < len(pcs) and pcs[j + 1] == pcs[j] + 8:
                j += 1
            run_set.add((tuple((pc, word(code, pc), word(code, pc + 4)) for pc in pcs[i:j + 1])))
            runs += 1
            i = j + 1
    n = len(vs)
    sizes = sorted(len(p) for c, p in vs)
    union = set().union(*(p for c, p in vs))
    common = set(vs[0][1]).intersection(*(p for c, p in vs))
    labels = {}
    for a in sorted(union):
        label = classify(vs, a)
        if label:
            labels[label] = labels.get(label, 0) + 1
    left = [len({key_of(c, sorted(p), entry, level) for c, p in vs}) for level in range(5)]
    return dict(n=n, keys_ok=keys_ok, lo=sizes[0], mid=sizes[n // 2], hi=sizes[-1], union=len(union), common=len(common),
                differing=sum(labels.values()), labels=labels, left=left, runs=runs, runs_distinct=len(run_set), fell=fell, wrap=wrap,
                far=far, far_max=far_max, zero_v=zero_v, zero_max=zero_max,
                build_bytes=sum_build * 8 // n, build_pct=sum_build * 800 // (n * 16384), reach_bytes=sum_pairs * 8 // n, quarters=quarters(ticks),
                rel0=rel0, rel2=rel2)


# ---------------------------------------------------------------- reading what the script prints
def run_script(args):
    for exe in ('powershell.exe', 'pwsh'):
        if shutil.which(exe):
            r = subprocess.run([exe, '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', str(SCRIPT)] + args,
                               capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=1800)
            if r.returncode != 0:
                sys.exit('script failed (%s):\n%s%s' % (exe, r.stdout[-2000:], r.stderr[-2000:]))
            return r.stdout
    sys.exit('neither powershell.exe nor pwsh found')


def parse(text):
    """{entry: dict} from the printed text"""
    out = {}
    m = re.search(r'variants of these entries in all: (\d+); different when the entry address is ignored .*?: (\d+); also ignoring LOI constants and immediates: (\d+)', text)
    out['rel'] = tuple(map(int, m.groups())) if m else None
    for block in re.split(r'(?m)^== entry ', text)[1:]:
        entry = int(block[:4], 16)
        d = {}
        m = re.search(r'(\d+) of (\d+) keys recomputed', block)
        d['keys_ok'], d['n'] = int(m.group(1)), int(m.group(2))
        m = re.search(r'per variant: (\d+) to (\d+) \(middle (\d+)\); in the union (\d+), in every variant (\d+)', block)
        d['lo'], d['hi'], d['mid'], d['union'], d['common'] = map(int, m.groups())
        m = re.search(r'pairs that differ between variants: (\d+) of', block)
        d['differing'] = int(m.group(1))
        d['labels'] = {lab: int(c) for c, lab in re.findall(r'(?m)^ +(\d+) pairs: (.*)$', block)}
        m = re.search(r'nothing (\d+) \| LOI constants (\d+) \| \+ immediates \(not branch offsets\) (\d+) \| \+ branch offsets (\d+) \| all words \(only the reachable addresses\) (\d+)', block)
        d['left'] = list(map(int, m.groups()))
        m = re.search(r'pairs: (\d+) in all variants, (\d+) different', block)
        d['runs'], d['runs_distinct'] = map(int, m.groups())
        m = re.search(r'into 0000 in (\d+) variants; .* wraps in (\d+); .* more than 1000 bytes from the entry in (\d+) \(farthest: ([0-9A-F]+) bytes\)', block)
        d['fell'], d['wrap'], d['far'], d['far_max'] = int(m.group(1)), int(m.group(2)), int(m.group(3)), int(m.group(4), 16)
        m = re.search(r'empty memory \(all zero\): in (\d+) variants, up to (\d+) pairs', block)
        d['zero_v'], d['zero_max'] = int(m.group(1)), int(m.group(2))
        m = re.search(r'would average (\d+) bytes, (\d+) % of the 16384 a recording takes; reachable alone: (\d+) bytes', block)
        d['build_bytes'], d['build_pct'], d['reach_bytes'] = map(int, m.groups())
        m = re.search(r'recorded: over .* files in each quarter of that time: (\d+) (\d+) (\d+) (\d+)', block)
        d['quarters'] = list(map(int, m.groups())) if m else None
        out[entry] = d
    return out


# ---------------------------------------------------------------- made-up recordings
NOP_UP, NOP_LO = 0x000002FF, 0x8000033C
E = 0x40000000


def iaddiu(it, is_, imm):
    return (0x08 << 25) | (((imm >> 11) & 15) << 21) | (it << 16) | (is_ << 11) | (imm & 0x7FF)


def lq(ft, fs, off):
    return (0x00 << 25) | (15 << 21) | (ft << 16) | (fs << 11) | (off & 0x7FF)


def fcset(imm):
    return (0x11 << 25) | (imm & 0xFFFFFF)


def iaddi(it, is_, imm5):
    return (0x40 << 25) | (is_ << 11) | (it << 16) | ((imm5 & 31) << 6) | 0x32


def ibeq(it, is_, off):
    return (0x28 << 25) | (it << 16) | (is_ << 11) | (off & 0x7FF)


def jr(is_):
    return (0x24 << 25) | (is_ << 11)


def image(pairs):
    """pairs = {address: (lower, upper)} in a 16 KiB code memory of zeros"""
    code = bytearray(16384)
    for a, (lo, hi) in pairs.items():
        struct.pack_into('<II', code, a, lo, hi)
    return bytes(code)


def ending(base, address):
    """an end: the pair with the E bit and its delay slot"""
    base[address] = (NOP_LO, NOP_UP | E)
    base[address + 8] = (NOP_LO, NOP_UP)


def made_up(folder):
    """writes the recordings; returns {entry: [expected label counts]} and the extra checks"""
    cases = {}

    def record(entry, images):
        for code in images:
            pcs, _, _ = reach(code, entry)
            (folder / ('%016X_%04X.bin' % (key_of(code, pcs, entry), entry))).write_bytes(code)

    def base(entry, count):
        b = {entry + 8 * i: (NOP_LO, NOP_UP) for i in range(count)}
        ending(b, entry + 8 * count)
        return b

    # 1. only a LOI constant differs (3 values)
    b = base(0x100, 4)
    imgs = []
    for value in (0x3F800000, 0x40000000, 0x40400000):
        p = dict(b)
        p[0x110] = (value, NOP_UP | 0x80000000)
        imgs.append(image(p))
    record(0x100, imgs)
    cases[0x100] = dict(n=3, labels={'lower LOI constant': 1}, left=[3, 1, 1, 1, 1])

    # 2. immediates of integer instructions: IADDIU (base vi00), LQ offset, FCSET, IADDI, ILW-free
    b = base(0x200, 8)
    imgs = []
    for imm, off, fc, i5 in ((5, 1, 3, 1), (6, 1, 3, 1), (5, 2, 3, 1), (5, 1, 4, 1), (5, 1, 3, 2)):
        p = dict(b)
        p[0x208] = (iaddiu(1, 0, imm), NOP_UP)
        p[0x210] = (lq(1, 2, off), NOP_UP)
        p[0x218] = (fcset(fc), NOP_UP)
        p[0x220] = (iaddi(1, 2, i5), NOP_UP)
        imgs.append(image(p))
    record(0x200, imgs)
    cases[0x200] = dict(n=5, labels={'lower immediate only: IADDIU/ISUBIU, base vi00 (a constant or an address)': 1,
                                     'lower immediate only: LQ/SQ offset': 1,
                                     'lower immediate only: flag test (FCEQ/FCSET/FCAND/FCOR/FSEQ/FSSET/FSAND/FSOR)': 1,
                                     'lower immediate only: IADDI': 1}, left=[5, 5, 1, 1, 1])

    # 3. a branch offset: the two arms lead to different code (reach differs)
    imgs = []
    for target in (0x340, 0x360):
        p = {0x300: (NOP_LO, NOP_UP), 0x308: (ibeq(1, 2, (target - 0x310) // 8), NOP_UP), 0x310: (NOP_LO, NOP_UP)}
        ending(p, 0x318)
        ending(p, target)
        imgs.append(image(p))
    record(0x300, imgs)
    cases[0x300] = dict(n=2, labels={'lower immediate only: branch offset': 1, 'same words, reachable in some variants only': 4},
                        left=[2, 2, 2, 2, 2])

    # 4. flag bits of the upper word (the M bit) and the E bit moved by one pair is not this: only M here
    b = base(0x400, 4)
    imgs = []
    for up in (NOP_UP, NOP_UP | 0x20000000):
        p = dict(b)
        p[0x410] = (NOP_LO, up)
        imgs.append(image(p))
    record(0x400, imgs)
    cases[0x400] = dict(n=2, labels={'upper flag bits E/M/D/T only': 1}, left=[2, 2, 2, 2, 1])

    # 5. the I bit: a constant in one variant, an instruction in the other
    b = base(0x500, 4)
    imgs = []
    for lo, up in ((0x3F800000, NOP_UP | 0x80000000), (NOP_LO, NOP_UP)):
        p = dict(b)
        p[0x510] = (lo, up)
        imgs.append(image(p))
    record(0x500, imgs)
    cases[0x500] = dict(n=2, labels={'I bit differs (lower word: constant in some variants, instruction in others)': 1}, left=[2, 2, 2, 2, 1])

    # 6. other instructions: the upper word and the lower word (IADD against ISUB: the same major opcode 0x40)
    b = base(0x600, 4)
    imgs = []
    for lo, up in (((0x40 << 25) | 0x30, 0x000002A8), ((0x40 << 25) | 0x31, 0x000002A9)):
        p = dict(b)
        p[0x610] = (lo, up)
        imgs.append(image(p))
    record(0x600, imgs)
    cases[0x600] = dict(n=2, labels={'upper instruction + lower instruction (same opcode, other fields)': 1}, left=[2, 2, 2, 2, 1])

    # 7. a shared area that other uploads change: the entry's program branches into 0x1000.., the area holds a different routine each time
    imgs = []
    for k in range(4):
        p = {0x700: (NOP_LO, NOP_UP), 0x708: (ibeq(1, 2, (0x1000 - 0x710) // 8), NOP_UP), 0x710: (NOP_LO, NOP_UP)}
        ending(p, 0x718)
        p[0x1000] = (NOP_LO, NOP_UP ^ k)
        p[0x1008] = (NOP_LO, NOP_UP ^ (k << 2))
        ending(p, 0x1010)
        imgs.append(image(p))
    record(0x700, imgs)
    cases[0x700] = dict(n=4, labels={'upper instruction': 2}, left=[4, 4, 4, 4, 1])

    # 8. a program that never ends runs past the end of memory into 0000.. (and differs there)
    imgs = []
    for k in range(2):
        p = {0x3FF0: (NOP_LO, NOP_UP), 0x3FF8: (NOP_LO, NOP_UP), 0x0000: (NOP_LO, NOP_UP ^ k)}
        ending(p, 0x8)
        imgs.append(image(p))
    record(0x3FF0, imgs)
    cases[0x3FF0] = dict(n=2, labels={'upper instruction': 1}, left=[2, 2, 2, 2, 1], fell=2, far_max=0x3FF0)
    return cases


def check_made_up():
    ok = True
    with tempfile.TemporaryDirectory() as tmp:
        folder = Path(tmp) / 'vu1'
        folder.mkdir()
        cases = made_up(folder)
        got = parse(run_script(['-Dir', str(folder), '-Top', '20']))
        for entry, want in sorted(cases.items()):
            g = got.get(entry)
            problems = []
            if g is None:
                problems.append('entry missing from the output')
            else:
                if g['n'] != want['n'] or g['keys_ok'] != want['n']:
                    problems.append('variants/keys %s/%s want %s' % (g['n'], g['keys_ok'], want['n']))
                if g['labels'] != want['labels']:
                    problems.append('kinds %s want %s' % (g['labels'], want['labels']))
                if g['left'] != want['left']:
                    problems.append('left %s want %s' % (g['left'], want['left']))
                if 'fell' in want and g['fell'] != want['fell']:
                    problems.append('fell %s want %s' % (g['fell'], want['fell']))
                if 'far_max' in want and g['far_max'] != want['far_max']:
                    problems.append('farthest %X want %X' % (g['far_max'], want['far_max']))
                # the Python version must agree on everything too
                files = [(p, int(p.name.split('_')[0], 16)) for p in sorted(folder.glob('*_%04X.bin' % entry))]
                mine = analyse(files, entry)
                for k in ('n', 'keys_ok', 'labels', 'left', 'union', 'common', 'differing', 'runs', 'runs_distinct', 'zero_v', 'zero_max', 'build_bytes', 'build_pct', 'reach_bytes', 'quarters'):
                    if g[k] != mine[k]:
                        problems.append('python %s=%s script %s' % (k, mine[k], g[k]))
            print('%s made-up entry %04X: %s' % ('PASS' if not problems else 'FAIL', entry, '; '.join(problems) or 'ok'))
            ok = ok and not problems
    return ok


# ---------------------------------------------------------------- recorded programs
def check_recorded(games, top):
    ok = True
    for game in sorted(games.iterdir()):
        folder = game / 'vu1'
        names = sorted(folder.glob('*.bin')) if folder.is_dir() else []
        if not names:
            continue
        by_entry = {}
        for p in names:
            m = re.fullmatch(r'([0-9A-Fa-f]{16})_([0-9A-Fa-f]{4})', p.stem)
            if m:
                by_entry.setdefault(int(m.group(2), 16), []).append((p, int(m.group(1), 16)))
        busiest = sorted(by_entry, key=lambda e: (-len(by_entry[e]), e))[:top]
        got = parse(run_script(['-Dir', str(folder), '-Top', str(top)]))
        rel0, rel2, total = set(), set(), 0
        for entry in busiest:
            g = got.get(entry)
            mine = analyse(by_entry[entry], entry)
            rel0 |= mine['rel0']
            rel2 |= mine['rel2']
            total += mine['n']
            problems = []
            if g is None:
                problems.append('entry missing')
            else:
                for k in ('n', 'keys_ok', 'lo', 'mid', 'hi', 'union', 'common', 'differing', 'labels', 'left', 'runs', 'runs_distinct',
                          'fell', 'wrap', 'far', 'far_max', 'zero_v', 'zero_max', 'build_bytes', 'build_pct', 'reach_bytes', 'quarters'):
                    if g[k] != mine[k]:
                        problems.append('%s: script %s python %s' % (k, g[k], mine[k]))
            if mine['keys_ok'] != mine['n']:
                problems.append('python keys %d of %d' % (mine['keys_ok'], mine['n']))
            print('%s %s entry %04X: %d variants%s' % ('PASS' if not problems else 'FAIL', game.name, entry, mine['n'], ' ' + '; '.join(problems) if problems else ', same numbers, all keys equal the file names'))
            ok = ok and not problems
        if len(busiest) >= 2:
            want = (total, len(rel0), len(rel2))
            same = got['rel'] == want
            print('%s %s the %d entries together: %d variants, %d without the entry address, %d also without immediates%s' % (
                'PASS' if same else 'FAIL', game.name, len(busiest), *want, '' if same else ' (script: %s)' % (got['rel'],)))
            ok = ok and same
    return ok


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--games', type=Path, default=None, help='folder holding <name>\\vu1 (default: the main checkout games\\ next to wt\\)')
    parser.add_argument('--top', type=int, default=5)
    args = parser.parse_args()
    games = args.games
    if games is None:
        for base in (root, root.parent.parent):
            if (base / 'games').is_dir():
                games = base / 'games'
                break
    ok = check_made_up()
    if games and games.is_dir():
        ok = check_recorded(games, args.top) and ok
    else:
        print('no recorded vector programs found (pass --games): only the made-up recordings were checked')
    print('ALL PASS' if ok else 'FAILED')
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
