#pragma once

#include "cpu.hpp"
#include "fpu.hpp"
#include "vu.hpp"

#if defined(__SSE4_1__)
#include <smmintrin.h>
#elif defined(__SSE2__)
#include <emmintrin.h>
#endif

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#define PS2_VU_INLINE inline __attribute__((always_inline))

namespace ps2::vu {

void kick(int unit, std::uint32_t address);
std::uint32_t top();
std::uint32_t itop();
std::uint32_t itop(int unit);  // VU0 reads VIF0; the no-argument form retains the VU1 live-library ABI.
void interpret(int unit, std::uint32_t pc);
// AOT VU0 checks for a host stop at the same interval as the reference loop.
bool stopped();
// A compiled VU0 program that calls subroutines (vurecomp_gen.cpp, vu0_calls): the key of the program that starts at `entry` in the VU0
// code memory as it is now, and the way out when a call goes somewhere the program does not know (the registers and data memory are
// already back as they were at the start; the whole program runs in the interpreter).
std::uint64_t vu0_key(std::uint32_t entry);
void vu0_restart(std::uint32_t entry);

using ProgramFn = void (*)(Vu0Regs &r, std::uint8_t *mem);

struct Program {
    std::uint64_t key;
    ProgramFn function;
    int unit = 1;
};

void register_programs(const Program *programs, std::uint32_t count);

// The functions of the runner that generated code calls. A library made while the game runs (runtime/src/vu_live.cpp) is not
// linked to the runner: it gets these four in a table when it is loaded.
struct LiveHost {
    void (*interpret)(int unit, std::uint32_t pc);
    void (*kick)(int unit, std::uint32_t address);
    std::uint32_t (*top)();
    std::uint32_t (*itop)();
};

inline std::uint32_t lower_major(std::uint32_t lo) { return lo >> 25; }
// Flag reads, ILW/ILWR, and XTOP/XITOP bypass the VI delay seen by branch operands.
inline bool branch_delay_exempt(std::uint32_t lo) {
    const std::uint32_t op = lower_major(lo);
    if (op == 0x40 && (lo & 0x3Cu) == 0x3Cu) {
        const unsigned index = ((lo >> 6) & 31u) * 4u + (lo & 3u);
        return index == 0x3E || index == 0x68 || index == 0x69;
    }
    switch (op) {
    case 0x04:  // ILW does not incur branch VI delay
    case 0x10: case 0x12: case 0x13: case 0x14: case 0x16: case 0x17: case 0x18: case 0x1A: case 0x1B: case 0x1C: case 0x21: case 0x25: return true;
    default: return false;
    }
}
inline unsigned vi_written_reg(std::uint32_t lo, std::uint32_t hi) {
    if (hi & 0x80000000u) return 0;
    const unsigned dest = (lo >> 21) & 15u, it = (lo >> 16) & 15u, is = (lo >> 11) & 15u, id = (lo >> 6) & 15u;
    if ((lo >> 25) == 0x40u) {
        const unsigned funct = lo & 63u;
        if (funct < 0x3C) return funct == 0x32 ? it : (funct == 0x30 || funct == 0x31 || funct == 0x34 || funct == 0x35) ? id : 0;
        switch (((lo >> 6) & 31u) * 4u + (funct & 3u)) {
        case 0x34: case 0x36: return is;
        case 0x35: case 0x37: case 0x3C: case 0x68: case 0x69: return it;
        case 0x3E: return dest ? it : 0;
        default: return 0;
        }
    }
    switch (lo >> 25) {
    case 0x04: return dest ? it : 0;
    case 0x08: case 0x09: case 0x10: case 0x12: case 0x13: case 0x14: case 0x16: case 0x17: case 0x18: case 0x1A: case 0x1B: case 0x1C: case 0x21: case 0x25: return it;
    case 0x11: case 0x15: case 0x19: return 1u;
    default: return 0;
    }
}
inline bool is_branch(std::uint32_t lo) {
    const std::uint32_t op = lower_major(lo);
    return op == 0x20 || op == 0x21 || op == 0x24 || op == 0x25 || (op >= 0x28 && op <= 0x2F && op != 0x2A && op != 0x2B);
}
// A branch whose delay slot holds another branch: the compiled and the Wasm code leave it to the interpreter (upstream c243416).
inline bool branch_in_delay_slot(const std::uint8_t *code, std::uint32_t mask, std::uint32_t pc) {
    std::uint32_t lo, hi, next_lo, next_hi;
    std::memcpy(&lo, code + (pc & mask), 4);
    std::memcpy(&hi, code + ((pc + 4u) & mask), 4);
    std::memcpy(&next_lo, code + ((pc + 8u) & mask), 4);
    std::memcpy(&next_hi, code + ((pc + 12u) & mask), 4);
    return !(hi & 0x80000000u) && is_branch(lo) && !(next_hi & 0x80000000u) && is_branch(next_lo);
}
inline bool is_jump_register(std::uint32_t lo) { return lower_major(lo) == 0x24 || lower_major(lo) == 0x25; }
inline bool is_unconditional(std::uint32_t lo) { return lower_major(lo) == 0x20 || lower_major(lo) == 0x21; }
inline bool links(std::uint32_t lo) { return lower_major(lo) == 0x21 || lower_major(lo) == 0x25; }
inline std::uint32_t branch_target(std::uint32_t lo, std::uint32_t pc, std::uint32_t mask) {
    const std::int32_t imm11 = static_cast<std::int32_t>(lo << 21) >> 21;
    return (pc + 8u + static_cast<std::uint32_t>(imm11) * 8u) & mask;
}

inline std::vector<std::uint32_t> reachable(const std::uint8_t *code, std::uint32_t mask, std::uint32_t entry) {
    std::vector<std::uint8_t> seen((mask + 1) / 8, 0);
    std::vector<std::uint32_t> work{entry & mask}, out;
    while (!work.empty()) {
        std::uint32_t pc = work.back();
        work.pop_back();
        int stop_after = -1;
        for (int guard = 0; guard < 4096; ++guard) {
            if (seen[pc >> 3]) break;
            seen[pc >> 3] = 1;
            out.push_back(pc);
            if (stop_after == 0) break;
            if (stop_after > 0) --stop_after;
            std::uint32_t lo, hi;
            std::memcpy(&lo, code + pc, 4);
            std::memcpy(&hi, code + pc + 4, 4);
            if (hi & 0x40000000u) stop_after = 0;
            if (!(hi & 0x80000000u) && is_branch(lo)) {
                if (!is_jump_register(lo)) work.push_back(branch_target(lo, pc, mask));
                if (links(lo)) work.push_back((pc + 16u) & mask);
                if (is_unconditional(lo) || is_jump_register(lo)) stop_after = 0;
            }
            pc = (pc + 8u) & mask;
        }
    }
    std::vector<std::uint32_t> sorted(out);
    std::sort(sorted.begin(), sorted.end());
    return sorted;
}

inline std::uint32_t q_unit_latency(std::uint32_t lo, std::uint32_t hi) {
    if ((hi & 0x80000000u) || lower_major(lo) != 0x40 || (lo & 0x3Cu) != 0x3Cu) return 0;
    const unsigned index = ((lo >> 6) & 31u) * 4u + (lo & 3u);
    return index == 0x38 || index == 0x39 ? 7u : index == 0x3A ? 13u : 0u;
}

inline bool waits_q(std::uint32_t lo, std::uint32_t hi) {
    return !(hi & 0x80000000u) && lower_major(lo) == 0x40 && (lo & 0x3Fu) == 0x3Fu && ((lo >> 6) & 31u) == 0x0Eu;
}

// What an instruction pair reads and writes that the pipeline can still be busy with: the vector lanes it needs and the ones
// it writes (register * 4 + lane), the integer registers it reads and the one an ILW loads, and the divide unit (a WAITQ or
// a new divide waits for the quotient of the last one, q_set is the latency of the divide it starts).
struct VfUse {
    std::uint8_t lanes_read[16] = {}, lanes_written[8] = {};
    std::uint8_t nr = 0, nw = 0, vi_load = 0, q_wait = 0, q_set = 0;
    std::uint16_t vi_reads = 0;
    void read_vi(unsigned r) { vi_reads = static_cast<std::uint16_t>(vi_reads | ((1u << (r & 15u)) & ~1u)); }
    void read(unsigned r, unsigned m) {
        if (r)
            for (unsigned lane = 0; lane < 4; ++lane)
                if (m & (8u >> lane)) lanes_read[nr++] = static_cast<std::uint8_t>(r * 4u + lane);
    }
    void write(unsigned r, unsigned m) {
        if (r)
            for (unsigned lane = 0; lane < 4; ++lane)
                if (m & (8u >> lane)) lanes_written[nw++] = static_cast<std::uint8_t>(r * 4u + lane);
    }
};

inline VfUse vf_use(std::uint32_t lo, std::uint32_t hi) {
    VfUse u;
    const unsigned dest = (hi >> 21) & 15u, ft = (hi >> 16) & 31u, fs = (hi >> 11) & 31u, fd = (hi >> 6) & 31u, bc = 8u >> (hi & 3u), funct = hi & 63u;
    if (funct < 0x3C) {
        if (funct < 0x1C) {
            u.read(fs, dest);
            u.read(ft, bc);
        } else if (funct < 0x28) {
            u.read(fs, dest);
        } else if (funct == 0x2E) {
            u.read(fs, 0xE);
            u.read(ft, 0xE);
        } else {
            u.read(fs, dest);
            u.read(ft, dest);
        }
        u.write(fd, funct == 0x2E ? 0xE : dest);
    } else {
        const unsigned index = ((hi >> 6) & 31u) * 4u + (funct & 3u);
        if (index < 0x10 || (index >= 0x18 && index < 0x1C)) {
            u.read(fs, dest);
            u.read(ft, bc);
        } else if (index < 0x18 || index == 0x1D) {
            u.read(fs, dest);
            u.write(ft, dest);
        } else if (index == 0x1F) {
            u.read(fs, 0xE);
            u.read(ft, 1);
        } else if (index < 0x28) {
            u.read(fs, dest);
        } else if (index == 0x2E) {
            u.read(fs, 0xE);
            u.read(ft, 0xE);
        } else if (index < 0x2F) {
            u.read(fs, dest);
            u.read(ft, dest);
        }
    }
    if (hi & 0x80000000u) return u;
    const unsigned ldest = (lo >> 21) & 15u, lft = (lo >> 16) & 31u, lfs = (lo >> 11) & 31u, fsf = 8u >> ((lo >> 21) & 3u), ftf = 8u >> ((lo >> 23) & 3u);
    const unsigned it = (lo >> 16) & 15u, is = (lo >> 11) & 15u, major = lower_major(lo);
    if (major == 0x00 || major == 0x08 || major == 0x09 || major == 0x18 || major == 0x1A || major == 0x1B || major == 0x24 || major == 0x25 || (major >= 0x2C && major <= 0x2F)) u.read_vi(is);
    if (major == 0x01) u.read_vi(it);
    if (major == 0x04 || major == 0x05 || major == 0x28 || major == 0x29) u.read_vi(is);
    if (major == 0x05 || major == 0x28 || major == 0x29) u.read_vi(it);
    if (major == 0x04) u.vi_load = it;
    if (major == 0x40 && (lo & 0x3Cu) != 0x3Cu) {
        const unsigned funct = lo & 0x3Fu;
        if (funct == 0x30 || funct == 0x31 || funct == 0x32 || funct == 0x34 || funct == 0x35) u.read_vi(is);
        if (funct == 0x30 || funct == 0x31 || funct == 0x34 || funct == 0x35) u.read_vi(it);
    }
    if (major == 0x40 && (lo & 0x3Cu) == 0x3Cu) {
        const unsigned index = ((lo >> 6) & 31u) * 4u + (lo & 3u);
        if (index == 0x34 || index == 0x36 || index == 0x3D || index == 0x3E || index == 0x3F || index == 0x6C) u.read_vi(is);
        if (index == 0x35 || index == 0x37 || index == 0x3F) u.read_vi(it);
        if (index == 0x3E) u.vi_load = it;
    }
    if (major == 0x00) u.write(lft, ldest);
    else if (major == 0x01) u.read(lfs, ldest);
    else if (major == 0x40 && (lo & 0x3Cu) == 0x3Cu) {
        const unsigned index = ((lo >> 6) & 31u) * 4u + (lo & 3u);
        switch (index) {
        case 0x30: u.read(lfs, ldest); u.write(lft, ldest); break;
        case 0x31: u.read(lfs, ((ldest >> 1) | (ldest << 3)) & 15u); u.write(lft, ldest); break;
        case 0x34: case 0x36: case 0x3D: case 0x40: case 0x41: case 0x64: u.write(lft, ldest); break;
        case 0x35: case 0x37: u.read(lfs, ldest); break;
        case 0x38: case 0x3A: u.read(lfs, fsf); u.read(lft, ftf); break;
        case 0x39: u.read(lft, ftf); break;
        case 0x3C: case 0x42: case 0x43: u.read(lfs, fsf); break;
        default: if (index >= 0x70) u.read(lfs, 0xF); break;
        }
    }
    u.q_set = static_cast<std::uint8_t>(q_unit_latency(lo, hi));
    u.q_wait = u.q_set || waits_q(lo, hi);
    return u;
}

inline std::uint32_t issue(const VfUse &u, std::uint32_t cycle, const std::uint32_t (&ready)[32][4], const std::uint32_t (&vi_ready)[16]) {
    const std::uint32_t *lanes = &ready[0][0];
    for (unsigned i = 0; i < u.nr; ++i) cycle = std::max(cycle, lanes[u.lanes_read[i]]);
    for (unsigned m = u.vi_reads; m; m &= m - 1u) cycle = std::max(cycle, vi_ready[__builtin_ctz(m)]);
    return cycle;
}

inline void retire(const VfUse &u, std::uint32_t cycle, std::uint32_t (&ready)[32][4], std::uint32_t (&vi_ready)[16]) {
    std::uint32_t *lanes = &ready[0][0];
    for (unsigned i = 0; i < u.nw; ++i) lanes[u.lanes_written[i]] = cycle + 4u;
    if (u.vi_load) vi_ready[u.vi_load] = cycle + 4u;
}

// The cycle an instruction pair issues at when it may not before `cycle`. The interpreters count these cycles in
// FlagPipe::t: the pair waits for the registers it reads (and a WAITQ or divide for the last quotient, q_ready), so a
// result that was four cycles away when it was written can be read by the third instruction after it, or by the first.
inline std::uint32_t issue_cycle(const VfUse &u, std::uint32_t cycle, const std::uint32_t (&ready)[32][4], const std::uint32_t (&vi_ready)[16], std::uint32_t q_ready) {
    cycle = issue(u, cycle, ready, vi_ready);
    return u.q_wait ? std::max(cycle, q_ready) : cycle;
}

// The same bookkeeping done ahead of time for a compiled program, where the stall of every instruction is known. The state
// before an instruction is how many cycles are left until each result it could wait for is ready (vector lanes 0 to 127,
// integer registers 128 to 143, the quotient 144); the stall is the longest of those minus the cycle the instruction gets
// anyway. Where two paths reach an instruction with different states and the stalls differ, the compiled program keeps the
// index of the state it is in (`tracked`).
constexpr unsigned kPipeVi = 128, kPipeQ = 144, kPipeSize = 145;
using PipeState = std::array<std::uint8_t, kPipeSize>;

inline unsigned pipe_stall(const PipeState &s, const VfUse &u) {
    unsigned wait = 0;
    for (unsigned i = 0; i < u.nr; ++i) wait = std::max<unsigned>(wait, s[u.lanes_read[i]]);
    for (unsigned m = u.vi_reads; m; m &= m - 1u) wait = std::max<unsigned>(wait, s[kPipeVi + static_cast<unsigned>(__builtin_ctz(m))]);
    if (u.q_wait) wait = std::max<unsigned>(wait, s[kPipeQ]);
    return wait ? wait - 1u : 0u;
}

inline PipeState pipe_step(const PipeState &s, const VfUse &u, unsigned stall) {
    PipeState o;
    for (unsigned i = 0; i < kPipeSize; ++i) o[i] = s[i] > stall + 1u ? static_cast<std::uint8_t>(s[i] - stall - 1u) : 0;
    for (unsigned i = 0; i < u.nw; ++i) o[u.lanes_written[i]] = 4;
    if (u.vi_load) o[kPipeVi + u.vi_load] = 4;
    if (u.q_set) o[kPipeQ] = u.q_set;
    return o;
}

struct StallPlan {
    struct Edge {
        std::uint32_t from, to;
        std::vector<std::uint8_t> next;  // the state index at `to` for each state index at `from`
    };
    bool ok = true;                                    // false: more paths than the plan keeps apart
    bool any_tracked = false;
    unsigned start = 0;                                // the state index at the entry
    std::vector<std::uint8_t> stall;                   // by pc / 8: the stall, for the instructions every path agrees on
    std::vector<std::uint8_t> tracked;                 // by pc / 8: the stall depends on the path
    std::vector<std::vector<std::uint8_t>> by_state;   // by pc / 8, tracked ones: the stall in each state
    std::vector<Edge> edges;                           // the edges into tracked instructions

