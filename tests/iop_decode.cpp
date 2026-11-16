// No game data: the IOP interpreter runs from a table of decoded instructions (runtime/src/iop/cpu.cpp). This test
// runs random R3000 programs (register, multiply/divide, load/store with RAM mirrors and kseg addresses, branches and jumps with
// their delay slots, loops) through the interpreter and through the interpreter as it was before the table (kept here as the
// reference), in slices of random budgets, and compares registers, hi/lo, pc/npc, the budget left and all of IOP RAM after every
// slice. Then the cases where code changes under the table: the program stores over one of its own later instructions, the host
// writes code into RAM as a module load or a DMA does, a save state puts older RAM back, and code is run through a RAM mirror.
// Build: ninja ps2iopdecodetest, run build/.../ps2iopdecodetest (prints "iop decode: 0 failed"); "bench" also times both.
#include "iop/iop.hpp"
#include "iop/kernel.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

namespace ps2::iop {
void execute(Cpu &c, std::int64_t &budget);
std::uint32_t fetch(std::uint32_t pc);
}

namespace {
using namespace ps2::iop;
namespace kernel = ps2::iop::kernel;

int failed = 0, checks = 0;
void check(bool ok, const std::string &message) {
    ++checks;
    if (!ok && ++failed <= 20) std::fprintf(stderr, "FAIL: %s\n", message.c_str());
}

inline std::int32_t simm(std::uint32_t w) { return static_cast<std::int16_t>(w & 0xFFFFu); }
void bad(Cpu &c, std::uint32_t pc, std::uint32_t w) {
    std::fprintf(stderr, "[iop] unknown instruction %08X at 0x%08X (ra 0x%08X)\n", w, pc, c.r[31]);
    kernel::fault("unknown instruction");
}

// the interpreter before the decode table, unchanged but for the name and the PS2_IOP_PROBE lines
void ref_execute(Cpu &c, std::int64_t &budget) {
    while (budget-- > 0) {
        const std::uint32_t pc = c.pc;
        const std::uint32_t w = fetch(pc);
        c.pc = c.npc;
        c.npc += 4;
        const std::uint32_t rs = (w >> 21) & 31u, rt = (w >> 16) & 31u, rd = (w >> 11) & 31u, sa = (w >> 6) & 31u;
        std::uint32_t *r = c.r;
        switch (w >> 26) {
        case 0x00:
            switch (w & 63u) {
            case 0x00: r[rd] = r[rt] << sa; break;
            case 0x02: r[rd] = r[rt] >> sa; break;
            case 0x03: r[rd] = static_cast<std::uint32_t>(static_cast<std::int32_t>(r[rt]) >> sa); break;
            case 0x04: r[rd] = r[rt] << (r[rs] & 31u); break;
            case 0x06: r[rd] = r[rt] >> (r[rs] & 31u); break;
            case 0x07: r[rd] = static_cast<std::uint32_t>(static_cast<std::int32_t>(r[rt]) >> (r[rs] & 31u)); break;
            case 0x08: c.npc = r[rs]; break;
            case 0x09: { const std::uint32_t t = r[rs]; r[rd] = pc + 8; c.npc = t; break; }
            case 0x0C:
                kernel::syscall(c, pc);
                break;
            case 0x0D:
                std::fprintf(stderr, "[iop] break at 0x%08X\n", pc);
                break;
            case 0x0F: break;
            case 0x10: r[rd] = c.hi; break;
            case 0x11: c.hi = r[rs]; break;
            case 0x12: r[rd] = c.lo; break;
            case 0x13: c.lo = r[rs]; break;
            case 0x18: {
                const std::int64_t p = static_cast<std::int64_t>(static_cast<std::int32_t>(r[rs])) * static_cast<std::int32_t>(r[rt]);
                c.lo = static_cast<std::uint32_t>(p);
                c.hi = static_cast<std::uint32_t>(static_cast<std::uint64_t>(p) >> 32);
                break;
            }
            case 0x19: {
                const std::uint64_t p = static_cast<std::uint64_t>(r[rs]) * r[rt];
                c.lo = static_cast<std::uint32_t>(p);
                c.hi = static_cast<std::uint32_t>(p >> 32);
                break;
            }
            case 0x1A: {
                const auto n = static_cast<std::int32_t>(r[rs]), d = static_cast<std::int32_t>(r[rt]);
                if (d == 0) { c.lo = n < 0 ? 1u : 0xFFFFFFFFu; c.hi = static_cast<std::uint32_t>(n); }
                else if (n == INT32_MIN && d == -1) { c.lo = static_cast<std::uint32_t>(INT32_MIN); c.hi = 0; }
                else { c.lo = static_cast<std::uint32_t>(n / d); c.hi = static_cast<std::uint32_t>(n % d); }
                break;
            }
            case 0x1B:
                if (r[rt] == 0) { c.lo = 0xFFFFFFFFu; c.hi = r[rs]; }
                else { c.lo = r[rs] / r[rt]; c.hi = r[rs] % r[rt]; }
                break;
            case 0x20: case 0x21: r[rd] = r[rs] + r[rt]; break;
            case 0x22: case 0x23: r[rd] = r[rs] - r[rt]; break;
            case 0x24: r[rd] = r[rs] & r[rt]; break;
            case 0x25: r[rd] = r[rs] | r[rt]; break;
            case 0x26: r[rd] = r[rs] ^ r[rt]; break;
            case 0x27: r[rd] = ~(r[rs] | r[rt]); break;
            case 0x2A: r[rd] = static_cast<std::int32_t>(r[rs]) < static_cast<std::int32_t>(r[rt]); break;
            case 0x2B: r[rd] = r[rs] < r[rt]; break;
            default: bad(c, pc, w); return;
            }
            break;
        case 0x01: {
            const bool link = (rt & 0x1Eu) == 0x10u;
            const bool taken = (rt & 1u) ? static_cast<std::int32_t>(r[rs]) >= 0 : static_cast<std::int32_t>(r[rs]) < 0;
            if (link) r[31] = pc + 8;
            if (taken) c.npc = pc + 4 + (static_cast<std::uint32_t>(simm(w)) << 2);
            break;
        }
        case 0x02: c.npc = (pc & 0xF0000000u) | ((w & 0x03FFFFFFu) << 2); break;
        case 0x03: r[31] = pc + 8; c.npc = (pc & 0xF0000000u) | ((w & 0x03FFFFFFu) << 2); break;
        case 0x04: if (r[rs] == r[rt]) c.npc = pc + 4 + (static_cast<std::uint32_t>(simm(w)) << 2); break;
        case 0x05: if (r[rs] != r[rt]) c.npc = pc + 4 + (static_cast<std::uint32_t>(simm(w)) << 2); break;
        case 0x06: if (static_cast<std::int32_t>(r[rs]) <= 0) c.npc = pc + 4 + (static_cast<std::uint32_t>(simm(w)) << 2); break;
        case 0x07: if (static_cast<std::int32_t>(r[rs]) > 0) c.npc = pc + 4 + (static_cast<std::uint32_t>(simm(w)) << 2); break;
        case 0x08: case 0x09: r[rt] = r[rs] + static_cast<std::uint32_t>(simm(w)); break;
        case 0x0A: r[rt] = static_cast<std::int32_t>(r[rs]) < simm(w); break;
        case 0x0B: r[rt] = r[rs] < static_cast<std::uint32_t>(simm(w)); break;
        case 0x0C: r[rt] = r[rs] & (w & 0xFFFFu); break;
        case 0x0D: r[rt] = r[rs] | (w & 0xFFFFu); break;
        case 0x0E: r[rt] = r[rs] ^ (w & 0xFFFFu); break;
        case 0x0F: r[rt] = w << 16; break;
        case 0x10:
            if (rs == 0) r[rt] = rd == 12 ? c.status : rd == 13 ? c.cause : rd == 14 ? c.epc : rd == 15 ? 0x1Fu : 0u;
            else if (rs == 4) {
                if (rd == 12) kernel::set_status(c, r[rt]);
                else if (rd == 13) c.cause = r[rt];
                else if (rd == 14) c.epc = r[rt];
            } else if ((w & 63u) == 0x10) {
                c.status = (c.status & ~0xFu) | ((c.status >> 2) & 0xFu);
            }
            break;
        case 0x20: r[rt] = static_cast<std::uint32_t>(static_cast<std::int8_t>(read8(r[rs] + static_cast<std::uint32_t>(simm(w))))); break;
        case 0x21: r[rt] = static_cast<std::uint32_t>(static_cast<std::int16_t>(read16(r[rs] + static_cast<std::uint32_t>(simm(w))))); break;
        case 0x22: {
            const std::uint32_t a = r[rs] + static_cast<std::uint32_t>(simm(w)), shift = (3u - (a & 3u)) * 8u;
            const std::uint32_t mem = read32(a & ~3u);
            r[rt] = (mem << shift) | (shift ? (r[rt] & ((1u << shift) - 1u)) : 0u);
            break;
        }
        case 0x23: r[rt] = read32(r[rs] + static_cast<std::uint32_t>(simm(w))); break;
        case 0x24: r[rt] = read8(r[rs] + static_cast<std::uint32_t>(simm(w))); break;
        case 0x25: r[rt] = read16(r[rs] + static_cast<std::uint32_t>(simm(w))); break;
        case 0x26: {
            const std::uint32_t a = r[rs] + static_cast<std::uint32_t>(simm(w)), shift = (a & 3u) * 8u;
            const std::uint32_t mem = read32(a & ~3u);
            r[rt] = (mem >> shift) | (shift ? (r[rt] & ~(0xFFFFFFFFu >> shift)) : 0u);
            break;
        }
        case 0x28: write8(r[rs] + static_cast<std::uint32_t>(simm(w)), static_cast<std::uint8_t>(r[rt])); break;
        case 0x29: write16(r[rs] + static_cast<std::uint32_t>(simm(w)), static_cast<std::uint16_t>(r[rt])); break;
        case 0x2A: {
            const std::uint32_t a = r[rs] + static_cast<std::uint32_t>(simm(w)), shift = (3u - (a & 3u)) * 8u;
            const std::uint32_t mem = read32(a & ~3u);
            write32(a & ~3u, (r[rt] >> shift) | (shift ? (mem & ~(0xFFFFFFFFu >> shift)) : 0u));
            break;
        }
        case 0x2B: write32(r[rs] + static_cast<std::uint32_t>(simm(w)), r[rt]); break;
        case 0x2E: {
            const std::uint32_t a = r[rs] + static_cast<std::uint32_t>(simm(w)), shift = (a & 3u) * 8u;
            const std::uint32_t mem = read32(a & ~3u);
            write32(a & ~3u, (r[rt] << shift) | (shift ? (mem & ((1u << shift) - 1u)) : 0u));
            break;
        }
        case 0x2F: break;
        case kHleOpcode:
            c.pc = r[31];
            c.npc = c.pc + 4;
            r[0] = 0;
            call_hle(c, w & 0x03FFFFFFu);
            r[0] = 0;
            if (kernel::switch_pending()) return;
            continue;
        default:
            bad(c, pc, w);
            return;
        }
        r[0] = 0;
        if (kernel::switch_pending()) return;
    }
}

constexpr std::uint32_t kCode = 0x00010000u, kData = 0x00100000u, kDataBytes = 0x10000u;

std::uint32_t i_type(std::uint32_t op, std::uint32_t rs, std::uint32_t rt, std::uint32_t imm) { return op << 26 | rs << 21 | rt << 16 | (imm & 0xFFFFu); }
std::uint32_t r_type(std::uint32_t funct, std::uint32_t rs, std::uint32_t rt, std::uint32_t rd, std::uint32_t sa) {
    return rs << 21 | rt << 16 | rd << 11 | sa << 6 | funct;
}

// a random program of n words at kCode whose loads and stores use r1 (kseg0) and r2 (a RAM mirror) as bases into the data
// area, branches and jumps that stay inside the program, and a jump back to the start at the end
std::vector<std::uint32_t> random_program(std::mt19937 &rng, unsigned n) {
    auto pick = [&](unsigned k) { return static_cast<std::uint32_t>(rng() % k); };
    auto dest = [&]() { const std::uint32_t d = pick(31); return d == 0 ? 0u : d + 2u > 31u ? 0u : d + 2u; };  // never r1, r2
    std::vector<std::uint32_t> p;
    static const std::uint32_t kAlu[] = {0x00, 0x02, 0x03, 0x04, 0x06, 0x07, 0x0F, 0x10, 0x11, 0x12, 0x13, 0x18, 0x19, 0x1A, 0x1B,
                                         0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x2A, 0x2B};
    static const std::uint32_t kLoads[] = {0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26}, kStores[] = {0x28, 0x29, 0x2A, 0x2B, 0x2E};
    for (unsigned i = 0; i + 2 < n; ++i) {
        const std::uint32_t kind = pick(100), rs = pick(32), rt = pick(32);
        const auto offset = static_cast<std::uint32_t>(static_cast<std::int32_t>(pick(n)) - static_cast<std::int32_t>(i) - 1);
        const std::uint32_t base = pick(2) ? 1u : 2u, disp = pick(0x1000) - 0x800u;
        if (kind < 35) p.push_back(r_type(kAlu[pick(std::size(kAlu))], rs, rt, dest(), pick(32)));
        else if (kind < 55) p.push_back(i_type(0x08 + pick(8), rs, dest(), rng()));
        else if (kind < 68) p.push_back(i_type(kLoads[pick(std::size(kLoads))], base, dest(), disp));
        else if (kind < 80) p.push_back(i_type(kStores[pick(std::size(kStores))], base, rt, disp));
        else if (kind < 88) p.push_back(i_type(0x04 + pick(4), rs, rt, offset));
        else if (kind < 92) { static const std::uint32_t kRt[] = {0, 1, 0x10, 0x11}; p.push_back(i_type(0x01, rs, kRt[pick(4)], offset)); }
        else if (kind < 95) p.push_back((pick(2) ? 0x02u : 0x03u) << 26 | (((kCode >> 2) + pick(n)) & 0x03FFFFFFu));
        else if (kind < 97) p.push_back(0x10u << 26 | 0u << 21 | dest() << 16 | (12u + pick(4)) << 11);       // mfc0
        else if (kind < 99) p.push_back(0x10u << 26 | 4u << 21 | rt << 16 | (12u + pick(3)) << 11);         // mtc0
        else p.push_back(0x42000010u);                                                                      // rfe
    }
    p.push_back(0x02u << 26 | (kCode >> 2));  // j start
    p.push_back(0);
    return p;
}

std::uint64_t hash_area(std::uint32_t at, std::uint32_t bytes) {
    std::uint64_t h = 1469598103934665603ull;
    for (std::uint32_t i = 0; i < bytes; i += 8) {
        std::uint64_t v;
        std::memcpy(&v, g_ram + at + i, 8);
        h = (h ^ v) * 1099511628211ull;
    }
    return h;
}

struct Snap {
    Cpu cpu;
    std::int64_t budget;
    std::uint64_t ram;
};

std::vector<Snap> run_slices(void (*run)(Cpu &, std::int64_t &), const std::vector<std::uint32_t> &budgets) {
    std::vector<Snap> out;
    out.reserve(budgets.size());
    for (const std::uint32_t b : budgets) {
        std::int64_t budget = b;
        run(g_cpu, budget);
        kernel::g_switch = false;
        out.push_back(Snap{g_cpu, budget, hash_area(kCode, 0x1000u) ^ hash_area(kData, kDataBytes) * 3u});
    }
    return out;
}

void random_programs(unsigned count) {
    std::mt19937 rng(20261111);
    for (unsigned t = 0; t < count; ++t) {
        const unsigned n = 16 + rng() % 400;
        const std::vector<std::uint32_t> prog = random_program(rng, n);
        std::vector<std::uint8_t> data(kDataBytes);
        for (auto &b : data) b = static_cast<std::uint8_t>(rng());
        Cpu start{};
        for (auto &x : start.r) x = rng();
        start.r[0] = 0;
        start.r[1] = 0x80000000u | (kData + kDataBytes / 2);
        start.r[2] = 0x00200000u | (kData + kDataBytes / 2);  // the RAM's first mirror
        if (t % 5 == 4) start.r[2] = 0x00200000u | (kCode + 0x800u);  // loads and stores through r2 hit the program itself (blocks)
        start.hi = rng(), start.lo = rng(), start.status = rng(), start.cause = rng(), start.epc = rng();
        static const std::uint32_t kSeg[] = {0x00000000u, 0x80000000u, 0xA0000000u, 0x00600000u};
        start.pc = kSeg[t % 4] | kCode;
        start.npc = start.pc + 4;
        std::vector<std::uint32_t> budgets;
        for (std::uint32_t total = 0; total < 60000;) budgets.push_back(1 + rng() % 400), total += budgets.back();
        g_watch_lo = g_watch_hi = 0;
        if (t % 3 == 2) g_watch_lo = 0x1F0000u, g_watch_hi = 0x1F0004u;  // stores the slow way, through write32 (nothing is written there)

        std::vector<Snap> runs[2];
        for (int which = 0; which < 2; ++which) {
            std::memcpy(g_ram + kCode, prog.data(), prog.size() * 4);
            std::memset(g_ram + kCode + prog.size() * 4, 0, 0x1000u - prog.size() * 4);
            std::memcpy(g_ram + kData, data.data(), kDataBytes);
            g_cpu = start;
            runs[which] = run_slices(which ? ref_execute : execute, budgets);
        }
        g_watch_lo = g_watch_hi = 0;
        bool same = true;
        for (std::size_t i = 0; i < budgets.size() && same; ++i) {
            const Snap &a = runs[0][i], &b = runs[1][i];
            same = std::memcmp(&a.cpu, &b.cpu, sizeof(Cpu)) == 0 && a.budget == b.budget && a.ram == b.ram;
            if (!same)
                check(false, "program " + std::to_string(t) + " slice " + std::to_string(i) + ": pc " + std::to_string(a.cpu.pc) + " / " +
                                 std::to_string(b.cpu.pc));
        }
        if (same) check(true, "");
    }
}

// one instruction at `pc`, budget 1
void step(std::uint32_t pc) {
    g_cpu.pc = pc;
    g_cpu.npc = pc + 4;
    std::int64_t budget = 1;
    execute(g_cpu, budget);
}

void code_changes() {
    const std::uint32_t at = 0x00020000u;
    // a store of the IOP itself over an instruction that has been run (and decoded) before
    set_ram32(at, i_type(0x09, 0, 3, 1));  // addiu r3, r0, 1
    g_cpu.r[3] = 0;
    step(at);
    check(g_cpu.r[3] == 1, "first run of the instruction");
    set_ram32(at + 4, i_type(0x2B, 5, 4, 0));  // sw r4, 0(r5)
    g_cpu.r[4] = i_type(0x09, 0, 3, 2);         // addiu r3, r0, 2
    g_cpu.r[5] = 0x80000000u | at;
    step(at + 4);
    step(at);
    check(g_cpu.r[3] == 2, "the IOP's own store over a decoded instruction");
    // the same through a byte store (SB) into the immediate
    g_cpu.r[4] = 7;
    set_ram32(at + 4, i_type(0x28, 5, 4, 0));  // sb r4, 0(r5)
    step(at + 4);
    step(at);
    check(g_cpu.r[3] == 7, "a byte store into a decoded instruction");
    // the host writes code, as a module load, a relocation or a DMA into IOP RAM does
    const std::uint32_t lui = i_type(0x0F, 0, 3, 0x1234);
    std::memcpy(g_ram + at, &lui, 4);
    step(at);
    check(g_cpu.r[3] == 0x12340000u, "a host write over a decoded instruction");
    // a save state puts older RAM back
    std::vector<std::uint8_t> saved(g_ram, g_ram + kRamSize);
    set_ram32(at, i_type(0x0D, 0, 3, 0x55));  // ori r3, r0, 0x55
    step(at);
    check(g_cpu.r[3] == 0x55, "the new code runs");
    std::memcpy(g_ram, saved.data(), kRamSize);
    step(at);
    check(g_cpu.r[3] == 0x12340000u, "the old code runs after the RAM is put back");
    // the same word run through a mirror and kseg1, then changed through the mirror's address
    step(0xA0200000u | at);
    check(g_cpu.r[3] == 0x12340000u, "run through a mirror");
    g_cpu.r[4] = i_type(0x09, 0, 3, 9);
    g_cpu.r[5] = 0x00600000u | at;
    set_ram32(at + 4, i_type(0x2B, 5, 4, 0));
    step(at + 4);
    step(at);
    check(g_cpu.r[3] == 9, "a store through a mirror over a decoded instruction");
    // a register operation into r0 (decoded as a no-op) leaves r0 at 0 and the others alone
    g_cpu.r[6] = 5;
    set_ram32(at, r_type(0x21, 6, 6, 0, 0));  // addu r0, r6, r6
    step(at);
    check(g_cpu.r[0] == 0 && g_cpu.r[6] == 5, "addu into r0");
    // a load into r0 still reads (a hardware register would see the read) and r0 stays 0
    set_ram32(kData, 0xDEADBEEFu);
    g_cpu.r[5] = kData;
    set_ram32(at, i_type(0x23, 5, 0, 0));  // lw r0, 0(r5)
    step(at);
    check(g_cpu.r[0] == 0, "lw into r0");
}

// Blocks: code changes while the instructions run as a block (a large budget), new interpreter against the old one
void block_changes() {
    const std::uint32_t at = 0x00030000u;
    auto run_both = [&](const std::vector<std::uint32_t> &prog, std::int64_t budget, const char *what) {
        Cpu out[2];
        std::int64_t left[2];
        for (int which = 0; which < 2; ++which) {
            std::memcpy(g_ram + at, prog.data(), prog.size() * 4);
            g_cpu = Cpu{};
            g_cpu.r[4] = i_type(0x09, 0, 3, 5);  // addiu r3, r0, 5
            g_cpu.r[5] = 0x80000000u | at;
            g_cpu.pc = at, g_cpu.npc = at + 4;
            left[which] = budget;
            (which ? ref_execute : execute)(g_cpu, left[which]);
            out[which] = g_cpu;
        }
        check(std::memcmp(&out[0], &out[1], sizeof(Cpu)) == 0 && left[0] == left[1], what);
        return out[0];
    };
    // a store over a later instruction of the same block, and over the delay slot of its branch
    const std::uint32_t loop_end = 0x02u << 26 | (((at + 0x40u) >> 2) & 0x03FFFFFFu);  // j at+0x40
    std::vector<std::uint32_t> p = {i_type(0x09, 0, 3, 1), i_type(0x2B, 5, 4, 12), 0, i_type(0x09, 0, 3, 2), loop_end, i_type(0x09, 3, 6, 1)};
    p.resize(16, 0);
    p.push_back(0x0000000Du);  // break (at+0x40: the jump's target)
    Cpu c = run_both(p, 64, "a store over a later instruction of the same block");
    check(c.r[3] == 5 && c.r[6] == 6, "the changed instruction ran in the block");
    p[1] = i_type(0x2B, 5, 4, 20);  // over the delay slot
    c = run_both(p, 64, "a store over the delay slot of the block's jump");
    check(c.r[3] == 5 && c.r[6] == 0, "the changed delay slot ran");
    // the same block run again after the host changed it between two calls, and with a budget smaller than the block
    p[1] = 0;
    run_both(p, 64, "the block as built");
    p[3] = i_type(0x09, 0, 3, 3);
    c = run_both(p, 64, "the block after the host changed it");
    check(c.r[3] == 3, "the host's change ran");
    for (std::int64_t b = 1; b < 9; ++b) run_both(p, b, "a budget smaller than the block");
}

void bench() {
    std::mt19937 rng(7);
    std::vector<std::uint32_t> prog = random_program(rng, 256);
    for (auto &w : prog)  // no stores, so the code stays as it is
        if ((w >> 26) >= 0x28 && (w >> 26) <= 0x2E) w = i_type(0x09, 0, 3, 1);
    std::memcpy(g_ram + kCode, prog.data(), prog.size() * 4);
    for (int which = 0; which < 2; ++which) {
        g_cpu = Cpu{};
        g_cpu.r[1] = 0x80000000u | (kData + kDataBytes / 2);
        g_cpu.r[2] = 0x00200000u | (kData + kDataBytes / 2);
        g_cpu.pc = kCode, g_cpu.npc = kCode + 4;
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < 20000; ++i) {
            std::int64_t budget = 512;
            (which ? ref_execute : execute)(g_cpu, budget);
        }
        const double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / (20000.0 * 512);
        std::printf("%s: %.2f ns per instruction\n", which ? "before (decode every time)" : "now (decode table)", ns);
    }
}

}

int main(int argc, char **argv) {
    random_programs(300);
    code_changes();
    block_changes();
    if (argc > 1 && std::string(argv[1]) == "bench") bench();
    std::printf("iop decode: %d failed (of %d checks)\n", failed, checks);
    return failed ? 1 : 0;
}
