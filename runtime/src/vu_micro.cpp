#include "vu_micro.hpp"

#include "gs/gif.hpp"
#include "gs/queue.hpp"
#include "vif.hpp"
#include "vu_jit_web.hpp"
#include "hw.hpp"
#include "ps2/fpu.hpp"
#include "ps2/os.hpp"
#include "ps2/system.hpp"
#include "ps2/vu.hpp"
#include "ps2/vu_live.hpp"
#include "ps2/vu_ops.hpp"

#include "ps2/state.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace ps2::vu {

std::uint32_t g_trace_vu1_pc = 0;  // entry of the vector program run last (shown by the GS draw trace)

namespace {

struct Unit {
    Vu0Regs *regs;
    std::uint8_t *code;
    std::uint8_t *data;
    std::uint32_t code_mask;
    std::uint32_t data_qwords;
    std::uint32_t tpc;
    vuops::FlagPipe *pipe = nullptr;
    // What a branch reads of the integer registers: the values from before the
    // previous instruction when that one wrote them (see BranchView).
    const std::uint16_t *branch_vi = nullptr;
};

Vu0Regs g_vu1;
std::uint32_t g_generation[2] = {1, 1};
std::uint32_t g_top = 0, g_itop = 0;

struct QDelay {
    std::uint32_t generation = 0, value = 0;
};

QDelay g_q_kind[2][2048];

void read_q(const Unit &u, vuops::FlagPipe &pipe, Vu0Regs &r, std::uint32_t pc, std::uint32_t hi) {
    if (!pipe.q_pending || !reads_q(hi)) return;
    const int n = u.code == g_vu1_code ? 1 : 0;
    QDelay &d = g_q_kind[n][(pc & u.code_mask) >> 3];
    if (d.generation != g_generation[n]) {
        d.generation = g_generation[n];
        d.value = q_read_kind(u.code, u.code_mask, pc);
    }
    if (d.value == kQNew) pipe.flush_q(r);
    else if (d.value == kQDynamic) pipe.sync_q(r);
}

std::uint64_t g_reported = 0;
bool g_reference_dry = false;

Unit unit_of(int n) {
    if (n == 0) return Unit{&g_cpu.vu0, g_vu0_code, g_vu0_mem, 4095u, 256u, g_cpu.vu0.tpc};
    return Unit{&g_vu1, g_vu1_code, g_vu1_mem, 16383u, 1024u, g_vu1.tpc};
}

struct UpperOut {
    int target = -1;
    Reg128 value{};
    unsigned dest = 0;
    bool flags = false;
    bool has_clip = false;
    std::uint32_t clip = 0;
};

constexpr int kAcc = 32;

std::uint32_t mac_flags(const Reg128 &v, unsigned dest) {
    std::uint32_t mac = 0;
    for (int i = 0; i < 4; ++i) {
        if (!(dest & (8u >> i))) continue;
        const std::uint32_t bit = 3u - static_cast<std::uint32_t>(i);
        const std::uint32_t x = v.w[i];
        if (!(x & 0x7F800000u)) mac |= 1u << bit;
        if (x & 0x80000000u) mac |= 1u << (bit + 4);
        if ((x & 0x7FFFFFFFu) == 0x7FFFFFFFu) mac |= 1u << (bit + 12);
    }
    return mac;
}

void set_flags(Vu0Regs &r, const Reg128 &v, unsigned dest) {
    const std::uint32_t mac = mac_flags(v, dest);
    r.mac = mac;
    std::uint32_t status = r.status & 0xFF0u;
    if (mac & 0x000Fu) status |= 0x1u;
    if (mac & 0x00F0u) status |= 0x2u;
    if (mac & 0x0F00u) status |= 0x4u;
    if (mac & 0xF000u) status |= 0x8u;
    status |= (status & 0xFu) << 6;
    r.status = (status & ~0x30u) | (r.status & 0x30u);
}

template <typename F>
UpperOut lanes(int target, unsigned dest, bool flags, F f) {
    UpperOut o;
    o.target = target;
    o.dest = dest;
    o.flags = flags;
    for (int i = 0; i < 4; ++i)
        if (dest & (8u >> i)) o.value.w[i] = f(i);
    return o;
}

UpperOut upper(Vu0Regs &r, std::uint32_t w) {
    const unsigned dest = (w >> 21) & 15u, ft = (w >> 16) & 31u, fs = (w >> 11) & 31u, fd = (w >> 6) & 31u, bc = w & 3u;
    const Reg128 s = r.vf[fs], t = r.vf[ft], acc = r.acc;
    const std::uint32_t b = t.w[bc];
    const unsigned funct = w & 63u;
    if (funct < 0x3C) {
        switch (funct) {
        case 0x00: case 0x01: case 0x02: case 0x03: return lanes(fd, dest, true, [&](int i) { return fp::add(s.w[i], b); });
        case 0x04: case 0x05: case 0x06: case 0x07: return lanes(fd, dest, true, [&](int i) { return fp::sub(s.w[i], b); });
        case 0x08: case 0x09: case 0x0A: case 0x0B: return lanes(fd, dest, true, [&](int i) { return fp::add(acc.w[i], fp::mul(s.w[i], b)); });
        case 0x0C: case 0x0D: case 0x0E: case 0x0F: return lanes(fd, dest, true, [&](int i) { return fp::sub(acc.w[i], fp::mul(s.w[i], b)); });
        case 0x10: case 0x11: case 0x12: case 0x13: return lanes(fd, dest, false, [&](int i) { return fp::max(s.w[i], b); });
        case 0x14: case 0x15: case 0x16: case 0x17: return lanes(fd, dest, false, [&](int i) { return fp::min(s.w[i], b); });
        case 0x18: case 0x19: case 0x1A: case 0x1B: return lanes(fd, dest, true, [&](int i) { return fp::mul(s.w[i], b); });
        case 0x1C: return lanes(fd, dest, true, [&](int i) { return fp::mul(s.w[i], r.q); });
        case 0x1D: return lanes(fd, dest, false, [&](int i) { return fp::max(s.w[i], r.i); });
        case 0x1E: return lanes(fd, dest, true, [&](int i) { return fp::mul(s.w[i], r.i); });
        case 0x1F: return lanes(fd, dest, false, [&](int i) { return fp::min(s.w[i], r.i); });
        case 0x20: return lanes(fd, dest, true, [&](int i) { return fp::add(s.w[i], r.q); });
        case 0x21: return lanes(fd, dest, true, [&](int i) { return fp::add(acc.w[i], fp::mul(s.w[i], r.q)); });
        case 0x22: return lanes(fd, dest, true, [&](int i) { return fp::add(s.w[i], r.i); });
        case 0x23: return lanes(fd, dest, true, [&](int i) { return fp::add(acc.w[i], fp::mul(s.w[i], r.i)); });
        case 0x24: return lanes(fd, dest, true, [&](int i) { return fp::sub(s.w[i], r.q); });
        case 0x25: return lanes(fd, dest, true, [&](int i) { return fp::sub(acc.w[i], fp::mul(s.w[i], r.q)); });
        case 0x26: return lanes(fd, dest, true, [&](int i) { return fp::sub(s.w[i], r.i); });
        case 0x27: return lanes(fd, dest, true, [&](int i) { return fp::sub(acc.w[i], fp::mul(s.w[i], r.i)); });
        case 0x28: return lanes(fd, dest, true, [&](int i) { return fp::add(s.w[i], t.w[i]); });
        case 0x29: return lanes(fd, dest, true, [&](int i) { return fp::add(acc.w[i], fp::mul(s.w[i], t.w[i])); });
        case 0x2A: return lanes(fd, dest, true, [&](int i) { return fp::mul(s.w[i], t.w[i]); });
        case 0x2B: return lanes(fd, dest, false, [&](int i) { return fp::max(s.w[i], t.w[i]); });
        case 0x2C: return lanes(fd, dest, true, [&](int i) { return fp::sub(s.w[i], t.w[i]); });
        case 0x2D: return lanes(fd, dest, true, [&](int i) { return fp::sub(acc.w[i], fp::mul(s.w[i], t.w[i])); });
        case 0x2E: {
            UpperOut o;
            o.target = static_cast<int>(fd);
            o.dest = 0xEu;
            o.flags = true;
            o.value.w[0] = fp::sub(acc.w[0], fp::mul(s.w[1], t.w[2]));
            o.value.w[1] = fp::sub(acc.w[1], fp::mul(s.w[2], t.w[0]));
            o.value.w[2] = fp::sub(acc.w[2], fp::mul(s.w[0], t.w[1]));
            return o;
        }
        case 0x2F: return lanes(fd, dest, false, [&](int i) { return fp::min(s.w[i], t.w[i]); });
        default: return UpperOut{};
        }
    }
    const unsigned index = ((w >> 6) & 31u) * 4u + (funct & 3u);
    switch (index) {
    case 0x00: case 0x01: case 0x02: case 0x03: return lanes(kAcc, dest, true, [&](int i) { return fp::add(s.w[i], b); });
    case 0x04: case 0x05: case 0x06: case 0x07: return lanes(kAcc, dest, true, [&](int i) { return fp::sub(s.w[i], b); });
    case 0x08: case 0x09: case 0x0A: case 0x0B: return lanes(kAcc, dest, true, [&](int i) { return fp::add(acc.w[i], fp::mul(s.w[i], b)); });
    case 0x0C: case 0x0D: case 0x0E: case 0x0F: return lanes(kAcc, dest, true, [&](int i) { return fp::sub(acc.w[i], fp::mul(s.w[i], b)); });
    case 0x10: case 0x11: case 0x12: case 0x13: {
        static const int kFrac[4] = {0, 4, 12, 15};
        return lanes(ft, dest, false, [&](int i) { return fp::from_fixed(s.sw[i], kFrac[index & 3u]); });
    }
    case 0x14: case 0x15: case 0x16: case 0x17: {
        static const int kFrac[4] = {0, 4, 12, 15};
        return lanes(ft, dest, false, [&](int i) { return fp::to_fixed(s.w[i], kFrac[index & 3u]); });
    }
    case 0x18: case 0x19: case 0x1A: case 0x1B: return lanes(kAcc, dest, true, [&](int i) { return fp::mul(s.w[i], b); });
    case 0x1C: return lanes(kAcc, dest, true, [&](int i) { return fp::mul(s.w[i], r.q); });
    case 0x1D: return lanes(ft, dest, false, [&](int i) { return s.w[i] & fp::kAbs; });
    case 0x1E: return lanes(kAcc, dest, true, [&](int i) { return fp::mul(s.w[i], r.i); });
    case 0x1F: {
        UpperOut o;
        o.has_clip = true;
        o.clip = vuops::clip_value(r, s, t.w[3]);
        return o;
    }
    case 0x20: return lanes(kAcc, dest, true, [&](int i) { return fp::add(s.w[i], r.q); });
    case 0x21: return lanes(kAcc, dest, true, [&](int i) { return fp::add(acc.w[i], fp::mul(s.w[i], r.q)); });
    case 0x22: return lanes(kAcc, dest, true, [&](int i) { return fp::add(s.w[i], r.i); });
    case 0x23: return lanes(kAcc, dest, true, [&](int i) { return fp::add(acc.w[i], fp::mul(s.w[i], r.i)); });
    case 0x24: return lanes(kAcc, dest, true, [&](int i) { return fp::sub(s.w[i], r.q); });
    case 0x25: return lanes(kAcc, dest, true, [&](int i) { return fp::sub(acc.w[i], fp::mul(s.w[i], r.q)); });
    case 0x26: return lanes(kAcc, dest, true, [&](int i) { return fp::sub(s.w[i], r.i); });
    case 0x27: return lanes(kAcc, dest, true, [&](int i) { return fp::sub(acc.w[i], fp::mul(s.w[i], r.i)); });
    case 0x28: return lanes(kAcc, dest, true, [&](int i) { return fp::add(s.w[i], t.w[i]); });
    case 0x29: return lanes(kAcc, dest, true, [&](int i) { return fp::add(acc.w[i], fp::mul(s.w[i], t.w[i])); });
    case 0x2A: return lanes(kAcc, dest, true, [&](int i) { return fp::mul(s.w[i], t.w[i]); });
    case 0x2C: return lanes(kAcc, dest, true, [&](int i) { return fp::sub(s.w[i], t.w[i]); });
    case 0x2D: return lanes(kAcc, dest, true, [&](int i) { return fp::sub(acc.w[i], fp::mul(s.w[i], t.w[i])); });
    case 0x2E: {
        UpperOut o;
        o.target = kAcc;
        o.dest = 0xEu;
        o.flags = true;
        o.value.w[0] = fp::mul(s.w[1], t.w[2]);
        o.value.w[1] = fp::mul(s.w[2], t.w[0]);
        o.value.w[2] = fp::mul(s.w[0], t.w[1]);
        return o;
    }
    default: return UpperOut{};
    }
}

enum UpperKind : std::uint8_t { kNop, kAdd, kSub, kMadd, kMsub, kMax, kMin, kMul, kAbs, kFtoi, kItof, kClip, kOpmula, kOpmsub };
enum UpperSource : std::uint8_t { kVec, kBc, kQ, kI };

struct Decoded {
    std::uint32_t lo = 0, hi = 0;
    bool valid = false;
    UpperKind kind = kNop;
    UpperSource source = kVec;
    std::uint8_t target = 0, dest = 0, fs = 0, ft = 0, bc = 0, frac = 0;
    bool flags = false;
    VfUse use;
};

Decoded g_decoded[2][2048];
bool g_flags_used[2] = {true, true};
// Whether anything in the unit's code memory reads a flag, the clip flags or Q: only then do the cycles of the pipeline matter.
bool g_clock_used[2] = {true, true};
bool g_scan[2] = {true, true};

Decoded decode_upper(std::uint32_t lo, std::uint32_t hi) {
    Decoded d;
    d.lo = lo;
    d.hi = hi;
    d.valid = true;
    d.use = vf_use(lo, hi);
    d.dest = static_cast<std::uint8_t>((hi >> 21) & 15u);
    d.ft = static_cast<std::uint8_t>((hi >> 16) & 31u);
    d.fs = static_cast<std::uint8_t>((hi >> 11) & 31u);
    d.bc = static_cast<std::uint8_t>(hi & 3u);
    const std::uint8_t fd = static_cast<std::uint8_t>((hi >> 6) & 31u);
    const unsigned funct = hi & 63u;
    const auto set = [&](UpperKind k, UpperSource src, int target, bool flags) {
        d.kind = k;
        d.source = src;
        d.target = static_cast<std::uint8_t>(target);
        d.flags = flags;
    };
    if (funct < 0x3C) {
        switch (funct) {
        case 0x00: case 0x01: case 0x02: case 0x03: set(kAdd, kBc, fd, true); break;
        case 0x04: case 0x05: case 0x06: case 0x07: set(kSub, kBc, fd, true); break;
        case 0x08: case 0x09: case 0x0A: case 0x0B: set(kMadd, kBc, fd, true); break;
        case 0x0C: case 0x0D: case 0x0E: case 0x0F: set(kMsub, kBc, fd, true); break;
        case 0x10: case 0x11: case 0x12: case 0x13: set(kMax, kBc, fd, false); break;
        case 0x14: case 0x15: case 0x16: case 0x17: set(kMin, kBc, fd, false); break;
        case 0x18: case 0x19: case 0x1A: case 0x1B: set(kMul, kBc, fd, true); break;
        case 0x1C: set(kMul, kQ, fd, true); break;
        case 0x1D: set(kMax, kI, fd, false); break;
        case 0x1E: set(kMul, kI, fd, true); break;
        case 0x1F: set(kMin, kI, fd, false); break;
        case 0x20: set(kAdd, kQ, fd, true); break;
        case 0x21: set(kMadd, kQ, fd, true); break;
        case 0x22: set(kAdd, kI, fd, true); break;
        case 0x23: set(kMadd, kI, fd, true); break;
        case 0x24: set(kSub, kQ, fd, true); break;
        case 0x25: set(kMsub, kQ, fd, true); break;
        case 0x26: set(kSub, kI, fd, true); break;
        case 0x27: set(kMsub, kI, fd, true); break;
        case 0x28: set(kAdd, kVec, fd, true); break;
        case 0x29: set(kMadd, kVec, fd, true); break;
        case 0x2A: set(kMul, kVec, fd, true); break;
        case 0x2B: set(kMax, kVec, fd, false); break;
        case 0x2C: set(kSub, kVec, fd, true); break;
        case 0x2D: set(kMsub, kVec, fd, true); break;
        case 0x2E: set(kOpmsub, kVec, fd, true); d.dest = 0xE; break;
        case 0x2F: set(kMin, kVec, fd, false); break;
        default: break;
        }
        return d;
    }
    static const std::uint8_t kFrac[4] = {0, 4, 12, 15};
    const unsigned index = ((hi >> 6) & 31u) * 4u + (funct & 3u);
    switch (index) {
    case 0x00: case 0x01: case 0x02: case 0x03: set(kAdd, kBc, kAcc, true); break;
    case 0x04: case 0x05: case 0x06: case 0x07: set(kSub, kBc, kAcc, true); break;
    case 0x08: case 0x09: case 0x0A: case 0x0B: set(kMadd, kBc, kAcc, true); break;
    case 0x0C: case 0x0D: case 0x0E: case 0x0F: set(kMsub, kBc, kAcc, true); break;
    case 0x10: case 0x11: case 0x12: case 0x13: set(kItof, kVec, d.ft, false); d.frac = kFrac[index & 3u]; break;
    case 0x14: case 0x15: case 0x16: case 0x17: set(kFtoi, kVec, d.ft, false); d.frac = kFrac[index & 3u]; break;
    case 0x18: case 0x19: case 0x1A: case 0x1B: set(kMul, kBc, kAcc, true); break;
    case 0x1C: set(kMul, kQ, kAcc, true); break;
    case 0x1D: set(kAbs, kVec, d.ft, false); break;
    case 0x1E: set(kMul, kI, kAcc, true); break;
    case 0x1F: set(kClip, kVec, 0, false); break;
    case 0x20: set(kAdd, kQ, kAcc, true); break;
    case 0x21: set(kMadd, kQ, kAcc, true); break;
    case 0x22: set(kAdd, kI, kAcc, true); break;
    case 0x23: set(kMadd, kI, kAcc, true); break;
    case 0x24: set(kSub, kQ, kAcc, true); break;
    case 0x25: set(kMsub, kQ, kAcc, true); break;
    case 0x26: set(kSub, kI, kAcc, true); break;
    case 0x27: set(kMsub, kI, kAcc, true); break;
    case 0x28: set(kAdd, kVec, kAcc, true); break;
    case 0x29: set(kMadd, kVec, kAcc, true); break;
    case 0x2A: set(kMul, kVec, kAcc, true); break;
    case 0x2C: set(kSub, kVec, kAcc, true); break;
    case 0x2D: set(kMsub, kVec, kAcc, true); break;
    case 0x2E: set(kOpmula, kVec, kAcc, true); d.dest = 0xE; break;
    default: break;
    }
    return d;
}

float fin(std::uint32_t x) {
    const std::uint32_t e = x & 0x7F800000u;
    if (!e) x &= 0x80000000u;
    else if (e == 0x7F800000u) x = (x & 0x80000000u) | 0x7F7FFFFFu;
    float f;
    std::memcpy(&f, &x, 4);
    return f;
}

std::uint32_t fout(float f) {
    std::uint32_t x;
    std::memcpy(&x, &f, 4);
    const std::uint32_t e = x & 0x7F800000u;
    if (!e) return x & 0x80000000u;
    if (e == 0x7F800000u) return (x & 0x80000000u) | 0x7F7FFFFFu;
    return x;
}

float norm(float f) { return fin(fout(f)); }

void fast_upper(Vu0Regs &r, const Decoded &d, UpperOut &o) {
    o.target = -1;
    o.has_clip = false;
    if (d.kind == kNop) return;
    const Reg128 &s = r.vf[d.fs], &t = r.vf[d.ft];
    if (d.kind == kClip) {
        o.has_clip = true;
        o.clip = vuops::clip_value(r, s, t.w[3]);
        return;
    }
    o.target = d.target;
    o.dest = d.dest;
    o.flags = d.flags;
    switch (d.kind) {
    case kAbs:
        for (int i = 0; i < 4; ++i) o.value.w[i] = s.w[i] & fp::kAbs;
        return;
    case kFtoi:
        for (int i = 0; i < 4; ++i) if (d.dest & (8u >> i)) o.value.w[i] = fp::to_fixed(s.w[i], d.frac);
        return;
    case kItof:
        for (int i = 0; i < 4; ++i) if (d.dest & (8u >> i)) o.value.w[i] = fp::from_fixed(s.sw[i], d.frac);
        return;
    case kOpmula:
        o.value.w[0] = fout(norm(fin(s.w[1]) * fin(t.w[2])));
        o.value.w[1] = fout(norm(fin(s.w[2]) * fin(t.w[0])));
        o.value.w[2] = fout(norm(fin(s.w[0]) * fin(t.w[1])));
        return;
    case kOpmsub:
        o.value.w[0] = fout(fin(r.acc.w[0]) - norm(fin(s.w[1]) * fin(t.w[2])));
        o.value.w[1] = fout(fin(r.acc.w[1]) - norm(fin(s.w[2]) * fin(t.w[0])));
        o.value.w[2] = fout(fin(r.acc.w[2]) - norm(fin(s.w[0]) * fin(t.w[1])));
        return;
    default: break;
    }
    std::uint32_t b[4];
    switch (d.source) {
    case kVec: for (int i = 0; i < 4; ++i) b[i] = t.w[i]; break;
    case kBc: for (int i = 0; i < 4; ++i) b[i] = t.w[d.bc]; break;
    case kQ: for (int i = 0; i < 4; ++i) b[i] = r.q; break;
    case kI: for (int i = 0; i < 4; ++i) b[i] = r.i; break;
    }
    switch (d.kind) {
    case kAdd: for (int i = 0; i < 4; ++i) o.value.w[i] = fout(fin(s.w[i]) + fin(b[i])); break;
    case kSub: for (int i = 0; i < 4; ++i) o.value.w[i] = fout(fin(s.w[i]) - fin(b[i])); break;
    case kMul: for (int i = 0; i < 4; ++i) o.value.w[i] = fout(fin(s.w[i]) * fin(b[i])); break;
    case kMadd: for (int i = 0; i < 4; ++i) o.value.w[i] = fout(fin(r.acc.w[i]) + norm(fin(s.w[i]) * fin(b[i]))); break;
    case kMsub: for (int i = 0; i < 4; ++i) o.value.w[i] = fout(fin(r.acc.w[i]) - norm(fin(s.w[i]) * fin(b[i]))); break;
    case kMax: for (int i = 0; i < 4; ++i) o.value.w[i] = fp::max(s.w[i], b[i]); break;
    case kMin: for (int i = 0; i < 4; ++i) o.value.w[i] = fp::min(s.w[i], b[i]); break;
    default: break;
    }
}

void scan_flags(int unit, const std::uint8_t *code, std::uint32_t size) {
    bool used = false, clock = false;
    for (std::uint32_t pc = 0; pc < size && !(used && clock); pc += 8) {
        std::uint32_t lo, hi;
        std::memcpy(&lo, code + pc, 4);
        std::memcpy(&hi, code + pc + 4, 4);
        if (reads_q(hi)) clock = true;
        if (hi & 0x80000000u) continue;
        switch (lo >> 25) {
        case 0x14: case 0x15: case 0x16: case 0x17: case 0x18: case 0x1A: case 0x1B: used = clock = true; break;
        case 0x10: case 0x11: case 0x12: case 0x13: case 0x1C: clock = true; break;
        case 0x40: clock = clock || waits_q(lo, hi) || q_unit_latency(lo, hi); break;
        default: break;
        }
    }
    g_flags_used[unit] = used;
    g_clock_used[unit] = clock;
    g_scan[unit] = false;
}

void commit(Vu0Regs &r, const UpperOut &o) {
    if (o.target < 0) return;
    if (o.target > 0) {
        Reg128 &dst = o.target == kAcc ? r.acc : r.vf[o.target];
        for (int i = 0; i < 4; ++i)
            if (o.dest & (8u >> i)) dst.w[i] = o.value.w[i];
    }
    if (o.flags) set_flags(r, o.value, o.dest);
}

std::uint8_t *qword(Unit &u, std::uint32_t index) {
    return u.data + (index & (u.data_qwords - 1)) * 16;
}

void store_vi(Vu0Regs &r, unsigned index, std::uint32_t value) {
    if (index) r.vi[index] = static_cast<std::uint16_t>(value);
}

void xgkick(Unit &u, std::uint32_t address) {
    if (g_reference_dry) return;
    std::uint8_t buffer[16 * 1024 + 16];
    std::uint32_t q = address & (u.data_qwords - 1), count = 0;
    bool done = false;
    while (!done && count < u.data_qwords) {
        Reg128 tag;
        std::memcpy(&tag, qword(u, q), 16);
        std::memcpy(buffer + count * 16, &tag, 16);
        ++q;
        ++count;
        const std::uint32_t nloop = static_cast<std::uint32_t>(tag.d[0]) & 0x7FFFu;
        const bool eop = (tag.d[0] >> 15) & 1u;
        const std::uint32_t flg = static_cast<std::uint32_t>(tag.d[0] >> 58) & 3u;
        std::uint32_t nreg = static_cast<std::uint32_t>(tag.d[0] >> 60) & 0xFu;
        if (!nreg) nreg = 16;
        std::uint32_t body = flg == 0 ? nloop * nreg : flg == 1 ? (nloop * nreg + 1) / 2 : nloop;
        for (std::uint32_t i = 0; i < body && count < u.data_qwords; ++i, ++q, ++count)
            std::memcpy(buffer + count * 16, qword(u, q), 16);
        done = eop;
    }
    gif::transfer(1, buffer, count);
}

// A VU branch that reads an integer register the instruction right before it
// wrote sees the value from before that write: the integer pipeline commits
// after the branch unit reads, and compiled game code counts on it (a loop
// that decrements its counter just before testing it runs one pass more). A
// flag reading instruction (FMAND, FCAND...) writing the register does not
// delay it. PCSX2 calls this the branch VI delay.
struct BranchView {
    std::uint16_t before[16] = {};
    bool live = false;

