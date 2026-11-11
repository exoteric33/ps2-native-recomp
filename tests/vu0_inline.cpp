// The four-lane forms of the first vector unit's macro instructions (runtime/include/ps2/vu.hpp, the functions that end
// in 4) against the one-lane helpers they replace in the recompiler's output. No game is needed.
//
//   1. Every instruction alone, on random and on picked values (zeros of both signs, denormals, the smallest and the
//      largest numbers, exponents of 255 with every kind of mantissa, operands whose exponents lie close together or
//      far apart, sums and products on the edge of underflow and overflow), with every destination mask and with the
//      registers of the instruction being the same register in every combination. With the flags wanted the whole
//      machine state must be the same; with the flags not wanted everything but the MAC register and the low four
//      status bits.
//   2. Random straight lines of instructions, run once with the old helpers and once the way the recompiler writes
//      them: the flags are left out where recomp::vu0_flags_dead says nothing can see them. Wherever something could
//      look (an instruction that is not known to be harmless, and the end of the line) the two machines must be the
//      same, bit for bit.
//   3. The same lines with a liveness that is wrong on purpose (flags dropped everywhere; flags dropped across a
//      reader). The comparison of part 2 must notice both, or the test could not see a dropped flag.
//   4. A few lines written by hand with the answers recomp::vu0_flags_dead must give.
//
// Usage: ps2vu0inlinetest [rounds]   (rounds multiplies the number of random cases; 1 when missing)
#include "ps2/vu.hpp"
#include "r5900.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

using namespace ps2;
using recomp::Insn;
using recomp::Op;
using recomp::Vu0Flags;

