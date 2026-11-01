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
GENERATOR = r'''
#include "recomp/vurecomp_gen.cpp"
#include "cases.h"
#include "tops.h"
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
    // Code wrap and 4 KiB data wrap, plus final flags with no microcode reader.
    add({{tc::sq(15, 1, 0, 256), tc::add(3, 1, 2) | tc::kEnd}, {tc::lq(15, 4, 0, 256), tc::kUnop}}, 4088);
    add({{tc::kLnop, tc::fmac(0x28, 0, 3, 1, 2) | tc::kEnd}, tc::kNop});
    // A cycle without an end bit must stop at the reference interpreter's budget.
    add({{tc::iaddiu(1, 1, 1), tc::kUnop}, {tc::branch(0x20, 0, 0, 8, 0), tc::kUnop}, tc::kNop});
    // Dynamic jumps retain the interpreter's pipeline; reject the whole entry.
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
std::uint32_t itop(int unit) { return test_itop[unit]; }
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
int main(int argc, char **argv) {
    register_generated_vu();
    check_tops();
    for (unsigned i = 0; i < 150; ++i) compare(tc::random_program(i), i);
    unsigned i = 0;
    for (const auto &c : tc::cases()) compare(c.pairs, ++i);
    compare({{tc::sq(15, 1, 0, 256), tc::add(3, 1, 2) | tc::kEnd}, {tc::lq(15, 4, 0, 256), tc::kUnop}}, 7, 4088);
    compare({{tc::kLnop, tc::fmac(0x28, 0, 3, 1, 2) | tc::kEnd}, tc::kNop}, 17);
    compare({{(0x24u << 25) | (1u << 11), tc::kUnop | tc::kEnd}, tc::kNop}, 17);
    compare({{tc::iaddiu(1, 1, 1), tc::kUnop}, {tc::branch(0x20, 0, 0, 8, 0), tc::kUnop}, tc::kNop}, 17);
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
    (tmp / 'generator.cpp').write_text(GENERATOR)
    (tmp / 'driver.cpp').write_text('#define RANDOM_PROGRAMS 150\n' + STUBS + INTERP + DRIVER)
    base = [CXX, '-std=c++20', '-O1', '-I.', '-Iruntime/include', '-Iruntime/src', '-I' + str(tmp)]
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
    assert len(recordings) == 2, 'only the two intentional fallback entries should be recorded; compiled entries must execute'
    assert all(p.stat().st_size == 4096 for p in recordings)
    run([str(tmp / 'check')], env=environment)
    assert sorted(record.glob('*.bin')) == recordings, 'recordings must be deduplicated'
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