    // Before an instruction runs: what its branch, if it is one, reads.
    void arm(Unit &u) const { u.branch_vi = live ? before : nullptr; }
    // Keep the registers as they are before this instruction for the next one.
    void snapshot(const Vu0Regs &r, std::uint16_t (&now)[16]) const { std::memcpy(now, r.vi, sizeof now); }
    void commit(const std::uint16_t (&was)[16], std::uint32_t lo, bool lower_ran) {
        std::memcpy(before, was, sizeof before);
        live = lower_ran && !branch_delay_exempt(lo);
    }
};

void lower(Unit &u, std::uint32_t w, std::uint32_t pc, std::uint32_t &npc, bool &branch) {
    Vu0Regs &r = *u.regs;
    const unsigned dest = (w >> 21) & 15u, it = (w >> 16) & 15u, is = (w >> 11) & 15u, id = (w >> 6) & 15u;
    const std::uint32_t ft = (w >> 16) & 31u, fs = (w >> 11) & 31u;
    const std::int32_t imm11 = static_cast<std::int32_t>(w << 21) >> 21;
    const auto clip_flags = [&]() { return u.pipe ? u.pipe->clip() : r.clip; };
    const auto status_flags = [&]() { return u.pipe ? u.pipe->status() : r.status; };
    const auto mac_flags_now = [&]() { return u.pipe ? u.pipe->mac() : r.mac; };
    const auto branch_to = [&](std::uint32_t target) {
        npc = target & u.code_mask;
        branch = true;
    };
    const auto bvi = [&](unsigned k) -> std::uint16_t { return u.branch_vi ? u.branch_vi[k] : r.vi[k]; };
    if ((w >> 25) == 0x40u) {
        const unsigned funct = w & 63u;
        if (funct < 0x3C) {
            switch (funct) {
            case 0x30: store_vi(r, id, r.vi[is] + r.vi[it]); break;
            case 0x31: store_vi(r, id, r.vi[is] - r.vi[it]); break;
            case 0x32: store_vi(r, it, r.vi[is] + static_cast<std::uint32_t>(static_cast<std::int32_t>(((w >> 6) & 31u) << 27) >> 27)); break;
            case 0x34: store_vi(r, id, r.vi[is] & r.vi[it]); break;
            case 0x35: store_vi(r, id, r.vi[is] | r.vi[it]); break;
            default:
                if (g_reported++ < 32) std::fprintf(stderr, "[vu] lower op 0x%08X at 0x%04X\n", w, pc);
                break;
            }
            return;
        }
        const unsigned index = ((w >> 6) & 31u) * 4u + (funct & 3u);
        const unsigned fsf = (w >> 21) & 3u, ftf = (w >> 23) & 3u;
        switch (index) {
        case 0x30: if (ft) for (int i = 0; i < 4; ++i) if (dest & (8u >> i)) r.vf[ft].w[i] = r.vf[fs].w[i]; break;
        case 0x31: if (ft) { const Reg128 s = r.vf[fs]; for (int i = 0; i < 4; ++i) if (dest & (8u >> i)) r.vf[ft].w[i] = s.w[(i + 1) & 3]; } break;
        case 0x34: {
            const std::uint8_t *q = qword(u, r.vi[is]);
            if (ft) for (int i = 0; i < 4; ++i) if (dest & (8u >> i)) std::memcpy(&r.vf[ft].w[i], q + i * 4, 4);
            store_vi(r, is, r.vi[is] + 1u);
            break;
        }
        case 0x35: {
            std::uint8_t *q = qword(u, r.vi[it]);
            for (int i = 0; i < 4; ++i) if (dest & (8u >> i)) std::memcpy(q + i * 4, &r.vf[fs].w[i], 4);
            store_vi(r, it, r.vi[it] + 1u);
            break;
        }
        case 0x36: {
            store_vi(r, is, r.vi[is] - 1u);
            const std::uint8_t *q = qword(u, r.vi[is]);
            if (ft) for (int i = 0; i < 4; ++i) if (dest & (8u >> i)) std::memcpy(&r.vf[ft].w[i], q + i * 4, 4);
            break;
        }
        case 0x37: {
            store_vi(r, it, r.vi[it] - 1u);
            std::uint8_t *q = qword(u, r.vi[it]);
            for (int i = 0; i < 4; ++i) if (dest & (8u >> i)) std::memcpy(q + i * 4, &r.vf[fs].w[i], 4);
            break;
        }
        case 0x38: {
            const std::uint32_t before = u.pipe ? u.pipe->begin_q(r) : r.q;
            vu::div(r, r.vf[fs].w[fsf], r.vf[ft].w[ftf]);
            if (u.pipe) u.pipe->end_q(r, before, 7);
            break;
        }
        case 0x39: {
            const std::uint32_t before = u.pipe ? u.pipe->begin_q(r) : r.q;
            vu::sqrt(r, r.vf[ft].w[ftf]);
            if (u.pipe) u.pipe->end_q(r, before, 7);
            break;
        }
        case 0x3A: {
            const std::uint32_t before = u.pipe ? u.pipe->begin_q(r) : r.q;
            vu::rsqrt(r, r.vf[fs].w[fsf], r.vf[ft].w[ftf]);
            if (u.pipe) u.pipe->end_q(r, before, 13);
            break;
        }
        case 0x3B: if (u.pipe) u.pipe->flush_q(r); break;
        case 0x3C: store_vi(r, it, r.vf[fs].w[fsf]); break;
        case 0x3D: if (ft) { const std::uint32_t v = static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int16_t>(r.vi[is]))); for (int i = 0; i < 4; ++i) if (dest & (8u >> i)) r.vf[ft].w[i] = v; } break;
        case 0x3E: {
            const std::uint8_t *q = qword(u, r.vi[is]);
            for (int i = 0; i < 4; ++i)
                if (dest & (8u >> i)) {
                    std::uint16_t v;
                    std::memcpy(&v, q + i * 4, 2);
                    store_vi(r, it, v);
                    break;
                }
            break;
        }
        case 0x3F: {
            std::uint8_t *q = qword(u, r.vi[is]);
            const std::uint32_t v = r.vi[it];
            for (int i = 0; i < 4; ++i) if (dest & (8u >> i)) std::memcpy(q + i * 4, &v, 4);
            break;
        }
        case 0x40:
            r.r = vu::rand_next(r.r);
            if (ft) for (int i = 0; i < 4; ++i) if (dest & (8u >> i)) r.vf[ft].w[i] = r.r | fp::kOne;
            break;
        case 0x41: if (ft) for (int i = 0; i < 4; ++i) if (dest & (8u >> i)) r.vf[ft].w[i] = (r.r & 0x7FFFFFu) | fp::kOne; break;
        case 0x42: r.r = r.vf[fs].w[fsf] & 0x7FFFFFu; break;
        case 0x43: r.r = (r.r ^ r.vf[fs].w[fsf]) & 0x7FFFFFu; break;
        case 0x64: if (ft) for (int i = 0; i < 4; ++i) if (dest & (8u >> i)) r.vf[ft].w[i] = r.p; break;
        // g_top and g_itop are VU1's, set by the VIF1 on the thread that runs VU1; VU0 (run from the EE thread) has no TOP and its own
        // ITOP in the VIF0. Sharing them lost VU1 its TOP whenever the VIF0 started a program, and gave VU0 whatever VU1 had latched.
        case 0x68: store_vi(r, it, u.code == g_vu1_code ? g_top : 0u); break;
        case 0x69: store_vi(r, it, u.code == g_vu1_code ? g_itop : vif::itop(0)); break;
        case 0x6C: xgkick(u, r.vi[is]); break;
        case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x78: case 0x79: case 0x7A: case 0x7C: case 0x7D: case 0x7E:
            vuops::efu(r, index, fs, fsf);
            break;
        case 0x7B: break;
        default:
            if (g_reported++ < 32) std::fprintf(stderr, "[vu] lower special 0x%X (0x%08X) at 0x%04X\n", index, w, pc);
            break;
        }
        return;
    }
    const std::uint32_t target = pc + 8u + static_cast<std::uint32_t>(imm11) * 8u;
    switch (w >> 25) {
    case 0x00: {
        const std::uint8_t *q = qword(u, r.vi[is] + static_cast<std::uint32_t>(imm11));
        if (ft) for (int i = 0; i < 4; ++i) if (dest & (8u >> i)) std::memcpy(&r.vf[ft].w[i], q + i * 4, 4);
        break;
    }
    case 0x01: {
        std::uint8_t *q = qword(u, r.vi[it] + static_cast<std::uint32_t>(imm11));
        for (int i = 0; i < 4; ++i) if (dest & (8u >> i)) std::memcpy(q + i * 4, &r.vf[fs].w[i], 4);
        break;
    }
    case 0x04: {
        const std::uint8_t *q = qword(u, r.vi[is] + static_cast<std::uint32_t>(imm11));
        for (int i = 0; i < 4; ++i)
            if (dest & (8u >> i)) {
                std::uint16_t v;
                std::memcpy(&v, q + i * 4, 2);
                store_vi(r, it, v);
                break;
            }
        break;
    }
    case 0x05: {
        std::uint8_t *q = qword(u, r.vi[is] + static_cast<std::uint32_t>(imm11));
        const std::uint32_t v = r.vi[it];
        for (int i = 0; i < 4; ++i) if (dest & (8u >> i)) std::memcpy(q + i * 4, &v, 4);
        break;
    }
    case 0x08: store_vi(r, it, r.vi[is] + (((w >> 10) & 0x7800u) | (w & 0x7FFu))); break;
    case 0x09: store_vi(r, it, r.vi[is] - (((w >> 10) & 0x7800u) | (w & 0x7FFu))); break;
    case 0x10: store_vi(r, 1, (clip_flags() & 0xFFFFFFu) == (w & 0xFFFFFFu) ? 1u : 0u); break;
    case 0x11:
        r.clip = w & 0xFFFFFFu;
        if (u.pipe) u.pipe->set_clip(r.clip, u.pipe->t - 4u);
        break;
    case 0x12: store_vi(r, 1, (clip_flags() & w & 0xFFFFFFu) ? 1u : 0u); break;
    case 0x13: store_vi(r, 1, ((clip_flags() | w) & 0xFFFFFFu) == 0xFFFFFFu ? 1u : 0u); break;
    case 0x14: store_vi(r, it, (status_flags() & 0xFFFu) == ((((w >> 10) & 0x800u) | (w & 0x7FFu))) ? 1u : 0u); break;
    case 0x15:
        r.status = (status_flags() & 0x3Fu) | ((((w >> 10) & 0x800u) | (w & 0x7FFu)) & 0xFC0u);
        if (u.pipe) u.pipe->force_mac(r.mac, r.status);
        break;
    case 0x16: store_vi(r, it, status_flags() & (((w >> 10) & 0x800u) | (w & 0x7FFu))); break;
    case 0x17: store_vi(r, it, ((status_flags() | (((w >> 10) & 0x800u) | (w & 0x7FFu))) & 0xFFFu) == 0xFFFu ? 1u : 0u); break;
    case 0x18: store_vi(r, it, (mac_flags_now() & 0xFFFFu) == r.vi[is] ? 1u : 0u); break;
    case 0x1A: store_vi(r, it, mac_flags_now() & r.vi[is]); break;
    case 0x1B: store_vi(r, it, ((mac_flags_now() | r.vi[is]) & 0xFFFFu) == 0xFFFFu ? 1u : 0u); break;
    case 0x1C: store_vi(r, it, clip_flags() & 0xFFFu); break;
    case 0x20: branch_to(target); break;
    case 0x21: store_vi(r, it, (pc + 16u) / 8u); branch_to(target); break;
    case 0x24: branch_to(static_cast<std::uint32_t>(r.vi[is]) * 8u); break;
    case 0x25: { const std::uint32_t t = static_cast<std::uint32_t>(r.vi[is]) * 8u; store_vi(r, it, (pc + 16u) / 8u); branch_to(t); break; }
    case 0x28: if (bvi(is) == bvi(it)) branch_to(target); break;
    case 0x29: if (bvi(is) != bvi(it)) branch_to(target); break;
    case 0x2C: if (static_cast<std::int16_t>(bvi(is)) < 0) branch_to(target); break;
    case 0x2D: if (static_cast<std::int16_t>(bvi(is)) > 0) branch_to(target); break;
    case 0x2E: if (static_cast<std::int16_t>(bvi(is)) <= 0) branch_to(target); break;
    case 0x2F: if (static_cast<std::int16_t>(bvi(is)) >= 0) branch_to(target); break;
    default:
        if (g_reported++ < 32) std::fprintf(stderr, "[vu] lower 0x%08X at 0x%04X\n", w, pc);
        break;
    }
}

}

