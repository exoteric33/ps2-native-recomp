#!/usr/bin/env python3
"""Exact VU0 differential tests, using the shared synthetic VU instruction fixtures.

No disc or graphics dependency. Runs real generated code through the runtime's
unit registry, cache, fallback and full-state verifier, including code replacement.
"""
import ast
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
CXX = os.environ.get('CXX', 'clang++')
# Read fixture literals without importing the executable VU1/Wasm test driver.
module = ast.parse((ROOT / 'scripts/test_vu_codegen.py').read_text())
fixtures = {n.targets[0].id: ast.literal_eval(n.value) for n in module.body
            if isinstance(n, ast.Assign) and isinstance(n.value, ast.Constant)
            and isinstance(n.value.value, str)}
TOP_CASES = r'''
#pragma once
#include "cases.h"
inline std::vector<std::vector<tc::Pair>> top_cases() {
    using namespace tc;
    const auto xtop = [](unsigned vi) { return lower_special(0x68, 0, vi, 0); };
    const auto xitop = [](unsigned vi) { return lower_special(0x69, 0, vi, 0); };
    return {
        finish({{xtop(7), kUnop}, {xitop(8), kUnop}, {xtop(0), kUnop}, {xitop(0), kUnop}}),
        finish({{xitop(1), kUnop}, {xtop(2), kUnop}, {lq(15, 3, 1, 0), kUnop},
                {sq(15, 4, 1, 1), kUnop}, {lq(15, 5, 2, 0), kUnop}, {sq(15, 6, 2, 1), kUnop}}),
        finish({{xitop(1), kUnop}, kNop, {branch(0x28, 1, 0, 16, 40), kUnop}, kNop,
                {iaddiu(2, 0, 11), kUnop}, {iaddiu(3, 0, 22), kUnop}}),
    };
}
'''
CALLS_H = r'''
#pragma once
#include "cases.h"
// VU0 programs that call subroutines through a register (vurecomp 13, vu0_calls): a loop that picks one of two subroutines by a
// branch, calls it with JALR, and goes on after the return; the subroutines follow the program's end, each ends in a JR. The pairs
// around the call write vector results, flags and integer registers that the other side of the call reads (a stall, a flag read and
// a branch with the integer branch delay right at a subroutine's start, results of a subroutine read right after the return).
// bad = 1: the second target is a pair inside a subroutine, not its start (the compiled program does not know it: it has to put the
// state back and run the whole program in the interpreter). bad = 2: as 0, the driver changes a subroutine's code afterwards.
namespace callprog {
using namespace tc;
inline std::vector<Pair> program(unsigned seed, unsigned bad, unsigned &poke) {
    std::mt19937 g(seed * 2246822519u + 99u);
    const auto R = [&](unsigned n) { return static_cast<unsigned>(g() % n); };
    // nothing here writes vi12 (the loop counter), vi14 or vi15 (the links), or uses the divide unit
    const auto up = [&]() -> Word {
        const unsigned c = R(10), dest = 1 + R(15), fd = 1 + R(6), fs = R(7), ft = R(7);
        if (c < 2) return kUnop;
        if (c < 6) { static const unsigned f[] = {0x28, 0x2C, 0x2A, 0x29, 0x2D}; return fmac(f[R(5)], dest, fd, fs, ft); }
        if (c < 8) return fmac(R(4), dest, fd, fs, ft);  // ADDbc
        return clip(fs, ft);
    };
    // FCAND writes vi1, the target register: not between the choice of the target and the call (`keep`)
    const auto low = [&](bool keep = false) -> Word {
        const unsigned c = R(14), res = 7 + R(5), v = 2 + R(5), w = 2 + R(5), f = R(7), dest = 1 + R(15);
        switch (c) {
        case 0: case 1: return kLnop;
        case 2: return lq(dest, 1 + R(6), 0, R(40));
        case 3: return sq(dest, f, 0, R(40));
        case 4: return ialu(R(2) ? 0x30 : 0x31, res, v, w);
        case 5: return iaddi(v, w, static_cast<int>(R(16)) - 8);
        case 6: return fmflag(0x1A, res, v);
        case 7: return fsflag(0x16, res, R(4096));
        case 8: return fcget(res);
        case 9: return ilw(15, res, 0, R(40));
        case 10: return lower_special(0x30, dest, 1 + R(6), f);
        case 11: return keep ? kLnop : fcflag(0x12, R(1u << 24));
        case 12: return fsflag(0x15, 0, R(4096));
        default: return fmflag(0x18, res, v);
        }
    };
    const auto pair = [&](bool keep = false) -> Pair { return {low(keep), up()}; };
    std::vector<Pair> p;
    for (unsigned v = 2; v <= 6; ++v) p.push_back({iaddiu(v, 0, R(4)), up()});
    p.push_back({iaddiu(12, 0, 2 + R(3)), up()});
    const unsigned top = static_cast<unsigned>(p.size());
    for (unsigned i = 0, n = 1 + R(3); i < n; ++i) p.push_back(pair());
    const unsigned set_a = static_cast<unsigned>(p.size());
    p.push_back({0, up()});  // iaddiu vi1, vi0, A (filled in below)
    const unsigned pick = static_cast<unsigned>(p.size());
    p.push_back({branch(0x28, 3, 4, pick * 8u, (pick + 3u) * 8u), up()});
    p.push_back(pair(true));
    const unsigned set_b = static_cast<unsigned>(p.size());
    p.push_back({0, up()});  // iaddiu vi1, vi0, B
    for (unsigned i = 0, n = R(2); i < n; ++i) p.push_back(pair(true));
    const unsigned link = R(3) ? 15u : 14u;
    p.push_back({(0x25u << 25) | (link << 16) | (1u << 11), up()});  // JALR link, vi1
    // the delay slot: often a write of the register the subroutine's first branch reads
    p.push_back(R(2) ? Pair{iaddiu(5, 0, R(3)), up()} : pair());
    for (unsigned i = 0, n = 1 + R(3); i < n; ++i) p.push_back(pair());
    p.push_back({iaddi(12, 12, -1), up()});
    p.push_back({branch(0x29, 12, 0, static_cast<unsigned>(p.size()) * 8u, top * 8u), up()});
    p.push_back(pair());
    p.push_back({fsflag(0x16, 13, 0xFFF), up()});
    p.push_back({fcget(11), up()});
    p.push_back({fmflag(0x1A, 10, 1), up()});
    p.push_back(pair());
    p.back()[1] |= kEnd;
    p.push_back(pair());
    std::vector<unsigned> starts;
    for (int s = 0; s < 2; ++s) {
        starts.push_back(static_cast<unsigned>(p.size()));
        if (R(2)) {
            const unsigned at = static_cast<unsigned>(p.size());
            p.push_back({branch(0x28, 5, 6, at * 8u, (at + 3u) * 8u), up()});
            p.push_back(pair());
            p.push_back(pair());
        }
        for (unsigned i = 0, n = 2 + R(4); i < n; ++i) p.push_back(pair());
        p.push_back({(0x24u << 25) | (link << 11), up()});  // JR link
        p.push_back(pair());
    }
    const unsigned a = starts[0], b = bad == 1 ? starts[1] + 1u : starts[1];
    p[set_a][0] = iaddiu(1, 0, a);
    p[set_b][0] = iaddiu(1, 0, b);
    poke = starts[1] + 1u;
    return p;
}
}
'''
CALL_PROGRAMS = 240
GENERATOR = r'''
#include "recomp/vurecomp_gen.cpp"
#include "cases.h"
#include "tops.h"
#include "calls.h"
int main(int argc, char **argv) {
    std::vector<ps2::vurecomp::Input> inputs;
    const auto add = [&](const std::vector<tc::Pair> &pairs, unsigned entry = 0, int unit = 0) {
        const unsigned mask = unit ? 16383u : 4095u;
        ps2::vurecomp::Input p{0, entry, std::vector<std::uint8_t>(mask + 1), unit};
        for (unsigned i = 0; i < pairs.size(); ++i) std::memcpy(p.code.data() + ((entry + i * 8) & mask), pairs[i].data(), 8);
        p.key = ps2::vu::program_key(p.code.data(), ps2::vu::reachable(p.code.data(), mask, entry), entry);
        inputs.push_back(p);
    };
    for (int unit : {0, 1}) for (const auto &pairs : top_cases()) add(pairs, 0, unit);
    for (const auto &c : tc::cases()) add(c.pairs);
    for (unsigned i = 0; i < 150; ++i) add(tc::random_program(i));
    // Programs that call subroutines through a register: every one must compile (with both subroutines found in its code).
    for (unsigned i = 0; i < CALL_PROGRAMS; ++i) {
        unsigned poke = 0;
        const auto pairs = callprog::program(i, i % 4 == 2 ? 1u : 0u, poke);
        add(pairs);
        Program p{inputs.back().key, 0, inputs.back().code, 0};
        std::string cname;
        const std::string text = generate(p, cname);
        if (text.empty()) { std::fprintf(stderr, "call program %u was not compiled\n", i); return 8; }
        if (text.find("vu::vu0_key(") == std::string::npos || text.find("goto bail;") == std::string::npos) { std::fprintf(stderr, "call program %u has no checked call\n", i); return 8; }
    }
    // Code wrap and 4 KiB data wrap, plus final flags with no microcode reader.
    add({{tc::sq(15, 1, 0, 256), tc::add(3, 1, 2) | tc::kEnd}, {tc::lq(15, 4, 0, 256), tc::kUnop}}, 4088);
    add({{tc::kLnop, tc::fmac(0x28, 0, 3, 1, 2) | tc::kEnd}, tc::kNop});
    // A cycle without an end bit must stop at the reference interpreter's budget.
    add({{tc::iaddiu(1, 1, 1), tc::kUnop}, {tc::branch(0x20, 0, 0, 8, 0), tc::kUnop}, tc::kNop});
    // The rules VU1 got from the original project (c243416, b7371e8) hold for compiled VU0 too: FSSET takes effect after the upper
    // instruction of its pair (the code must contain the late flag write), and a branch with a branch in its delay slot is left to the
    // interpreter (the whole VU0 program is refused, at the entry and further in).
    unsigned fsset_cases = 0;
    for (const auto &c : tc::cases()) {
        if (std::string(c.name).rfind("FSSET", 0) != 0) continue;
        ++fsset_cases;
        Program p{1000u + fsset_cases, 0, std::vector<std::uint8_t>(4096), 0};
        for (unsigned i = 0; i < c.pairs.size(); ++i) std::memcpy(p.code.data() + i * 8, c.pairs[i].data(), 8);
        std::string fname;
        const std::string text = generate(p, fname);
        if (text.find("force_mac(") == std::string::npos) { std::fprintf(stderr, "VU0 FSSET case '%s' has no late flag write\n", c.name); return 6; }
    }
    if (fsset_cases < 2) { std::fprintf(stderr, "VU0 FSSET cases not found\n"); return 6; }
    for (const bool at_entry : {true, false}) {
        Program p{900u + at_entry, 0, std::vector<std::uint8_t>(4096), 0};
        const unsigned at = at_entry ? 0 : 16;
        const auto put = [&](unsigned pc, std::uint32_t lo, std::uint32_t hi = tc::kUnop) {
            std::memcpy(p.code.data() + pc, &lo, 4);
            std::memcpy(p.code.data() + pc + 4, &hi, 4);
        };
        for (unsigned pc = 0; pc < 128; pc += 8) put(pc, tc::kLnop);
        put(at, tc::branch(0x28, 1, 0, at, 64));
        put(at + 8, tc::branch(0x28, 1, 0, at + 8, 72));
        put(at + 16, tc::kLnop, tc::kUnop | tc::kEnd);
        put(64, tc::kLnop, tc::kUnop | tc::kEnd);
        put(72, tc::kLnop, tc::kUnop | tc::kEnd);
        p.key = ps2::vu::program_key(p.code.data(), ps2::vu::reachable(p.code.data(), 4095u, 0), 0);
        std::string dname;
        if (!generate(p, dname).empty()) { std::fprintf(stderr, "VU0 double branch at %u was compiled\n", at); return 7; }
        const auto result = ps2::vurecomp::generate_files({ps2::vurecomp::Input{p.key, 0, p.code, 0}}, fs::path(argv[1]) / ("double" + std::to_string(at)), 0);
        if (result.programs) { std::fprintf(stderr, "VU0 double branch at %u was written out\n", at); return 7; }
    }
    // A jump through a register with no call (JR alone): left to the interpreter, the whole entry.
    Program jump{1, 0, std::vector<std::uint8_t>(4096), 0};
    const tc::Pair pair{(0x24u << 25) | (1u << 11), tc::kUnop | tc::kEnd};
    std::memcpy(jump.code.data(), pair.data(), 8);
    std::string name;
    if (!generate(jump, name).empty() || !name.empty()) return 2;
    const auto result = ps2::vurecomp::generate_files(inputs, argv[1], 0);
    if (result.skipped) return 1;
    auto invalid_unit = inputs.front(); invalid_unit.unit = 2;
    auto invalid_entry = inputs.front(); invalid_entry.entry = 4096;
    auto unaligned = inputs.front(); unaligned.entry = 1;
    auto truncated = inputs.front(); truncated.code.pop_back();
    if (ps2::vurecomp::generate_files({invalid_unit, invalid_entry, unaligned, truncated}, fs::path(argv[1]) / "invalid", 0).programs) return 3;
    if (ps2::vurecomp::generate_files({inputs.front()}, fs::path(argv[1]) / "live-vu0", 1).programs) return 4;
    return 0;
}
'''
# Shared interpreter and stubs; replace its main with a full-state differential driver.
INTERP = fixtures['INTERP'].split('int main(int argc, char **argv) {')[0]
STUBS = r'''
#include <atomic>
#include "ps2/vu_live.hpp"
namespace ps2 {
std::uint64_t frame_count() { return 0; } bool test_halted = false; bool halted() { return test_halted; }
namespace vif {
std::uint32_t test_itop[2] = {};
std::uint32_t itop(int unit) { return test_itop[unit]; } void describe_vu1() {}
}
namespace vu::live {
std::atomic<bool> g_ready{false}; bool active() { return false; }
void note_call(std::uint64_t) {} std::vector<Program> take() { return {}; }
void unknown(std::uint64_t, std::uint32_t, const std::uint8_t *) {}
}}
'''
DRIVER = r'''
#include <cstddef>
#include "tops.h"
#include "calls.h"
namespace ps2::vu { extern std::uint64_t g_vu0_restarts, g_vu0_compiled; }
void register_generated_vu();
static unsigned padding_calls = 0;
static void padding_only(Vu0Regs &r, std::uint8_t *) {
    ++padding_calls;
    ps2::vu::interpret(0, 0);
    const std::size_t end = offsetof(Vu0Regs, tpc) + sizeof r.tpc;
    std::memset(reinterpret_cast<std::uint8_t *>(&r) + end, 0xA5, sizeof r - end);
}
static unsigned calls = 0, equivalent_calls = 0;
static void equivalent(Vu0Regs &, std::uint8_t *) { ++equivalent_calls; ps2::vu::interpret(0, 0); }
static void wrong(Vu0Regs &r, std::uint8_t *) { ++calls; r.vi[7] = 0xBAD; }
static void compare(const std::vector<tc::Pair> &pairs, unsigned seed, unsigned entry = 0,
                    int unit = 0, bool replace_code = true) {
    const unsigned size = unit ? 16384u : 4096u;
    auto *code = unit ? g_vu1_code : g_vu0_code;
    auto *data = unit ? g_vu1_mem : g_vu0_mem;
    if (replace_code) {
        std::memset(code, 0, size);
        for (unsigned i = 0; i < pairs.size(); ++i) std::memcpy(code + ((entry + i * 8) & (size - 1)), pairs[i].data(), 8);
        vu::code_changed(unit);
    }
    auto &r = vu::regs(unit);
    std::memset(data, 0, size);
    tc::random_init(r, data, seed);
    const auto before = r;
    const std::vector<std::uint8_t> memory(data, data + size);
    vu::run(unit, entry);
    const auto actual = r;
    const std::vector<std::uint8_t> after(data, data + size);
    r = before; std::memcpy(data, memory.data(), size);
    // interpret() bypasses compiled dispatch; VU0 uses the exact reference.
    vu::interpret(unit, entry);
    if (std::memcmp(&actual, &r, sizeof r) || std::memcmp(after.data(), data, size)) {
        std::fprintf(stderr, "VU%d differential mismatch seed %u entry %u tpc %u/%u\n", unit, seed, entry, actual.tpc, r.tpc);
        std::fputs(tc::describe(actual, after.data()).c_str(), stderr);
        std::fputs(tc::describe(r, data).c_str(), stderr);
        std::exit(1);
    }
}
static void check_tops() {
    for (int unit : {0, 1}) for (const auto &pairs : top_cases()) {
        bool replace_code = true;
        for (unsigned itop : {0u, 1u, 0x2Bu, 0xFFu, 0x100u, 0x3FFu}) {
            vif::test_itop[0] = itop;
            const unsigned vu1_itop = (itop + 0x2AAu) & 0x3FFu;
            vif::test_itop[1] = (vu1_itop + 1u) & 0x3FFu;  // also differs from VU1's latched value
            vu::set_tops((itop + 0x155u) & 0x3FFu, vu1_itop);
            // Keep the program and its cached entry while the VIF values change.
            compare(pairs, itop, 0, unit, replace_code);
            replace_code = false;
        }
    }
    vif::test_itop[0] = vif::test_itop[1] = 0;
    vu::set_tops(0, 0);
    std::puts("TOP/ITOP unit isolation, cached value updates, branches and addressed memory passed");
}
// The call programs (calls.h), compiled against the interpreter: known targets run compiled to the end, an unknown target or a
// subroutine whose code changed after the build puts the state back and runs the interpreter, and the result is the same every time.
static int check_calls() {
    std::uint64_t clean = 0, restarted = 0, changed = 0;
    for (unsigned i = 0; i < CALL_PROGRAMS; ++i) {
        const unsigned bad = i % 4 == 2 ? 1u : i % 4 == 3 ? 2u : 0u;
        unsigned poke = 0;
        const auto pairs = callprog::program(i, bad == 1 ? 1u : 0u, poke);
        for (unsigned init = 0; init < 4; ++init) {
            const std::uint64_t restarts = vu::g_vu0_restarts, compiled = vu::g_vu0_compiled;
            compare(pairs, i * 4u + init);
            if (vu::g_vu0_compiled == compiled) { std::fprintf(stderr, "call program %u did not run compiled\n", i); return 9; }
            if (bad == 1) restarted += vu::g_vu0_restarts - restarts;
            else if (vu::g_vu0_restarts != restarts) { std::fprintf(stderr, "call program %u went to the interpreter\n", i); return 9; }
            else ++clean;
            if (bad == 2) {
                auto other = pairs;
                other[poke][1] = other[poke][1] == tc::add(1, 2, 3) ? tc::add(4, 2, 3) : tc::add(1, 2, 3);
                const std::uint64_t before = vu::g_vu0_restarts;
                compare(other, i * 4u + init);
                changed += vu::g_vu0_restarts - before;
            }
        }
    }
    if (!restarted || !changed) { std::fprintf(stderr, "the way back to the interpreter was not taken (%llu, %llu)\n", (unsigned long long)restarted, (unsigned long long)changed); return 9; }
    std::printf("VU0 calls: %llu runs compiled to the end, %llu unknown targets and %llu changed subroutines through the interpreter, all exact\n",
                (unsigned long long)clean, (unsigned long long)restarted, (unsigned long long)changed);
    return 0;
}
int main(int argc, char **argv) {
    register_generated_vu();
    if (argc > 1 && std::string(argv[1]) == "calls") return check_calls();
    check_tops();
    for (unsigned i = 0; i < 150; ++i) compare(tc::random_program(i), i);
    unsigned i = 0;
    for (const auto &c : tc::cases()) compare(c.pairs, ++i);
    compare({{tc::sq(15, 1, 0, 256), tc::add(3, 1, 2) | tc::kEnd}, {tc::lq(15, 4, 0, 256), tc::kUnop}}, 7, 4088);
    compare({{tc::kLnop, tc::fmac(0x28, 0, 3, 1, 2) | tc::kEnd}, tc::kNop}, 17);
    compare({{(0x24u << 25) | (1u << 11), tc::kUnop | tc::kEnd}, tc::kNop}, 17);
    compare({{tc::iaddiu(1, 1, 1), tc::kUnop}, {tc::branch(0x20, 0, 0, 8, 0), tc::kUnop}, tc::kNop}, 17);
    // A branch in the delay slot of a branch: the interpreter runs it (compiled or not, the result must be the interpreter's).
    compare({{tc::branch(0x28, 1, 0, 0, 32), tc::kUnop}, {tc::branch(0x28, 1, 0, 8, 40), tc::kUnop}, {tc::kLnop, tc::kUnop | tc::kEnd},
             tc::kNop, {tc::kLnop, tc::kUnop | tc::kEnd}, {tc::kLnop, tc::kUnop | tc::kEnd}}, 23);
    test_halted = true;
    compare(tc::random_program(0), 0);
    test_halted = false;
    // Unknown code and a known code replacement at the same entry.
    compare({{tc::kLnop, tc::kUnop | tc::kEnd}, tc::kNop}, 17);
    compare({{tc::kLnop, tc::kUnop | tc::kEnd}, tc::kNop}, 17);
    compare(tc::random_program(0), 0);
    const auto key = vu::program_key(g_vu0_code, vu::reachable(g_vu0_code, 4095, 0), 0);
    const bool force_interpret = argc > 1 && std::string(argv[1]) == "force-interpret";
    const vu::Program counted{key, equivalent, 0}; vu::register_programs(&counted, 1);
    compare(tc::random_program(0), 0);
    if (equivalent_calls != (force_interpret ? 0u : 1u)) return 5;
    // Identical key in VU1 must not replace VU0's exact function.
    const vu::Program collision{key, wrong, 1}; vu::register_programs(&collision, 1);
    compare(tc::random_program(0), 0);
    if (calls) return 2;
    if (argc > 1 && std::string(argv[1]) == "negative-control") {
        const vu::Program padded{key, padding_only, 0}; vu::register_programs(&padded, 1);
        compare(tc::random_program(0), 0);
        compare(tc::random_program(0), 0);
        if (padding_calls != 2) return 4;  // ABI padding must not blacklist a correct function.
        const vu::Program bad{key, wrong, 0}; vu::register_programs(&bad, 1);
        compare(tc::random_program(0), 0); // verifier rejects and keeps exact result
        compare(tc::random_program(0), 0); // blacklisted, never called again
        if (calls != 1) return 3;
    }
    std::puts("VU0 exact full-state, wrap, registry, invalidation and fallback checks passed");
}
'''

