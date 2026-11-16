#!/usr/bin/env python3
"""A compiled VU1 program stands for its code at every address in the code memory (#112, Killzone).

The key of a VU1 program (ps2::vu::program_key_moved) does not hold the address of its code, so one piece of code that the game lays at many
places is one program, built once from the first copy that was recorded. The generated function finds how far the copy that runs is from
that one (`sh`, from vu::entry()) and adds it to every address it hands on. This test builds small programs that do hand addresses on, lays
each at several places (also across the end of the code memory, where the addresses wrap), and runs them twice through the real runtime
(runtime/src/vu_micro.cpp): once in the interpreter and once with the compiled program registered, which was made from the copy at the first
place only. Both runs must leave the same integer registers, TPC and data memory, and the compiled run must really have used the compiled
program (it makes no more calls to the interpreter than the program's own way out needs).

Also checked, in the generator's process: the key is the same for the same code at every place, differs when one word of it differs, and the
old key (program_key) differs between places, which is what this replaces.

Usage: python tests/vu_shift.py [c++ compiler]   (a C++20 compiler with std::format, as for scripts/test_vu_codegen.py)
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
#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

namespace sc {

using Pair = std::array<std::uint32_t, 2>;  // lower, upper
constexpr std::uint32_t kUnop = 0x000002FF, kLnop = 0x8000033C, kEnd = 0x40000000;
constexpr Pair kNop = {kLnop, kUnop};

inline std::uint32_t iaddiu(unsigned it, unsigned is, unsigned imm) { return (8u << 25) | (it << 16) | (is << 11) | ((imm & 0x7800u) << 10) | (imm & 0x7FFu); }
inline std::uint32_t ilw(unsigned dest, unsigned it, unsigned is, unsigned imm) { return (4u << 25) | (dest << 21) | (it << 16) | (is << 11) | (imm & 0x7FFu); }
// a branch from pair `from` to pair `to` (their numbers in the program); op 0x21 is BAL (it = the link register), 0x28 is IBEQ
inline std::uint32_t branch(unsigned op, unsigned is, unsigned it, int from, int to) { return (op << 25) | (is << 11) | (it << 16) | (static_cast<unsigned>(to - from - 1) & 0x7FFu); }
inline std::uint32_t jalr(unsigned it, unsigned is) { return (0x25u << 25) | (is << 11) | (it << 16); }

struct Case {
    const char *name;
    std::vector<Pair> pairs;
    int jump_to = -1;  // the pair a JALR goes to: its address, divided by 8, is put at the start of the data memory
};

inline std::vector<Case> cases() {
    std::vector<Case> c;
    // a call that is compiled all the way: BAL writes the return address (vi15), the callee ends the program (TPC after its delay slot)
    c.push_back({"call and end", {
        {iaddiu(1, 0, 3), kUnop},                       // 0
        {branch(0x21, 0, 15, 1, 4), kUnop},             // 1  BAL vi15, pair 4
        {iaddiu(2, 0, 7), kUnop},                       // 2  delay slot
        {iaddiu(3, 0, 9), kUnop},                       // 3  (the return point, not reached)
        {iaddiu(4, 0, 5), kUnop | kEnd},                // 4
        {iaddiu(5, 0, 6), kUnop},                       // 5  delay slot of the E bit
    }});
    // a call through a register: JALR writes the return address and the program goes on in the interpreter at the address in the register
    c.push_back({"register call", {
        {ilw(8, 1, 0, 0), kUnop},                       // 0  vi1 = the target / 8, from the data memory
        {kLnop, kUnop},                                 // 1
        {jalr(14, 1), kUnop},                           // 2  JALR vi14, vi1
        {iaddiu(2, 0, 7), kUnop},                       // 3  delay slot
        {iaddiu(3, 0, 9), kUnop},                       // 4  (the return point, not reached)
        {iaddiu(4, 0, 5), kUnop | kEnd},                // 5  the target
        {iaddiu(5, 0, 6), kUnop},                       // 6
    }, 5});
    // a branch with a branch in its delay slot: the compiled code gives the first of them to the interpreter, at its address
    c.push_back({"double branch", {
        {iaddiu(1, 0, 1), kUnop},                       // 0
        kNop,                                           // 1  (a VI written just before a branch reads it late: keep that out of this case)
        {branch(0x28, 1, 0, 2, 6), kUnop},              // 2  IBEQ vi1, vi0 -> 6 (not taken)
        {branch(0x28, 1, 0, 3, 7), kUnop},              // 3  IBEQ in its delay slot -> 7 (not taken)
        {iaddiu(7, 0, 4), kUnop},                       // 4
        {iaddiu(8, 0, 5), kUnop | kEnd},                // 5
        {iaddiu(9, 0, 6), kUnop},                       // 6
        {iaddiu(10, 0, 7), kUnop | kEnd},               // 7
        {iaddiu(11, 0, 8), kUnop},                      // 8
    }});
    // a conditional branch to the end: both ways leave a TPC (the E bit's, and the one after the branch's own delay slot)
    c.push_back({"branch and end", {
        {iaddiu(1, 0, 1), kUnop},                       // 0
        {branch(0x29, 1, 0, 1, 4), kUnop},              // 1  IBNE vi1, vi0 -> 4 (taken)
        {iaddiu(2, 0, 7), kUnop},                       // 2  delay slot
        {iaddiu(3, 0, 9), kUnop | kEnd},                // 3
        {iaddiu(4, 0, 5), kUnop | kEnd},                // 4
        {iaddiu(5, 0, 6), kUnop},                       // 5
    }});
    return c;
}

// where each case is laid: the first place is the one the compiled program is made from. The last crosses the end of the code memory.
constexpr std::uint32_t kPlaces[] = {0x0100, 0x2A18, 0x3FD8, 0x0028};

}  // namespace sc
'''

GENERATOR = r'''
#define main vurecomp_main
#include "recomp/vurecomp_gen.cpp"
#undef main
#include "cases.h"

static std::vector<std::uint8_t> image(const sc::Case &c, std::uint32_t place, std::uint32_t change_word_of_pair = ~0u) {
    std::vector<std::uint8_t> code(16384, 0);
    for (std::size_t i = 0; i < c.pairs.size(); ++i) {
        const std::uint32_t at = (place + static_cast<std::uint32_t>(i) * 8u) & 16383u;
        std::uint32_t lo = c.pairs[i][0], hi = c.pairs[i][1];
        if (i == change_word_of_pair) lo ^= 0x10u;
        std::memcpy(code.data() + at, &lo, 4);
        std::memcpy(code.data() + at + 4, &hi, 4);
    }
    return code;
}

int main(int argc, char **argv) {
    const fs::path out = argv[1];
    int failures = 0;
    // the table holds a function of the test's around each program, which counts the calls
    std::string bodies, wrappers, table = "extern const ps2::vu::Program kVuPrograms[];\nextern const unsigned kVuCount;\nconst ps2::vu::Program kVuPrograms[] = {\n";
    const auto cases = sc::cases();
    for (std::size_t n = 0; n < cases.size(); ++n) {
        const sc::Case &c = cases[n];
        std::uint64_t first = 0, moved_all_same = 1, old_differs = 0;
        for (std::size_t k = 0; k < std::size(sc::kPlaces); ++k) {
            const std::uint32_t place = sc::kPlaces[k];
            const auto code = image(c, place);
            const auto pcs = ps2::vu::reachable(code.data(), 16383u, place);
            const std::uint64_t key = ps2::vu::program_key_moved(code.data(), pcs, place, 16383u);
            if (k == 0) first = key;
            else {
                if (key != first) moved_all_same = 0;
                if (ps2::vu::program_key(code.data(), pcs, place) != ps2::vu::program_key(image(c, sc::kPlaces[0]).data(), ps2::vu::reachable(image(c, sc::kPlaces[0]).data(), 16383u, sc::kPlaces[0]), sc::kPlaces[0])) old_differs = 1;
            }
            if (key != vurecomp_key_of(code, place)) { std::printf("FAIL %s: key_of differs from program_key_moved\n", c.name); ++failures; }
        }
        // one word changed: another program
        const auto other = image(c, sc::kPlaces[1], 1);
        if (ps2::vu::program_key_moved(other.data(), ps2::vu::reachable(other.data(), 16383u, sc::kPlaces[1]), sc::kPlaces[1], 16383u) == first) { std::printf("FAIL %s: a changed word keeps the key\n", c.name); ++failures; }
        std::printf("%s %s: the key is the same at every place, the old key is not\n", moved_all_same && old_differs ? "ok  " : "FAIL", c.name);
        if (!moved_all_same || !old_differs) ++failures;
        // the program the compiled run uses is made from the first place only
        Program p{first, sc::kPlaces[0], image(c, sc::kPlaces[0]), 1};
        std::string name;
        const std::string body = generate(p, name);
        if (name.empty()) { std::printf("FAIL %s: not compiled\n", c.name); ++failures; continue; }
        if (body.find("sh)") == std::string::npos) { std::printf("FAIL %s: no shift in the generated code\n", c.name); ++failures; }
        bodies += body;
        wrappers += std::format("void w{0}(ps2::Vu0Regs &r, std::uint8_t *mem) {{ ++g_compiled_calls; {1}(r, mem); }}\n", n, name);
        table += std::format("    {{0x{:016X}ull, w{}, 1}},\n", first, n);
    }
    table += "};\nconst unsigned kVuCount = " + std::to_string(cases.size()) + ";\n";
    std::ofstream(out / "compiled.cpp") << "#include \"ps2/vu_ops.hpp\"\n#include <cstdio>\n\nnamespace gen {\n\nusing namespace ps2;\nusing namespace ps2::vuops;\n\n"
                                        << "unsigned long long g_compiled_calls = 0;\n\n" << bodies << wrappers << "\n" << table << "}\n";
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

namespace gen { extern const ps2::vu::Program kVuPrograms[]; extern const unsigned kVuCount; extern unsigned long long g_compiled_calls; }

using namespace ps2;

int main(int argc, char **argv) {
    const bool compiled = argc > 1 && std::string(argv[1]) == "compiled";
    if (compiled) vu::register_programs(gen::kVuPrograms, gen::kVuCount);
    const auto cases = sc::cases();
    for (std::size_t n = 0; n < cases.size(); ++n)
        for (std::uint32_t place : sc::kPlaces) {
            const sc::Case &c = cases[n];
            std::memset(g_vu1_code, 0, sizeof g_vu1_code);
            std::memset(g_vu1_mem, 0, sizeof g_vu1_mem);
            for (std::size_t i = 0; i < c.pairs.size(); ++i) {
                const std::uint32_t at = (place + static_cast<std::uint32_t>(i) * 8u) & 16383u;
                std::memcpy(g_vu1_code + at, &c.pairs[i][0], 4);
                std::memcpy(g_vu1_code + at + 4, &c.pairs[i][1], 4);
            }
            if (c.jump_to >= 0) {
                const std::uint32_t target = ((place + static_cast<std::uint32_t>(c.jump_to) * 8u) & 16383u) / 8u;
                std::memcpy(g_vu1_mem, &target, 4);
            }
            vu::code_changed(1);
            vu::regs(1) = Vu0Regs{};
            gen::g_compiled_calls = 0;
            vu::run(1, place);
            const Vu0Regs &r = vu::regs(1);
            std::printf("%s @%04X: tpc %04X vi", c.name, place, r.tpc);
            for (int i = 0; i < 16; ++i) std::printf(" %04X", r.vi[i]);
            std::printf(" compiled calls %llu\n", gen::g_compiled_calls);
        }
    return 0;
}
'''

STUBS = ('#include <atomic>\n#include <vector>\n#include "ps2/vu_live.hpp"\n'
         'namespace ps2 { std::uint64_t frame_count() { return 0; } bool halted() { return false; } '
         'namespace vif { std::uint32_t itop(int) { return 0; } void describe_vu1() {} } '
         'namespace vu::live { std::atomic<bool> g_ready{false}; bool active() { return false; } void note_call(std::uint64_t) {} '
         'std::vector<Program> take() { return {}; } void unknown(std::uint64_t, std::uint32_t, const std::uint8_t *) {} } }\n')


def run(*args, **kwargs):
    return subprocess.run(args, check=True, cwd=ROOT, **kwargs)


def compile_(*args):
    r = subprocess.run(args, cwd=ROOT, capture_output=True, text=True)
    if r.returncode:
        sys.exit(r.stderr or 'compile failed')


# VU_KEEP=<folder>: build in that folder and keep it (the generated programs, to look at a failure)
with (contextlib.nullcontext(os.environ['VU_KEEP']) if os.environ.get('VU_KEEP') else tempfile.TemporaryDirectory(prefix='vu-shift-')) as tmp:
    tmp = pathlib.Path(tmp)
    tmp.mkdir(parents=True, exist_ok=True)
    (tmp / 'cases.h').write_text(CASES_H)
    # key_of is the generator's own (recomp/vurecomp_gen.cpp); the test calls it through the name below
    (tmp / 'generator.cpp').write_text('#define vurecomp_key_of(code, place) ps2::vurecomp::key_of(1, code, place)\n' + GENERATOR)
    (tmp / 'run.cpp').write_text(STUBS + RUN)
    base = [CXX, '-std=c++20', '-I.', '-Iruntime/include', f'-I{tmp}']
    compile_(*base, '-O1', str(tmp / 'generator.cpp'), 'runtime/src/vu_jit.cpp', '-o', str(tmp / 'generator'))
    result = subprocess.run([str(tmp / 'generator'), str(tmp)], cwd=ROOT, capture_output=True, text=True)
    sys.stdout.write(result.stdout)
    if result.returncode:
        sys.exit('the keys or the generated programs are wrong')
    compile_(*base, '-O1', '-w', '-Iruntime/src', str(tmp / 'run.cpp'), str(tmp / 'compiled.cpp'), 'runtime/src/vu_micro.cpp', 'runtime/src/vu_jit_web.cpp', '-o', str(tmp / 'run'))
    reference = subprocess.run([str(tmp / 'run'), 'interpreter'], cwd=ROOT, capture_output=True, text=True, check=True).stdout.splitlines()
    compiled = subprocess.run([str(tmp / 'run'), 'compiled'], cwd=ROOT, capture_output=True, text=True, check=True).stdout.splitlines()
    if len(reference) != len(compiled) or not reference:
        sys.exit('the two runs printed a different number of lines')
    # the state the program ends with (everything before "compiled calls") must be the same, and the compiled run must have called the program
    # made from the first place once in every run, the interpreter run never
    bad = 0
    for r, c in zip(reference, compiled):
        state_r, calls_r = r.rsplit(' compiled calls ', 1)
        state_c, calls_c = c.rsplit(' compiled calls ', 1)
        if state_r != state_c:
            bad += 1
            print('FAIL the compiled program leaves another state')
            print('  interpreter: ' + state_r)
            print('  compiled:    ' + state_c)
        elif calls_r != '0' or calls_c != '1':
            bad += 1
            print('FAIL the compiled program was not used exactly once: ' + c)
    if bad:
        sys.exit(f'{bad} of {len(reference)} runs differ or did not use the compiled program')
    for line in compiled[:4]:
        print('  e.g. ' + line)
    print(f'{len(reference)} runs (4 programs at 4 places each, one of them across the end of the code memory): the same registers and TPC in the interpreter and in the program made from the first place, which was called in every one of them')