void reset() {
    if (gs::queue::vu_active() && gs::queue::role() != gs::queue::Role::Vu) {
        gs::queue::vu().post(gs::queue::kVuReset, 0, 0, nullptr, 0);
        return;
    }
    std::memset(&g_vu1, 0, sizeof g_vu1);
    g_vu1.vf[0].w[3] = fp::kOne;
    g_top = g_itop = 0;
}

Vu0Regs &regs(int unit) { return unit ? g_vu1 : g_cpu.vu0; }

std::uint32_t tpc(int unit) { return regs(unit).tpc; }

void set_tops(std::uint32_t top, std::uint32_t itop) {
    g_top = top;
    g_itop = itop;
}

void run_fast(int n, std::uint32_t address);

std::unordered_map<std::uint64_t, ProgramFn> g_programs;
std::unordered_map<std::uint64_t, ProgramFn> g_vu0_programs;
void run_reference(int n, std::uint32_t address);
struct Vu0Entry {
    std::uint32_t generation = 0;
    std::uint64_t key = 0;
    ProgramFn function = nullptr;
};
Vu0Entry g_vu0_entries[512];
std::unordered_set<std::uint64_t> g_vu0_rejected;
std::uint64_t g_vu0_compiled = 0, g_vu0_interpreted = 0, g_vu0_checked = 0, g_vu0_mismatches = 0;

