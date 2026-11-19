#!/usr/bin/env python3
"""One VU1 key, one result: the Q decision looks only at code the program can reach.

A compiled VU1 program is found by a key over the instruction pairs its entry can reach (ps2::vu::program_key_moved). Whether an
instruction that reads Q sees a divide's new quotient (ps2::vu::q_read_kind) must then depend on those pairs only. It used to walk back
from the read without knowing the entry, and took the three pairs before a divide as the pipeline it starts in even when the program never
runs them. An EFU instruction there (one the program never executes) made the generator believe that a later WAITP waits for it, long
enough to see the new quotient. Two programs with the same key then compiled into different results, and whichever was compiled first
was used for both.

The fixture: the 16 KiB code memory is filled with NOP pairs; at the entry 0x40 SQRT Q, vf1.x; at 0x48 ADDq.x vf6, vf0, Q
| WAITP with the E bit. vf1.x = 4.0, the old Q = 0, the EFU is idle (no EFU instruction runs), so the console's WAITP does not wait and ADDq
reads the old quotient: vf6.x = 0.0 (tests/vu_waitp.py, case 4). Program B differs only in the lower word at 0x38 = ESIN P, vf0.x, which
neither program executes. A relocated copy of each sits at 0x2040.

Checked:
  - both programs (and the relocated copies) have the same key;
  - the generator's code for A and B is the same text;
  - in the real runtime (runtime/src/vu_micro.cpp) every image gives vf6.x = 0.0 and Q = 2.0 at the end: in the interpreter, and with the
    compiled program of A, of B, of both registered A first and B first (the cache insertion orders), each called once per run.

Usage: python tests/vu_qkey.py [c++ compiler]
"""
import contextlib
import os
import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
CXX = sys.argv[1] if len(sys.argv) > 1 else 'clang++'

CASES_H = r'''
#pragma once
#include <cstdint>
#include <cstring>

namespace qk {

constexpr std::uint32_t kLnop = 0x8000033C, kUnop = 0x000002FF;
constexpr std::uint32_t kSqrt = 0x800103BD;              // SQRT Q, vf1.x
constexpr std::uint32_t kAddqLo = 0x800007BF;            // WAITP
constexpr std::uint32_t kAddqHi = 0x410001A0;            // ADDq.x vf6, vf0, Q with the E bit
constexpr std::uint32_t kEsin = 0x80000FFC;              // ESIN P, vf0.x (never executed)
constexpr std::uint32_t kPlaces[] = {0x0040, 0x2040};

// variant 0: NOP before the entry, 1: ESIN before the entry
inline void fill(std::uint8_t *code, int variant, std::uint32_t place) {
    for (std::uint32_t pc = 0; pc < 16384; pc += 8) {
        std::memcpy(code + pc, &kLnop, 4);
        std::memcpy(code + pc + 4, &kUnop, 4);
    }
    std::memcpy(code + place, &kSqrt, 4);
    std::memcpy(code + place + 8, &kAddqLo, 4);
    std::memcpy(code + place + 12, &kAddqHi, 4);
    if (variant) std::memcpy(code + ((place - 8u) & 16383u), &kEsin, 4);
}

}  // namespace qk
'''

GENERATOR = r'''
#define main vurecomp_main
#include "recomp/vurecomp_gen.cpp"
#undef main
#include "cases.h"

int main(int, char **argv) {
    const fs::path out = argv[1];
    int failures = 0;
    std::uint64_t keys[2][2];
    std::string bodies[2], names[2];
    for (int v = 0; v < 2; ++v)
        for (int k = 0; k < 2; ++k) {
            std::vector<std::uint8_t> code(16384);
            qk::fill(code.data(), v, qk::kPlaces[k]);
            keys[v][k] = ps2::vurecomp::key_of(1, code, qk::kPlaces[k]);
            if (k == 0) {
                Program p{keys[v][k], qk::kPlaces[k], code, 1};
                bodies[v] = generate(p, names[v]);
                if (names[v].empty()) { std::printf("FAIL variant %d: not compiled\n", v); ++failures; }
            }
        }
    const bool same_key = keys[0][0] == keys[1][0] && keys[0][0] == keys[0][1] && keys[0][0] == keys[1][1];
    std::printf("%s the four images have one key: %016llX\n", same_key ? "ok  " : "FAIL", static_cast<unsigned long long>(keys[0][0]));
    failures += !same_key;
    const bool same_code = bodies[0] == bodies[1];
    std::printf("%s the generator makes the same code for both\n", same_code ? "ok  " : "FAIL");
    failures += !same_code;
    std::string text = "#include \"ps2/vu_ops.hpp\"\n#include <cstdio>\n\nnamespace gen {\n\nusing namespace ps2;\nusing namespace ps2::vuops;\n\n";
    // the two programs under one key would clash as names; give each its own
    for (int v = 0; v < 2; ++v) {
        std::string b = bodies[v];
        for (std::size_t at = b.find(names[v]); at != std::string::npos; at = b.find(names[v], at + 1)) b.replace(at, names[v].size(), names[v] + "_v" + std::to_string(v));
        text += b;
    }
    text += "unsigned long long g_compiled_calls = 0;\n";
    for (int v = 0; v < 2; ++v) text += std::format("void w{0}(ps2::Vu0Regs &r, std::uint8_t *mem) {{ ++g_compiled_calls; {1}_v{0}(r, mem); }}\n", v, names[v]);
    // tables: A only, B only, A then B, B then A
    const int orders[4][2] = {{0, -1}, {1, -1}, {0, 1}, {1, 0}};
    for (int t = 0; t < 4; ++t) {
        text += std::format("extern const ps2::vu::Program kTable{}[];\nconst ps2::vu::Program kTable{}[] = {{\n", t, t);
        for (int v : orders[t])
            if (v >= 0) text += std::format("    {{0x{:016X}ull, w{}, 1}},\n", keys[0][0], v);
        text += "};\n";
    }
    text += "}\n";
    std::ofstream(out / "compiled.cpp") << text;
    return failures ? 1 : 0;
}
'''

