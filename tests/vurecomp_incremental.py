#!/usr/bin/env python3
"""Regression (#112, a game with thousands of vector programs): "Make it faster" compiled every recorded program again.

`vurecomp` sorted the programs by key into files of 16 and deleted and wrote every `vu1_*.cpp` file on every run. One new program
moved every later program into another file, and a file written again has a newer time than its object file, so the build tool
(ninja, no ccache in the app) compiled all of them again: 42 of 43 files differed after 10 of 610 programs were added. Now the
programs go into files by the order they were recorded (the time of the `.bin` file), a program followed by its jump targets, and
a file whose text did not change is not written.

No game data is used: the recordings are made up (a handful of instruction pairs in a 16 KiB code memory each). The checks:
  1. a second run with nothing new writes no file (same text, same time stamp);
  2. recording more programs leaves the files of the earlier ones byte for byte and time stamp for time stamp as they were, and changes
     the last file and adds one;
  3. a program recorded later whose key a jump target already had (the target is dropped, the program joins the end) changes only
     the file of the program that had the target and the last one;
  4. recordings taken away leave no file behind; every function of the table has its body in some file;
  5. the C++ of every file is accepted by clang++ (with clang++ on the PATH, like the other generator tests).

Build `vurecomp` first (`ninja -C build/tools vurecomp`) or name one with --vurecomp.
"""
from pathlib import Path
import argparse
import os
import re
import platform
import shutil
import struct
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--vurecomp', type=Path, default=None)
args = parser.parse_args()
tool = args.vurecomp
if tool is None:
    for name in ('build/tools/vurecomp.exe', 'build/tools/vurecomp'):
        if (root / name).exists():
            tool = root / name
            break
if tool is None or not tool.exists():
    sys.exit('vurecomp not found: build it (ninja -C build/tools vurecomp) or pass --vurecomp')

MASK = 16383
FNV = 0x100000001B3
M64 = (1 << 64) - 1
NOP_UPPER, NOP_LOWER = 0x000002FF, 0x8000033C
E_BIT = 0x40000000


# --- the key of a program, as runtime/include/ps2/vu_ops.hpp reachable() and program_key() make it (these programs have no branches)
def reachable(code, entry):
    out, pc, stop_after = [], entry & MASK, -1
    while True:
        out.append(pc)
        if stop_after == 0:
            break
        if stop_after > 0:
            stop_after -= 1
        hi = struct.unpack_from('<I', code, pc + 4)[0]
        if hi & E_BIT:
            stop_after = 0
        pc = (pc + 8) & MASK
    return sorted(out)


def key_of(code, entry):
    h = 0xCBF29CE484222325 ^ entry
    for pc in reachable(code, entry):
        lo, hi = struct.unpack_from('<II', code, pc)
        for v in (pc, lo, hi):
            for i in range(4):
                h = ((h ^ ((v >> (i * 8)) & 0xFF)) * FNV) & M64
    return h


def iaddiu(it, is_, imm):
    return (0x08 << 25) | (((imm >> 11) & 0xF) << 21) | (it << 16) | (is_ << 11) | (imm & 0x7FF)