ProgramFn lookup_vu0(std::uint32_t address) {
    address &= 4095u;
    Vu0Entry &e = g_vu0_entries[address >> 3];
    if (e.generation == g_generation[0]) return e.function;
    e.generation = g_generation[0];
    e.key = program_key(g_vu0_code, reachable(g_vu0_code, 4095u, address), address);
    const auto it = g_vu0_programs.find(e.key);
    e.function = it == g_vu0_programs.end() || g_vu0_rejected.count(e.key) ? nullptr : it->second;
    static const std::string record = ps2::os::env("PS2_VU0_RECORD");
    if (!e.function && !record.empty()) {
        char name[64];
        std::snprintf(name, sizeof name, "%016llX_%04X.bin", static_cast<unsigned long long>(e.key), address);
        const auto path = ps2::os::path(record) / name;
        if (!std::filesystem::exists(path)) {
            std::filesystem::create_directories(ps2::os::path(record));
            std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char *>(g_vu0_code), 4096);
        }
    }
    return e.function;
}

bool same_vu0_regs(const Vu0Regs &a, const Vu0Regs &b) {
    // Compare architectural state, excluding compiler/ABI-dependent padding.
    return std::memcmp(a.vf, b.vf, sizeof a.vf) == 0 && std::memcmp(&a.acc, &b.acc, sizeof a.acc) == 0 &&
           std::memcmp(a.vi, b.vi, sizeof a.vi) == 0 && a.q == b.q && a.p == b.p && a.i == b.i && a.r == b.r &&
           a.mac == b.mac && a.status == b.status && a.clip == b.clip && a.cmsar0 == b.cmsar0 && a.tpc == b.tpc;
}

