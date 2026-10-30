#!/usr/bin/env python3
"""Exercise native and Wasm VU code generation without a game disc.

Requires a C++20 compiler with std::format and Node with Wasm SIMD/tail calls.
Run from any directory: python3 scripts/test_vu_codegen.py [c++ compiler]
With VU_BENCH=1 in the environment it also times the compiled random programs.

Besides the branch and exit regressions, it runs the pipeline stall cases (CLIP, an instruction that has to wait for an
FMAC result, FCGET three instructions after CLIP, and the same without the wait) through the interpreter of
runtime/src/vu_micro.cpp, the compiled programs of recomp/vurecomp_gen.cpp and the Wasm translator, and compares the
interpreters with the compiled programs on random microprograms.
"""
import concurrent.futures
import os
import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
CXX = sys.argv[1] if len(sys.argv) > 1 else 'clang++'
RANDOM_PROGRAMS = 150

CASES_H = r'''
#pragma once
#include "ps2/vu_ops.hpp"
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

namespace tc {

using Word = std::uint32_t;
using Pair = std::array<Word, 2>;  // lower, upper
constexpr Word kUnop = 0x000002FF, kLnop = 0x8000033C, kEnd = 0x40000000, kLoi = 0x80000000;
constexpr Pair kNop = {kLnop, kUnop};

inline Word fmac(unsigned funct, unsigned dest, unsigned fd, unsigned fs, unsigned ft) { return (dest << 21) | (ft << 16) | (fs << 11) | (fd << 6) | funct; }
inline Word special(unsigned index, unsigned dest, unsigned ft, unsigned fs) { return (dest << 21) | (ft << 16) | (fs << 11) | ((index >> 2) << 6) | (0x3Cu | (index & 3u)); }
inline Word lower_special(unsigned index, unsigned dest, unsigned ft, unsigned fs) { return (0x40u << 25) | special(index, dest, ft, fs); }
inline Word add(unsigned fd, unsigned fs, unsigned ft) { return fmac(0x28, 15, fd, fs, ft); }
inline Word clip(unsigned fs, unsigned ft) { return special(0x1F, 14, ft, fs); }
inline Word fcget(unsigned it) { return (0x1Cu << 25) | (it << 16); }
inline Word fmflag(unsigned op, unsigned it, unsigned is) { return (op << 25) | (it << 16) | (is << 11); }
inline Word fsflag(unsigned op, unsigned it, unsigned imm12) { return (op << 25) | (it << 16) | ((imm12 & 0x800u) << 10) | (imm12 & 0x7FFu); }
inline Word fcflag(unsigned op, unsigned imm24) { return (op << 25) | (imm24 & 0xFFFFFFu); }
inline Word iaddiu(unsigned it, unsigned is, unsigned imm) { return (8u << 25) | (it << 16) | (is << 11) | ((imm & 0x7800u) << 10) | (imm & 0x7FFu); }
inline Word ilw(unsigned dest, unsigned it, unsigned is, unsigned imm) { return (4u << 25) | (dest << 21) | (it << 16) | (is << 11) | (imm & 0x7FFu); }
inline Word lq(unsigned dest, unsigned ft, unsigned is, unsigned imm) { return (dest << 21) | (ft << 16) | (is << 11) | (imm & 0x7FFu); }
inline Word sq(unsigned dest, unsigned fs, unsigned it, unsigned imm) { return (1u << 25) | (dest << 21) | (it << 16) | (fs << 11) | (imm & 0x7FFu); }
inline Word ialu(unsigned funct, unsigned id, unsigned is, unsigned it) { return (0x40u << 25) | (it << 16) | (is << 11) | (id << 6) | funct; }
inline Word iaddi(unsigned it, unsigned is, int imm5) { return (0x40u << 25) | (it << 16) | (is << 11) | ((static_cast<unsigned>(imm5) & 31u) << 6) | 0x32u; }
inline Word branch(unsigned op, unsigned is, unsigned it, unsigned from, unsigned to) { return (op << 25) | (is << 11) | (it << 16) | (((to - from - 8u) / 8u) & 0x7FFu); }
inline Word divide(unsigned fsf, unsigned fs, unsigned ftf, unsigned ft) { return (0x40u << 25) | (ftf << 23) | (fsf << 21) | (ft << 16) | (fs << 11) | (0xEu << 6) | 0x3C; }
inline Word root(unsigned ftf, unsigned ft) { return (0x40u << 25) | (ftf << 23) | (ft << 16) | (0xEu << 6) | 0x3D; }
inline Word rsqrt(unsigned fsf, unsigned fs, unsigned ftf, unsigned ft) { return (0x40u << 25) | (ftf << 23) | (fsf << 21) | (ft << 16) | (fs << 11) | (0xEu << 6) | 0x3E; }
inline Word waitq() { return (0x40u << 25) | (0xEu << 6) | 0x3F; }

// Small numbers whose sums and products are exact, so the interpreter that truncates and the SIMD code that rounds to
// nearest agree, and that rarely run into overflow, where the exact interpreter answers 7FFFFFFF and the SIMD code 7F7FFFFF.
constexpr std::uint32_t kPool[12] = {0x00000000, 0x80000000, 0x3F800000, 0xBF800000, 0x40000000, 0xC0000000, 0x3F000000, 0xBF000000,
                                     0x40800000, 0xC0800000, 0x3E800000, 0x3F800000};

inline void init(ps2::Vu0Regs &r, unsigned vi1) {
    std::memset(&r, 0, sizeof r);
    for (int i = 0; i < 32; ++i)
        for (int k = 0; k < 4; ++k) r.vf[i].w[k] = i == 2 ? 0xC0000000u : 0x40000000u;
    for (int k = 0; k < 4; ++k) r.vf[0].w[k] = k == 3 ? 0x3F800000u : 0u;
    r.clip = 0x2A;
    r.vi[1] = static_cast<std::uint16_t>(vi1);
}

struct Case {
    const char *name;
    std::vector<Pair> pairs;
    unsigned vi1, check, want;
    bool jit;  // false: the pipeline state depends on the path, the Wasm translator leaves the program to the interpreter
};

inline std::vector<Pair> finish(std::vector<Pair> p) {
    p.back()[1] |= kEnd;
    p.push_back(kNop);
    return p;
}

// Registers: vf1 = {2, 2, 2, 2} against vf0.w = 1 gives the clip flags 0x15, which shift in behind the 0x2A the clip register
// starts with: FCGET (into vi2) reads 0x2A before the CLIP has taken effect and 0xA95 after it.
inline std::vector<Case> cases() {
    const Pair W = {kLnop, add(6, 3, 4)}, C = {kLnop, clip(1, 0)}, A = {kLnop, add(7, 6, 4)}, G = {fcget(2), kUnop};
    std::vector<Case> c;
    c.push_back({"gow2 shape: CLIP, a pair that waits for an FMAC result, a pair, FCGET reads the new flags", finish({W, C, A, kNop, G}), 0, 2, 0xA95, true});
    c.push_back({"no wait: FCGET three pairs after CLIP reads the old flags", finish({kNop, C, A, kNop, G}), 0, 2, 0x2A, true});
    c.push_back({"no wait: FCGET four pairs after CLIP reads the new flags", finish({kNop, C, kNop, kNop, kNop, G}), 0, 2, 0xA95, true});
    c.push_back({"a store of the FMAC result waits too", finish({W, C, {sq(15, 6, 0, 10), kUnop}, kNop, G}), 0, 2, 0xA95, true});
    c.push_back({"an integer register an ILW loads makes the reader wait", finish({{ilw(15, 3, 0, 4), kUnop}, C, {ialu(0x30, 4, 3, 3), kUnop}, kNop, G}), 0, 2, 0xA95, true});
    c.push_back({"an integer register nobody loads does not", finish({kNop, C, {ialu(0x30, 4, 3, 3), kUnop}, kNop, G}), 0, 2, 0x2A, true});
    c.push_back({"a WAITQ waits for the quotient", finish({{divide(3, 0, 3, 0), kUnop}, C, kNop, {waitq(), kUnop}, G}), 0, 2, 0xA95, true});
    c.push_back({"a divide waits for the previous one", finish({{divide(3, 0, 3, 0), kUnop}, C, {divide(3, 0, 3, 0), kUnop}, kNop, G}), 0, 2, 0xA95, true});
    c.push_back({"no WAITQ: FCGET three pairs after CLIP reads the old flags", finish({{divide(3, 0, 3, 0), kUnop}, C, kNop, kNop, G}), 0, 2, 0x2A, true});
    // The two paths into the CLIP differ in what is in flight: only the one that ran the ADD makes its reader wait.
    const std::vector<Pair> join = {{branch(0x28, 1, 0, 0, 24), kUnop}, kNop, W, C, A, kNop, G};
    c.push_back({"a join: the path with an ADD in flight waits", finish(join), 1, 2, 0xA95, false});
    c.push_back({"a join: the path without does not", finish(join), 0, 2, 0x2A, false});
    // Tekken 5: LOI 3 and MAXi vf3, vf0, I in one pair; the denormal 3 must come out as it went in (MTIR vi2, vf3.x reads 3).
    const Pair L3 = {3u, kUnop | kLoi}, M = {kLnop, fmac(0x1D, 15, 3, 0, 0)}, T = {lower_special(0x3C, 0, 2, 3), kUnop};
    c.push_back({"MAXi keeps a denormal operand", finish({L3, M, kNop, T}), 0, 2, 3, true});
    const Pair N = {kLnop, fmac(0x1F, 15, 3, 0, 0)};
    c.push_back({"MINIi keeps a denormal operand (vf0.x = 0 against 3: the smaller is 0)", finish({L3, N, kNop, T}), 0, 2, 0, true});
    return c;
}

// A random microprogram: straight runs, forward branches and counted loops (with a counter in vi14), a lot of FMAC
// results, integer loads and flag readers close together, and an E bit at the end.
inline std::vector<Pair> random_program(unsigned seed) {
    std::mt19937 g(seed * 2654435761u + 12345u);
    const auto R = [&](unsigned n) { return static_cast<unsigned>(g() % n); };
    const auto upper = [&]() -> Word {
        const unsigned c = R(100), dest = 1 + R(15), fd = R(7), fs = R(7), ft = R(7), bc = R(4);
        if (c < 16) return kUnop;
        if (c < 46) {
            static const unsigned f[] = {0x28, 0x2C, 0x2A, 0x28, 0x2C};
            return fmac(f[R(5)], dest, fd, fs, ft);
        }
        if (c < 58) {
            static const unsigned f[] = {0x00, 0x04, 0x08, 0x0C, 0x18};
            return fmac(f[R(5)] + bc, dest, fd, fs, ft);
        }
        if (c < 66) return fmac(R(2) ? 0x29 : 0x2D, dest, fd, fs, ft);
        if (c < 74) {
            static const unsigned f[] = {0x28, 0x2A, 0x29, 0x2D, 0x2C};
            return special(f[R(5)], dest, ft, fs);
        }
        if (c < 78) {
            static const unsigned f[] = {0x1D, 0x10, 0x14};
            return special(f[R(3)], dest, ft, fs);
        }
        if (c < 90) return clip(fs, ft);
        if (c < 93) return R(2) ? special(0x2E, 0xE, ft, fs) : fmac(0x2E, 0xE, fd, fs, ft);
        static const unsigned f[] = {0x1E, 0x22, 0x26, 0x23};
        return fmac(f[R(4)], dest, fd, fs, ft);
    };
    const auto lower = [&]() -> Word {
        const unsigned c = R(100), dest = 1 + R(15), v = 1 + R(6), w = 1 + R(6), res = 7 + R(7), f = R(7), f2 = R(7);
        if (c < 20) return kLnop;
        if (c < 28) return R(2) ? lq(dest, f, 0, R(40)) : sq(dest, f, 0, R(40));
        if (c < 36) {
            switch (R(5)) {
            case 0: return lower_special(0x30, dest, f, f2);
            case 1: return lower_special(0x31, dest, f, f2);
            case 2: return lower_special(0x3D, dest, f, v);
            case 3: return lower_special(0x3C, R(4), res, f);
            default: return lower_special(0x30, dest, f, f2);
            }
        }
        if (c < 48) {
            switch (R(6)) {
            case 0: return ialu(0x30, res, v, w);
            case 1: return ialu(0x31, res, v, w);
            case 2: return iaddi(v, w, static_cast<int>(R(32)) - 16);
            case 3: return iaddiu(v, w, R(200));
            case 4: return ialu(0x34, res, v, w);
            default: return ialu(0x35, res, v, w);
            }
        }
        if (c < 68) {
            switch (R(11)) {
            case 0: return fmflag(0x1A, res, v);
            case 1: return fmflag(0x18, res, v);
            case 2: return fmflag(0x1B, res, v);
            case 3: return fsflag(0x16, res, R(4096));
            case 4: return fsflag(0x14, res, R(4096));
            case 5: return fsflag(0x17, res, R(4096));
            case 6: return fcflag(0x12, R(1u << 24));
            case 7: return fcflag(0x10, R(1u << 24));
            case 8: return fcflag(0x13, R(1u << 24));
            default: return fcget(res);
            }
        }
        if (c < 74) return ilw(dest, res, 0, R(40));
        if (c < 88) {
            switch (R(4)) {
            case 0: return divide(R(4), f, R(4), f2);
            case 1: return root(R(4), f);
            case 2: return rsqrt(R(4), f, R(4), f2);
            default: return waitq();
            }
        }
        if (c < 90) return R(2) ? fcflag(0x11, R(1u << 24)) : fsflag(0x15, 0, R(4096));
        return kLnop;
    };
    const auto pair = [&]() -> Pair {
        Pair p = {lower(), upper()};
        if (R(40) == 0) {
            p[0] = kPool[g() % 12];  // a loaded I constant in place of the lower instruction
            p[1] |= kLoi;
        }
        return p;
    };
    std::vector<Pair> p;
    for (unsigned i = 0, n = 2 + R(3); i < n; ++i) p.push_back({iaddiu(1 + R(6), 0, R(30)), kUnop});
    for (unsigned segments = 3 + R(4); segments--;) {
        const unsigned kind = R(10);
        if (kind < 5) {
            for (unsigned i = 0, n = 3 + R(6); i < n; ++i) p.push_back(pair());
        } else if (kind < 8) {
            static const unsigned ops[] = {0x28, 0x29, 0x2C, 0x2D, 0x2E, 0x2F, 0x20};
            const unsigned skip = 1 + R(4), from = static_cast<unsigned>(p.size()) * 8u, to = from + (2 + skip) * 8u;
            p.push_back({branch(ops[R(7)], 1 + R(6), 1 + R(6), from, to), upper()});
            p.push_back(pair());
            for (unsigned i = 0; i < skip; ++i) p.push_back(pair());
        } else {
            p.push_back({iaddiu(14, 0, 1 + R(2)), upper()});
            const unsigned top = static_cast<unsigned>(p.size()) * 8u;
            for (unsigned i = 0, n = 3 + R(5); i < n; ++i) p.push_back(pair());
            p.push_back({iaddi(14, 14, -1), upper()});
            p.push_back({branch(0x29, 14, 0, static_cast<unsigned>(p.size()) * 8u, top), upper()});
            p.push_back(pair());
        }
    }
    // The program always reads the flags and the status at least once, so every flag instruction counts.
    p.push_back({fsflag(0x16, 13, 0xFFF), upper()});
    p.push_back({fcget(12), upper()});
    p.push_back({fmflag(0x1A, 11, 1), upper()});
    p.push_back(pair());
    p.back()[1] |= kEnd;
    p.push_back(pair());
    return p;
}

inline void random_init(ps2::Vu0Regs &r, std::uint8_t *mem, unsigned seed) {
    std::mt19937 g(seed * 40503u + 7u);
    std::memset(&r, 0, sizeof r);
    for (auto &q : r.vf)
        for (auto &x : q.w) x = kPool[g() % 12];
    for (auto &x : r.acc.w) x = kPool[g() % 12];
    r.vf[0] = {};
    r.vf[0].w[3] = 0x3F800000u;
    for (int i = 1; i < 16; ++i) r.vi[i] = static_cast<std::uint16_t>(g() % 8);
    r.q = kPool[g() % 12];
    r.i = kPool[g() % 12];
    r.p = kPool[g() % 12];
    r.r = 0x3F800000u | (g() & 0x7FFFFFu);
    r.clip = g() & 0xFFFFFFu;
    r.status = g() & 0xFFFu;
    r.mac = g() & 0xFFFFu;
    for (int i = 0; i < 256 * 4; ++i) {
        const std::uint32_t x = kPool[g() % 12];
        std::memcpy(mem + i * 4, &x, 4);
    }
}

inline std::string describe(const ps2::Vu0Regs &r, const std::uint8_t *mem) {
    std::string s;
    char b[96];
    const auto line = [&](const char *name, std::uint32_t a, std::uint32_t c, std::uint32_t d, std::uint32_t e) {
        std::snprintf(b, sizeof b, "%s %08X %08X %08X %08X\n", name, a, c, d, e);
        s += b;
    };
    for (int i = 1; i < 32; ++i) {
        char n[8];
        std::snprintf(n, sizeof n, "vf%02d", i);
        line(n, r.vf[i].w[0], r.vf[i].w[1], r.vf[i].w[2], r.vf[i].w[3]);
    }
    line("acc", r.acc.w[0], r.acc.w[1], r.acc.w[2], r.acc.w[3]);
    for (int i = 1; i < 16; i += 4) line("vi", r.vi[i], r.vi[i + 1], r.vi[i + 2], i + 3 < 16 ? r.vi[i + 3] : 0);
    line("qpir", r.q, r.p, r.i, r.r);
    line("flags", r.mac, r.status, r.clip, 0);
    std::uint64_t h = 0xCBF29CE484222325ull;
    for (int i = 0; i < 4096; ++i) h = (h ^ mem[i]) * 0x100000001B3ull;
    line("mem", static_cast<std::uint32_t>(h), static_cast<std::uint32_t>(h >> 32), 0, 0);
    return s;
}

inline std::uint64_t digest(const std::string &s) {
    std::uint64_t h = 0xCBF29CE484222325ull;
    for (char c : s) h = (h ^ static_cast<unsigned char>(c)) * 0x100000001B3ull;
    return h;
}

// The vector registers and the rest (integers, Q, flags, memory) hashed apart.
inline std::string digests(const std::string &s) {
    std::string vec, rest;
    for (std::size_t at = 0; at < s.size();) {
        const std::size_t end = s.find('\n', at);
        const std::string l = s.substr(at, end - at + 1);
        (l.rfind("vf", 0) == 0 || l.rfind("acc", 0) == 0 ? vec : rest) += l;
        at = end + 1;
    }
    char b[64];
    std::snprintf(b, sizeof b, "%016llX %016llX", static_cast<unsigned long long>(digest(vec)), static_cast<unsigned long long>(digest(rest)));
    return b;
}

}
'''