    const Edge *edge(std::uint32_t from, std::uint32_t to) const {
        for (const Edge &e : edges)
            if (e.from == from && e.to == to) return &e;
        return nullptr;
    }
};

// `jump_targets` (VU0 programs compiled with their subroutines, vurecomp_gen.cpp): where a jump through a register (JR, JALR) can go
// from its delay slot. Without it such a jump leaves the program, as for VU1.
inline StallPlan stall_plan(const std::uint8_t *code, std::uint32_t mask, const std::vector<std::uint32_t> &pcs, std::uint32_t entry,
                            const std::vector<std::uint32_t> *jump_targets = nullptr) {
    constexpr std::size_t kMaxStates = 24, kMaxSteps = 200000;
    const std::size_t slots = (mask + 1u) / 8u;
    StallPlan plan;
    plan.stall.assign(slots, 0);
    plan.tracked.assign(slots, 0);
    plan.by_state.resize(slots);
    entry &= mask;
    std::vector<VfUse> use(slots);
    std::vector<std::uint32_t> lower(slots, 0), upper(slots, 0);
    std::vector<std::uint8_t> in(slots, 0), direct(slots, 0);
    for (std::uint32_t pc : pcs) {
        const std::size_t at = pc >> 3;
        std::memcpy(&lower[at], code + pc, 4);
        std::memcpy(&upper[at], code + pc + 4, 4);
        use[at] = vf_use(lower[at], upper[at]);
        in[at] = 1;
    }
    direct[entry >> 3] = 1;
    std::vector<std::uint32_t> dynamic;
    if (jump_targets)
        for (std::uint32_t t : *jump_targets)
            if (in[(t & mask) >> 3]) {
                dynamic.push_back(t & mask);
                direct[(t & mask) >> 3] = 1;
            }
    for (std::uint32_t pc : pcs) {
        const std::size_t at = pc >> 3;
        if (!(upper[at] & 0x80000000u) && is_branch(lower[at]) && !is_jump_register(lower[at])) direct[(branch_target(lower[at], pc, mask) & mask) >> 3] = 1;
    }
    // What can follow an instruction: its neighbour, and when the one before it is a branch (it is a delay slot) the branch
    // target; when that one ends the program or jumps through a register, nothing. An instruction that is also a jump
    // target or the entry can be reached without that branch having run, so the neighbour stays possible for it.
    struct Next {
        std::uint32_t pc[2];
        unsigned n = 0;
        const std::vector<std::uint32_t> *more = nullptr;
        unsigned size() const { return n + (more ? static_cast<unsigned>(more->size()) : 0u); }
        std::uint32_t at(unsigned k) const { return k < n ? pc[k] : (*more)[k - n]; }
    };
    const auto successors = [&](std::uint32_t pc) {
        Next s;
        const std::uint32_t prev = (pc - 8u) & mask, after = (pc + 8u) & mask;
        const std::size_t p = prev >> 3;
        bool slot = false, continues = true;
        if (in[p]) {
            const bool lower_op = !(upper[p] & 0x80000000u);
            if (upper[p] & 0x40000000u || (lower_op && is_jump_register(lower[p]))) {
                slot = true;
                continues = false;
                if (!(upper[p] & 0x40000000u) && !dynamic.empty()) s.more = &dynamic;
            } else if (lower_op && is_branch(lower[p])) {
                slot = true;
                continues = !is_unconditional(lower[p]);
                const std::uint32_t target = branch_target(lower[p], prev, mask) & mask;
                if (in[target >> 3]) s.pc[s.n++] = target;
            }
        }
        if ((continues || (slot && direct[pc >> 3])) && in[after >> 3] && !(s.n && s.pc[0] == after)) s.pc[s.n++] = after;
        return s;
    };
    // Which entries of the state can still make an instruction wait: the vector lanes and integer registers it or the two
    // after it read (an entry is at most 4 and loses one per instruction), the quotient when a WAITQ or divide follows
    // within 12 instructions (it is at most 13).
    struct Need {
        std::uint64_t w[3] = {};
        bool test(unsigned i) const { return (w[i >> 6] >> (i & 63u)) & 1u; }
        void set(unsigned i) { w[i >> 6] |= 1ull << (i & 63u); }
        void merge(const Need &o) {
            for (int i = 0; i < 3; ++i) w[i] |= o.w[i];
        }
    };
    std::vector<Need> reads(slots), near1(slots), near2(slots);
    std::vector<std::uint8_t> q_distance(slots, 255);
    for (std::uint32_t pc : pcs) {
        const std::size_t at = pc >> 3;
        const VfUse &u = use[at];
        for (unsigned i = 0; i < u.nr; ++i) reads[at].set(u.lanes_read[i]);
        for (unsigned m = u.vi_reads; m; m &= m - 1u) reads[at].set(kPipeVi + static_cast<unsigned>(__builtin_ctz(m)));
        if (u.q_wait) q_distance[at] = 0;
    }
    for (int round = 0; round < 2; ++round) {
        std::vector<Need> &from = round ? near1 : reads, &to = round ? near2 : near1;
        for (std::uint32_t pc : pcs) {
            const std::size_t at = pc >> 3;
            to[at] = reads[at];
            const Next s = successors(pc);
            for (unsigned k = 0; k < s.size(); ++k) to[at].merge(from[s.at(k) >> 3]);
        }
    }
    for (int round = 0; round < 12; ++round)
        for (std::uint32_t pc : pcs) {
            const Next s = successors(pc);
            for (unsigned k = 0; k < s.size(); ++k) q_distance[pc >> 3] = static_cast<std::uint8_t>(std::min<unsigned>(q_distance[pc >> 3], q_distance[s.at(k) >> 3] + 1u));
        }
    const auto canonical = [&](PipeState s, std::uint32_t pc) {
        for (unsigned i = 0; i < kPipeQ; ++i)
            if (!near2[pc >> 3].test(i)) s[i] = 0;
        if (q_distance[pc >> 3] > 11) s[kPipeQ] = 0;
        return s;
    };
    std::vector<std::vector<PipeState>> states(slots);
    std::vector<std::size_t> done(slots, 0);
    std::vector<std::uint32_t> work;
    const auto reach = [&](std::uint32_t pc, const PipeState &raw) {
        const PipeState s = canonical(raw, pc);
        std::vector<PipeState> &v = states[pc >> 3];
        for (const PipeState &known : v)
            if (known == s) return;
        if (v.size() >= kMaxStates) {
            plan.ok = false;
            return;
        }
        v.push_back(s);
        work.push_back(pc);
    };
    reach(entry, PipeState{});
    for (std::size_t steps = 0; !work.empty() && plan.ok; ++steps) {
        if (steps > kMaxSteps) {
            plan.ok = false;
            break;
        }
        const std::uint32_t pc = work.back();
        work.pop_back();
        const std::size_t at = pc >> 3;
        const Next s = successors(pc);
        for (; done[at] < states[at].size(); ++done[at]) {
            const PipeState now = states[at][done[at]];
            const PipeState after = pipe_step(now, use[at], pipe_stall(now, use[at]));
            for (unsigned k = 0; k < s.size(); ++k) reach(s.at(k), after);
        }
    }
    if (!plan.ok) return plan;
    for (std::uint32_t pc : pcs) {
        const std::size_t at = pc >> 3;
        const std::vector<PipeState> &v = states[at];
        if (v.empty()) continue;
        std::vector<std::uint8_t> stalls;
        for (const PipeState &s : v) stalls.push_back(static_cast<std::uint8_t>(pipe_stall(s, use[at])));
        plan.stall[at] = stalls[0];
        if (v.size() > 1) {
            plan.tracked[at] = 1;
            plan.any_tracked = true;
            plan.by_state[at] = stalls;
        }
        const Next s = successors(pc);
        for (unsigned k = 0; k < s.size(); ++k) {
            const std::vector<PipeState> &target = states[s.at(k) >> 3];
            if (target.size() < 2) continue;
            StallPlan::Edge e{pc, s.at(k), {}};
            for (std::size_t i = 0; i < v.size(); ++i) {
                const PipeState after = canonical(pipe_step(v[i], use[at], stalls[i]), s.at(k));
                std::size_t found = 0;
                while (found < target.size() && !(target[found] == after)) ++found;
                if (found == target.size()) return plan.ok = false, plan;
                e.next.push_back(static_cast<std::uint8_t>(found));
            }
            plan.edges.push_back(std::move(e));
        }
    }
    for (std::size_t i = 0; i < states[entry >> 3].size(); ++i)
        if (states[entry >> 3][i] == PipeState{}) plan.start = static_cast<unsigned>(i);
    return plan;
}

inline bool reads_q(std::uint32_t hi) {
    const unsigned funct = hi & 63u, op = funct < 0x3C ? funct : ((hi >> 6) & 31u) * 4u + (funct & 3u);
    return op == 0x1C || op == 0x20 || op == 0x21 || op == 0x24 || op == 0x25;
}

enum QRead : std::uint8_t { kQDynamic, kQNew, kQOld };

inline QRead q_read_kind(const std::uint8_t *code, std::uint32_t mask, std::uint32_t pc) {
    const auto load = [&](std::uint32_t a, std::uint32_t &lo, std::uint32_t &hi) {
        std::memcpy(&lo, code + (a & mask), 4);
        std::memcpy(&hi, code + (a & mask) + 4, 4);
    };
    {
        std::uint32_t lo, hi;
        load(pc, lo, hi);
        if (waits_q(lo, hi) || q_unit_latency(lo, hi)) return kQNew;
    }
    std::vector<std::pair<std::uint32_t, std::uint32_t>> jumps;
    for (std::uint32_t a = 0; a <= mask; a += 8) {
        std::uint32_t lo, hi;
        load(a, lo, hi);
        if (!(hi & 0x80000000u) && is_branch(lo) && !is_jump_register(lo)) jumps.emplace_back(branch_target(lo, a, mask), (a + 8u) & mask);
    }
    const auto preds = [&](std::uint32_t y) {
        std::vector<std::uint32_t> out;
        std::uint32_t lo, hi;
        load(y - 16u, lo, hi);
        const bool jumped = !(hi & 0x80000000u) && is_branch(lo) && (is_unconditional(lo) || is_jump_register(lo));
        if (!jumped && !(hi & 0x40000000u)) out.push_back((y - 8u) & mask);
        for (const auto &[target, slot] : jumps)
            if (target == (y & mask)) out.push_back(slot);
        return out;
    };
    const auto complete = [&](const std::vector<std::uint32_t> &back, std::uint32_t latency) {
        std::vector<std::uint32_t> order;
        const std::uint32_t div = back.back();
        for (std::uint32_t k = 3; k > 0; --k) order.push_back((div - 8u * k) & mask);
        order.insert(order.end(), back.rbegin(), back.rend());
        std::uint32_t ready[32][4] = {}, vi_ready[16] = {};
        std::uint32_t cycle = 0, issued = 0;
        for (std::size_t i = 0; i < order.size(); ++i) {
            std::uint32_t lo, hi;
            load(order[i], lo, hi);
            const VfUse u = vf_use(lo, hi);
            cycle = issue(u, cycle + 1u, ready, vi_ready);
            if (i == 3) issued = cycle;
            retire(u, cycle, ready, vi_ready);
        }
        return cycle >= issued + latency;
    };
    bool any_new = false, any_old = false;
    unsigned budget = 4096;
    std::vector<std::vector<std::uint32_t>> stack{{pc & mask}};
    while (!stack.empty()) {
        if (!--budget) return kQDynamic;
        const std::vector<std::uint32_t> path = std::move(stack.back());
        stack.pop_back();
        const std::uint32_t at = path.back();
        if (path.size() > 1) {
            std::uint32_t lo, hi;
            load(at, lo, hi);
            if (const std::uint32_t latency = q_unit_latency(lo, hi)) {
                (complete(path, latency) ? any_new : any_old) = true;
                continue;
            }
            if (waits_q(lo, hi) || path.size() > 15) {
                any_new = true;
                continue;
            }
        }
        const std::vector<std::uint32_t> from = preds(at);
        if (from.empty()) any_new = true;
        for (std::uint32_t p : from) {
            std::vector<std::uint32_t> next = path;
            next.push_back(p);
            stack.push_back(std::move(next));
        }
        if (any_new && any_old) return kQDynamic;
    }
    return any_old ? kQOld : kQNew;
}

inline std::uint64_t program_key(const std::uint8_t *code, const std::vector<std::uint32_t> &pcs, std::uint32_t entry) {
    std::uint64_t h = 0xCBF29CE484222325ull ^ entry;
    const auto mix = [&](std::uint32_t v) {
        for (int i = 0; i < 4; ++i) {
            h ^= (v >> (i * 8)) & 0xFFu;
            h *= 0x100000001B3ull;
        }
    };
    for (std::uint32_t pc : pcs) {
        std::uint32_t lo, hi;
        std::memcpy(&lo, code + pc, 4);
        std::memcpy(&hi, code + pc + 4, 4);
        mix(pc);
        mix(lo);
        mix(hi);
    }
    return h;
}

}