bool stopped() { return ps2::halted(); }

bool run_compiled_vu0(std::uint32_t address) {
    const ProgramFn fn = lookup_vu0(address);
    static const bool interpret_only = std::getenv("PS2_VU0_INTERPRET") != nullptr;
    if (!fn || interpret_only) return false;
    static const bool verify = std::getenv("PS2_VU0_VERIFY") != nullptr;
    if (!verify) { fn(g_cpu.vu0, g_vu0_mem); return true; }
    // Both executions start from identical state. Retain the exact result on a
    // mismatch and blacklist the program, so verification never installs bad state.
    const Vu0Regs before = g_cpu.vu0;
    std::uint8_t memory[4096], after_memory[4096];
    std::memcpy(memory, g_vu0_mem, sizeof memory);
    fn(g_cpu.vu0, g_vu0_mem);
    const Vu0Regs after = g_cpu.vu0;
    std::memcpy(after_memory, g_vu0_mem, sizeof after_memory);
    g_cpu.vu0 = before;
    std::memcpy(g_vu0_mem, memory, sizeof memory);
    run_reference(0, address);
    auto &checked = g_vu0_checked;
    auto &mismatches = g_vu0_mismatches;
    ++checked;
    const bool registers_match = same_vu0_regs(after, g_cpu.vu0);
    if (!registers_match || std::memcmp(after_memory, g_vu0_mem, sizeof after_memory)) {
        ++mismatches;
        Vu0Entry &e = g_vu0_entries[(address & 4095u) >> 3];
        std::fprintf(stderr, "[vu0-check] key %016llX entry %04X mismatch: regs %d mem %d; using reference\n",
                     static_cast<unsigned long long>(e.key), address,
                     !registers_match,
                     std::memcmp(after_memory, g_vu0_mem, sizeof after_memory) != 0);
        g_vu0_rejected.insert(e.key);
        for (Vu0Entry &entry : g_vu0_entries) entry.generation = 0;
    }
    if (checked == 1 || checked % 10000 == 0)
        std::fprintf(stderr, "[vu0-check-summary] %llu calls, %llu mismatches\n", static_cast<unsigned long long>(checked), static_cast<unsigned long long>(mismatches));
    return true;
}


struct Candidate {
    std::vector<std::uint32_t> runs;
    std::vector<std::uint8_t> code;
    std::uint64_t key = 0;
    ProgramFn function = nullptr;
    ProgramFn jit = nullptr;
};

struct EntryCache {
    std::uint32_t generation = 0;
    std::uint64_t key = 0;
    ProgramFn function = nullptr;
    ProgramFn jit = nullptr;
    std::vector<Candidate> candidates;
    std::uint32_t next = 0;
};

bool candidate_matches(const Candidate &c) {
    const std::uint8_t *code = c.code.data();
    for (std::size_t i = 0; i < c.runs.size(); i += 2) {
        if (std::memcmp(g_vu1_code + c.runs[i], code, c.runs[i + 1]) != 0) return false;
        code += c.runs[i + 1];
    }
    return true;
}

Candidate make_candidate(std::vector<std::uint32_t> pcs) {
    std::sort(pcs.begin(), pcs.end());
    Candidate c;
    for (std::uint32_t pc : pcs) {
        if (!c.runs.empty() && c.runs[c.runs.size() - 2] + c.runs.back() == pc) c.runs.back() += 8;
        else {
            c.runs.push_back(pc);
            c.runs.push_back(8);
        }
        c.code.insert(c.code.end(), g_vu1_code + pc, g_vu1_code + pc + 8);
    }
    return c;
}
std::unordered_map<std::uint64_t, ProgramFn> g_jit_programs;
std::vector<std::uint32_t> *g_kick_log = nullptr;
std::int64_t *g_capture = nullptr;