GENERATOR = r'''
#define main vurecomp_main
#include "recomp/vurecomp_gen.cpp"
#undef main
#include "runtime/src/vu_jit.hpp"
#include "cases.h"
#include <cstddef>

int main(int argc, char **argv) {
    const fs::path out = argv[1];
    std::ofstream native(out / "native.cpp");
    native << "#include <stdexcept>\n#include <chrono>\n#include <cstdio>\n#include <cstdlib>\n#include \"cases.h\"\n"
              "namespace ps2::vu { void interpret(int, std::uint32_t) { throw std::runtime_error(\"unexpected fallback\"); } }\n"
              "using namespace ps2; using namespace ps2::vuops;\n";
    constexpr std::uint32_t nop = 0x8000033C, unop = 0x000002FF, end = 0x40000000;
    const auto addi = [](unsigned it, unsigned is, unsigned imm) { return (8u << 25) | (it << 16) | (is << 11) | imm; };
    const auto branch = [](unsigned op, unsigned is, unsigned it, unsigned from, unsigned to) {
        return (op << 25) | (is << 11) | (it << 16) | (((to - from - 8u) / 8u) & 0x7FFu);
    };
    for (int test = 0; test < 8; ++test) {
        Program p{static_cast<std::uint64_t>(test), 0, std::vector<std::uint8_t>(16384)};
        const auto put = [&](unsigned pc, std::uint32_t lo, std::uint32_t hi = unop) {
            std::memcpy(p.code.data() + pc, &lo, 4);
            std::memcpy(p.code.data() + pc + 4, &hi, 4);
        };
        for (unsigned pc = 0; pc < 128; pc += 8) put(pc, nop);
        if (test < 3 || test == 5) {
            // A preceding VI write delays a comparison, but FCAND does not.
            put(0, test == 2 ? (0x12u << 25) | 1u : addi(1, 0, 1));
            put(8, branch(test == 5 ? 0x2C : test == 2 ? 0x29 : 0x28, test == 1 ? 0 : 1, test == 0 ? 3 : 0, 8, 40));
            put(24, addi(2, 0, 1), unop | end);
            put(40, addi(2, 0, 2), unop | end);
        } else if (test == 3) {
            // The instruction executed before PC 40 is a taken branch's
            // delay slot at PC 8, rather than the instruction at PC 32.
            put(0, branch(0x20, 0, 0, 0, 40));
            put(8, addi(1, 0, 1));
            put(32, addi(1, 0, 3));
            put(40, branch(0x28, 1, 3, 40, 72));
            put(56, addi(2, 0, 1), unop | end);
            put(72, addi(2, 0, 2), unop | end);
        } else if (test >= 6) {
            // JR/JALR read the operand without the delay; JALR also overwrites it.
            put(0, addi(1, 0, 1));
            put(8, ((test == 6 ? 0x24u : 0x25u) << 25) | (1u << 11) | (1u << 16), unop | end);
        } else {
            // Flags remain observable after exit in a program that reads them.
            put(0, nop, (0xFu << 21) | (2u << 16) | (1u << 11) | (3u << 6) | 0x28u | end);
            put(8, (0x16u << 25) | (2u << 16));
        }
        std::string name;
        native << generate(p, name);
        std::vector<std::uint8_t> wasm;
        if (!ps2::vu::jit::translate(p.code.data(), 0, {0x1000}, wasm)) return 1;
        std::ofstream(out / std::format("{}.wasm", test), std::ios::binary).write(reinterpret_cast<const char *>(wasm.data()), wasm.size());
    }

    // The pipeline stall cases: compiled here, run by the interpreter harness, the native main and node.
    const auto programs = [&](const std::vector<tc::Pair> &pairs, std::uint64_t key) {
        Program p{key, 0, std::vector<std::uint8_t>(16384)};
        for (std::size_t i = 0; i < pairs.size(); ++i) {
            std::memcpy(p.code.data() + i * 8, &pairs[i][0], 4);
            std::memcpy(p.code.data() + i * 8 + 4, &pairs[i][1], 4);
        }
        return p;
    };
    const std::vector<tc::Case> cases = tc::cases();
    std::string case_table = "static void (*const kCaseFns[])(Vu0Regs &, std::uint8_t *) = {\n", rand_table = "static void (*const kRandFns[])(Vu0Regs &, std::uint8_t *) = {\n";
    std::ofstream json(out / "cases.json");
    json << "[";
    for (std::size_t i = 0; i < cases.size(); ++i) {
        const Program p = programs(cases[i].pairs, 100 + i);
        std::string name;
        native << generate(p, name);
        if (name.empty()) { std::fprintf(stderr, "case %zu: no plan\n", i); return 1; }
        case_table += "    " + name + ",\n";
        std::vector<std::uint8_t> wasm;
        const bool jit = ps2::vu::jit::translate(p.code.data(), 0, {0x1000}, wasm);
        if (jit != cases[i].jit) { std::fprintf(stderr, "case %zu: Wasm translation %d, expected %d\n", i, jit, cases[i].jit); return 1; }
        if (jit) std::ofstream(out / std::format("case{}.wasm", i), std::ios::binary).write(reinterpret_cast<const char *>(wasm.data()), wasm.size());
        json << std::format("{}{{\"name\":\"{}\",\"jit\":{},\"vi1\":{},\"check\":{},\"want\":{},\"file\":\"case{}.wasm\"}}", i ? "," : "", cases[i].name, jit ? "true" : "false", cases[i].vi1, cases[i].check, cases[i].want, i);
    }
    json << "]";
    unsigned tracked = 0;
    for (unsigned i = 0; i < RANDOM_PROGRAMS; ++i) {
        const Program p = programs(tc::random_program(i), 1000 + i);
        std::string name;
        native << generate(p, name);
        if (name.empty()) { std::fprintf(stderr, "random program %u: no plan\n", i); return 1; }
        rand_table += "    " + name + ",\n";
        const auto pcs = ps2::vu::reachable(p.code.data(), kMask, 0);
        if (ps2::vu::stall_plan(p.code.data(), kMask, pcs, 0).any_tracked) ++tracked;
    }
    std::fprintf(stderr, "%u of %u random programs have an instruction whose stall depends on the path\n", tracked, static_cast<unsigned>(RANDOM_PROGRAMS));
    native << case_table << "};\n" << rand_table << "};\n";

    native << R"(
static int run_cases() {
    int failures = 0;
    const std::vector<tc::Case> cases = tc::cases();
    for (std::size_t i = 0; i < cases.size(); ++i) {
        Vu0Regs r;
        std::uint8_t mem[16384] = {};
        tc::init(r, cases[i].vi1);
        kCaseFns[i](r, mem);
        const bool ok = r.vi[cases[i].check] == cases[i].want;
        if (!ok) ++failures;
        std::printf("%s compiled %s: vi%u = %04X, want %04X\n", ok ? "ok  " : "FAIL", cases[i].name, cases[i].check, r.vi[cases[i].check], cases[i].want);
    }
    return failures;
}
)";
    native << R"(
// `native bench` times the compiled random programs (after the checks have passed).
static void bench() {
    constexpr unsigned N = sizeof kRandFns / sizeof kRandFns[0];
    static Vu0Regs init[N];
    static std::uint8_t mem[N][16384];
    for (unsigned i = 0; i < N; ++i) tc::random_init(init[i], mem[i], i);
    std::uint64_t sink = 0;
    const auto start = std::chrono::steady_clock::now();
    for (int round = 0; round < 4000; ++round)
        for (unsigned i = 0; i < N; ++i) {
            Vu0Regs r = init[i];
            kRandFns[i](r, mem[i]);
            sink += r.vi[7] + r.mac;
        }
    std::printf("%.1f ms (%llu)\n", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count(), static_cast<unsigned long long>(sink));
}
)";
    native << "int main(int argc, char **argv) {\n"
              "    if (argc > 1 && std::string(argv[1]) == \"bench\") { bench(); return 0; }\n"
              "    if (argc > 2) {\n"
              "        const unsigned i = std::atoi(argv[2]);\n"
              "        Vu0Regs r; std::uint8_t mem[16384] = {};\n"
              "        tc::random_init(r, mem, i); kRandFns[i](r, mem);\n"
              "        std::fputs(tc::describe(r, mem).c_str(), stdout);\n"
              "        return 0;\n"
              "    }\n"
              "    if (argc > 1) {\n"
              "        for (unsigned i = 0; i < " << RANDOM_PROGRAMS << "; ++i) {\n"
              "            Vu0Regs r; std::uint8_t mem[16384] = {};\n"
              "            tc::random_init(r, mem, i); kRandFns[i](r, mem);\n"
              "            std::printf(\"%u %s\\n\", i, tc::digests(tc::describe(r, mem)).c_str());\n"
              "        }\n"
              "        return 0;\n"
              "    }\n"
              "    if (run_cases()) return 1;\n";
    for (int test = 0; test < 8; ++test) {
        native << "{ Vu0Regs r{}; std::uint8_t mem[16384]{}; r.vi[1]=2; r.vi[3]=2; r.clip=1;"
                  "for (int i=0;i<4;++i) r.vf[1].w[i]=0xBF800000; r.vf[0].w[3]=0x3F800000;\n";
        if (test == 5) native << "r.vi[1]=0xFFFF;\n";
        native << std::format("vu1_{:016X}(r, mem);\n", test);
        native << (test >= 6 ? "if (r.tpc != 8) return 1;\n" : test == 4 ? "if (r.mac != 0xF0 || r.status != 0x82) return 1;\n" : "if (r.vi[2] != 2) return 1;\n");
        native << "}\n";
    }
    native << "std::puts(\"8 native VU regressions passed\"); }\n";
    std::ofstream offsets(out / "offsets.json");
    offsets << std::format("{{\"vi\":{},\"vf\":{},\"clip\":{},\"mac\":{},\"status\":{},\"tpc\":{},\"fl_t\":{},\"fl_ch\":{},\"fl_clip_ts\":{},\"fl_clip_v\":{}}}", offsetof(ps2::Vu0Regs, vi), offsetof(ps2::Vu0Regs, vf), offsetof(ps2::Vu0Regs, clip), offsetof(ps2::Vu0Regs, mac), offsetof(ps2::Vu0Regs, status), offsetof(ps2::Vu0Regs, tpc),
                          offsetof(ps2::vuops::FlagPipe, t), offsetof(ps2::vuops::FlagPipe, ch), offsetof(ps2::vuops::FlagPipe, clip_ts), offsetof(ps2::vuops::FlagPipe, clip_v));
}
'''