namespace ps2::vuops {

PS2_VU_INLINE float fin(std::uint32_t x) {
    const std::uint32_t e = x & 0x7F800000u;
    if (!e) x &= 0x80000000u;
    else if (e == 0x7F800000u) x = (x & 0x80000000u) | 0x7F7FFFFFu;
    float f;
    std::memcpy(&f, &x, 4);
    return f;
}

PS2_VU_INLINE std::uint32_t fout(float f) {
    std::uint32_t x;
    std::memcpy(&x, &f, 4);
    const std::uint32_t e = x & 0x7F800000u;
    if (!e) return x & 0x80000000u;
    if (e == 0x7F800000u) return (x & 0x80000000u) | 0x7F7FFFFFu;
    return x;
}

PS2_VU_INLINE float norm(float f) { return fin(fout(f)); }
PS2_VU_INLINE std::uint32_t add(std::uint32_t a, std::uint32_t b) { return fout(fin(a) + fin(b)); }
PS2_VU_INLINE std::uint32_t sub(std::uint32_t a, std::uint32_t b) { return fout(fin(a) - fin(b)); }
PS2_VU_INLINE std::uint32_t mul(std::uint32_t a, std::uint32_t b) { return fout(fin(a) * fin(b)); }
PS2_VU_INLINE std::uint32_t madd(std::uint32_t acc, std::uint32_t a, std::uint32_t b) { return fout(fin(acc) + norm(fin(a) * fin(b))); }
PS2_VU_INLINE std::uint32_t msub(std::uint32_t acc, std::uint32_t a, std::uint32_t b) { return fout(fin(acc) - norm(fin(a) * fin(b))); }
PS2_VU_INLINE std::uint32_t max(std::uint32_t a, std::uint32_t b) { return fp::vu_max(a, b); }
PS2_VU_INLINE std::uint32_t min(std::uint32_t a, std::uint32_t b) { return fp::vu_min(a, b); }

typedef std::uint32_t v4u __attribute__((vector_size(16)));
typedef std::int32_t v4i __attribute__((vector_size(16)));
typedef float v4f __attribute__((vector_size(16)));

PS2_VU_INLINE v4u vload(const Reg128 &r) {
    v4u v;
    std::memcpy(&v, &r, 16);
    return v;
}
PS2_VU_INLINE void vstore(Reg128 &r, v4u v) { std::memcpy(&r, &v, 16); }
PS2_VU_INLINE v4u vload_ptr(const void *p) {
    v4u v;
    std::memcpy(&v, p, 16);
    return v;
}
PS2_VU_INLINE v4u vsplat(std::uint32_t x) { return v4u{x, x, x, x}; }
PS2_VU_INLINE v4u vsel(v4u old, v4u now, v4u mask) { return (now & mask) | (old & ~mask); }
// What the vector units do to every number they take in or give out: an exponent of 255 is an ordinary number that is cut
// down to the largest float (0x7F7FFFFF), and a denormal becomes a zero with its sign.
PS2_VU_INLINE v4u vfix_generic(v4u x) {
    const v4i clamped = __builtin_elementwise_min((v4i)(x & 0x7FFFFFFFu), (v4i){0x7F7FFFFF, 0x7F7FFFFF, 0x7F7FFFFF, 0x7F7FFFFF});
    const v4i normal = (v4f)clamped >= (v4f)(v4i){0x00800000, 0x00800000, 0x00800000, 0x00800000};
    return (x & 0x80000000u) | (v4u)(clamped & normal);
}
#if defined(__SSE4_1__)
// The same on every one of the 2^32 inputs (checked one by one against vfix_generic), as integer operations only. The magnitude is
// cut at the largest float with an unsigned minimum (an exponent of 255, NaNs included, ends up at 0x7F7FFFFF), everything below the
// smallest normal number is zeroed, and the sign is put back. Compared with the float version below (MINPS and CMPPS on the
// magnitude) it has the same six instructions, but the chain from input to output is five cycles long instead of eleven: the
// vector programs are one long chain of such steps (every add and multiply ends in one), which is where their time goes.
PS2_VU_INLINE v4u vfix(v4u x) {
    const __m128i xi = (__m128i)x;
    const __m128i magnitude = _mm_and_si128(xi, _mm_set1_epi32(0x7FFFFFFF));
    const __m128i clamped = _mm_min_epu32(magnitude, _mm_set1_epi32(0x7F7FFFFF));
    const __m128i normal = _mm_cmpgt_epi32(clamped, _mm_set1_epi32(0x007FFFFF));
    return (v4u)_mm_or_si128(_mm_and_si128(xi, _mm_set1_epi32(static_cast<int>(0x80000000u))), _mm_and_si128(clamped, normal));
}
#elif defined(__SSE2__)
// The same on every one of the 2^32 inputs (checked one by one against vfix_generic), in fewer instructions: the SSE2 integer
// minimum only exists for 16-bit lanes and takes four instructions for 32 bits, but MINPS on the magnitude read as a float does the
// cut for all the cases. It hands back its second operand when the first is a NaN (an exponent of 255 with a mantissa) and the
// largest float is the answer in that case too, and +infinity is above it. A denormal is smaller than the smallest normal number,
// which the compare finds (the MXCSR denormal flags are at their default, so the compare sees the true value).
PS2_VU_INLINE v4u vfix(v4u x) {
    const __m128i xi = (__m128i)x;
    const __m128 magnitude = _mm_castsi128_ps(_mm_and_si128(xi, _mm_set1_epi32(0x7FFFFFFF)));
    const __m128 clamped = _mm_min_ps(magnitude, _mm_castsi128_ps(_mm_set1_epi32(0x7F7FFFFF)));
    const __m128 normal = _mm_cmpge_ps(clamped, _mm_castsi128_ps(_mm_set1_epi32(0x00800000)));
    return (v4u)_mm_or_si128(_mm_and_si128(xi, _mm_set1_epi32(static_cast<int>(0x80000000u))), _mm_castps_si128(_mm_and_ps(clamped, normal)));
}
#else
PS2_VU_INLINE v4u vfix(v4u x) { return vfix_generic(x); }
#endif
PS2_VU_INLINE v4f vin(v4u x) { return (v4f)vfix(x); }
PS2_VU_INLINE v4u vout(v4f f) { return vfix((v4u)f); }
PS2_VU_INLINE v4f vnorm(v4f f) { return (v4f)vfix((v4u)f); }
// The n variants take operands that already went through vfix.
PS2_VU_INLINE v4u vaddn(v4u a, v4u b) { return vout((v4f)a + (v4f)b); }
PS2_VU_INLINE v4u vsubn(v4u a, v4u b) { return vout((v4f)a - (v4f)b); }
PS2_VU_INLINE v4u vmuln(v4u a, v4u b) { return vout((v4f)a * (v4f)b); }
PS2_VU_INLINE v4u vmaddn(v4u acc, v4u a, v4u b) { return vout((v4f)acc + vnorm((v4f)a * (v4f)b)); }
PS2_VU_INLINE v4u vmsubn(v4u acc, v4u a, v4u b) { return vout((v4f)acc - vnorm((v4f)a * (v4f)b)); }
PS2_VU_INLINE v4u vadd(v4u a, v4u b) { return vaddn(vfix(a), vfix(b)); }
PS2_VU_INLINE v4u vsub(v4u a, v4u b) { return vsubn(vfix(a), vfix(b)); }
PS2_VU_INLINE v4u vmul(v4u a, v4u b) { return vmuln(vfix(a), vfix(b)); }
PS2_VU_INLINE v4u vmadd(v4u acc, v4u a, v4u b) { return vmaddn(vfix(acc), vfix(a), vfix(b)); }
PS2_VU_INLINE v4u vmsub(v4u acc, v4u a, v4u b) { return vmsubn(vfix(acc), vfix(a), vfix(b)); }
// The order of MAX and MINI (fp::order_key): sign and magnitude, -0 equal to +0, denormals not flushed (they must come out as they went in).
PS2_VU_INLINE v4i vkey(v4u x) {
    const v4i m = (v4i)(x & 0x7FFFFFFFu);
    const v4i negative = (v4i)((x & 0x80000000u) != 0u);
    return (m & ~negative) | (-m & negative);
}
PS2_VU_INLINE v4u vmax(v4u a, v4u b) {
    const v4u ge = (v4u)(vkey(a) >= vkey(b));
    return (a & ge) | (b & ~ge);
}
PS2_VU_INLINE v4u vmin(v4u a, v4u b) {
    const v4u le = (v4u)(vkey(a) <= vkey(b));
    return (a & le) | (b & ~le);
}
PS2_VU_INLINE v4u vabs(v4u a) { return a & 0x7FFFFFFFu; }
PS2_VU_INLINE v4u vftoi_generic(v4u x, int fraction) {
    const v4f f = (v4f)vfix(x) * static_cast<float>(1u << fraction);
    const v4i big = f >= 2147483648.0f;
    const v4i small = f < -2147483648.0f;
    const v4i out = big | small;
    const v4i t = __builtin_convertvector((v4f)((v4i)f & ~out), v4i);
    return (v4u)((t & ~out) | (big & 0x7FFFFFFF) | (small & (v4i)vsplat(0x80000000u)));
}
#if defined(__SSE2__)
// CVTTPS2DQ truncates, and answers 0x80000000 for everything that does not fit, which is the right answer below -2^31 (and for
// -2^31 itself); a number from 2^31 up wants 0x7FFFFFFF, which is that answer turned over bit by bit. Checked against vftoi_generic
// on every one of the 2^32 inputs for each of the four fractions.
PS2_VU_INLINE v4u vftoi(v4u x, int fraction) {
    const __m128 f = _mm_mul_ps((__m128)vfix(x), _mm_set1_ps(static_cast<float>(1u << fraction)));
    const __m128i t = _mm_cvttps_epi32(f);
    const __m128 big = _mm_cmpge_ps(f, _mm_set1_ps(2147483648.0f));
    return (v4u)_mm_xor_si128(t, _mm_castps_si128(big));
}
#else
PS2_VU_INLINE v4u vftoi(v4u x, int fraction) { return vftoi_generic(x, fraction); }
#endif
PS2_VU_INLINE v4u vitof(v4u x, int fraction) {
    const v4i v = (v4i)x;
    v4f f = __builtin_convertvector(v, v4f);
    if (__builtin_reduce_or((v4i)(x + 0x01000000u > 0x02000000u))) {
        const v4i big = f >= 2147483648.0f;
        const v4i back = __builtin_convertvector((v4f)((v4i)f & ~big), v4i);
        const v4i over = big | ((v >= 0) & (back > v)) | ((v < 0) & (back < v));
        f = (v4f)((v4i)f + over);
    }
    return (v4u)(f * (1.0f / static_cast<float>(1u << fraction)));
}

PS2_VU_INLINE void flags(Vu0Regs &r, const Reg128 &v, unsigned dest) {
    std::uint32_t mac = 0;
    for (int i = 0; i < 4; ++i) {
        if (!(dest & (8u >> i))) continue;
        const std::uint32_t bit = 3u - static_cast<std::uint32_t>(i);
        const std::uint32_t x = v.w[i];
        if (!(x & 0x7F800000u)) mac |= 1u << bit;
        if (x & 0x80000000u) mac |= 1u << (bit + 4);
        if ((x & 0x7FFFFFFFu) == 0x7FFFFFFFu) mac |= 1u << (bit + 12);
    }
    r.mac = mac;
    std::uint32_t status = r.status & 0xFF0u;
    if (mac & 0x000Fu) status |= 0x1u;
    if (mac & 0x00F0u) status |= 0x2u;
    if (mac & 0x0F00u) status |= 0x4u;
    if (mac & 0xF000u) status |= 0x8u;
    status |= (status & 0xFu) << 6;
    r.status = (status & ~0x30u) | (r.status & 0x30u);
}

#if defined(__SSE2__)
// The MAC and status flags of a result held in a register, from three movemasks. Lane x is bit 0 of a movemask and bit 3 of a
// flag nibble, hence the table that turns four bits around. Same flags as the version above for the lanes in `dest`
// (checked against it on random and special values).
inline constexpr std::uint8_t kReverse4[16] = {0, 8, 4, 12, 2, 10, 6, 14, 1, 9, 5, 13, 3, 11, 7, 15};
PS2_VU_INLINE void flags(Vu0Regs &r, v4u v, unsigned dest) {
    const __m128i x = (__m128i)v;
    const unsigned zero = static_cast<unsigned>(_mm_movemask_ps(_mm_castsi128_ps(_mm_cmpeq_epi32(_mm_and_si128(x, _mm_set1_epi32(0x7F800000)), _mm_setzero_si128()))));
    const unsigned sign = static_cast<unsigned>(_mm_movemask_ps(_mm_castsi128_ps(x)));
    const unsigned over = static_cast<unsigned>(_mm_movemask_ps(_mm_castsi128_ps(_mm_cmpeq_epi32(_mm_and_si128(x, _mm_set1_epi32(0x7FFFFFFF)), _mm_set1_epi32(0x7FFFFFFF)))));
    const std::uint32_t z = kReverse4[zero] & dest, s = kReverse4[sign] & dest, o = kReverse4[over] & dest;
    r.mac = z | (s << 4) | (o << 12);
    std::uint32_t status = r.status & 0xFF0u;
    if (z) status |= 0x1u;
    if (s) status |= 0x2u;
    if (o) status |= 0x8u;
    status |= (status & 0xFu) << 6;
    r.status = (status & ~0x30u) | (r.status & 0x30u);
}
#else
PS2_VU_INLINE void flags(Vu0Regs &r, v4u v, unsigned dest) {
    Reg128 uf;
    std::memcpy(&uf, &v, 16);
    flags(r, uf, dest);
}
#endif

PS2_VU_INLINE std::uint32_t clip_value(const Vu0Regs &r, const Reg128 &s, std::uint32_t w) {
    const std::int32_t lim = fp::key(w & fp::kAbs);
    std::uint32_t f = 0;
    for (int i = 0; i < 3; ++i) {
        const std::int32_t x = fp::key(s.w[i]);
        if (x > lim) f |= 1u << (i * 2);
        if (x < -lim) f |= 2u << (i * 2);
    }
    return ((r.clip << 6) | f) & 0xFFFFFFu;
}

PS2_VU_INLINE void clip(Vu0Regs &r, const Reg128 &s, std::uint32_t w) {
    const std::int32_t lim = fp::key(w & fp::kAbs);
    std::uint32_t f = 0;
    for (int i = 0; i < 3; ++i) {
        const std::int32_t x = fp::key(s.w[i]);
        if (x > lim) f |= 1u << (i * 2);
        if (x < -lim) f |= 2u << (i * 2);
    }
    r.clip = ((r.clip << 6) | f) & 0xFFFFFFu;
}

// The MAC, status and clip flags an instruction sets can be read four cycles later, and `t` counts cycles, not instructions:
// it is the cycle the current pair issued at, after the stalls of ps2::vu::issue_cycle (or of the compiled program's plan).
struct FlagPipe {
    std::uint32_t t = 16;
    std::uint32_t mac_ts[8] = {}, mac_v[8] = {}, status_v[8] = {}, clip_ts[8] = {}, clip_v[8] = {};
    unsigned mh = 0, ch = 0;
    std::uint32_t q_value = 0, q_ready = 0;
    bool q_pending = false;