ProgramFn jit_program(std::uint64_t key, std::uint32_t address) {
    const auto it = g_jit_programs.find(key);
    if (it != g_jit_programs.end()) return it->second;
    const ProgramFn fn = jit::compile(g_vu1_code, address & 16383u);
    g_jit_programs[key] = fn;
    return fn;
}
EntryCache g_entries[2048];

void code_changed(int unit) {
    g_scan[unit & 1] = true;
    ++g_generation[unit & 1];
}

void register_programs(const Program *programs, std::uint32_t count) {
    for (std::uint32_t i = 0; i < count; ++i)
        if (programs[i].function) (programs[i].unit == 0 ? g_vu0_programs : g_programs)[programs[i].key] = programs[i].function;
    for (Vu0Entry &entry : g_vu0_entries) entry.generation = 0;
}

// Programs the background thread compiled and loaded (vu_live.cpp) join the table of built-in ones. A program the build knew keeps
// its built-in code. What lookup() remembered is thrown away so that programs that were unknown are looked at again.
void install_live() {
    for (const Program &p : live::take()) g_programs.emplace(p.key, p.function);
    for (EntryCache &e : g_entries) {
        e.generation = 0;
        e.candidates.clear();
        e.next = 0;
    }
}

void kick(int unit, std::uint32_t address) {
    if (g_kick_log && unit == 1) g_kick_log->push_back(address);
    Unit u = unit_of(unit);
    xgkick(u, address);
}

std::uint32_t top() { return g_top; }
std::uint32_t itop() { return g_itop; }

ProgramFn lookup(std::uint32_t address);

std::atomic<std::uint64_t> g_interpreted_calls{0};

std::uint64_t interpreted_calls() { return g_interpreted_calls.load(std::memory_order_relaxed); }

void check_jit(ProgramFn compiled, ProgramFn jit) {
    static std::uint64_t calls = 0, escapes = 0, mismatches = 0;
    static std::uint8_t before_mem[16384], jit_mem[16384];
    const Vu0Regs before = g_vu1;
    std::memcpy(before_mem, g_vu1_mem, sizeof before_mem);
    std::vector<std::uint32_t> jit_kicks, kicks;
    jit::g_dry = true;
    jit::g_escape = -1;
    jit::g_kicks = &jit_kicks;
    jit(g_vu1, g_vu1_mem);
    jit::g_dry = false;
    const std::int64_t jit_escape = jit::g_escape;
    const Vu0Regs after = g_vu1;
    std::memcpy(jit_mem, g_vu1_mem, sizeof jit_mem);
    g_vu1 = before;
    std::memcpy(g_vu1_mem, before_mem, sizeof before_mem);
    std::int64_t escape = -1;
    g_kick_log = &kicks;
    g_capture = &escape;
    compiled(g_vu1, g_vu1_mem);
    g_capture = nullptr;
    g_kick_log = nullptr;
    ++calls;
    if (escape >= 0) ++escapes;
    if (escape != jit_escape || std::memcmp(&after, &g_vu1, sizeof after) != 0 || std::memcmp(jit_mem, g_vu1_mem, sizeof jit_mem) != 0 || kicks != jit_kicks) {
        if (++mismatches <= 10) {
            int vf = -1;
            for (int i = 0; i < 32 && vf < 0; ++i)
                if (std::memcmp(&after.vf[i], &g_vu1.vf[i], 16) != 0) vf = i;
            std::fprintf(stderr, "[jit] mismatch at tpc 0x%X escape %lld/%lld: vf%d acc %d vi %d q/p/i/r %d flags %d mem %d kicks %zu/%zu\n", before.tpc, static_cast<long long>(jit_escape), static_cast<long long>(escape), vf,
                         std::memcmp(&after.acc, &g_vu1.acc, 16) != 0, std::memcmp(after.vi, g_vu1.vi, sizeof after.vi) != 0,
                         after.q != g_vu1.q || after.p != g_vu1.p || after.i != g_vu1.i || after.r != g_vu1.r,
                         after.mac != g_vu1.mac || after.status != g_vu1.status || after.clip != g_vu1.clip,
                         std::memcmp(jit_mem, g_vu1_mem, sizeof jit_mem) != 0, jit_kicks.size(), kicks.size());
        }
    }
    if (calls % 2000 == 0)
        std::fprintf(stderr, "[jit] checked %llu calls, %llu mismatches, %llu through the interpreter\n", static_cast<unsigned long long>(calls),
                     static_cast<unsigned long long>(mismatches), static_cast<unsigned long long>(escapes));
    if (escape >= 0) interpret(1, static_cast<std::uint32_t>(escape));
}

void run_entry(int unit, std::uint32_t pc);

bool g_chaining = false;
std::int64_t g_chained = -1;
std::uint64_t g_last_key = 0;

void interpret(int unit, std::uint32_t pc) {
    if (unit == 1 && g_capture) {
        *g_capture = pc;
        return;
    }
    if (unit != 1) {
        run_reference(unit, pc);
        return;
    }
    if (g_chaining) {
        g_chained = pc;
        return;
    }
    g_chaining = true;
    for (;;) {
        run_entry(1, pc);
        if (g_chained < 0) break;
        pc = static_cast<std::uint32_t>(g_chained);
        g_chained = -1;
    }
    g_chaining = false;
}

// PS2_VU_TIME=1: the time stamp counter ticks spent in each compiled vector program, printed when the runner ends (a headless
// run of N frames from a state: the same work every time, so two builds can be compared program by program).
std::uint64_t time_stamp() {
#if defined(__x86_64__) || defined(__i386__)
    return __builtin_ia32_rdtsc();
#else
    return static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
#endif
}

struct TimeRow {
    std::uint64_t calls = 0, ticks = 0;
};

std::unordered_map<std::uint64_t, TimeRow> &time_rows() {
    static std::unordered_map<std::uint64_t, TimeRow> rows;
    return rows;
}

std::chrono::steady_clock::time_point &time_start() {
    static std::chrono::steady_clock::time_point start;
    return start;
}
std::uint64_t &time_first_stamp() {
    static std::uint64_t v = 0;
    return v;
}
std::uint64_t &time_last_stamp() {
    static std::uint64_t v = 0;
    return v;
}

void time_report() {
    time_last_stamp() = time_stamp();
    std::uint64_t calls = 0, ticks = 0;
    std::vector<std::pair<std::uint64_t, TimeRow>> sorted(time_rows().begin(), time_rows().end());
    for (const auto &[k, row] : sorted) {
        calls += row.calls;
        ticks += row.ticks;
    }
    std::sort(sorted.begin(), sorted.end(), [](const auto &a, const auto &b) { return a.second.ticks > b.second.ticks; });
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - time_start()).count();
    std::fprintf(stderr, "[vu-time] %zu programs, %llu calls, %llu ticks in compiled programs (%.3f ticks per nanosecond of run time)\n", sorted.size(), static_cast<unsigned long long>(calls),
                 static_cast<unsigned long long>(ticks), static_cast<double>(time_first_stamp()) > 0 ? static_cast<double>(time_last_stamp() - time_first_stamp()) / (seconds * 1e9) : 0.0);
    for (std::size_t i = 0; i < sorted.size() && i < 12; ++i)
        std::fprintf(stderr, "[vu-time] %016llX calls %llu ticks %llu (%llu per call)\n", static_cast<unsigned long long>(sorted[i].first),
                     static_cast<unsigned long long>(sorted[i].second.calls), static_cast<unsigned long long>(sorted[i].second.ticks),
                     static_cast<unsigned long long>(sorted[i].second.ticks / std::max<std::uint64_t>(1, sorted[i].second.calls)));
}

void time_note(std::uint64_t key, std::uint64_t ticks) {
    static const bool registered = (std::atexit(time_report), time_start() = std::chrono::steady_clock::now(), time_first_stamp() = time_stamp(), true);
    (void)registered;
    TimeRow &row = time_rows()[key];
    ++row.calls;
    row.ticks += ticks;
}