# The interpreters of vu_micro.cpp, linked against stand-ins for the rest of the runtime.
INTERP = r'''
#include "cases.h"
#include "hw.hpp"
#include "gs/gif.hpp"
#include "gs/queue.hpp"
#include "ps2/system.hpp"
#include "vu_micro.hpp"
#include <chrono>
#include <cstdlib>

namespace ps2 {
alignas(64) std::uint8_t g_vu0_code[4096], g_vu0_mem[4096], g_vu1_code[16384], g_vu1_mem[16384];
Cpu g_cpu;
Options g_options;
namespace gif { void transfer(int, const std::uint8_t *, std::uint32_t) {} }
namespace gs::queue {
Ring &vu() { static Ring ring; return ring; }
Role &role() { static Role r = Role::Emulation; return r; }
bool vu_active() { return false; }
void Ring::post(Type, std::uint32_t, std::uint32_t, const void *, std::uint32_t) {}
}
}

using namespace ps2;

// Runs the program in unit 0 (the exact interpreter) or unit 1 (the decoding one); returns the registers and memory.
static void run(int unit, const std::vector<tc::Pair> &pairs, void (*init)(Vu0Regs &, std::uint8_t *, unsigned), unsigned arg, Vu0Regs &r, std::uint8_t *mem) {
    std::uint8_t *code = unit ? g_vu1_code : g_vu0_code, *data = unit ? g_vu1_mem : g_vu0_mem;
    std::memset(code, 0, unit ? 16384 : 4096);
    for (std::size_t i = 0; i < pairs.size(); ++i) {
        std::memcpy(code + i * 8, &pairs[i][0], 4);
        std::memcpy(code + i * 8 + 4, &pairs[i][1], 4);
    }
    vu::code_changed(unit);
    std::memset(data, 0, unit ? 16384 : 4096);
    Vu0Regs &regs = vu::regs(unit);
    init(regs, data, arg);
    vu::run(unit, 0);
    r = regs;
    std::memcpy(mem, data, 4096);
}

// `interp bench`: how long each interpreter takes for the random programs, loaded once and run again and again.
static void bench() {
    for (int unit = 0; unit < 2; ++unit) {
        std::uint8_t *code = unit ? g_vu1_code : g_vu0_code, *data = unit ? g_vu1_mem : g_vu0_mem;
        double ms = 0;
        for (unsigned i = 0; i < RANDOM_PROGRAMS; ++i) {
            const std::vector<tc::Pair> pairs = tc::random_program(i);
            std::memset(code, 0, unit ? 16384 : 4096);
            for (std::size_t k = 0; k < pairs.size(); ++k) {
                std::memcpy(code + k * 8, &pairs[k][0], 4);
                std::memcpy(code + k * 8 + 4, &pairs[k][1], 4);
            }
            vu::code_changed(unit);
            Vu0Regs init;
            tc::random_init(init, data, i);
            std::uint8_t saved[4096];
            std::memcpy(saved, data, 4096);
            const auto start = std::chrono::steady_clock::now();
            for (int rep = 0; rep < 300; ++rep) {
                vu::regs(unit) = init;
                vu::run(unit, 0);
            }
            ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        }
        std::printf("interpreter %d: %.1f ms\n", unit, ms);
    }
}

int main(int argc, char **argv) {
    int failures = 0;
    if (argc > 1 && std::string(argv[1]) == "bench") {
        bench();
        return 0;
    }
    if (argc > 2) {
        const unsigned i = std::atoi(argv[2]);
        Vu0Regs r;
        std::uint8_t mem[4096];
        run(std::atoi(argv[1]), tc::random_program(i), [](Vu0Regs &r, std::uint8_t *m, unsigned seed) { tc::random_init(r, m, seed); }, i, r, mem);
        std::fputs(tc::describe(r, mem).c_str(), stdout);
        return 0;
    }
    const std::vector<tc::Case> cases = tc::cases();
    for (std::size_t i = 0; i < cases.size(); ++i)
        for (int unit = 0; unit < 2; ++unit) {
            Vu0Regs r;
            std::uint8_t mem[4096];
            run(unit, cases[i].pairs, [](Vu0Regs &r, std::uint8_t *, unsigned vi1) { tc::init(r, vi1); }, cases[i].vi1, r, mem);
            const bool ok = r.vi[cases[i].check] == cases[i].want;
            if (!ok) ++failures;
            std::printf("%s interpreter %d %s: vi%u = %04X, want %04X\n", ok ? "ok  " : "FAIL", unit, cases[i].name, cases[i].check, r.vi[cases[i].check], cases[i].want);
        }
    if (argc > 1) {
        for (unsigned i = 0; i < RANDOM_PROGRAMS; ++i)
            for (int unit = 0; unit < 2; ++unit) {
                Vu0Regs r;
                std::uint8_t mem[4096];
                run(unit, tc::random_program(i), [](Vu0Regs &r, std::uint8_t *m, unsigned seed) { tc::random_init(r, m, seed); }, i, r, mem);
                std::printf("%d %u %s\n", unit, i, tc::digests(tc::describe(r, mem)).c_str());
            }
    }
    return failures ? 1 : 0;
}
'''

