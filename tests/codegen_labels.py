#!/usr/bin/env python3
"""Regression (issue #55): the translator wrote a goto to a label that its unit does not have.

A branch whose delay slot is also a jump target skips that slot with a goto to the instruction after it, so that instruction
needs a label. The labels were collected in one pass, so a slot that became a target only later in the pass (a branch further
down, in the delay slot of another branch, going back to it) did not give that instruction its label, and the C++ compiler
stopped the build of the whole game with "use of undeclared label". No game data is used: the inputs are made up, one of them
a relocatable module of the SNR2 kind (the kind of module of Cold Winter's DNAS library).

Every check is the same: each `goto L_x;` of a generated file has its `L_x:`, and (with clang++ on the PATH) the file compiles.
A last part feeds random programs to the translator and holds it to the same.
"""
from pathlib import Path
import argparse
import random
import re
import shutil
import struct
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--recompiler', type=Path, default=root / 'build/tools/eerecomp')
parser.add_argument('--random', type=int, default=400, help='how many random programs (0: none)')
args = parser.parse_args()
compiler = shutil.which('clang++')   # clang only, like scripts/cxx.sh: the runtime headers use clang builtins that GCC lacks
BASE = 0x100000

# --- instruction words
NOP = 0
JR_RA = 0x03E00008
def addiu(reg): return 0x24000001 | reg << 21 | reg << 16
def beq(rs, rt, off): return 0x10000000 | rs << 21 | rt << 16 | off & 0xFFFF
def bne(rs, rt, off): return 0x14000000 | rs << 21 | rt << 16 | off & 0xFFFF
def rel(frm, to): return (to - (frm + 4)) // 4            # branch offset in words

def elf(words):
    data = bytearray(0x100 + 4 * len(words))
    data[:7] = b'\x7fELF\x01\x01\x01'
    struct.pack_into('<HHIIIIIHHHHHH', data, 16, 2, 8, 1, BASE, 52, 0, 0, 52, 32, 1, 40, 0, 0)
    struct.pack_into('<8I', data, 52, 1, 0x100, BASE, 0, 4 * len(words), 4 * len(words), 5, 4)
    struct.pack_into(f'<{len(words)}I', data, 0x100, *words)
    return bytes(data)

def snr2(words, name=b'DNAS.IRX'):
    """A relocatable module the way scripts read it: 0x40 bytes of header, the code, then the name. The code starts at 0x40."""
    code = 0x40
    name_at = code + 4 * len(words)
    size = name_at + len(name) + 1
    size = (size + 3) & ~3
    data = bytearray(size)
    struct.pack_into('<4s', data, 0, b'SNR2')
    struct.pack_into('<I', data, 0x14, name_at)     # the name, which the code ends at
    struct.pack_into('<I', data, 0x18, code)        # the entry point
    struct.pack_into('<I', data, 0x2C, size)
    struct.pack_into(f'<{len(words)}I', data, code, *words)
    data[name_at:name_at + len(name)] = name
    return bytes(data)

def run(*argv):
    return subprocess.run([str(args.recompiler.resolve()), *map(str, argv)], capture_output=True, text=True)

def broken_labels(out):
    """The goto targets of the generated units that the file has no label for."""
    missing = []
    for unit in sorted(Path(out).glob('*unit*.cpp')):
        text = unit.read_text()
        labels = set(re.findall(r'^(L_[0-9A-F]+):', text, re.M))
        for target in sorted(set(re.findall(r'goto (L_[0-9A-F]+);', text)) - labels):
            missing.append(f'{unit.name}: goto {target}')
    return missing

def compile_check(out):
    """The compiler's errors for the generated units (none when there is no clang++ to ask)."""
    if not compiler:
        return []
    units = sorted(Path(out).glob('*.cpp'))
    result = subprocess.run([compiler, '-std=c++20', '-fsyntax-only', '-I' + str(root / 'runtime/include'), '-I' + str(out), *map(str, units)],
                            capture_output=True, text=True)
    return [line for line in result.stderr.splitlines() if 'error' in line] if result.returncode else []

