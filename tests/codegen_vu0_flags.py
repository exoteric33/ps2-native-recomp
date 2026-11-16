#!/usr/bin/env python3
"""The flags of the first vector unit in the translator's output: left out only where nothing can see them.

The translator writes the vector unit's macro arithmetic as calls of four-lane helpers (runtime/include/ps2/vu.hpp,
`vu::add4<F>` and the like). F is 0 when the MAC register and the low status bits the instruction sets cannot be seen:
on the straight line after it another instruction sets them again before anything that could read them. This test feeds
the translator made-up programs (no game data) and reads F back from the C++ it writes:

  * the flags are left out in front of another flag-setting instruction, with register work in between;
  * they are kept in front of everything else: CFC2 of a control register, a load, a store, VCALLMS, QMFC2 with the
    interlock bit, a branch, a label (something jumps in there), a delay slot, the end of the function;
  * a program with every macro instruction compiles (with clang++ on the PATH), so every call the translator writes
    exists in vu.hpp with those arguments;
  * PS2_VU0_HELPERS=old writes the one-lane calls as before.
"""
from pathlib import Path
import argparse
import os
import re
import shutil
import struct
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--recompiler', type=Path, default=root / 'build/tools/eerecomp')
args = parser.parse_args()
compiler = shutil.which('clang++')   # clang only, like scripts/cxx.sh: the runtime headers use clang builtins that GCC lacks
BASE = 0x100000

# --- instruction words
NOP = 0
JR_RA = 0x03E00008
T0, A0 = 8, 4
def addiu(reg): return 0x24000001 | reg << 21 | reg << 16
def beq(rs, rt, off): return 0x10000000 | rs << 21 | rt << 16 | off & 0xFFFF
def lw(rt, base): return 0x8C000000 | base << 21 | rt << 16
def sw(rt, base): return 0xAC000000 | base << 21 | rt << 16
def sqc2(ft, base): return 0xF8000000 | base << 21 | ft << 16
def qmfc2(rt, fs, interlock=0): return 0x48200000 | rt << 16 | fs << 11 | interlock
def cfc2(rt, reg): return 0x48400000 | rt << 16 | reg << 11
def special1(funct, dest=15, ft=1, fs=2, fd=3): return 0x4A000000 | dest << 21 | ft << 16 | fs << 11 | fd << 6 | funct
def special2(index, dest=15, ft=1, fs=2): return 0x4A000000 | dest << 21 | ft << 16 | fs << 11 | (index >> 2) << 6 | 0x3C | index & 3
VADD, VSUB, VMUL = special1(0x28), special1(0x2C), special1(0x2A)
VMADDW = special1(0x0B)                       # vmaddw: funct 0x08 + the lane
VCALLMS = special1(0x38, dest=0, ft=0, fs=0, fd=0)
VMULAX, VMADDAY, VMADDAZ = special2(0x18), special2(0x09), special2(0x0A)
VMOVE = special2(0x30)
STATUS = 16                                   # the control register with the status flags

def elf(words):
    data = bytearray(0x100 + 4 * len(words))
    data[:7] = b'\x7fELF\x01\x01\x01'
    struct.pack_into('<HHIIIIIHHHHHH', data, 16, 2, 8, 1, BASE, 52, 0, 0, 52, 32, 1, 40, 0, 0)
    struct.pack_into('<8I', data, 52, 1, 0x100, BASE, 0, 4 * len(words), 4 * len(words), 5, 4)
    struct.pack_into(f'<{len(words)}I', data, 0x100, *words)
    return bytes(data)

def translate(work, name, words, env=None):
    (work / f'{name}.elf').write_bytes(elf(words))
    out = work / f'{name}-out'
    result = subprocess.run([str(args.recompiler.resolve()), str(work / f'{name}.elf'), '--out', str(out)], capture_output=True, text=True,
                            env={**os.environ, **(env or {})})
    assert result.returncode == 0, result.stderr
    return out, '\n'.join(unit.read_text() for unit in sorted(out.glob('*unit*.cpp')))

def calls(text):
    """The four-lane calls that take the flag argument, in the order they are written: [(name, '0' or '1')]."""
    return re.findall(r'vu::(\w+?)4<([01])>\(', text)

def compile_errors(out):
    if not compiler:
        return []
    units = sorted(Path(out).glob('*.cpp'))
    result = subprocess.run([compiler, '-std=c++20', '-fsyntax-only', '-I' + str(root / 'runtime/include'), '-I' + str(out), *map(str, units)],
                            capture_output=True, text=True)
    return [line for line in result.stderr.splitlines() if 'error' in line] if result.returncode else []