def run(args, **kw):
    result = subprocess.run(args, cwd=ROOT, text=True, capture_output=True, **kw)
    if result.returncode:
        sys.exit(result.stdout + result.stderr)
    return result

with tempfile.TemporaryDirectory(prefix='vu0-codegen-') as tmp:
    tmp = Path(tmp)
    (tmp / 'cases.h').write_text(fixtures['CASES_H'])
    (tmp / 'tops.h').write_text(TOP_CASES)
    (tmp / 'calls.h').write_text(CALLS_H)
    (tmp / 'generator.cpp').write_text(GENERATOR)
    (tmp / 'driver.cpp').write_text('#define RANDOM_PROGRAMS 150\n' + STUBS + INTERP + DRIVER)
    base = [CXX, '-std=c++20', '-O1', '-I.', '-Iruntime/include', '-Iruntime/src', '-I' + str(tmp), '-DCALL_PROGRAMS=%d' % CALL_PROGRAMS]
    run(base + [str(tmp / 'generator.cpp'), '-o', str(tmp / 'generator')])
    run([str(tmp / 'generator'), str(tmp)])
    run(base + [str(tmp / 'driver.cpp'), 'runtime/src/vu_micro.cpp', 'runtime/src/vu_jit_web.cpp']
        + [str(p) for p in sorted(tmp.glob('vu1_*.cpp'))] + ['-o', str(tmp / 'check')])
    record = tmp / 'recorded-vu0'
    environment = {k: v for k, v in os.environ.items() if k not in ('PS2_VU0_INTERPRET', 'PS2_VU0_VERIFY', 'PS2_VU0_RECORD', 'PS2_VU0_STATS')}
    environment['PS2_VU0_RECORD'] = str(record)
    normal = run([str(tmp / 'check')], env=environment)
    print(normal.stdout, end='')
    recordings = sorted(record.glob('*.bin'))
    assert len(recordings) == 3, 'only the three intentional fallback entries (the jump, the cycle, the double branch) should be recorded; compiled entries must execute'
    assert all(p.stat().st_size == 4096 for p in recordings)
    run([str(tmp / 'check')], env=environment)
    assert sorted(record.glob('*.bin')) == recordings, 'recordings must be deduplicated'
    # The call programs: compiled against the interpreter. Only the targets of calls are recorded (the programs themselves are compiled).
    call_record = tmp / 'recorded-calls'
    called = run([str(tmp / 'check'), 'calls'], env={**environment, 'PS2_VU0_RECORD': str(call_record)})
    print(called.stdout, end='')
    assert all(not p.stem.endswith('_0000') for p in call_record.glob('*.bin')), 'a compiled call program was recorded'
    assert any(call_record.glob('*.bin')), 'the targets of calls are recorded'
    forced = run([str(tmp / 'check'), 'force-interpret'], env={**environment, 'PS2_VU0_INTERPRET': '1', 'PS2_VU0_RECORD': str(tmp / 'forced-recordings')})
    print('Forced interpretation override leaves native functions uncalled')
    checked = run([str(tmp / 'check'), 'negative-control'], env={**environment, 'PS2_VU0_VERIFY': '1', 'PS2_VU0_RECORD': str(tmp / 'verified-recordings')})
    if checked.stderr.count('mismatch:') != 1:
        sys.exit('verifier must report exactly the injected mismatch:\n' + checked.stderr)
    print(checked.stdout, end='')
    print('Verifier negative control: one injected mismatch detected and blacklisted')

    # Exercise the public CLI with synthetic recordings, including equal keys in
    # both units. Every path is temporary and independent of any game layout.
    run(base + ['recomp/vurecomp.cpp', 'recomp/vurecomp_gen.cpp', '-o', str(tmp / 'vurecomp')])
    cli = str(tmp / 'vurecomp')
    zero = next(p for p in recordings if p.read_bytes()[:4] == bytes.fromhex('3c030080'))
    # The end-bit NOP entry has the same reachable bytes in both memory sizes.
    one_dir = tmp / 'recorded-vu1'
    one_dir.mkdir()
    (one_dir / zero.name).write_bytes(zero.read_bytes() + bytes(12288))
    run([cli, '-', str(tmp / 'only-zero'), '--vu0-dir', str(record)])
    key = zero.stem.split('_')[0]
    assert 'vu0_' + key in (tmp / 'only-zero' / 'vu1_table.cpp').read_text()
    run([cli, str(one_dir), str(tmp / 'mixed'), '--vu0-dir', str(record)])
    table = (tmp / 'mixed' / 'vu1_table.cpp').read_text()
    assert 'vu0_' + key in table and 'vu1_' + key in table
    run([cli, str(one_dir), str(tmp / 'legacy')])
    assert 'vu0_' not in (tmp / 'legacy' / 'vu1_table.cpp').read_text()
    for args in [['-', str(tmp / 'bad')], [str(one_dir), str(tmp / 'bad'), '--unknown', str(record)],
                 ['-', str(tmp / 'bad'), '--vu0-dir', str(tmp / 'missing')]]:
        assert subprocess.run([cli] + args, cwd=ROOT, capture_output=True).returncode != 0
    (record / 'invalid.bin').write_bytes(bytes(4096))
    (record / (key + '_0001.bin')).write_bytes(zero.read_bytes())
    (record / (key + '_0008.bin')).write_bytes(zero.read_bytes()[:-1])
    (record / (key + '_0010.bin')).write_bytes(zero.read_bytes() + b'\0')
    malformed = run([cli, '-', str(tmp / 'malformed'), '--vu0-dir', str(record)])
    assert 'invalid recording name' in malformed.stderr
    assert malformed.stderr.count('recording must contain 4096 bytes') == 2
    assert 'vu0_' + key in (tmp / 'malformed' / 'vu1_table.cpp').read_text()
    print('CLI: VU0-only, mixed-unit key isolation, legacy VU1, arguments and malformed recordings passed')