# The shape: A is a branch, its delay slot S is where the branch Q in the delay slot of P goes back to.
#   A:  beq  t0, zero, done
#   S:  addiu t1                 <- the slot of A, and the target of Q
#   N:  addiu t2                 <- needs a label: A skips S with a goto to it
#   P:  beq  t0, t1, done
#   Q:  bne  t0, t1, S           <- a branch in a delay slot
#       jr ra / nop
#   done: jr ra / nop
def shape(origin):
    at = lambda i: origin + 4 * i
    return [
        beq(8, 0, rel(at(0), at(8))),
        addiu(9),
        addiu(10),
        beq(8, 9, rel(at(3), at(8))),
        bne(8, 9, rel(at(4), at(1))),
        JR_RA, NOP,
        NOP,
        JR_RA, NOP,
    ]

with tempfile.TemporaryDirectory(prefix='ps2-labels-test-') as temp:
    work = Path(temp)
    problems = []

    # 1. the made-up module, next to a main program that only returns
    (work / 'main.elf').write_bytes(elf([JR_RA, NOP]))
    (work / 'modules').mkdir()
    (work / 'modules' / 'DNAS.bin').write_bytes(snr2(shape(0x40)))
    result = run(work / 'main.elf', '--modules', work / 'modules', '--out', work / 'module-out')
    assert result.returncode == 0, result.stderr
    assert '1 modules' in result.stdout, result.stdout
    assert list((work / 'module-out').glob('m_DNAS_unit_*.cpp')), 'the module was not translated'
    problems += [f'module: {m}' for m in broken_labels(work / 'module-out')]
    problems += [f'module: clang++: {e.split("error:")[-1].strip()}' for e in compile_check(work / 'module-out')]

    # 2. the same shape in a plain program: it is not a matter of modules
    (work / 'shape.elf').write_bytes(elf(shape(BASE)))
    result = run(work / 'shape.elf', '--out', work / 'plain-out')
    assert result.returncode == 0, result.stderr
    problems += [f'program: {m}' for m in broken_labels(work / 'plain-out')]
    problems += [f'program: clang++: {e.split("error:")[-1].strip()}' for e in compile_check(work / 'plain-out')]

    # 3. random programs (branches of every kind, jumps, calls, syscalls, much of it in delay slots). A program that the translator
    # refuses (an exception, e.g. "no entry for ..." for a call from a delay slot) is not what is tested here.
    rng = random.Random(55)
    checked = 0
    for case in range(args.random):
        n = rng.randint(8, 40)
        words = []
        for i in range(n):
            k, t = rng.random(), rng.randrange(n)
            off, jt = rel(i, t) & 0xFFFF, ((BASE + 4 * t) >> 2) & 0x3FFFFFF
            words.append(
                addiu(8 + rng.randrange(4)) if k < 0.30 else
                0x10000000 | rng.choice([8, 9, 0]) << 21 | rng.choice([0, 9]) << 16 | off if k < 0.38 else   # beq
                0x14000000 | rng.choice([8, 9]) << 21 | off if k < 0.44 else                                # bne
                0x50000000 | rng.choice([8, 9]) << 21 | off if k < 0.48 else                                # beql
                0x1C000000 | 8 << 21 | off if k < 0.50 else                                                 # bgtz
                0x04000000 | 8 << 21 | off if k < 0.52 else                                                 # bltz
                0x45010000 | off if k < 0.54 else                                                           # bc1t
                0x45000000 | off if k < 0.56 else                                                           # bc1f
                0x08000000 | jt if k < 0.61 else                                                            # j
                0x0C000000 | jt if k < 0.67 else                                                            # jal
                JR_RA if k < 0.71 else
                0x01000008 if k < 0.73 else                                                                 # jr t0
                0x0000000C if k < 0.75 else                                                                 # syscall
                0x0100F809 if k < 0.77 else                                                                 # jalr t0
                NOP)
        (work / 'random.elf').write_bytes(elf(words))
        out = work / 'random-out'
        shutil.rmtree(out, ignore_errors=True)
        if run(work / 'random.elf', '--out', out).returncode:
            continue
        checked += 1
        problems += [f'random program {case}: {m} (words {" ".join(f"{w:08x}" for w in words)})' for m in broken_labels(out)]
        if len(problems) > 5:
            break

    if problems:
        raise SystemExit('goto to a label that does not exist:\n  ' + '\n  '.join(problems[:6]))
print(f'Labels regression passed ({checked} random programs translated)')