END = [JR_RA, NOP]
# name, the program, the calls it must be written as
cases = [
    ('next sets them again', [VADD, VSUB, cfc2(T0, STATUS)] + END, [('add', '0'), ('sub', '1')]),
    ('register work in between', [VADD, VMOVE, addiu(A0), qmfc2(T0, 1), cfc2(T0, 3), VMULAX] + END, [('add', '0'), ('mulabc', '1')]),
    ('a matrix times a vector', [VMULAX, VMADDAY, VMADDAZ, VMADDW, sqc2(3, A0)] + END,
     [('mulabc', '0'), ('maddabc', '0'), ('maddabc', '0'), ('maddbc', '1')]),
    ('cfc2 of the status register', [VADD, cfc2(T0, STATUS), VSUB] + END, [('add', '1'), ('sub', '1')]),
    ('a load', [VADD, lw(T0, A0), VSUB] + END, [('add', '1'), ('sub', '1')]),
    ('a store', [VADD, sw(T0, A0), VSUB] + END, [('add', '1'), ('sub', '1')]),
    ('a vector program is called', [VADD, VCALLMS, VSUB] + END, [('add', '1'), ('sub', '1')]),
    ('qmfc2 with the interlock bit', [VADD, qmfc2(T0, 1, 1), VSUB] + END, [('add', '1'), ('sub', '1')]),
    ('the end of the function', [VADD, JR_RA, NOP], [('add', '1')]),
    # the branch goes to the vsub, so the code can be entered there: the vadd in front of the label keeps its flags
    ('a label', [beq(T0, 0, 2), NOP, VADD, VSUB] + END, [('add', '1'), ('sub', '1')]),
]

with tempfile.TemporaryDirectory(prefix='ps2-vu0-flags-test-') as temp:
    work = Path(temp)
    problems = []
    for number, (name, words, expected) in enumerate(cases):
        out, text = translate(work, f'case{number}', words)
        if calls(text) != expected:
            problems.append(f'{name}: written as {calls(text)}, expected {expected}')

    # a branch between two of them, and one in the delay slot: all of them keep their flags, however often the slot is written
    out, text = translate(work, 'branch', [VADD, beq(T0, 0, 2), VSUB, VMUL, NOP] + END)
    found = calls(text)
    if not found or {name for name, _ in found} != {'add', 'sub', 'mul'} or any(flag != '1' for _, flag in found):
        problems.append(f'a branch and its delay slot: written as {found}, expected add, sub and mul, all with their flags')

    # every macro instruction once, each followed by a reader so that the flags are wanted, then once more in a row
    every = [special1(f) for f in range(0x30)] + [special2(i) for i in range(0x32) if i != 0x2B]   # 0x2B is no instruction
    words = []
    for word in every:
        words += [word, cfc2(T0, STATUS)]
    out, text = translate(work, 'every', words + every + END)
    untranslated = re.findall(r'stop\(c, [^;]*"(V\w+|Invalid)"', text)
    if untranslated:
        problems.append(f'every instruction: not translated: {sorted(set(untranslated))}')
    names = {name for name, _ in calls(text)}
    wanted = {f'{op}{form}' for op in ('add', 'sub', 'mul', 'madd', 'msub') for form in ('', 'bc', 'a', 'abc')} | {'opmula', 'opmsub'}
    if names != wanted:
        problems.append(f'every instruction: four-lane calls missing or unexpected: {sorted(names ^ wanted)}')
    for plain in ('max4', 'maxbc4', 'mini4', 'minibc4', 'abs4', 'move4', 'mr32_4'):
        if f'vu::{plain}(' not in text:
            problems.append(f'every instruction: no call of vu::{plain}')
    if sum(flag == '0' for _, flag in calls(text)) == 0:
        problems.append('every instruction: no flags were left out in the row without readers')
    problems += [f'every instruction: clang++: {e.split("error:")[-1].strip()}' for e in compile_errors(out)]

    # the switch back to the one-lane helpers
    out, text = translate(work, 'old', [VADD, VSUB, VMOVE, VMULAX] + END, env={'PS2_VU0_HELPERS': 'old'})
    if calls(text) or '4(' in text or not all(call in text for call in ('vu::add(', 'vu::sub(', 'vu::move(', 'vu::mulabc(')):
        problems.append('PS2_VU0_HELPERS=old: the one-lane calls were not written')
    problems += [f'PS2_VU0_HELPERS=old: clang++: {e.split("error:")[-1].strip()}' for e in compile_errors(out)]

for problem in problems:
    print('FAIL', problem)
print('ok' if not problems else f'FAILED: {len(problems)}', '' if compiler else '(no clang++ on the PATH: the compile checks were skipped)')
raise SystemExit(1 if problems else 0)