    PS2_VU_INLINE std::uint32_t begin_q(Vu0Regs &r) {
        flush_q(r);
        return r.q;
    }
    PS2_VU_INLINE void end_q(Vu0Regs &r, std::uint32_t before, std::uint32_t latency) {
        q_value = r.q;
        r.q = before;
        q_ready = t + latency;
        q_pending = true;
    }
    PS2_VU_INLINE void sync_q(Vu0Regs &r) {
        if (q_pending && t >= q_ready) flush_q(r);
    }
    PS2_VU_INLINE void flush_q(Vu0Regs &r) {
        if (!q_pending) return;
        r.q = q_value;
        q_pending = false;
    }

    void start(const Vu0Regs &r) {
        for (int i = 0; i < 8; ++i) {
            mac_ts[i] = clip_ts[i] = 0;
            mac_v[i] = r.mac;
            status_v[i] = r.status;
            clip_v[i] = r.clip;
        }
    }
    PS2_VU_INLINE void set_mac(std::uint32_t mac, std::uint32_t status) {
        const unsigned i = mh++ & 7u;
        mac_ts[i] = t;
        mac_v[i] = mac;
        status_v[i] = status;
    }
    PS2_VU_INLINE void set_clip(std::uint32_t clip, std::uint32_t at) {
        const unsigned i = ch++ & 7u;
        clip_ts[i] = at;
        clip_v[i] = clip;
    }
    PS2_VU_INLINE void force_mac(std::uint32_t mac, std::uint32_t status) {
        set_mac(mac, status);
        mac_ts[(mh - 1) & 7u] = t - 4u;
    }
    PS2_VU_INLINE unsigned mac_slot() const {
        for (unsigned k = 1; k <= 8; ++k) {
            const unsigned i = (mh - k) & 7u;
            if (mac_ts[i] + 4u <= t) return i;
        }
        return (mh - 8) & 7u;
    }
    PS2_VU_INLINE std::uint32_t mac() const { return mac_v[mac_slot()]; }
    PS2_VU_INLINE std::uint32_t status() const { return status_v[mac_slot()]; }
    // The low four bits of the status (zero, sign, underflow, overflow): FSAND with a mask inside them (see FlagPipeV::status_low).
    PS2_VU_INLINE std::uint32_t status_low() const { return status() & 0xFu; }
    PS2_VU_INLINE std::uint32_t clip() const {
        for (unsigned k = 1; k <= 8; ++k) {
            const unsigned i = (ch - k) & 7u;
            if (clip_ts[i] + 4u <= t) return clip_v[i];
        }
        return clip_v[(ch - 8) & 7u];
    }
};

// The MAC and status flags of a compiled VU1 program, worked out where they are read instead of where they are set
// (recomp/vurecomp_gen.cpp). Used for programs that read the MAC flags (FMAND, FMEQ, FMOR) but never the
// status register (no FSAND, FSEQ, FSOR, FSSET): there the status is only seen after the program, by the next program or
// by the interpreter at a hop. The generator pushes at exactly the instructions where it would otherwise call flags() and
// set_mac(), and calls finish() at every way out of the program; the MAC a reader gets and the MAC and status left in the
// registers are the same bits as with flags() + set_mac():
//  - push() keeps the result instead of its flags, with the lanes the instruction does not write set to 1.0 (exponent 127,
//    no sign, not the largest magnitude: it sets none of the flags). The MAC of a kept value is flags()' MAC of the result
//    for its dest (mac_of, the same three tests).
//  - mac() picks the slot exactly as FlagPipe::mac_slot() does (the slots of before the program start as r.mac, at cycle 0).
//  - flags() leaves status = (status before & 0xFF0) | low | low << 6, where low is the zero (1), sign (2) and overflow (8)
//    bits of that result; so after n results it is (status at the start & 0xFF0) | (OR of every low) << 6 | the last low.
//    The OR of every low is kept as two running lanes: the smallest magnitude (zero: one below 2^-126, the exponent
//    field is 0) and the OR of the sign bits. The overflow bit needs no lane: no value a compiled VU1 program gives a flag
//    site has the largest magnitude 0x7FFFFFFF in a lane it writes (every result goes through fout, vfix or vclamp_result,
//    which clamp to 0x7F7FFFFF, or is a sum or product of such a value with 0 or 1), so its overflow and sticky overflow
//    bits are never set.
//  - The sticky bits only ever go from 0 to 1 in such a program (no FSSET). The generator makes every program body a
//    template on kSticky and runs it with kSticky false when the status already has the sticky zero and sign bits (0xC0)
//    at the start, which is all but the first few calls of a game: every add_sticky would then be a no-op,
//    so push, push_last and stick leave it out.
// A push is about five vector instructions and two stores, against about fifty for flags() + set_mac().
struct FlagPipeV : FlagPipe {
    v4u fv[8];
    std::uint32_t fts[8];
    unsigned fn = 0;
    std::uint32_t mac0 = 0;
    v4i lo_mag = (v4i){0x7FFFFFFF, 0x7FFFFFFF, 0x7FFFFFFF, 0x7FFFFFFF};
    v4u signs = v4u{0u, 0u, 0u, 0u};
    std::uint32_t status0 = 0;

