#!/usr/bin/env python3
"""Compiled VU1 programs: WAITP and a second EFU instruction wait for the EFU (public PR #119).

A WAITP stalls its whole instruction pair until the elementary function unit (EFU) has finished, and so does a new EFU
instruction while the last one still runs. An upper instruction in that pair issues late, so it can see a quotient that a
divide started just before has delivered by then. Digital Devil Saga sizes the icons above its enemies that way:
SQRT Q, ESQRT P, then ADDq.x | WAITP.

No game data: the programs are made up. Each one is translated, compiled with clang++ and run; every vf starts at 4.0,
so SQRT gives Q = 2 while the old Q is 0. The checks (vi2 = the integer ADDq got):
  1. SQRT, ESQRT, ADDq | WAITP: the WAITP waits 11 cycles for ESQRT, the quotient (7 cycles) is there: vi2 = 2;
  2. SQRT, ESQRT, ADDq | NOP: nothing waits, ADDq reads the old quotient: vi2 = 0;
  3. SQRT, ESQRT, ADDq | ESIN: the second EFU instruction waits for the first: vi2 = 2;
  4. SQRT, ADDq | WAITP with no EFU instruction before: nothing to wait for, the old quotient: vi2 = 0.

Run from the repository root: python tests/vu_waitp.py [clang++]
"""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
cxx = sys.argv[1] if len(sys.argv) > 1 else shutil.which('clang++')
if not cxx:
    sys.exit('no clang++ on the PATH')

GENERATOR = r'''
#define main vurecomp_main
#include "recomp/vurecomp_gen.cpp"
#undef main
#include <cstdio>

constexpr std::uint32_t nop = 0x8000033C, unop = 0x000002FF, end = 0x40000000;
static std::uint32_t fmac(unsigned funct, unsigned dest, unsigned fd, unsigned fs, unsigned ft) { return (dest << 21) | (ft << 16) | (fs << 11) | (fd << 6) | funct; }
static std::uint32_t special(unsigned index, unsigned dest, unsigned ft, unsigned fs) { return (dest << 21) | (ft << 16) | (fs << 11) | ((index >> 2) << 6) | (0x3Cu | (index & 3u)); }
// Lower instructions of the 0x40 group: SQRT Q = sqrt(vf[ft].ftf), EFU ones on vf[fs].fsf, WAITP, MTIR.
static std::uint32_t sqrt_q(unsigned ftf, unsigned ft) { return (0x40u << 25) | (ftf << 23) | (ft << 16) | (0xEu << 6) | 0x3D; }
static std::uint32_t efu(unsigned index, unsigned fsf, unsigned fs) { return (0x40u << 25) | (fsf << 21) | (fs << 11) | ((index >> 2) << 6) | (0x3Cu | (index & 3u)); }
static std::uint32_t waitp() { return (0x40u << 25) | (0x1Eu << 6) | 0x3F; }
static std::uint32_t mtir(unsigned it, unsigned fs) { return (0x40u << 25) | (it << 16) | (fs << 11) | (0xFu << 6) | 0x3C; }
constexpr unsigned kEsqrt = 0x78, kEsin = 0x7C;

int main(int, char **argv) {
    std::ofstream out(argv[1]);
    out << "#include \"ps2/vu_ops.hpp\"\n#include <cstdio>\n"
           "namespace ps2::vu { unsigned g_hops = 0; void interpret(int, std::uint32_t) { ++g_hops; } std::uint32_t entry() { return 0; } }\n"
           "using namespace ps2; using namespace ps2::vuops;\n";
    std::string table = "static void (*const kFns[])(Vu0Regs &, std::uint8_t *) = {\n";
    for (int test = 0; test < 4; ++test) {
        Program p{static_cast<std::uint64_t>(test + 1), 0, std::vector<std::uint8_t>(16384)};
        const auto put = [&](unsigned pc, std::uint32_t lo, std::uint32_t hi = unop) {
            std::memcpy(p.code.data() + pc, &lo, 4);
            std::memcpy(p.code.data() + pc + 4, &hi, 4);
        };
        for (unsigned pc = 0; pc < 256; pc += 8) put(pc, nop);
        // 0x00 SQRT Q, vf1.x   0x08 ESQRT P, vf1.x (or NOP)   0x10 ADDq.x vf6, vf0, Q | lower   0x18 FTOI0 vf7, vf6   0x40 MTIR vi2, vf7.x, end
        const std::uint32_t lower[] = {waitp(), nop, efu(kEsin, 0, 1), waitp()};
        put(0x00, sqrt_q(0, 1));
        if (test != 3) put(0x08, efu(kEsqrt, 0, 1));
        put(0x10, lower[test], fmac(0x20, 8, 6, 0, 0));
        put(0x18, nop, special(0x14, 15, 7, 6));
        put(0x40, mtir(2, 7), unop | end);
        std::string name;
        out << generate(p, name);
        if (name.empty()) { std::fprintf(stderr, "test %d: not translated\n", test); return 1; }
        table += "    " + name + ",\n";
    }
    out << table << "};\n" << R"(
int main() {
    struct Run { int fn; unsigned want; const char *what; };
    const Run runs[] = {
        {0, 2, "SQRT, ESQRT, ADDq | WAITP: the pair waits for the EFU and reads the new quotient"},
        {1, 0, "SQRT, ESQRT, ADDq | NOP: nothing waits, the old quotient"},
        {2, 2, "SQRT, ESQRT, ADDq | ESIN: the second EFU instruction waits for the first, the new quotient"},
        {3, 0, "SQRT, ADDq | WAITP with the EFU idle: no wait, the old quotient"},
    };
    int failed = 0;
    for (const Run &run : runs) {
        Vu0Regs r;
        std::memset(&r, 0, sizeof r);
        for (int i = 0; i < 32; ++i)
            for (int k = 0; k < 4; ++k) r.vf[i].w[k] = 0x40800000u;
        for (int k = 0; k < 4; ++k) r.vf[0].w[k] = k == 3 ? 0x3F800000u : 0u;
        static std::uint8_t mem[16384];
        vu::g_hops = 0;
        kFns[run.fn](r, mem);
        const bool ok = r.vi[2] == run.want && !vu::g_hops;
        failed += !ok;
        std::printf("%s %s: vi2 = %u, want %u\n", ok ? "ok  " : "FAIL", run.what, r.vi[2], run.want);
    }
    std::printf("%s\n", failed ? "FAILED" : "all passed");
    return failed ? 1 : 0;
}
)";
    return 0;
}
'''

import contextlib, os
# VU_KEEP=<folder>: build there and keep the generated C++
with (contextlib.nullcontext(os.environ['VU_KEEP']) if os.environ.get('VU_KEEP') else tempfile.TemporaryDirectory(prefix='vu-waitp-')) as tmp:
    tmp = Path(tmp)
    tmp.mkdir(parents=True, exist_ok=True)
    (tmp / 'generator.cpp').write_text(GENERATOR)
    base = [cxx, '-std=c++20', '-w', '-I' + str(root), '-I' + str(root / 'runtime/include')]
    exe = '.exe' if sys.platform == 'win32' else ''
    subprocess.run(base + ['-O1', str(tmp / 'generator.cpp'), str(root / 'runtime/src/vu_jit.cpp'), '-o', str(tmp / ('generator' + exe))], check=True)
    subprocess.run([str(tmp / ('generator' + exe)), str(tmp / 'native.cpp')], check=True)
    subprocess.run(base + ['-O1', str(tmp / 'native.cpp'), '-o', str(tmp / ('native' + exe))], check=True)
    sys.exit(subprocess.run([str(tmp / ('native' + exe))]).returncode)