def program(image, at, tag, jump_to=None):
    """Four pairs at `at`: [jump constant or nothing], a pair that makes this program differ from the others (`tag`), the end (E bit), its delay slot."""
    pairs = [(NOP_UPPER, iaddiu(1, 0, jump_to // 8) if jump_to is not None else NOP_LOWER),
             (NOP_UPPER, iaddiu(2, 1, tag)),
             (NOP_UPPER | E_BIT, NOP_LOWER),
             (NOP_UPPER, NOP_LOWER)]
    for n, (hi, lo) in enumerate(pairs):
        struct.pack_into('<II', image, at + n * 8, lo, hi)


TAIL = 0x1000  # where a jump target program sits in the image of the program that jumps to it


def recording(folder, index, entry, tag, jump_to=None, tail_only=False):
    image = bytearray(16384)
    if tail_only:
        program(image, TAIL, 900)
        entry = TAIL
    else:
        program(image, entry, tag, jump_to)
        if jump_to is not None:
            program(image, jump_to, 900)
    name = '%016X_%04X.bin' % (key_of(image, entry), entry)
    path = folder / name
    path.write_bytes(bytes(image))
    stamp = 1_700_000_000_000_000_000 + index * 1_000_000_000  # one second apart, in recording order
    os.utime(path, ns=(stamp, stamp))
    return path


def run(folder, out):
    done = subprocess.run([str(tool), str(folder), str(out)], capture_output=True, text=True)
    if done.returncode != 0:
        sys.exit('vurecomp failed:\n' + done.stdout + done.stderr)
    return done.stdout.strip()


def snapshot(out):
    return {p.name: (p.read_bytes(), p.stat().st_mtime_ns) for p in out.iterdir() if p.name.startswith('vu1_')}


failures = []


def check(ok, what):
    print(('PASS  ' if ok else 'FAIL  ') + what)
    if not ok:
        failures.append(what)


def code_files(snap):
    return sorted(n for n in snap if n != 'vu1_table.cpp')


with tempfile.TemporaryDirectory() as tmp:
    tmp = Path(tmp)
    rec, out = tmp / 'vu1', tmp / 'generated'
    rec.mkdir()
    made = []

    def add(count, **kw):
        for _ in range(count):
            n = len(made)
            made.append(recording(rec, n, 0x100 + (n % 5) * 0x40, 1 + n, **kw))

    # run 1: 24 programs; the sixth jumps to a program of its own that it carries at TAIL (a jump target)
    add(5)
    made.append(recording(rec, 5, 0x100 + 5 * 0x40, 6, jump_to=TAIL))
    add(18)
    print(run(rec, out))
    first = snapshot(out)
    check(code_files(first) == ['vu1_000.cpp', 'vu1_001.cpp'], '24 recorded programs and one target make two files of 16 recorded programs (%s)' % code_files(first))

    # 1. nothing new: nothing written
    run(rec, out)
    again = snapshot(out)
    check(again == first, 'a second run with nothing new writes no file (text and time stamp as before)')

    # 2. twelve more programs: file 0 is untouched, file 1 is filled up, file 2 is new
    add(12)
    run(rec, out)
    second = snapshot(out)
    check(second['vu1_000.cpp'] == first['vu1_000.cpp'], 'recording 12 more programs leaves file 0 as it was (text and time stamp)')
    check(second['vu1_001.cpp'][0] != first['vu1_001.cpp'][0] and 'vu1_002.cpp' in second, 'the last file gets the new programs and a new file is added')

    # 3. a program recorded later that is a jump target of an earlier one: the earlier program's file and the last file change, the middle one does not
    made.append(recording(rec, len(made), 0, 0, tail_only=True))
    run(rec, out)
    third = snapshot(out)
    check(third['vu1_001.cpp'] == second['vu1_001.cpp'], 'a program that was a jump target before joins the end: the file in between is untouched')
    check(third['vu1_000.cpp'][0] != second['vu1_000.cpp'][0], 'the file that had the target as a jump target no longer has it')

    # 4. recordings taken away (the newest 8): the file they filled is gone, nothing else is left behind
    for path in made[-8:]:
        path.unlink()
    run(rec, out)
    fourth = snapshot(out)
    table = fourth['vu1_table.cpp'][0].decode()
    declared = set(re.findall(r'^void (vu1_\w+)\(ps2::Vu0Regs', table, re.M))
    defined = set()
    for name in code_files(fourth):
        defined |= set(re.findall(r'^void (vu1_\w+)\(Vu0Regs &r, std::uint8_t \*mem\) \{', fourth[name][0].decode(), re.M))
    check(len(code_files(fourth)) < len(code_files(third)), 'recordings taken away leave no file behind (%d files now, %d before)' % (len(code_files(fourth)), len(code_files(third))))
    check(declared and declared == defined, 'every function of the table has its body in some file and no file holds one the table lacks (%d programs)' % len(declared))

    # 5. the C++ is accepted
    compiler = shutil.which('clang++')
    x86_flags = ['-mavx2'] if platform.machine().lower() in ('x86_64', 'amd64') else []  # an ARM Mac has no AVX2
    if compiler:
        bad = []
        for p in sorted(out.glob('vu1_*.cpp')):
            r = subprocess.run([compiler, '-std=gnu++20', *x86_flags, '-ffp-contract=off', '-fsyntax-only', '-Wno-unused-label', '-Wno-unused-variable',
                                '-isystem', str(root / 'runtime/include'), str(p)], capture_output=True, text=True)
            if r.returncode != 0:
                bad.append(p.name + ': ' + r.stderr.strip().splitlines()[0] if r.stderr.strip() else p.name)
        check(not bad, 'clang++ accepts every generated file' + (' (%s)' % bad[:2] if bad else ''))
    else:
        print('SKIP  clang++ is not on the PATH: the generated files were not compiled')

print('vurecomp incremental: %d failed' % len(failures))
sys.exit(1 if failures else 0)