WASM_TEST = r'''
const fs = require('node:fs');
const path = require('node:path');
const dir = process.argv[2];
const off = JSON.parse(fs.readFileSync(path.join(dir, 'offsets.json')));
for (let test = 0; test < 8; ++test) {
  const memory = new WebAssembly.Memory({initial: 1, maximum: 65536, shared: true});
  const view = new DataView(memory.buffer);
  const r = 0x3000;
  view.setUint16(r + off.vi + 2, test === 5 ? 0xFFFF : 2, true);
  view.setUint16(r + off.vi + 6, 2, true);
  view.setUint32(r + off.clip, 1, true);
  view.setUint32(r + off.vf + 12, 0x3F800000, true);
  for (let i = 0; i < 4; ++i) view.setUint32(r + off.vf + 16 + 4*i, 0xBF800000, true);
  const mod = new WebAssembly.Module(fs.readFileSync(path.join(dir, `${test}.wasm`)));
  const env = {memory};
  for (const imp of WebAssembly.Module.imports(mod)) {
    if (imp.kind !== 'function') continue;
    env[imp.name] = () => { throw Error(`Unexpected import: ${imp.name}`); };
  }
  env.clip = () => 1;
  env.status = () => 0;
  new WebAssembly.Instance(mod, {env}).exports.run(r, 0x4000);
  if (test >= 6) {
    if (view.getUint32(r + off.tpc, true) !== 8) throw Error(`Register jump regression ${test}`);
  } else if (test === 4) {
    if (view.getUint32(r + off.mac, true) !== 0xF0 || view.getUint32(r + off.status, true) !== 0x82)
      throw Error('Exit flags were dropped');
  } else if (view.getUint16(r + off.vi + 4, true) !== 2) {
    throw Error(`Branch VI regression ${test}`);
  }
}
console.log('8 Wasm VU regressions passed');

// The stall cases, with the clip flags read from the flag ring the program keeps (what FlagPipe::clip does).
const cases = JSON.parse(fs.readFileSync(path.join(dir, 'cases.json')));
let ran = 0;
for (const c of cases) {
  if (!c.jit) continue;
  const memory = new WebAssembly.Memory({initial: 1, maximum: 65536, shared: true});
  const view = new DataView(memory.buffer);
  const r = 0x3000, fl = 0x1000;
  for (let i = 0; i < 32; ++i)
    for (let k = 0; k < 4; ++k) view.setUint32(r + off.vf + 16 * i + 4 * k, i === 2 ? 0xC0000000 : 0x40000000, true);
  for (let k = 0; k < 4; ++k) view.setUint32(r + off.vf + 4 * k, k === 3 ? 0x3F800000 : 0, true);
  view.setUint32(r + off.clip, 0x2A, true);
  view.setUint16(r + off.vi + 2, c.vi1, true);
  const mod = new WebAssembly.Module(fs.readFileSync(path.join(dir, c.file)));
  const env = {memory};
  for (const imp of WebAssembly.Module.imports(mod)) {
    if (imp.kind !== 'function') continue;
    env[imp.name] = () => { throw Error(`Unexpected import: ${imp.name}`); };
  }
  env.clip = (p) => {
    const t = view.getUint32(p + off.fl_t, true), ch = view.getUint32(p + off.fl_ch, true);
    for (let k = 1; k <= 8; ++k) {
      const i = (ch - k) & 7;
      if (view.getUint32(p + off.fl_clip_ts + 4 * i, true) + 4 <= t) return view.getUint32(p + off.fl_clip_v + 4 * i, true);
    }
    return view.getUint32(p + off.fl_clip_v + 4 * ((ch - 8) & 7), true);
  };
  env.status = () => 0;
  new WebAssembly.Instance(mod, {env}).exports.run(r, 0x4000);
  const got = view.getUint16(r + off.vi + 2 * c.check, true);
  if (got !== c.want) throw Error(`Wasm stall case "${c.name}": vi${c.check} = ${got.toString(16)}, want ${c.want.toString(16)}`);
  ++ran;
}
console.log(`${ran} Wasm stall cases passed`);
'''