void run_entry(int unit, std::uint32_t pc) {
    if (unit == 1) g_trace_vu1_pc = pc;
    if (unit == 1) { static const bool log_runs = std::getenv("PS2_TRACE_VU1RUN") != nullptr; if (log_runs) std::fprintf(stderr, "[vu1-run] pc %X\n", pc); }
    // Compiled programs are the default; retain an interpreter override for
    // differential checks and diagnosing newly recorded programs.
    static const bool interpret_only = std::getenv("PS2_VU_INTERPRET") != nullptr;
    static const bool count = std::getenv("PS2_VU_COUNT") != nullptr;
    static std::uint64_t compiled = 0, interpreted = 0;
    static std::unordered_map<std::uint32_t, std::uint64_t> misses;
    if (unit == 1 && !interpret_only) {
        if (live::ready()) install_live();
        const ProgramFn fn = lookup(pc);
        if (fn) {
            ++compiled;
            const ProgramFn jit = g_entries[(pc & 16383u) >> 3].jit;
            static std::uint64_t sampled = 0;
            static const bool check_vs_interpreter = std::getenv("PS2_VU_CHECK") != nullptr;
            static bool checking = false;
            if (check_vs_interpreter && !checking && ++sampled % 64 == 0) {
                // The interpreter is the reference: run the compiled function
                // and, from the same state, the interpreter, and report the
                // first registers or memory they disagree on.
                static std::uint64_t checked = 0, mismatches = 0;
                static std::uint8_t before_mem[16384], run_mem[16384];
                const Vu0Regs before = g_vu1;
                std::memcpy(before_mem, g_vu1_mem, sizeof before_mem);
                std::int64_t escape = -1;
                const std::uint64_t key = program_key(g_vu1_code, reachable(g_vu1_code, 16383u, pc & 16383u), pc & 16383u);
                g_capture = &escape;
                fn(g_vu1, g_vu1_mem);
                g_capture = nullptr;
                const Vu0Regs after = g_vu1;
                std::memcpy(run_mem, g_vu1_mem, sizeof run_mem);
                g_vu1 = before;
                std::memcpy(g_vu1_mem, before_mem, sizeof g_vu1_mem);
                // The reference is the interpreter loop itself: interpret()
                // would only queue the entry while a chain is running.
                checking = true;
                g_reference_dry = true;
                run_fast(1, pc);
                g_reference_dry = false;
                checking = false;
                if (escape < 0 && (std::memcmp(&after, &g_vu1, sizeof after) != 0 || std::memcmp(run_mem, g_vu1_mem, sizeof run_mem) != 0)) {
                    if (++mismatches <= 20) {
                        int vf = -1;
                        for (int i = 0; i < 32 && vf < 0; ++i)
                            if (std::memcmp(&after.vf[i], &g_vu1.vf[i], 16) != 0) vf = i;
                        int vi = -1;
                        for (int i = 0; i < 16 && vi < 0; ++i)
                            if (after.vi[i] != g_vu1.vi[i]) vi = i;
                        std::uint32_t word = ~0u;
                        for (int i = 0; i < 16384 && word == ~0u; ++i) word = run_mem[i] ^ g_vu1_mem[i] ? i : ~0u;
                        std::fprintf(stderr, "[vu-check] key %016llX entry %04X call %llu: vf%d vi%d q/p/i/r %d flags %d mem %d\n", static_cast<unsigned long long>(key),
                                     pc & 16383u, static_cast<unsigned long long>(checked), vf, vi,
                                     after.q != g_vu1.q || after.p != g_vu1.p || after.i != g_vu1.i || after.r != g_vu1.r,
                                     after.mac != g_vu1.mac || after.status != g_vu1.status || after.clip != g_vu1.clip, word == ~0u ? -1 : static_cast<int>(word));
                    }
                }
                ++checked;
                if (checked == 1 || checked % 100 == 0)
                    std::fprintf(stderr, "[vu-check-summary] sampled %llu calls, %llu mismatches\n", static_cast<unsigned long long>(checked), static_cast<unsigned long long>(mismatches));
                g_vu1 = after;
                std::memcpy(g_vu1_mem, run_mem, sizeof g_vu1_mem);
                if (escape >= 0) interpret(1, static_cast<std::uint32_t>(escape));
                return;
            }
            static const bool timed = std::getenv("PS2_VU_TIME") != nullptr;
            if (timed) {
                const std::uint64_t t0 = time_stamp();
                fn(g_vu1, g_vu1_mem);
                time_note(g_entries[(pc & 16383u) >> 3].key, time_stamp() - t0);
                return;
            }
            if (jit && jit != fn && ++sampled % 32 == 0) check_jit(fn, jit);
            else fn(g_vu1, g_vu1_mem);
            return;
        }
    }
    if (unit == 1) g_interpreted_calls.fetch_add(1, std::memory_order_relaxed);
    if (unit == 1 && !interpret_only && live::active()) live::note_call(g_entries[(pc & 16383u) >> 3].key);
    if (count && unit == 1) {
        ++misses[pc];
        if (++interpreted % 5000 == 0) {
            std::fprintf(stderr, "[vu1] compiled %llu interpreted %llu:", static_cast<unsigned long long>(compiled), static_cast<unsigned long long>(interpreted));
            for (const auto &[a, n] : misses) std::fprintf(stderr, " %04X:%llu", a, static_cast<unsigned long long>(n));
            std::fprintf(stderr, "\n");
        }
    }
    run_fast(unit, pc);
}

ProgramFn lookup(std::uint32_t address) {
    EntryCache &e = g_entries[(address & 16383u) >> 3];
    g_last_key = 0;
    if (e.generation == g_generation[1]) return e.function;
    e.generation = g_generation[1];
    for (const Candidate &c : e.candidates)
        if (candidate_matches(c)) {
            e.function = c.function;
            e.jit = c.jit;
            e.key = c.key;
            return e.function;
        }
    const std::vector<std::uint32_t> pcs = reachable(g_vu1_code, 16383u, address);
    const std::uint64_t key = program_key(g_vu1_code, pcs, address & 16383u);
    g_last_key = key;
    e.key = key;
    const auto it = g_programs.find(key);
    e.function = it == g_programs.end() ? nullptr : it->second;
    e.jit = nullptr;
    const int jit_mode = jit::mode();
    if (jit_mode == 2 || (jit_mode >= 1 && !e.function)) e.function = jit_program(key, address);
    else if (jit_mode == 3) e.jit = jit_program(key, address);
    Candidate c = make_candidate(pcs);
    c.key = key;
    c.function = e.function;
    c.jit = e.jit;
    if (e.candidates.size() < 8) e.candidates.push_back(std::move(c));
    else e.candidates[e.next++ % 8] = std::move(c);
    if (!e.function) live::unknown(key, address & 16383u, g_vu1_code);
    static const std::string record = ps2::os::env("PS2_VU_RECORD");
    if (!e.function && (!record.empty() || g_options.on_vu_program)) {
        char name[64];
        std::snprintf(name, sizeof name, "%016llX_%04X.bin", static_cast<unsigned long long>(key), address & 16383u);
        if (g_options.on_vu_program) {
            static std::unordered_set<std::string> reported;
            if (reported.insert(name).second) g_options.on_vu_program(name, g_vu1_code, 16384);
            return e.function;
        }
        const std::filesystem::path path = ps2::os::path(record) / name;
        if (!std::filesystem::exists(path)) {
            std::filesystem::create_directories(ps2::os::path(record));
            std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char *>(g_vu1_code), 16384);
        }
    }
    return e.function;
}

void run_fast(int n, std::uint32_t address) {
    Unit u = unit_of(n);
    Vu0Regs &r = *u.regs;
    if (g_scan[n]) scan_flags(n, u.code, u.code_mask + 1);
    const bool flags = g_flags_used[n], clock = g_clock_used[n];
    Decoded *cache = g_decoded[n];
    vuops::FlagPipe pipe;
    pipe.start(r);
    u.pipe = &pipe;
    std::uint32_t vf_ready[32][4] = {}, vi_ready[16] = {};
    std::uint32_t pc = address & u.code_mask, npc = (pc + 8u) & u.code_mask;
    bool ending = false;
    UpperOut out;
    static std::uint64_t total = 0, calls = 0;
    static const bool count = std::getenv("PS2_VU_COUNT") != nullptr;
    static const bool record_jumps = !ps2::os::env("PS2_VU_RECORD").empty() || static_cast<bool>(g_options.on_vu_program);
    if (count && ++calls % 20000 == 0) std::fprintf(stderr, "[vu1] %llu calls, %llu instructions\n", (unsigned long long)calls, (unsigned long long)total);
    BranchView view;
    for (std::uint32_t steps = 0; steps < 1'000'000; ++steps, ++total) {
        if (!(steps & 0xFFu) && halted()) return;  // the window was closed: a program that does not end must not hold the close up for seconds
        std::uint32_t lo, hi;
        std::memcpy(&lo, u.code + pc, 4);
        std::memcpy(&hi, u.code + pc + 4, 4);
        Decoded &d = cache[pc >> 3];
        if (!d.valid || d.lo != lo || d.hi != hi) {
            d = decode_upper(lo, hi);
            if (g_scan[n]) scan_flags(n, u.code, u.code_mask + 1);
        }
        const std::uint32_t current = pc;
        pc = npc;
        npc = (npc + 8u) & u.code_mask;
        if (clock) {
            pipe.t = issue_cycle(d.use, pipe.t + 1u, vf_ready, vi_ready, pipe.q_ready);
            retire(d.use, pipe.t, vf_ready, vi_ready);
        } else {
            ++pipe.t;
        }
        read_q(u, pipe, r, current, hi);
        fast_upper(r, d, out);
        std::uint16_t was[16];
        view.snapshot(r, was);
        view.arm(u);
        if (hi & 0x80000000u) {
            r.i = lo;
        } else {
            bool branch = false;
            std::uint32_t target = npc;
            lower(u, lo, current, target, branch);
            if (branch) npc = target;
            if (branch && record_jumps && n == 1 && ((lo >> 25) == 0x24u || (lo >> 25) == 0x25u)) lookup(target);
        }
        u.branch_vi = nullptr;
        view.commit(was, lo, !(hi & 0x80000000u));
        if (out.target > 0) {
            Reg128 &dst = out.target == kAcc ? r.acc : r.vf[out.target];
            for (int i = 0; i < 4; ++i)
                if (out.dest & (8u >> i)) dst.w[i] = out.value.w[i];
        }
        if (out.target >= 0 && out.flags && flags) {
            set_flags(r, out.value, out.dest);
            pipe.set_mac(r.mac, r.status);
        }
        if (out.has_clip) {
            r.clip = out.clip;
            pipe.set_clip(out.clip, pipe.t);
        }
        r.vf[0].d[0] = 0;
        r.vf[0].w[2] = 0;
        r.vf[0].w[3] = fp::kOne;
        r.vi[0] = 0;
        if (ending) {
            pipe.flush_q(r);
            r.tpc = pc;
            return;
        }
        if (hi & 0x40000000u) ending = true;
    }
    std::fprintf(stderr, "[vu%d] program at 0x%04X did not end\n", n, address);
    if (std::getenv("PS2_VU_TRACE_STUCK")) {  // where a program that never ends is stuck
        std::fprintf(stderr, "[vu%d] stuck at pc 0x%04X, vi:", n, pc);
        for (int i = 0; i < 16; ++i) std::fprintf(stderr, " %04X", r.vi[i]);
        std::fprintf(stderr, "\n[vu%d] code around pc:", n);
        for (std::uint32_t k = 0; k < 8; ++k) {
            const std::uint32_t a = (pc + k * 8u - 24u) & u.code_mask;
            std::uint32_t lo, hi;
            std::memcpy(&lo, u.code + a, 4);
            std::memcpy(&hi, u.code + a + 4, 4);
            std::fprintf(stderr, " [%04X] %08X %08X", a, hi, lo);
        }
        std::fprintf(stderr, "\n");
    }
}

