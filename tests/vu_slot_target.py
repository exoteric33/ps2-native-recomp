#!/usr/bin/env python3
"""Compiled VU1 programs: a delay slot that another branch jumps to (#115).

The VU1 translator (recomp/vurecomp_gen.cpp) puts the check "was the branch before me taken?" right after each delay slot. A
pair that is a delay slot and also a branch target is reached two ways: after its branch, and by a jump from somewhere else.
Reached by the jump, the branch before it never ran, so its check must not fire. It does fire when the slot itself sets the
"branch pending" flag, that is when the slot holds a branch of its own (a BAL to a subroutine): the code then went to the
target of the branch before the slot instead of the subroutine.

No game data: the programs are made up. Each one is translated, compiled with clang++ and run; the answer it must give is
what the vector unit does (written next to each program). The checks:
  1. a BAL in a delay slot, reached by a jump: the subroutine runs (vi2 = 3, not 2);
  2. the same slot reached through its own branch: that branch has a branch in its delay slot, which is left to the
     interpreter at the branch (upstream c243416), and nothing compiled runs after it;
  3. a slot that reads Q (ADDq) and is a jump target, after a divide: it reads the new quotient both ways (vi2 = 1);
  4. a plain slot that is a jump target, reached both ways: the code after it runs once (vi3 = 1 / 2).

Run from the repository root: python tests/vu_slot_target.py [clang++]
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
static std::uint32_t iaddiu(unsigned it, unsigned is, unsigned imm) { return (8u << 25) | (it << 16) | (is << 11) | imm; }
static std::uint32_t branch(unsigned op, unsigned is, unsigned it, unsigned from, unsigned to) {
    return (op << 25) | (is << 11) | (it << 16) | (((to - from - 8u) / 8u) & 0x7FFu);
}
static std::uint32_t fmac(unsigned funct, unsigned dest, unsigned fd, unsigned fs, unsigned ft) { return (dest << 21) | (ft << 16) | (fs << 11) | (fd << 6) | funct; }
static std::uint32_t special(unsigned index, unsigned dest, unsigned ft, unsigned fs) { return (dest << 21) | (ft << 16) | (fs << 11) | ((index >> 2) << 6) | (0x3Cu | (index & 3u)); }
static std::uint32_t divide(unsigned fsf, unsigned fs, unsigned ftf, unsigned ft) { return (0x40u << 25) | (ftf << 23) | (fsf << 21) | (ft << 16) | (fs << 11) | (0xEu << 6) | 0x3C; }
static std::uint32_t mtir(unsigned it, unsigned fs) { return (0x40u << 25) | (it << 16) | (fs << 11) | (0xFu << 6) | 0x3C; }

int main(int, char **argv) {
    std::ofstream out(argv[1]);
    out << "#include \"ps2/vu_ops.hpp\"\n#include <cstdio>\n"
           "namespace ps2::vu { unsigned g_hops = 0, g_hop_pc = 0; void interpret(int, std::uint32_t pc) { ++g_hops; g_hop_pc = pc; } std::uint32_t entry() { return 0; } }\n"
           "using namespace ps2; using namespace ps2::vuops;\n";
    std::string table = "static void (*const kFns[])(Vu0Regs &, std::uint8_t *) = {\n";
    for (int test = 0; test < 3; ++test) {
        Program p{static_cast<std::uint64_t>(test + 1), 0, std::vector<std::uint8_t>(16384)};
        const auto put = [&](unsigned pc, std::uint32_t lo, std::uint32_t hi = unop) {
            std::memcpy(p.code.data() + pc, &lo, 4);
            std::memcpy(p.code.data() + pc + 4, &hi, 4);
        };
        for (unsigned pc = 0; pc < 256; pc += 8) put(pc, nop);
        if (test == 0) {
            // 0x00 IBEQ vi1, vi0 -> 0x18 (taken when vi1 = 0: a jump to the slot at 0x18)   0x08 slot
            // 0x10 IBNE vi1, vi1 -> 0x38 (never taken; with vi1 != 0 we fall into it)       0x18 its slot: BAL vi15 -> 0x48
            // 0x20 the BAL's slot   0x28 vi2 = 1, end   0x38 vi2 = 2, end (the target of the branch at 0x10)   0x48 vi2 = 3, end
            put(0x00, branch(0x28, 1, 0, 0x00, 0x18));
            put(0x10, branch(0x29, 1, 1, 0x10, 0x38));
            put(0x18, branch(0x21, 0, 15, 0x18, 0x48));
            put(0x28, iaddiu(2, 0, 1), unop | end);
            put(0x38, iaddiu(2, 0, 2), unop | end);
            put(0x48, iaddiu(2, 0, 3), unop | end);
        } else if (test == 1) {
            // Q = vf1.x / vf1.x = 1 (every vf starts at 2.0), then 0x30 IBEQ vi1, vi0 -> 0x58 (a jump into the slot of the branch at 0x50,
            // which is never taken), late enough for the quotient to be ready on both ways: ADDq.x vf6, vf0, Q; FTOI0 vf7, vf6;
            // MTIR vi2, vf7.x. The new quotient gives vi2 = 1.
            put(0x00, divide(0, 1, 0, 1));
            put(0x30, branch(0x28, 1, 0, 0x30, 0x58));
            put(0x50, branch(0x29, 1, 1, 0x50, 0x90));
            put(0x58, nop, fmac(0x20, 8, 6, 0, 0));
            put(0x60, nop, special(0x14, 15, 7, 6));
            put(0x78, mtir(2, 7), unop | end);
            put(0x90, iaddiu(2, 0, 9), unop | end);
        } else {
            // 0x00 IBEQ vi1, vi0 -> 0x18   0x10 IBNE vi1, vi1 -> 0x38 (never taken)   0x18 slot: vi3 += 1   0x20 vi3 += 1, end
            // vi1 = 0 jumps into the slot: vi3 = 2; vi1 != 0 runs the slot after its branch: vi3 = 2 as well.
            put(0x00, branch(0x28, 1, 0, 0x00, 0x18));
            put(0x10, branch(0x29, 1, 1, 0x10, 0x38));
            put(0x18, iaddiu(3, 3, 1));
            put(0x20, iaddiu(3, 3, 1), unop | end);
            put(0x38, iaddiu(3, 0, 7), unop | end);
        }
        std::string name;
        out << generate(p, name);
        if (name.empty()) { std::fprintf(stderr, "test %d: not translated\n", test); return 1; }
        table += "    " + name + ",\n";
    }
    out << table << "};\n" << R"(
int main() {
    struct Run { int fn; unsigned vi1, reg, want; const char *what; unsigned hop = 0; };
    const Run runs[] = {
        {0, 0, 2, 3, "a BAL in a delay slot, reached by a jump, calls its subroutine"},
        {0, 1, 2, 0, "the slot reached through its own branch (a branch in a delay slot) is left to the interpreter at that branch", 0x10},
        {1, 0, 2, 1, "a slot that reads Q, reached by a jump, sees the new quotient"},
        {1, 1, 2, 1, "the same slot reached through its branch (not taken) sees it too"},
        {2, 0, 3, 2, "a plain slot reached by a jump runs once"},
        {2, 1, 3, 2, "a plain slot reached through its branch runs once"},
    };
    int failed = 0;
    for (const Run &run : runs) {
        Vu0Regs r;
        std::memset(&r, 0, sizeof r);
        for (int i = 0; i < 32; ++i)
            for (int k = 0; k < 4; ++k) r.vf[i].w[k] = 0x40000000u;
        for (int k = 0; k < 4; ++k) r.vf[0].w[k] = k == 3 ? 0x3F800000u : 0u;
        r.vi[1] = static_cast<std::uint16_t>(run.vi1);
        static std::uint8_t mem[16384];
        vu::g_hops = 0;
        kFns[run.fn](r, mem);
        const bool ok = r.vi[run.reg] == run.want && (run.hop ? vu::g_hops == 1 && vu::g_hop_pc == run.hop : !vu::g_hops);
        failed += !ok;
        std::printf("%s %s: vi%u = %u, want %u\n", ok ? "ok  " : "FAIL", run.what, run.reg, r.vi[run.reg], run.want);
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
with (contextlib.nullcontext(os.environ['VU_KEEP']) if os.environ.get('VU_KEEP') else tempfile.TemporaryDirectory(prefix='vu-slot-')) as tmp:
    tmp = Path(tmp)
    tmp.mkdir(parents=True, exist_ok=True)
    (tmp / 'generator.cpp').write_text(GENERATOR)
    base = [cxx, '-std=c++20', '-w', '-I' + str(root), '-I' + str(root / 'runtime/include')]
    exe = '.exe' if sys.platform == 'win32' else ''
    subprocess.run(base + ['-O1', str(tmp / 'generator.cpp'), str(root / 'runtime/src/vu_jit.cpp'), '-o', str(tmp / ('generator' + exe))], check=True)
    subprocess.run([str(tmp / ('generator' + exe)), str(tmp / 'native.cpp')], check=True)
    subprocess.run(base + ['-O1', str(tmp / 'native.cpp'), '-o', str(tmp / ('native' + exe))], check=True)
    sys.exit(subprocess.run([str(tmp / ('native' + exe))]).returncode)