def run(*args, **kwargs):
    return subprocess.run(args, check=True, cwd=ROOT, **kwargs)


def compile_(*args):
    return subprocess.run(args, check=True, cwd=ROOT, capture_output=True, text=True)


def lines(text):
    return {tuple(line.split(None, 2)[:2]): line.split(None, 2)[2] for line in text.splitlines() if line.strip()}


with tempfile.TemporaryDirectory(prefix='vu-codegen-') as tmp:
    tmp = pathlib.Path(tmp)
    (tmp / 'generator.cpp').write_text('#define RANDOM_PROGRAMS %d\n' % RANDOM_PROGRAMS + GENERATOR)
    (tmp / 'cases.h').write_text(CASES_H)
    (tmp / 'interp.cpp').write_text('#define RANDOM_PROGRAMS %d\n#include <atomic>\n#include <vector>\n#include "ps2/vu_live.hpp"\nnamespace ps2 { std::uint64_t frame_count() { return 0; } bool halted() { return false; } namespace vif { std::uint32_t itop(int) { return 0; } } namespace vu::live { std::atomic<bool> g_ready{false}; bool active() { return false; } void note_call(std::uint64_t) {} std::vector<Program> take() { return {}; } void unknown(std::uint64_t, std::uint32_t, const std::uint8_t *) {} } }\n' % RANDOM_PROGRAMS + INTERP)
    base = [CXX, '-std=c++20', '-I.', '-Iruntime/include', f'-I{tmp}']
    with concurrent.futures.ThreadPoolExecutor(2) as pool:
        interp = pool.submit(compile_, *base, '-O2' if os.environ.get('VU_BENCH') else '-O1', '-Iruntime/src', str(tmp / 'interp.cpp'), 'runtime/src/vu_micro.cpp', 'runtime/src/vu_jit_web.cpp', '-o', str(tmp / 'interp'))
        generator = pool.submit(compile_, *base, '-O2', str(tmp / 'generator.cpp'), 'runtime/src/vu_jit.cpp', '-o', str(tmp / 'generator'))
        for job in (interp, generator):
            try:
                job.result()
            except subprocess.CalledProcessError as e:
                sys.exit(e.stderr or str(e))
    run(str(tmp / 'generator'), str(tmp))
    run(CXX, '-std=c++20', '-O2' if os.environ.get('VU_BENCH') else '-O1', '-w', '-Iruntime/include', f'-I{tmp}', str(tmp / 'native.cpp'), '-o', str(tmp / 'native'))
    run(str(tmp / 'native'))
    for _ in range(5 if os.environ.get('VU_BENCH') else 0):
        run(str(tmp / 'native'), 'bench')
        run(str(tmp / 'interp'), 'bench')
    interpreter = subprocess.run([str(tmp / 'interp')], cwd=ROOT, capture_output=True, text=True)
    sys.stdout.write(interpreter.stdout)
    if interpreter.returncode:
        sys.exit('stall cases failed in the interpreter')
    print('%d stall cases passed in both interpreters' % (len([x for x in interpreter.stdout.splitlines() if x.startswith('ok')]) // 2))
    # Random microprograms: the compiled program must leave the same registers and memory as both interpreters.
    compiled = subprocess.run([str(tmp / 'native'), 'random'], cwd=ROOT, capture_output=True, text=True, check=True).stdout.split('\n')
    interpreted = subprocess.run([str(tmp / 'interp'), 'random'], cwd=ROOT, capture_output=True, text=True, check=True).stdout.split('\n')
    want = {int(l.split()[0]): l.split()[1:] for l in compiled if l.strip()}
    got = {}
    for l in interpreted:
        parts = l.split()
        if len(parts) == 4 and parts[0] in ('0', '1'):
            got[(int(parts[0]), int(parts[1]))] = parts[2:]
    # The interpreter of unit 1 does its arithmetic like the compiled code and must match it entirely. Unit 0 truncates
    # like the console (and answers 7FFFFFFF where the others clamp to 7F7FFFFF), so where the vector registers differ the
    # numbers are to blame; where they agree, everything else must agree too.
    bad = []
    skipped = 0
    for i in sorted(want):
        if got.get((1, i)) != want[i]:
            bad.append((i, 1))
        if got.get((0, i)) != want[i]:
            if got[(0, i)][0] == want[i][0]:
                bad.append((i, 0))
            else:
                skipped += 1
    if bad:
        for i, unit in bad[:5]:
            a = subprocess.run([str(tmp / 'native'), 'random', str(i)], cwd=ROOT, capture_output=True, text=True, check=True).stdout.splitlines()
            b = subprocess.run([str(tmp / 'interp'), str(unit), str(i)], cwd=ROOT, capture_output=True, text=True, check=True).stdout.splitlines()
            print(f'random program {i}: compiled and interpreter {unit} differ')
            for x, y in zip(a, b):
                if x != y:
                    print(f'  compiled    {x}\n  interpreter {y}')
        sys.exit(f'{len(bad)} random program runs differ')
    print(f'{len(want)} random microprograms leave the same state compiled and in the interpreters ({skipped} not compared with unit 0: its truncating arithmetic gives other numbers)')
    (tmp / 'wasm.cjs').write_text(WASM_TEST)
    run('node', str(tmp / 'wasm.cjs'), str(tmp))