void run(int n, std::uint32_t address) {
    if (n == 0) {
        static const bool report = [] {
            if (std::getenv("PS2_VU0_STATS") || std::getenv("PS2_VU0_VERIFY")) std::atexit([] {
                std::fprintf(stderr, "[vu0] compiled %llu interpreted %llu checked %llu mismatches %llu\n",
                    static_cast<unsigned long long>(g_vu0_compiled), static_cast<unsigned long long>(g_vu0_interpreted),
                    static_cast<unsigned long long>(g_vu0_checked), static_cast<unsigned long long>(g_vu0_mismatches));
            });
            return true;
        }();
        (void)report;
        const bool compiled = run_compiled_vu0(address);
        if (compiled) ++g_vu0_compiled; else ++g_vu0_interpreted;
        static const bool stats = std::getenv("PS2_VU0_STATS") != nullptr;
        if (stats && (g_vu0_compiled + g_vu0_interpreted) % 1000000 == 0)
            std::fprintf(stderr, "[vu0] compiled %llu interpreted %llu\n", static_cast<unsigned long long>(g_vu0_compiled), static_cast<unsigned long long>(g_vu0_interpreted));
        if (compiled) return;
    }
    static const bool exact = std::getenv("PS2_VU_EXACT") != nullptr;
    if (n == 1 && !exact) { interpret(n, address); return; }
    run_reference(n, address);
}

void run_reference(int n, std::uint32_t address) {
    Unit u = unit_of(n);
    Vu0Regs &r = *u.regs;
    if (g_scan[n]) scan_flags(n, u.code, u.code_mask + 1);
    static const bool log_calls = std::getenv("PS2_VU0_LOG") != nullptr;
    auto log_state = [&](const char *tag) {  // PS2_VU0_LOG=1: what each VU0 call sees and leaves (hashes of the code, the data and the registers), to compare two runs
        auto fnv = [](const void *p, std::size_t len) { std::uint32_t h = 2166136261u; for (std::size_t i = 0; i < len; ++i) h = (h ^ static_cast<const std::uint8_t *>(p)[i]) * 16777619u; return h; };
        std::fprintf(stderr, "[%s] frame %llu addr %04X code %08X data %08X vf %08X vi", tag, static_cast<unsigned long long>(frame_count()), address, fnv(u.code, 4096), fnv(g_vu0_mem, 4096), fnv(u.regs->vf, 32 * 16));
        for (int i = 0; i < 16; ++i) std::fprintf(stderr, " %04X", u.regs->vi[i]);
        std::fprintf(stderr, " q %08X i %08X r %08X mac %04X st %03X clip %06X acc %08X\n", u.regs->q, u.regs->i, u.regs->r, u.regs->mac, u.regs->status, u.regs->clip, fnv(&u.regs->acc, 16));
    };
    if (log_calls && n == 0) log_state("vu0call");
    const bool clock = g_clock_used[n];
    vuops::FlagPipe pipe;
    pipe.start(r);
    u.pipe = &pipe;
    std::uint32_t vf_ready[32][4] = {}, vi_ready[16] = {};
    Decoded *cache = g_decoded[n];
    std::uint32_t pc = address & u.code_mask, npc = (pc + 8u) & u.code_mask;
    bool ending = false;
    BranchView view;
    for (std::uint32_t steps = 0; steps < 1'000'000; ++steps) {
        if (!(steps & 0xFFu) && halted()) return;  // the window was closed: a program that does not end must not hold the close up for seconds
        std::uint32_t lo, hi;
        std::memcpy(&lo, u.code + pc, 4);
        std::memcpy(&hi, u.code + pc + 4, 4);
        const std::uint32_t current = pc;
        pc = npc;
        npc = (npc + 8u) & u.code_mask;
        if (clock) {
            Decoded &d = cache[current >> 3];
            if (!d.valid || d.lo != lo || d.hi != hi) d = decode_upper(lo, hi);
            pipe.t = issue_cycle(d.use, pipe.t + 1u, vf_ready, vi_ready, pipe.q_ready);
            retire(d.use, pipe.t, vf_ready, vi_ready);
        } else {
            ++pipe.t;
        }
        read_q(u, pipe, r, current, hi);
        UpperOut out = upper(r, hi);
        bool branch = false;
        std::uint16_t was[16];
        view.snapshot(r, was);
        view.arm(u);
        if (hi & 0x80000000u) {
            r.i = lo;
        } else {
            std::uint32_t target = npc;
            lower(u, lo, current, target, branch);
            if (branch) npc = target;
        }
        u.branch_vi = nullptr;
        view.commit(was, lo, !(hi & 0x80000000u));
        commit(r, out);
        if (out.target >= 0 && out.flags) pipe.set_mac(r.mac, r.status);
        if (out.has_clip) {
            r.clip = out.clip;
            pipe.set_clip(out.clip, pipe.t);
        }
        r.vf[0].d[0] = 0;
        r.vf[0].w[2] = 0;
        r.vf[0].w[3] = fp::kOne;
        r.vi[0] = 0;
        if (ending) {
            if (log_calls && n == 0) log_state("vu0ret");
            pipe.flush_q(r);
            r.tpc = pc;
            return;
        }
        if (hi & 0x40000000u) ending = true;
    }
    std::fprintf(stderr, "[vu%d] program at 0x%04X did not end\n", n, address);
    if (std::getenv("PS2_VU_TRACE_STUCK")) {  // where a program that never ends is stuck
        std::fprintf(stderr, "[vu%d] stuck at pc 0x%04X, vi:", n, pc);
        for (int i = 0; i < 16; ++i) std::fprintf(stderr, " %04X", r.vi[i]);
        std::fprintf(stderr, "\n[vu%d] code around pc:", n);
        for (std::uint32_t k = 0; k < 8; ++k) {
            const std::uint32_t a = (pc + k * 8u - 24u) & u.code_mask;
            std::uint32_t lo, hi;
            std::memcpy(&lo, u.code + a, 4);
            std::memcpy(&hi, u.code + a + 4, 4);
            std::fprintf(stderr, " [%04X] %08X %08X", a, hi, lo);
        }
        std::fprintf(stderr, "\n");
    }
}

}

namespace ps2 {

void vcallms(Cpu &, std::uint32_t address) { vu::run(0, address); }

}

__attribute__((weak)) void register_generated_vu() {}

namespace ps2::vu {

// PS2_DUMP_VU1AT (see the GS draw trace): the vector registers and the 16 KiB of data memory of unit 1, as text, at one primitive.
void trace_dump(const char *path) {
    std::FILE *f = std::fopen(path, "w");
    if (!f) return;
    for (int i = 0; i < 32; ++i) {
        float v[4];
        std::memcpy(v, &g_vu1.vf[i], 16);
        std::fprintf(f, "vf%02d %g %g %g %g\n", i, v[0], v[1], v[2], v[3]);
    }
    for (int qw = 0; qw < 1024; ++qw) {
        float v[4];
        std::uint32_t w[4];
        std::memcpy(v, g_vu1_mem + qw * 16, 16);
        std::memcpy(w, g_vu1_mem + qw * 16, 16);
        std::fprintf(f, "m%03X %08X %08X %08X %08X | %g %g %g %g\n", qw, w[0], w[1], w[2], w[3], v[0], v[1], v[2], v[3]);
    }
    std::fclose(f);
}

void state(ps2::state::Stream &s) {
    s.tag("vu");
    s.pod(g_vu1);
    s.pod(g_top);
    s.pod(g_itop);
    s.bytes(g_vu0_code, sizeof g_vu0_code);
    s.bytes(g_vu0_mem, sizeof g_vu0_mem);
    s.bytes(g_vu1_code, sizeof g_vu1_code);
    s.bytes(g_vu1_mem, sizeof g_vu1_mem);
    if (s.loading()) {
        code_changed(0);
        code_changed(1);
    }
}

}
