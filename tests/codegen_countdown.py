#!/usr/bin/env python3
"""Delay loops in the translator's output: whole turns are skipped only where nothing can tell.

A delay loop is a branch back over one `addiu rX, rX, -1` and nops, with a nop in its delay slot, that loops while rX is
not zero (games wait in them; God of War spins 2000 turns between two looks at a counter). The translator writes a line
in front of the branch that does the turns that end before the budget runs out at once (recomp/codegen.cpp,
countdown_register). This test feeds the translator made-up programs (no game data):

  * the line is written for such loops (bne rX, $0 and bne $0, rX, with and without nops), and not for anything else:
    a second instruction in the loop, a decrement of another register or by another amount, a delay slot that is not a
    nop, beq, a label inside the loop, a forward branch; PS2_EE_COUNTDOWN=0 never writes it;
  * the loop runs the same with and without the line (with clang++ on the PATH): the translated unit is compiled with a
    small stand-in for the runtime whose preempt() records the counter and the budget every time it is called, refills
    the budget by a made-up amount and sometimes asks to unwind (the caller then enters the loop again, as the
    dispatcher does). Many start values of the counter and the budget, both builds: the same calls with the same values,
    the same end.
"""
from pathlib import Path
import argparse
import os
import shutil
import struct
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--recompiler', type=Path, default=root / 'build/tools/eerecomp')
args = parser.parse_args()
compiler = shutil.which('clang++')
BASE = 0x100000

NOP = 0
JR_RA = 0x03E00008
V0, V1 = 2, 3
def addiu(rt, rs, imm): return 0x24000000 | rs << 21 | rt << 16 | imm & 0xFFFF
def bne(rs, rt, off): return 0x14000000 | rs << 21 | rt << 16 | off & 0xFFFF
def beq(rs, rt, off): return 0x10000000 | rs << 21 | rt << 16 | off & 0xFFFF
def lw(rt, base): return 0x8C000000 | base << 21 | rt << 16
DEC = addiu(V0, V0, -1)
END = [JR_RA, NOP]

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

def skips(text): return text.count('const std::uint32_t q = ')

# name, the program, whether the line is written
cases = [
    ('bne v0, $0 over the decrement alone', [DEC, bne(V0, 0, -2), NOP] + END, 1),
    ('with nops in the loop', [DEC, NOP, NOP, NOP, bne(V0, 0, -5), NOP] + END, 1),
    ('nops in front of the decrement', [NOP, DEC, NOP, bne(0, V0, -4), NOP] + END, 1),
    ('two decrements', [DEC, DEC, bne(V0, 0, -3), NOP] + END, 0),
    ('another instruction in the loop', [DEC, lw(V1, 4), bne(V0, 0, -3), NOP] + END, 0),
    ('the decrement is of another register', [addiu(V1, V1, -1), bne(V0, 0, -2), NOP] + END, 0),
    ('the decrement is from another register', [addiu(V0, V1, -1), bne(V0, 0, -2), NOP] + END, 0),
    ('a step of -2', [addiu(V0, V0, -2), bne(V0, 0, -2), NOP] + END, 0),
    ('a delay slot that is not a nop', [DEC, bne(V0, 0, -2), addiu(V1, V1, 1)] + END, 0),
    ('beq', [DEC, beq(V0, 0, -2), NOP] + END, 0),
    ('bne against another register', [DEC, bne(V0, V1, -2), NOP] + END, 0),
    # the branch at the end goes to the nop between, so the code can be entered inside the loop
    ('a label inside the loop', [DEC, NOP, bne(V0, 0, -3), NOP, beq(0, 0, -4), NOP] + END, 0),
]