RUN = r'''
#include "cases.h"
#include "hw.hpp"
#include "gs/gif.hpp"
#include "gs/queue.hpp"
#include "ps2/system.hpp"
#include "ps2/vu_ops.hpp"
#include "vu_micro.hpp"
#include <cstdio>
#include <string>

namespace ps2 {
alignas(64) std::uint8_t g_vu0_code[4096], g_vu0_mem[4096], g_vu1_code[16384], g_vu1_mem[16384];
Cpu g_cpu;
Options g_options;
namespace gif { void transfer(int, const std::uint8_t *, std::uint32_t) {} void end_runaway_kick(std::uint32_t) {} }
namespace gs::queue {
Ring &vu() { static Ring ring; return ring; }
Role &role() { static Role r = Role::Emulation; return r; }
bool vu_active() { return false; }
void Ring::post(Type, std::uint32_t, std::uint32_t, const void *, std::uint32_t) {}
}
}

namespace gen {
extern const ps2::vu::Program kTable0[], kTable1[], kTable2[], kTable3[];
extern unsigned long long g_compiled_calls;
}

using namespace ps2;

int main(int argc, char **argv) {
    const std::string mode = argc > 1 ? argv[1] : "interpreter";
    if (mode == "A") vu::register_programs(gen::kTable0, 1);
    if (mode == "B") vu::register_programs(gen::kTable1, 1);
    if (mode == "AB") vu::register_programs(gen::kTable2, 2);
    if (mode == "BA") vu::register_programs(gen::kTable3, 2);
    for (int v = 0; v < 2; ++v)
        for (std::uint32_t place : qk::kPlaces) {
            qk::fill(g_vu1_code, v, place);
            std::memset(g_vu1_mem, 0, sizeof g_vu1_mem);
            vu::code_changed(1);
            Vu0Regs &r = vu::regs(1);
            r = Vu0Regs{};
            r.vf[0].w[3] = 0x3F800000u;
            r.vf[1].w[0] = 0x40800000u;  // 4.0
            gen::g_compiled_calls = 0;
            vu::run(1, place);
            std::printf("%s @%04X: vf6.x %08X q %08X calls %llu\n", v ? "ESIN" : "NOP ", place, r.vf[6].w[0], r.q, gen::g_compiled_calls);
        }
    return 0;
}
'''

STUBS = ('#include <atomic>\n#include <vector>\n#include "ps2/vu_live.hpp"\n'
         'namespace ps2 { std::uint64_t frame_count() { return 0; } bool halted() { return false; } '
         'namespace vif { std::uint32_t itop(int) { return 0; } void describe_vu1() {} } '
         'namespace vu::live { std::atomic<bool> g_ready{false}; bool active() { return false; } void note_call(std::uint64_t) {} '
         'std::vector<Program> take() { return {}; } void unknown(std::uint64_t, std::uint32_t, const std::uint8_t *) {} } }\n')


def compile_(*args):
    r = subprocess.run(args, cwd=ROOT, capture_output=True, text=True)
    if r.returncode:
        sys.exit(r.stderr or 'compile failed')


# VU_KEEP=<folder>: build in that folder and keep it
with (contextlib.nullcontext(os.environ['VU_KEEP']) if os.environ.get('VU_KEEP') else tempfile.TemporaryDirectory(prefix='vu-q-')) as tmp:
    tmp = pathlib.Path(tmp)
    tmp.mkdir(parents=True, exist_ok=True)
    (tmp / 'cases.h').write_text(CASES_H)
    (tmp / 'generator.cpp').write_text(GENERATOR)
    (tmp / 'run.cpp').write_text(STUBS + RUN)
    base = [CXX, '-std=c++20', '-I.', '-Iruntime/include', f'-I{tmp}']
    compile_(*base, '-O1', str(tmp / 'generator.cpp'), 'runtime/src/vu_jit.cpp', '-o', str(tmp / 'generator'))
    result = subprocess.run([str(tmp / 'generator'), str(tmp)], cwd=ROOT, capture_output=True, text=True)
    sys.stdout.write(result.stdout)
    bad = result.returncode != 0
    compile_(*base, '-O1', '-w', '-Iruntime/src', str(tmp / 'run.cpp'), str(tmp / 'compiled.cpp'), 'runtime/src/vu_micro.cpp', 'runtime/src/vu_jit_web.cpp', '-o', str(tmp / 'run'))
    # the console: WAITP with an idle EFU does not wait, ADDq reads the old quotient (0.0); the SQRT's 2.0 is in Q when the program ends
    for mode in ('interpreter', 'A', 'B', 'AB', 'BA'):
        lines = subprocess.run([str(tmp / 'run'), mode], cwd=ROOT, capture_output=True, text=True, check=True).stdout.splitlines()
        for line in lines:
            want_calls = '0' if mode == 'interpreter' else '1'
            ok = 'vf6.x 00000000 ' in line and 'q 40000000 ' in line and line.endswith('calls ' + want_calls)
            bad |= not ok
            print(f'{"ok  " if ok else "FAIL"} {mode:11} {line}')
    if bad:
        sys.exit('FAILED: a program reads another quotient than the console (want vf6.x 00000000, q 40000000)')
    print('all passed: both programs, at both places, compiled from either and interpreted, read the old quotient as the console does')