    void start(const Vu0Regs &r) {
        for (int i = 0; i < 8; ++i) {
            clip_ts[i] = 0;
            clip_v[i] = r.clip;
        }
        mac0 = r.mac;
        status0 = r.status;
    }
    static PS2_VU_INLINE std::uint32_t mac_of(v4u v) {
#if defined(__SSE2__)
        const __m128i x = (__m128i)v;
        const unsigned zero = static_cast<unsigned>(_mm_movemask_ps(_mm_castsi128_ps(_mm_cmpeq_epi32(_mm_and_si128(x, _mm_set1_epi32(0x7F800000)), _mm_setzero_si128()))));
        const unsigned sign = static_cast<unsigned>(_mm_movemask_ps(_mm_castsi128_ps(x)));
        const unsigned over = static_cast<unsigned>(_mm_movemask_ps(_mm_castsi128_ps(_mm_cmpeq_epi32(_mm_and_si128(x, _mm_set1_epi32(0x7FFFFFFF)), _mm_set1_epi32(0x7FFFFFFF)))));
        return kReverse4[zero] | (static_cast<std::uint32_t>(kReverse4[sign]) << 4) | (static_cast<std::uint32_t>(kReverse4[over]) << 12);
#else
        Vu0Regs t{};
        Reg128 u;
        std::memcpy(&u, &v, 16);
        flags(t, u, 15u);
        return t.mac;
#endif
    }
    template <unsigned D>
    static PS2_VU_INLINE v4u written(v4u v) {
        if constexpr (D != 15u)
            v = vsel(vsplat(0x3F800000u), v, v4u{D & 8u ? ~0u : 0u, D & 4u ? ~0u : 0u, D & 2u ? ~0u : 0u, D & 1u ? ~0u : 0u});
        return v;
    }
    PS2_VU_INLINE void add_sticky(v4u v) {
        const v4i m = (v4i)(v & 0x7FFFFFFFu);
        lo_mag = __builtin_elementwise_min(lo_mag, m);
        signs |= v;
    }
    template <unsigned D, bool S = true>
    PS2_VU_INLINE void push(v4u v) {
        v = written<D>(v);
        const unsigned i = fn++ & 7u;
        fv[i] = v;
        fts[i] = t;
        if constexpr (S) add_sticky(v);
    }
    // A flag site whose MAC no reader and no way out can see (recomp/vurecomp_gen.cpp, the liveness of the flag sites): the
    // interpreter's flags() still ORs its zero, sign and overflow bits into the sticky part of the status the program leaves.
    template <unsigned D, bool S = true>
    PS2_VU_INLINE void stick(v4u v) {
        if constexpr (S) add_sticky(written<D>(v));
    }
    // A program that reads no flags: only the last result before a way out counts for the MAC and the low status bits it leaves
    // (finish() reads fv[(fn - 1) & 7]), so a live site of such a program keeps one slot instead of the ring.
    template <unsigned D, bool S = true>
    PS2_VU_INLINE void push_last(v4u v) {
        v = written<D>(v);
        fv[0] = v;
        fn = 1;
        if constexpr (S) add_sticky(v);
    }
    PS2_VU_INLINE std::uint32_t mac() const {
        for (unsigned k = 1; k <= 8; ++k) {
            if (k > fn) return mac0;
            const unsigned i = (fn - k) & 7u;
            if (fts[i] + 4u <= t) return mac_of(fv[i]);
        }
        return mac_of(fv[(fn - 8) & 7u]);
    }
    std::uint32_t status() const = delete;  // a program that reads the status keeps FlagPipe
    // FSAND with a mask inside the low four status bits, the one status read a program may have and still keep its flags here
    // (recomp/vurecomp_gen.cpp; every flag site of such a program pushes, so finish() also leaves the sticky bits of all of them).
    // flags() sets the low four bits from its own result alone: zero (1) if a MAC zero bit is set, sign (2) for a sign bit,
    // overflow (8) for an overflow bit, and never underflow (4), as no MAC underflow bit is ever set. So FlagPipe::status() & 0xF
    // of a slot is that function of the slot's MAC; a slot of before the program holds the status the program started with.
    // The slot is picked exactly as mac() picks it.
    static PS2_VU_INLINE std::uint32_t low_of(std::uint32_t mac) {
        return ((mac & 0x000Fu) ? 0x1u : 0u) | ((mac & 0x00F0u) ? 0x2u : 0u) | ((mac & 0xF000u) ? 0x8u : 0u);
    }
    PS2_VU_INLINE std::uint32_t status_low() const {
        for (unsigned k = 1; k <= 8; ++k) {
            if (k > fn) return status0 & 0xFu;
            const unsigned i = (fn - k) & 7u;
            if (fts[i] + 4u <= t) return low_of(mac_of(fv[i]));
        }
        return low_of(mac_of(fv[(fn - 8) & 7u]));
    }
    // The sticky bits are those of every flag site the program ran (push, push_last, stick), as the interpreter ORs them in at each
    // one; the MAC and the low status bits are those of the last pushed result. Before any push (fn == 0) they stay as they were:
    // the dead sites that ran before a hop to the interpreter are overwritten there by a later site before anything reads them.
    PS2_VU_INLINE void finish(Vu0Regs &r) const {
        std::uint32_t status = r.status;
        if (fn) {
            const std::uint32_t mac = mac_of(fv[(fn - 1) & 7u]);
            r.mac = mac;
            status &= 0xFF0u;
            if (mac & 0x000Fu) status |= 0x1u;
            if (mac & 0x00F0u) status |= 0x2u;
            if (mac & 0xF000u) status |= 0x8u;
        }
        if (__builtin_reduce_or((v4u)(lo_mag < 0x00800000))) status |= 0x40u;
        if (__builtin_reduce_or(signs) & 0x80000000u) status |= 0x80u;
        r.status = status;
    }
};

template <std::uint32_t Mask = 1023u>
PS2_VU_INLINE std::uint8_t *qword(std::uint8_t *mem, std::uint32_t index) { return mem + (index & Mask) * 16u; }
PS2_VU_INLINE void set_vi(Vu0Regs &r, unsigned i, std::uint32_t v) {
    if (i) r.vi[i] = static_cast<std::uint16_t>(v);
}
PS2_VU_INLINE void set_vi(std::uint16_t &dst, std::uint32_t v) { dst = static_cast<std::uint16_t>(v); }
PS2_VU_INLINE void drop_vi(std::uint32_t) {}

// The elementary function unit works on the same floats as the multipliers: no infinities or NaNs (the exponent 255
// holds ordinary numbers, a result too large is the largest number) and denormals are zero. The sums go through the
// truncating multiplier and adder, as on the console (vu/lower/efu matches them bit for bit). Sine, exp and arctangent
// are the console's own polynomials, built from that multiplier and adder, and all three overflow into the same odd
// values as the console for huge arguments.
//
// What is exact and what is not (ps2autotests vu/lower/efu, 16 inputs per instruction):
//   - exp and arctangent give the console's result on all their inputs; sine on 9 of 16 (the coefficients are right,
//     but how the console orders its sums and products is not known, see efu_sine);
//   - the reciprocal and the square root are exact roots and quotients with a rounding rule; the console's own units
//     are not exact (ERCPR on 0xDEADBEEF and 0x4096CBE4 is one step above the nearest float, the reciprocal root is
//     off by one in some cases), so ERCPR, ERSQRT and ERLENG still differ in a few last bits (6 lines);
//   - the rules marked FIT below were chosen so that this test passes. They are not derived from the hardware.
inline std::uint32_t efu_nearest(double d) {
    const std::uint64_t bits = std::bit_cast<std::uint64_t>(d);
    const std::uint32_t sign = static_cast<std::uint32_t>(bits >> 32) & fp::kSign;
    int e = static_cast<int>((bits >> 52) & 0x7FFu);
    if (!e) return sign;
    std::uint32_t m = static_cast<std::uint32_t>((bits >> 29) & 0x7FFFFFu);
    const std::uint32_t rest = static_cast<std::uint32_t>(bits & 0x1FFFFFFFu);
    if (rest > 0x10000000u || (rest == 0x10000000u && (m & 1u))) {
        if (++m == 0x800000u) {
            m = 0;
            ++e;
        }
    }
    return fp::pack(sign, e - 1023 + 127, m);
}

inline std::uint32_t efu_reciprocal(std::uint32_t x) {
    x = fp::flush(x);
    if (!(x & fp::kAbs)) return (x & fp::kSign) | fp::kAbs;
    return efu_nearest(1.0 / fp::to_double(x));
}

// FIT: the root is rounded to nearest when the (unbiased) exponent of the operand is even and cut off when it is odd.
// All the odd-exponent operands of the test (3, 2, pi, 14 and 12) came out cut off, and for two of them (pi and 14)
// the nearest float is the other one; the even-exponent operands whose root has a fraction above one half (0.72 to 0.92)
// came out rounded up. That is two deciding cases on one side and four on the other, and the console's reason for it is
// not known. The reciprocal root (ERSQRT, ERLENG) takes this root and rounds its reciprocal to nearest, which is still
// off by one for 4 of the 32 inputs of the two instructions.
inline std::uint32_t efu_root(std::uint32_t x) {
    x = fp::flush(x) & fp::kAbs;
    if (!x) return 0u;
    const double root = std::sqrt(fp::to_double(x));
    return (fp::exponent(x) & 1u) ? efu_nearest(root) : fp::from_double(root);
}

inline std::uint32_t efu_squares(const Reg128 &v) {
    return fp::add(fp::add(fp::mul(v.w[0], v.w[0]), fp::mul(v.w[1], v.w[1])), fp::mul(v.w[2], v.w[2]));
}

// The quotient of the arctangent argument is the numerator times the reciprocal of the denominator, the reciprocal
// rounded to nearest and the product cut off, both with the exponent range of a double (a huge denominator must not
// turn the reciprocal into zero before the product). A plain quotient cannot give the console's arguments: -2 / 6 needs
// the 0xBEAAAAAB the nearest reciprocal gives (a cut-off quotient is 0xBEAAAAAA), while (4.712389 - 1) / (4.712389 + 1)
// needs the cut-off value (the nearest quotient is one step higher). All 48 arctangent lines of the test agree.
inline std::uint32_t efu_quotient(std::uint32_t num, std::uint32_t den) {
    std::uint64_t r = std::bit_cast<std::uint64_t>(1.0 / fp::to_double(den));
    const std::uint64_t rest = r & 0x1FFFFFFFull;
    r &= ~0x1FFFFFFFull;
    if (rest > 0x10000000ull || (rest == 0x10000000ull && ((r >> 29) & 1u))) r += 1ull << 29;
    return fp::from_double(fp::to_double(num) * std::bit_cast<double>(r));
}

// atan(y / x) as the console forms it, pi/4 + atan((y - x) / (y + x)), which is also how EATAN gets its argument
// (y = the argument, x = 1). The arctangent is an odd polynomial of degree 15 in that argument; the constant pi/4 is
// the truncated 0x3F490FDA. A zero (y + x) leaves a result that is a number only in its low bits.
inline std::uint32_t efu_arctangent(std::uint32_t y, std::uint32_t x) {
    constexpr std::uint32_t kCoef[8] = {0x3F7FFFF5u, 0xBEAAA61Cu, 0x3E4C40A6u, 0xBE0E6C63u,
                                        0x3DC577DFu, 0xBD6501C4u, 0x3CB31652u, 0xBB84D7E7u};
    const std::uint32_t num = fp::flush(fp::sub(y, x)), den = fp::flush(fp::add(y, x));
    if (!(den & fp::kAbs)) return ((num ^ den) & fp::kSign) | 0x7FC90FD7u;
    const std::uint32_t t = efu_quotient(num, den);
    // The polynomial does not cancel pi/4 exactly at t = -1: atan(0) reads 1.07e-7.
    if (t == 0xBF800000u) return 0x33E60000u;
    const std::uint32_t t2 = fp::mul(t, t);
    std::uint32_t power = t, sum = fp::mul(t, kCoef[0]);
    for (int i = 1; i < 8; ++i) {
        power = fp::mul(power, t2);
        sum = fp::add(sum, fp::mul(power, kCoef[i]));
    }
    return fp::add(sum, 0x3F490FDAu);
}

// sin(x) = x + S2 x^3 + S3 x^5 + S4 x^7 + S5 x^9, one term after the other. This gives the console's result for x = 0,
// pi/2 and the huge and tiny values, but is a last bit or more off for 1, 2, 3, 1.9999999, pi and 3 pi/2.
// Not solved: no order of the five sums with any order of the products behind the terms (6.8 million combinations of
// both, tried) gives the console's value for pi; and the inputs 1, 2 and 3, whose powers are exact so that only the sums
// matter, want the x^5 term added last, while the largest float (saturated powers) wants it added early.
inline std::uint32_t efu_sine(std::uint32_t x) {
    constexpr std::uint32_t kCoef[4] = {0xBE2AAAA4u, 0x3C08873Eu, 0xB94FB222u, 0x362E9C14u};
    const std::uint32_t x2 = fp::mul(x, x);
    std::uint32_t power = x, sum = x;
    for (const std::uint32_t c : kCoef) {
        power = fp::mul(power, x2);
        sum = fp::add(sum, fp::mul(power, c));
    }
    return sum;
}

// exp(-x) = 1 / (1 + E1 x + E2 x^2 + ... + E6 x^6)^4, the powers of x one multiplication after the other, the sum
// squared twice and the reciprocal rounded to nearest.
//
// FIT: the order of the sums. It is not known from the hardware; it was found by trying all 10395 ways to add the
// seven terms. The inputs 1, -1, 2 and 3 have exact powers, so only the order can matter there: 440 orders give the
// console's four results. Four of those 440 also give the console's result for the other twelve inputs (pi/2, pi,
// 3 pi/2, 1.9999999 and the huge, tiny and garbage values), where the powers are rounded and which were not used to
// choose. The one used here is the first of the four: (E1 x + E3 x^3) + (E2 x^2 + E4 x^4) first, then 1 +, then
// E5 x^5, then E6 x^6. The other three swap E5 and E6, or add E3 x^3 after E1 x + (E2 x^2 + E4 x^4); they give the same
// results on this test, so the test cannot tell the four apart.
inline std::uint32_t efu_exponential(std::uint32_t x) {
    constexpr std::uint32_t kCoef[6] = {0x3E7FFFA8u, 0x3D0007F4u, 0x3B29D3FFu, 0x3933E553u, 0x36B63510u, 0x353961ACu};
    std::uint32_t term[6], power = x;
    term[0] = fp::mul(x, kCoef[0]);
    for (int i = 1; i < 6; ++i) {
        power = fp::mul(power, x);
        term[i] = fp::mul(power, kCoef[i]);
    }
    std::uint32_t sum = fp::add(fp::add(term[0], term[2]), fp::add(term[1], term[3]));
    sum = fp::add(fp::add(fp::kOne, sum), term[4]);
    sum = fp::add(sum, term[5]);
    sum = fp::mul(sum, sum);
    sum = fp::mul(sum, sum);
    return efu_reciprocal(sum);
}

inline void efu(Vu0Regs &r, unsigned index, unsigned fs, unsigned fsf) {
    const Reg128 &v = r.vf[fs];
    switch (index) {
    case 0x70: r.p = efu_squares(v); break;
    case 0x71: r.p = efu_reciprocal(efu_squares(v)); break;
    case 0x72: r.p = efu_root(efu_squares(v)); break;
    case 0x73: r.p = efu_reciprocal(efu_root(efu_squares(v))); break;
    case 0x74: r.p = efu_arctangent(v.w[1], v.w[0]); break;
    case 0x75: r.p = efu_arctangent(v.w[2], v.w[0]); break;
    case 0x76: r.p = fp::add(fp::add(fp::add(v.w[0], v.w[1]), v.w[2]), v.w[3]); break;
    case 0x78: r.p = efu_root(v.w[fsf]); break;
    case 0x79: r.p = efu_reciprocal(efu_root(v.w[fsf])); break;
    case 0x7A: r.p = efu_reciprocal(v.w[fsf]); break;
    case 0x7C: r.p = efu_sine(v.w[fsf]); break;
    case 0x7D: r.p = efu_arctangent(v.w[fsf], fp::kOne); break;
    case 0x7E: r.p = efu_exponential(v.w[fsf]); break;
    default: break;
    }
}

// ---- The clean-up of the compiled VU1 programs, done late ----
//
// vfix does two things: an exponent of 255 becomes the largest float, and a denormal becomes a zero with its sign. The processor can do
// the second one by itself: with the DAZ bit of MXCSR set ("denormals are zero"), every float instruction reads a denormal operand as a
// zero with its sign. So inside a compiled program, which runs with DAZ set, a result only needs the first half at once (vclamp, two
// instructions instead of six); a denormal may stay in the value, because every add, subtract and multiply that reads it sees the zero
// it stands for. The flags see the same too (zero is "exponent 0", the sign bit is the same). Only where the bits themselves are read
// (a store to memory, MAX, MINI, ABS, ITOF, MOVE, MTIR, the end of the program ...) the generator puts the second half first (vflush),
// for exactly the lanes an add, subtract or multiply wrote since. Rounding is untouched: DAZ changes how operands are read, never how a
// result is rounded (the flush-to-zero bit, which does, stays off: it disagrees with vfix for products just below the smallest normal
// number).
//
// What keeps this exact against the compiler, which assumes the default MXCSR:
//  - every float instruction of a program is in its body function, which is never inlined into the function that sets and restores
//    MXCSR, and the calls that leave the body (XGKICK, the elementary-function unit) go through functions that put MXCSR back for the
//    time of the call; a hop to the interpreter is done after the body has returned;
//  - the compiler works out arithmetic on constants with the default rules, so no denormal constant may reach a float instruction:
//    every value that becomes an operand goes through vopaque (no instruction, but the compiler no longer knows the value), except the
//    constant vf00 and what comes from it alone (whole numbers) and the I register after a full vfix.
// Without SSE4.1 (and on other processors) nothing changes: the helpers below are the old eager ones and vflush does nothing.
#if defined(__SSE4_1__) && !defined(PS2_VU_NO_DAZ)
#define PS2_VU_DAZ 1
#else
#define PS2_VU_DAZ 0
#endif

inline constexpr std::uint32_t kNoHop = 0xFFFFFFFFu;
// The operands of a program that cleans every result at once (vurecomp_gen.cpp, `worth`), with or without SSE4.1.
PS2_VU_INLINE v4u vshadow_eager(const Reg128 &r) { return vfix(vload(r)); }
PS2_VU_INLINE v4u vqd_eager(std::uint32_t q) { return vfix(vsplat(q)); }
PS2_VU_INLINE void vhop(std::uint32_t &hop, std::uint32_t pc) { hop = pc; }

#if PS2_VU_DAZ
#define PS2_VU_BODY static __attribute__((noinline))
inline constexpr unsigned kMxcsrDaz = 0x0040u;
PS2_VU_INLINE v4u vopaque(v4u x) {
    __m128i t = (__m128i)x;
    __asm__("" : "+x"(t));
    return (v4u)t;
}
// An exponent of 255 (any mantissa) becomes the largest float with the same sign; everything else, denormals included, is left alone.
// Unsigned, only a negative number with exponent 255 is above 0xFF7FFFFF; signed, only a positive one is above 0x7F7FFFFF.
// vflush_value(vclamp(x)) is vfix(x) on every one of the 2^32 inputs (tests/test_vu_cleanup.cpp).
PS2_VU_INLINE v4u vclamp(v4u x) {
    return (v4u)_mm_min_epi32(_mm_min_epu32((__m128i)x, _mm_set1_epi32(static_cast<int>(0xFF7FFFFFu))), _mm_set1_epi32(0x7F7FFFFF));
}
// The same for the result of an add, subtract or multiply, done with float instructions (MINPS, MAXPS), so that a result on its way
// from one float instruction to the next does not go through the integer side of the processor and back (a cycle each way on
// today's processors; measured on a Zen 3, a multiply-add chain takes 9.1 instead of 13.0 ticks per step). Such a result is never
// a NaN: no operand of the arithmetic has an exponent of 255 (every one of them went through vclamp, vfix or this), and without an
// infinity among the operands a sum or product is a finite number or an infinity. So the two instructions only pick: plus infinity
// becomes the largest float, minus infinity the largest negative one, everything else is below and above the limits and stays (a
// zero keeps its sign; a denormal counts as zero in the comparison and comes out as itself or as that zero, which is the same to
// everything after it). vflush_value(vclamp_result(x)) is vfix(x) on every input that is not a NaN, with the denormals-are-zero
// bit set (tests/test_vu_cleanup.cpp). Never for a value loaded from a register or from memory: that may be a NaN, which MINPS
// would turn into the positive limit whatever its sign.
PS2_VU_INLINE v4u vclamp_result(v4f x) {
    return (v4u)_mm_max_ps(_mm_min_ps((__m128)x, _mm_castsi128_ps(_mm_set1_epi32(0x7F7FFFFF))), _mm_castsi128_ps(_mm_set1_epi32(static_cast<int>(0xFF7FFFFFu))));
}
// The second half, for the lanes in `lanes` (all bits of a lane set): exponent 0 becomes a zero with its sign.
PS2_VU_INLINE v4u vflush_value(v4u x, v4u lanes) {
    const __m128i xi = (__m128i)x;
    const __m128i tiny = _mm_cmpgt_epi32(_mm_set1_epi32(0x00800000), _mm_and_si128(xi, _mm_set1_epi32(0x7FFFFFFF)));
    return (v4u)_mm_andnot_si128(_mm_and_si128(tiny, (__m128i)(lanes & 0x7FFFFFFFu)), xi);
}
PS2_VU_INLINE void vflush(Reg128 &r, v4u lanes) { vstore(r, vflush_value(vload(r), lanes)); }
// The second half done by the processor itself, where the generator knows the "denormals are zero" bit is set (`daz_in`): a
// multiply by one reads a denormal as the zero with its sign it stands for and gives that zero; every other number comes out as
// it went in (a multiply by one is exact, and the value is never a NaN or an infinity: the lanes flushed hold results of vclamp_result
// only). One float instruction, and a blend for some of the lanes, instead of four integer ones. The one is opaque, or the
// compiler would drop the multiply. vflush_mul_value(x, lanes) is vflush_value(x, lanes) on every input whose exponent is not 255,
// with the bit set (tests/test_vu_cleanup.cpp).
PS2_VU_INLINE v4u vflush_mul_value(v4u x, v4u lanes) { return vsel(x, (v4u)((v4f)x * (v4f)vopaque(vsplat(0x3F800000u))), lanes); }
PS2_VU_INLINE void vflushd(Reg128 &r, v4u lanes) { vstore(r, vflush_mul_value(vload(r), lanes)); }
PS2_VU_INLINE v4u vshadow(const Reg128 &r) { return vopaque(vclamp(vload(r))); }
PS2_VU_INLINE v4u vqd(std::uint32_t q) { return vopaque(vclamp(vsplat(q))); }
PS2_VU_INLINE v4u vaddd(v4u a, v4u b) { return vopaque(vclamp_result((v4f)a + (v4f)b)); }
PS2_VU_INLINE v4u vsubd(v4u a, v4u b) { return vopaque(vclamp_result((v4f)a - (v4f)b)); }
PS2_VU_INLINE v4u vmuld(v4u a, v4u b) { return vopaque(vclamp_result((v4f)a * (v4f)b)); }
PS2_VU_INLINE v4u vmaddd(v4u acc, v4u a, v4u b) { return vaddd(acc, vclamp_result((v4f)a * (v4f)b)); }
PS2_VU_INLINE v4u vmsubd(v4u acc, v4u a, v4u b) { return vsubd(acc, vclamp_result((v4f)a * (v4f)b)); }
// MXCSR as the caller of a program has it. Reading it (STMXCSR) costs far more than the instruction: it has to wait until every float
// instruction before it has left the processor, and with the value coming out of it the program's own float instructions cannot start
// before that either (measured on a Zen 3: about 50 cycles per program call, more than the late clean-up saves in a short program).
// So it is read once per thread and kept; writing a value that is already known costs next to nothing. This rests on the calling
// conventions of x86-64 (System V and Windows alike): the control bits of MXCSR belong to the caller and every function leaves them
// as it found them, so on one thread they are the same at every call unless some code switches the float mode of the thread for good,
// and nothing in the runtime touches MXCSR. Builds with assertions check it at every call. The six status bits are free for any
// function to change, so putting old ones back is allowed. (A value of 0, every float exception switched on, is read every time.)
inline constexpr unsigned kMxcsrStatus = 0x003Fu;
inline thread_local unsigned t_vu_csr = 0;
PS2_VU_INLINE unsigned vcsr() {
    if (t_vu_csr == 0) [[unlikely]] t_vu_csr = _mm_getcsr();
    assert((_mm_getcsr() | kMxcsrStatus) == (t_vu_csr | kMxcsrStatus));
    return t_vu_csr;
}
struct VuDaz {
    unsigned csr;
    bool on = true;  // false once the body has put MXCSR back itself (vkick_last)
    VuDaz() : csr(vcsr()) { _mm_setcsr(csr | kMxcsrDaz); }
    ~VuDaz() {
        if (on) _mm_setcsr(csr);
    }
    VuDaz(const VuDaz &) = delete;
    VuDaz &operator=(const VuDaz &) = delete;
};
inline __attribute__((noinline)) void vkick(unsigned csr, int unit, std::uint32_t address) {
    _mm_setcsr(csr);
    vu::kick(unit, address);
    _mm_setcsr(csr | kMxcsrDaz);
}
// The last XGKICK of a run: no add, subtract or multiply follows it on any path (the generator says so), so MXCSR stays as the caller
// has it for the rest of the body.
inline __attribute__((noinline)) void vkick_last(VuDaz &daz, int unit, std::uint32_t address) {
    _mm_setcsr(daz.csr);
    daz.on = false;
    vu::kick(unit, address);
}
inline __attribute__((noinline)) void vefu(unsigned csr, Vu0Regs &r, unsigned index, unsigned fs, unsigned fsf) {
    _mm_setcsr(csr);
    efu(r, index, fs, fsf);
    _mm_setcsr(csr | kMxcsrDaz);
}
#else
#define PS2_VU_BODY static inline __attribute__((always_inline))
PS2_VU_INLINE v4u vopaque(v4u x) { return x; }
PS2_VU_INLINE v4u vclamp(v4u x) { return vfix(x); }
PS2_VU_INLINE v4u vclamp_result(v4f x) { return vfix((v4u)x); }
PS2_VU_INLINE v4u vflush_value(v4u x, v4u) { return x; }
PS2_VU_INLINE void vflush(Reg128 &, v4u) {}
PS2_VU_INLINE v4u vflush_mul_value(v4u x, v4u) { return x; }
PS2_VU_INLINE void vflushd(Reg128 &, v4u) {}
PS2_VU_INLINE v4u vshadow(const Reg128 &r) { return vfix(vload(r)); }
PS2_VU_INLINE v4u vqd(std::uint32_t q) { return vfix(vsplat(q)); }
PS2_VU_INLINE v4u vaddd(v4u a, v4u b) { return vaddn(a, b); }
PS2_VU_INLINE v4u vsubd(v4u a, v4u b) { return vsubn(a, b); }
PS2_VU_INLINE v4u vmuld(v4u a, v4u b) { return vmuln(a, b); }
PS2_VU_INLINE v4u vmaddd(v4u acc, v4u a, v4u b) { return vmaddn(acc, a, b); }
PS2_VU_INLINE v4u vmsubd(v4u acc, v4u a, v4u b) { return vmsubn(acc, a, b); }
struct VuDaz {
    unsigned csr = 0;
};
PS2_VU_INLINE void vkick(unsigned, int unit, std::uint32_t address) { vu::kick(unit, address); }
PS2_VU_INLINE void vkick_last(VuDaz &, int unit, std::uint32_t address) { vu::kick(unit, address); }
PS2_VU_INLINE void vefu(unsigned, Vu0Regs &r, unsigned index, unsigned fs, unsigned fsf) { efu(r, index, fs, fsf); }
#endif

}