HARNESS = r'''
#include "units.hpp"
#include <cstdio>
#include <cstdlib>
namespace ps2 {
static std::uint64_t g_seed = 1;
static std::uint64_t next() { g_seed = g_seed * 6364136223846793005ull + 1442695040888963407ull; return g_seed >> 33; }
static std::uint64_t g_hash = 1469598103934665603ull, g_calls = 0;
static void mix(std::uint64_t v) { g_hash = (g_hash ^ v) * 1099511628211ull; }
bool preempt(Cpu &c) {
    ++g_calls;
    mix(c.r[2].d[0]); mix(static_cast<std::uint32_t>(c.budget));
    const std::uint64_t kind = next() % 4;
    c.budget = static_cast<std::int32_t>(kind == 0 ? next() % 8 : kind == 1 ? next() % 40 : next() % 30000);
    return next() % 5 == 0;
}
void bad_entry(Cpu &, std::uint32_t) { std::abort(); }
}
static ps2::Cpu g_cpu;
int main() {
    using namespace ps2;
    Cpu &c = g_cpu;
    for (int trial = 0; trial < 6000; ++trial) {
        const std::uint64_t kind = next() % 6;
        const std::uint32_t n = kind == 0 ? static_cast<std::uint32_t>(1 + next() % 3) : kind == 1 ? static_cast<std::uint32_t>(next() % 64)
                              : kind == 2 ? static_cast<std::uint32_t>(next() % 5000) : static_cast<std::uint32_t>(next() % 60000);
        // n = 0 would loop 2^32 times; the skip handles it like any other (n - 1 is the largest count), the old loop takes seconds
        c.r[2].sd[0] = static_cast<std::int32_t>(n);
        c.r[2].d[1] = next();
        c.budget = static_cast<std::int32_t>(next() % 3 == 0 ? -static_cast<std::int64_t>(next() % 20) : static_cast<std::int64_t>(next() % 40000));
        c.pc = 0;
        for (int round = 0;; ++round) {
            gen::unit_0000(c, 1u, 0u);
            if (!(c.pc & kUnwind)) break;
        }
        mix(c.r[2].d[0]); mix(c.r[2].d[1]); mix(static_cast<std::uint32_t>(c.budget)); mix(c.pc);
    }
    std::printf("%016llx %llu\n", static_cast<unsigned long long>(g_hash), static_cast<unsigned long long>(g_calls));
}
'''

def run_loop(work, name, out):
    (out / 'harness.cpp').write_text(HARNESS)
    exe = work / f'{name}-run'
    result = subprocess.run([compiler, '-std=gnu++20', '-O2', '-I' + str(root / 'runtime/include'), '-I' + str(out), str(out / 'unit_0000.cpp'),
                             str(out / 'harness.cpp'), '-o', str(exe)], capture_output=True, text=True)
    if result.returncode:
        return None, [line for line in result.stderr.splitlines() if 'error' in line][:3]
    return subprocess.run([str(exe)], capture_output=True, text=True, timeout=600).stdout.strip(), []

with tempfile.TemporaryDirectory(prefix='ps2-countdown-test-') as temp:
    work = Path(temp)
    problems = []
    for number, (name, words, expected) in enumerate(cases):
        _, text = translate(work, f'case{number}', words)
        if skips(text) != expected:
            problems.append(f'{name}: {skips(text)} skip lines written, expected {expected}')
        _, text = translate(work, f'off{number}', words, env={'PS2_EE_COUNTDOWN': '0'})
        if skips(text):
            problems.append(f'{name}: PS2_EE_COUNTDOWN=0 still writes the skip line')

    if compiler:
        for name, words in (('decrement alone', [DEC, bne(V0, 0, -2), NOP] + END), ('with nops', [NOP, DEC, NOP, NOP, bne(0, V0, -4), NOP] + END)):
            new, _ = translate(work, 'run-new', words)
            old, _ = translate(work, 'run-old', words, env={'PS2_EE_COUNTDOWN': '0'})
            got, errors_new = run_loop(work, 'new', new)
            want, errors_old = run_loop(work, 'old', old)
            problems += [f'{name}: does not compile: {e}' for e in errors_new + errors_old]
            if got is not None and want is not None:
                print(f'{name}: with the skip {got}, without {want} (hash of every preempt call and every end, number of calls)')
                if got != want:
                    problems.append(f'{name}: the loop runs differently with the skip line')

for problem in problems:
    print('FAIL', problem)
print('ok' if not problems else f'FAILED: {len(problems)}', '' if compiler else '(no clang++ on the PATH: the run checks were skipped)')
raise SystemExit(1 if problems else 0)