namespace {

std::mt19937_64 rng(0x56553049u);
std::uint32_t r32() { return static_cast<std::uint32_t>(rng()); }
unsigned pick(unsigned n) { return static_cast<unsigned>(rng() % n); }

const std::uint32_t kSpecial[] = {
    0x00000000u, 0x80000000u, 0x00000001u, 0x80000001u, 0x007FFFFFu, 0x807FFFFFu, 0x00400000u, 0x00800000u, 0x80800000u, 0x00800001u,
    0x00FFFFFFu, 0x01000000u, 0x01000001u, 0x3F800000u, 0xBF800000u, 0x3F7FFFFFu, 0x3F800001u, 0x40000000u, 0x3F000000u, 0x4B800000u,
    0x4B7FFFFFu, 0x4C000000u, 0x33800000u, 0x7F7FFFFFu, 0xFF7FFFFFu, 0x7F7FFFFEu, 0x7F000000u, 0x7EFFFFFFu, 0x7F800000u, 0xFF800000u,
    0x7F800001u, 0x7FC00000u, 0xFFC00000u, 0x7FFFFFFFu, 0xFFFFFFFFu, 0x7FFFFFFEu, 0x7FBFFFFFu, 0x00000003u, 0x0000000Eu, 0x000003E8u,
};

std::uint32_t mantissa() {
    switch (pick(8)) {
    case 0: return 0u;
    case 1: return 0x7FFFFFu;
    case 2: return 1u << pick(23);
    case 3: return 0x7FFFFFu & ~((1u << pick(23)) - 1u);
    case 4: return (r32() & 0x7FFFFFu) & (~0u << pick(23));
    case 5: return (r32() & 0x7FFFFFu) | ((1u << pick(23)) - 1u);
    default: return r32() & 0x7FFFFFu;
    }
}

int clamp_exponent(int e) { return std::clamp(e, 0, 255); }

// The exponent a case is centred on: sums need operands close together, products meet their edges where the two
// exponents add up to about 127 (underflow) or 382 (overflow).
int base_exponent() {
    static const int kEdges[] = {63, 64, 65, 126, 127, 128, 190, 191, 192};
    switch (pick(4)) {
    case 0: return static_cast<int>(pick(256));
    case 1: return kEdges[pick(9)];
    case 2: return static_cast<int>(pick(30));
    default: return 225 + static_cast<int>(pick(31));
    }
}

std::uint32_t value(int base) {
    const unsigned kind = pick(16);
    if (kind < 2) return r32();
    if (kind < 4) return kSpecial[pick(sizeof kSpecial / sizeof *kSpecial)];
    int e;
    if (kind < 11) e = clamp_exponent(base + static_cast<int>(pick(61)) - 30);
    else if (kind < 12) e = clamp_exponent(base + static_cast<int>(pick(5)) - 2);
    else if (kind < 13) e = static_cast<int>(pick(4));
    else if (kind < 14) e = 252 + static_cast<int>(pick(4));
    else e = static_cast<int>(pick(256));
    return (r32() & 0x80000000u) | (static_cast<std::uint32_t>(e) << 23) | mantissa();
}

void fill(Vu0Regs &v, unsigned registers) {
    std::memset(static_cast<void *>(&v), 0, sizeof v);
    const int base = base_exponent();
    for (unsigned r = 0; r < registers; ++r)
        for (auto &w : v.vf[r].w) w = value(base);
    v.vf[0].w[0] = v.vf[0].w[1] = v.vf[0].w[2] = 0u;
    v.vf[0].w[3] = fp::kOne;
    // The accumulator near the operands, or near their product.
    const int acc_base = pick(2) ? base : clamp_exponent(2 * base - 127 + static_cast<int>(pick(9)) - 4);
    for (auto &w : v.acc.w) w = value(acc_base);
    v.q = value(base);
    v.i = value(base);
    v.mac = r32() & 0xFFFFu;
    v.status = r32() & (pick(4) ? 0xFFFu : 0xFFFFFFFFu);
    v.clip = r32() & 0xFFFFFFu;
}

bool same(const Vu0Regs &a, const Vu0Regs &b) { return std::memcmp(&a, &b, sizeof a) == 0; }
void copy(Vu0Regs &to, const Vu0Regs &from) { std::memcpy(static_cast<void *>(&to), &from, sizeof to); }

// The same but for what an instruction may leave undone when its flags are not wanted.
bool same_but_flags(const Vu0Regs &x, const Vu0Regs &y) {
    Vu0Regs a, b;
    copy(a, x);
    copy(b, y);
    a.mac = b.mac = 0u;
    a.status &= 0xFF0u;
    b.status &= 0xFF0u;
    return same(a, b);
}

void show(const char *what, const Vu0Regs &before, const Vu0Regs &want, const Vu0Regs &got) {
    std::printf("  %s\n", what);
    for (unsigned r = 0; r < 4; ++r)
        std::printf("    vf%u   before %08X %08X %08X %08X  want %08X %08X %08X %08X  got %08X %08X %08X %08X\n", r, before.vf[r].w[0], before.vf[r].w[1],
                    before.vf[r].w[2], before.vf[r].w[3], want.vf[r].w[0], want.vf[r].w[1], want.vf[r].w[2], want.vf[r].w[3], got.vf[r].w[0], got.vf[r].w[1],
                    got.vf[r].w[2], got.vf[r].w[3]);
    std::printf("    acc   before %08X %08X %08X %08X  want %08X %08X %08X %08X  got %08X %08X %08X %08X\n", before.acc.w[0], before.acc.w[1], before.acc.w[2],
                before.acc.w[3], want.acc.w[0], want.acc.w[1], want.acc.w[2], want.acc.w[3], got.acc.w[0], got.acc.w[1], got.acc.w[2], got.acc.w[3]);
    std::printf("    mac %04X status %03X  want mac %04X status %03X  got mac %04X status %03X\n", before.mac, before.status, want.mac, want.status, got.mac,
                got.status);
}

// --- part 1: every instruction alone

using Call = void (*)(Vu0Regs &v, unsigned dest, unsigned a, unsigned b, unsigned c, std::uint32_t bc);
struct Single {
    const char *name;
    bool sets_flags;
    Call old_form, with_flags, without_flags;
};

#define P Vu0Regs &v, [[maybe_unused]] unsigned dest, [[maybe_unused]] unsigned a, [[maybe_unused]] unsigned b, [[maybe_unused]] unsigned c, [[maybe_unused]] std::uint32_t bc
#define ARITH(name, ...) \
    {#name, true, [](P) { vu::name(v, __VA_ARGS__); }, [](P) { vu::name##4<true>(v, __VA_ARGS__); }, [](P) { vu::name##4<false>(v, __VA_ARGS__); }}
#define PLAIN(name, name4, ...) {#name, false, [](P) { vu::name(v, __VA_ARGS__); }, [](P) { vu::name4(v, __VA_ARGS__); }, nullptr}
const Single kSingles[] = {
    ARITH(add, dest, a, b, c),    ARITH(addbc, dest, a, b, bc),  ARITH(sub, dest, a, b, c),    ARITH(subbc, dest, a, b, bc),
    ARITH(mul, dest, a, b, c),    ARITH(mulbc, dest, a, b, bc),  ARITH(madd, dest, a, b, c),   ARITH(maddbc, dest, a, b, bc),
    ARITH(msub, dest, a, b, c),   ARITH(msubbc, dest, a, b, bc), ARITH(adda, dest, b, c),      ARITH(addabc, dest, b, bc),
    ARITH(suba, dest, b, c),      ARITH(subabc, dest, b, bc),    ARITH(mula, dest, b, c),      ARITH(mulabc, dest, b, bc),
    ARITH(madda, dest, b, c),     ARITH(maddabc, dest, b, bc),   ARITH(msuba, dest, b, c),     ARITH(msubabc, dest, b, bc),
    ARITH(opmula, b, c),          ARITH(opmsub, a, b, c),
    PLAIN(max, max4, dest, a, b, c),     PLAIN(maxbc, maxbc4, dest, a, b, bc), PLAIN(mini, mini4, dest, a, b, c), PLAIN(minibc, minibc4, dest, a, b, bc),
    PLAIN(abs, abs4, dest, a, b),        PLAIN(move, move4, dest, a, b),       PLAIN(mr32, mr32_4, dest, a, b),
};
#undef ARITH
#undef PLAIN
#undef P

int singles(std::uint64_t cases) {
    int failures = 0;
    for (const Single &op : kSingles) {
        int shown = 0;
        for (std::uint64_t n = 0; n < cases; ++n) {
            Vu0Regs before;
            fill(before, 4);
            const unsigned dest = static_cast<unsigned>(n & 15u), a = pick(4), b = pick(4), c = pick(4);
            const std::uint32_t bc = pick(3) ? before.vf[c].w[pick(4)] : value(base_exponent());
            Vu0Regs want, got;
            copy(want, before);
            copy(got, before);
            op.old_form(want, dest, a, b, c, bc);
            op.with_flags(got, dest, a, b, c, bc);
            bool ok = same(want, got);
            const char *what = "with flags";
            if (ok && op.without_flags) {
                copy(got, before);
                op.without_flags(got, dest, a, b, c, bc);
                ok = same_but_flags(want, got);
                what = "without flags";
            }
            if (ok) continue;
            ++failures;
            if (shown++ < 3) {
                std::printf("FAIL %s dest %u registers %u %u %u bc %08X\n", op.name, dest, a, b, c, bc);
                show(what, before, want, got);
            }
        }
    }
    return failures;
}

// --- part 2: straight lines, the way the recompiler writes them

enum Mode { Old, WithFlags, WithoutFlags };

// One instruction on a machine; false when this test does not know the instruction. The calls are the ones
// recomp/codegen.cpp writes for the instruction.
bool exec(Vu0Regs &v, const Insn &i, Mode mode) {
    using enum Op;
    const unsigned dest = i.dest(), fd = i.fd(), fs = i.fs(), ft = i.ft();
    const unsigned fsf = (i.word >> 21) & 3u, ftf = (i.word >> 23) & 3u;
    const std::uint32_t bcv = v.vf[ft].w[i.bc()];
#define ARITH(name, ...) \
    do { \
        if (mode == Old) vu::name(v, __VA_ARGS__); \
        else if (mode == WithFlags) vu::name##4<true>(v, __VA_ARGS__); \
        else vu::name##4<false>(v, __VA_ARGS__); \
        return true; \
    } while (0)
#define PLAIN(name, name4, ...) \
    do { \
        if (mode == Old) vu::name(v, __VA_ARGS__); \
        else vu::name4(v, __VA_ARGS__); \
        return true; \
    } while (0)
    switch (i.op) {
    case Vaddbc: ARITH(addbc, dest, fd, fs, bcv);
    case Vsubbc: ARITH(subbc, dest, fd, fs, bcv);
    case Vmaddbc: ARITH(maddbc, dest, fd, fs, bcv);
    case Vmsubbc: ARITH(msubbc, dest, fd, fs, bcv);
    case Vmulbc: ARITH(mulbc, dest, fd, fs, bcv);
    case Vmulq: ARITH(mulbc, dest, fd, fs, v.q);
    case Vmuli: ARITH(mulbc, dest, fd, fs, v.i);
    case Vaddq: ARITH(addbc, dest, fd, fs, v.q);
    case Vmaddq: ARITH(maddbc, dest, fd, fs, v.q);
    case Vaddi: ARITH(addbc, dest, fd, fs, v.i);
    case Vmaddi: ARITH(maddbc, dest, fd, fs, v.i);
    case Vsubq: ARITH(subbc, dest, fd, fs, v.q);
    case Vmsubq: ARITH(msubbc, dest, fd, fs, v.q);
    case Vsubi: ARITH(subbc, dest, fd, fs, v.i);
    case Vmsubi: ARITH(msubbc, dest, fd, fs, v.i);
    case Vadd: ARITH(add, dest, fd, fs, ft);
    case Vmadd: ARITH(madd, dest, fd, fs, ft);
    case Vmul: ARITH(mul, dest, fd, fs, ft);
    case Vsub: ARITH(sub, dest, fd, fs, ft);
    case Vmsub: ARITH(msub, dest, fd, fs, ft);
    case Vopmsub: ARITH(opmsub, fd, fs, ft);
    case Vaddabc: ARITH(addabc, dest, fs, bcv);
    case Vsubabc: ARITH(subabc, dest, fs, bcv);
    case Vmaddabc: ARITH(maddabc, dest, fs, bcv);
    case Vmsubabc: ARITH(msubabc, dest, fs, bcv);
    case Vmulabc: ARITH(mulabc, dest, fs, bcv);
    case Vmulaq: ARITH(mulabc, dest, fs, v.q);
    case Vmulai: ARITH(mulabc, dest, fs, v.i);
    case Vaddaq: ARITH(addabc, dest, fs, v.q);
    case Vmaddaq: ARITH(maddabc, dest, fs, v.q);
    case Vaddai: ARITH(addabc, dest, fs, v.i);
    case Vmaddai: ARITH(maddabc, dest, fs, v.i);
    case Vsubaq: ARITH(subabc, dest, fs, v.q);
    case Vmsubaq: ARITH(msubabc, dest, fs, v.q);
    case Vsubai: ARITH(subabc, dest, fs, v.i);
    case Vmsubai: ARITH(msubabc, dest, fs, v.i);
    case Vadda: ARITH(adda, dest, fs, ft);
    case Vmadda: ARITH(madda, dest, fs, ft);
    case Vmula: ARITH(mula, dest, fs, ft);
    case Vsuba: ARITH(suba, dest, fs, ft);
    case Vmsuba: ARITH(msuba, dest, fs, ft);
    case Vopmula: ARITH(opmula, fs, ft);
    case Vmaxbc: PLAIN(maxbc, maxbc4, dest, fd, fs, bcv);
    case Vminibc: PLAIN(minibc, minibc4, dest, fd, fs, bcv);
    case Vmaxi: PLAIN(maxbc, maxbc4, dest, fd, fs, v.i);
    case Vminii: PLAIN(minibc, minibc4, dest, fd, fs, v.i);
    case Vmax: PLAIN(max, max4, dest, fd, fs, ft);
    case Vmini: PLAIN(mini, mini4, dest, fd, fs, ft);
    case Vabs: PLAIN(abs, abs4, dest, ft, fs);
    case Vmove: PLAIN(move, move4, dest, ft, fs);
    case Vmr32: PLAIN(mr32, mr32_4, dest, ft, fs);
    // Instructions between the others that leave the flags alone; the same call for both machines.
    case Vclip: vu::clip(v, fs, ft); return true;
    case Vdiv: vu::div(v, v.vf[fs].w[fsf], v.vf[ft].w[ftf]); return true;
    case Vsqrt: vu::sqrt(v, v.vf[ft].w[ftf]); return true;
    case Vrsqrt: vu::rsqrt(v, v.vf[fs].w[fsf], v.vf[ft].w[ftf]); return true;
    case Vftoi0: vu::ftoi(v, dest, ft, fs, 0); return true;
    case Vftoi4: vu::ftoi(v, dest, ft, fs, 4); return true;
    case Vitof0: vu::itof(v, dest, ft, fs, 0); return true;
    case Vitof12: vu::itof(v, dest, ft, fs, 12); return true;
    case Vnop: case Addiu: case Sll: return true;
    default: return false;
    }
#undef ARITH
#undef PLAIN
}

constexpr std::uint32_t kCfc2Status = 0x48400000u | (16u << 11) | (8u << 16);  // cfc2 t0, $16: reads the status register
constexpr std::uint32_t kLoad = 0x8C880000u;                                    // lw t0, 0(a0): could be a hardware register
constexpr std::uint32_t kAddiu = 0x24840004u;                                   // addiu a0, a0, 4

std::uint32_t random_word() {
    for (;;) {
        const unsigned kind = pick(20);
        if (kind == 0) return kCfc2Status;
        if (kind == 1) return kLoad;
        if (kind == 2) return kAddiu;
        // A macro instruction with registers 0 to 3, so that the instructions of a line feed each other.
        std::uint32_t word = 0x4A000000u | (pick(16) << 21) | (pick(4) << 16) | (pick(4) << 11) | (r32() & 0x7FFu);
        if ((word & 0x3Fu) < 0x3Cu) word = (word & ~0x7C0u) | (pick(4) << 6);
        const Insn insn = recomp::decode(word, 0);
        Vu0Regs scratch{};
        if (recomp::vu0_flag_effect(insn) != Vu0Flags::Reads && exec(scratch, insn, Old)) return word;
    }
}

enum Liveness { Right, DropAll, DropAcrossReaders };

// Runs one line on two machines; true when they are the same wherever something could look.
bool run_line(const std::vector<Insn> &line, const Vu0Regs &start, Liveness liveness, bool report) {
    Vu0Regs want, got;
    copy(want, start);
    copy(got, start);
    const auto next = [&](std::uint32_t pc, Insn &insn) {
        if (pc / 4u >= line.size()) return false;
        insn = line[pc / 4u];
        if (liveness == DropAcrossReaders && recomp::vu0_flag_effect(insn) == Vu0Flags::Reads) insn = recomp::decode(kAddiu, pc);
        return true;
    };
    for (const Insn &insn : line) {
        const Vu0Flags effect = recomp::vu0_flag_effect(insn);
        bool dead = false;
        if (effect == Vu0Flags::Reads) {
            if (!same(want, got)) {
                if (report) {
                    std::printf("FAIL line differs at %s\n", recomp::disassemble(insn).c_str());
                    show("where something could look", start, want, got);
                }
                return false;
            }
            continue;
        }
        if (effect == Vu0Flags::Sets) dead = liveness == DropAll || recomp::vu0_flags_dead(insn.pc, next);
        exec(want, insn, Old);
        exec(got, insn, dead ? WithoutFlags : WithFlags);
    }
    if (same(want, got)) return true;
    if (report) {
        std::printf("FAIL line differs at its end\n");
        show("at the end", start, want, got);
    }
    return false;
}

int lines(std::uint64_t count) {
    int failures = 0, shown = 0;
    std::uint64_t caught_all = 0, caught_readers = 0, dropped = 0, setters = 0;
    for (std::uint64_t n = 0; n < count; ++n) {
        std::vector<Insn> line(2 + pick(14));
        for (std::size_t k = 0; k < line.size(); ++k) line[k] = recomp::decode(random_word(), static_cast<std::uint32_t>(4 * k));
        Vu0Regs start;
        fill(start, 4);
        if (!run_line(line, start, Right, shown < 3)) {
            ++failures;
            if (shown++ < 3)
                for (const Insn &insn : line) std::printf("    %s\n", recomp::disassemble(insn).c_str());
        }
        caught_all += !run_line(line, start, DropAll, false);
        caught_readers += !run_line(line, start, DropAcrossReaders, false);
        const auto next = [&](std::uint32_t pc, Insn &insn) {
            if (pc / 4u >= line.size()) return false;
            insn = line[pc / 4u];
            return true;
        };
        for (const Insn &insn : line) {
            if (recomp::vu0_flag_effect(insn) != Vu0Flags::Sets) continue;
            ++setters;
            dropped += recomp::vu0_flags_dead(insn.pc, next);
        }
    }
    std::printf("lines: %llu, flag-setting instructions %llu, flags left out at %llu of them\n", static_cast<unsigned long long>(count),
                static_cast<unsigned long long>(setters), static_cast<unsigned long long>(dropped));
    std::printf("wrong on purpose: flags dropped everywhere noticed in %llu lines, flags dropped across a reader in %llu lines\n",
                static_cast<unsigned long long>(caught_all), static_cast<unsigned long long>(caught_readers));
    if (!caught_all || !caught_readers) {
        std::printf("FAIL the comparison did not notice a dropped flag\n");
        ++failures;
    }
    if (!dropped) {
        std::printf("FAIL no flag was ever left out, so nothing was tested\n");
        ++failures;
    }
    return failures;
}

// --- part 4: lines with known answers

constexpr std::uint32_t kVadd = 0x4BE110E8u;                   // vadd.xyzw vf3, vf2, vf1
constexpr std::uint32_t kVmove = 0x4BE0133Cu;                  // vmove.xyzw vf0, vf2
constexpr std::uint32_t kQmfc2 = 0x48280800u;                  // qmfc2 t0, vf1
constexpr std::uint32_t kVcallms = 0x4A000038u;                // vcallms 0
constexpr std::uint32_t kStore = 0xAC880000u;                  // sw t0, 0(a0)
constexpr std::uint32_t kBeq = 0x10000001u;                    // beq zero, zero, +1
constexpr std::uint32_t kSyscall = 0x0000000Cu;

int known() {
    struct Case {
        const char *what;
        std::vector<std::uint32_t> words;
        bool dead;  // of the first instruction's flags
    };
    const Case cases[] = {
        {"the next instruction sets them again", {kVadd, kVadd}, true},
        {"register work in between", {kVadd, kVmove, kAddiu, kQmfc2, kVadd}, true},
        {"the end of the code", {kVadd}, false},
        {"only register work up to the end", {kVadd, kVmove, kAddiu}, false},
        {"cfc2 of the status register", {kVadd, kCfc2Status, kVadd}, false},
        {"a load", {kVadd, kLoad, kVadd}, false},
        {"a store", {kVadd, kStore, kVadd}, false},
        {"a vector program is called", {kVadd, kVcallms, kVadd}, false},
        {"qmfc2 with the interlock bit", {kVadd, kQmfc2 | 1u, kVadd}, false},
        {"a branch", {kVadd, kBeq, kVadd}, false},
        {"a syscall", {kVadd, kSyscall, kVadd}, false},
    };
    int failures = 0;
    for (const Case &c : cases) {
        const auto next = [&](std::uint32_t pc, Insn &insn) {
            if (pc / 4u >= c.words.size()) return false;
            insn = recomp::decode(c.words[pc / 4u], pc);
            return recomp::flow(insn.op) == recomp::Flow::None;
        };
        const Insn first = recomp::decode(c.words[0], 0);
        if (recomp::vu0_flag_effect(first) != Vu0Flags::Sets) {
            std::printf("FAIL known line \"%s\": %s is not seen as setting the flags\n", c.what, recomp::disassemble(first).c_str());
            ++failures;
        } else if (recomp::vu0_flags_dead(0, next) != c.dead) {
            std::printf("FAIL known line \"%s\": the flags should be %s\n", c.what, c.dead ? "left out" : "kept");
            ++failures;
        }
    }
    return failures;
}

}

int main(int argc, char **argv) {
    const std::uint64_t rounds = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 1;
    const std::uint64_t cases = 160000 * (rounds ? rounds : 1);
    int failures = known();
    failures += singles(cases);
    std::printf("single instructions: %zu kinds, %llu cases each\n", sizeof kSingles / sizeof *kSingles, static_cast<unsigned long long>(cases));
    failures += lines(cases);
    std::printf(failures ? "FAILED: %d\n" : "ok\n", failures);
    return failures ? 1 : 0;
}
