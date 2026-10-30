#include "vu_jit.hpp"

#include "ps2/vu_ops.hpp"

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>

namespace ps2::vu::jit {

const char *const kImportNames[kImports] = {
    "mac", "status", "clip", "force_mac", "efu", "kick", "top", "itop", "interpret",
};

namespace {

constexpr std::uint32_t kMask = 16383u;
constexpr int kAcc = 32;

struct ImportType {
    std::uint8_t params;
    bool result;
};

constexpr ImportType kImportTypes[kImports] = {
    {1, true}, {1, true}, {1, true}, {2, false}, {4, false}, {1, false}, {0, true}, {0, true}, {2, false},
};

enum Local : std::uint32_t {
    kR, kMem, kPend, kPendj, kEnding, kJpc, kPcw, kExit, kUclip, kTmp, kTmp2, kMacv, kStat, kSlot, kT, kSign, kExp, kBi, kBk, kBkNext, kVi0, kI32Locals = kVi0 + 16,
    kBits = kI32Locals, kUv, kV0, kV1, kV2, kV3, kV4, kV5, kShadow0
};
constexpr std::uint32_t kI32Count = kI32Locals - 2;
constexpr std::uint32_t kVecTemps = kShadow0 - kUv;

constexpr std::uint32_t kOne = 0x3F800000u;

std::uint32_t vf_off(unsigned reg) { return static_cast<std::uint32_t>(offsetof(Vu0Regs, vf) + reg * 16u); }
std::uint32_t reg_off(int reg) { return reg == kAcc ? static_cast<std::uint32_t>(offsetof(Vu0Regs, acc)) : vf_off(static_cast<unsigned>(reg)); }
std::uint32_t vi_off(unsigned i) { return static_cast<std::uint32_t>(offsetof(Vu0Regs, vi) + i * 2u); }
constexpr std::uint32_t kOffQ = offsetof(Vu0Regs, q), kOffP = offsetof(Vu0Regs, p), kOffI = offsetof(Vu0Regs, i), kOffR = offsetof(Vu0Regs, r);
constexpr std::uint32_t kOffMac = offsetof(Vu0Regs, mac);
constexpr std::uint32_t kOffMh = offsetof(vuops::FlagPipe, mh), kOffCh = offsetof(vuops::FlagPipe, ch), kOffMacTs = offsetof(vuops::FlagPipe, mac_ts),
                        kOffMacV = offsetof(vuops::FlagPipe, mac_v), kOffStatusV = offsetof(vuops::FlagPipe, status_v), kOffClipTs = offsetof(vuops::FlagPipe, clip_ts),
                        kOffClipV = offsetof(vuops::FlagPipe, clip_v), kOffT = offsetof(vuops::FlagPipe, t), kOffQPending = offsetof(vuops::FlagPipe, q_pending),
                        kOffQValue = offsetof(vuops::FlagPipe, q_value), kOffQReady = offsetof(vuops::FlagPipe, q_ready);
constexpr std::uint32_t kOffStatus = offsetof(Vu0Regs, status), kOffClip = offsetof(Vu0Regs, clip), kOffTpc = offsetof(Vu0Regs, tpc);

bool lane(unsigned dest, int i) { return dest & (8u >> i); }

struct Out {
    std::vector<std::uint8_t> b;
    void byte(std::uint8_t v) { b.push_back(v); }
    void u32(std::uint32_t v) {
        do {
            std::uint8_t c = v & 0x7Fu;
            v >>= 7;
            if (v) c |= 0x80u;
            b.push_back(c);
        } while (v);
    }
    void s32(std::int32_t v) {
        for (;;) {
            const std::uint8_t c = static_cast<std::uint8_t>(v & 0x7F);
            v >>= 7;
            if ((v == 0 && !(c & 0x40)) || (v == -1 && (c & 0x40))) {
                b.push_back(c);
                return;
            }
            b.push_back(c | 0x80u);
        }
    }
    void s64(std::int64_t v) {
        for (;;) {
            const std::uint8_t c = static_cast<std::uint8_t>(v & 0x7F);
            v >>= 7;
            if ((v == 0 && !(c & 0x40)) || (v == -1 && (c & 0x40))) {
                b.push_back(c);
                return;
            }
            b.push_back(c | 0x80u);
        }
    }
    void name(const char *s) {
        u32(static_cast<std::uint32_t>(std::strlen(s)));
        b.insert(b.end(), s, s + std::strlen(s));
    }
    void section(std::uint8_t id, const Out &body) {
        byte(id);
        u32(static_cast<std::uint32_t>(body.b.size()));
        b.insert(b.end(), body.b.begin(), body.b.end());
    }
};

struct Code : Out {
    std::uint32_t depth = 0;
    void simd(std::uint32_t o) {
        byte(0xFD);
        u32(o);
    }
    void i32c(std::uint32_t v) {
        byte(0x41);
        s32(static_cast<std::int32_t>(v));
    }
    void i64c(std::uint64_t v) {
        byte(0x42);
        s64(static_cast<std::int64_t>(v));
    }
    void get(std::uint32_t l) {
        byte(0x20);
        u32(l);
    }
    void set(std::uint32_t l) {
        byte(0x21);
        u32(l);
    }
    void tee(std::uint32_t l) {
        byte(0x22);
        u32(l);
    }
    void load(std::uint32_t off) {
        byte(0x28);
        u32(2);
        u32(off);
    }
    void load8(std::uint32_t off) {
        byte(0x2D);
        u32(0);
        u32(off);
    }
    void load16(std::uint32_t off) {
        byte(0x2F);
        u32(1);
        u32(off);
    }
    void store(std::uint32_t off) {
        byte(0x36);
        u32(2);
        u32(off);
    }
    void store8(std::uint32_t off) {
        byte(0x3A);
        u32(0);
        u32(off);
    }
    void store16(std::uint32_t off) {
        byte(0x3B);
        u32(1);
        u32(off);
    }
    void vload(std::uint32_t off) {
        simd(0x00);
        u32(4);
        u32(off);
    }
    void vstore(std::uint32_t off) {
        simd(0x0B);
        u32(4);
        u32(off);
    }
    void vconst(std::uint32_t a, std::uint32_t b2, std::uint32_t c, std::uint32_t d) {
        simd(0x0C);
        for (std::uint32_t w : {a, b2, c, d})
            for (int k = 0; k < 4; ++k) byte(static_cast<std::uint8_t>(w >> (k * 8)));
    }
    void vsplatc(std::uint32_t v) { vconst(v, v, v, v); }
    void shuffle(const std::uint8_t (&lanes)[16]) {
        simd(0x0D);
        for (std::uint8_t l : lanes) byte(l);
    }
    void call(std::uint32_t f) {
        byte(0x10);
        u32(f);
    }
    void tail(std::uint32_t f) {
        byte(0x12);
        u32(f);
    }
    void op(std::uint8_t o) { byte(o); }
    void open_if() {
        byte(0x04);
        byte(0x40);
        ++depth;
    }
    void end() {
        byte(0x0B);
        --depth;
    }
};

enum : std::uint8_t {
    kOpDrop = 0x1A, kOpSelect = 0x1B, kOpEq = 0x46, kOpNe = 0x47, kOpLtS = 0x48, kOpGtS = 0x4A, kOpLeS = 0x4C, kOpGeS = 0x4E, kOpGeU = 0x4F,
    kOpAdd = 0x6A, kOpSub = 0x6B, kOpMul = 0x6C, kOpAnd = 0x71, kOpOr = 0x72, kOpXor = 0x73, kOpShl = 0x74, kOpShrU = 0x76, kOpExt16 = 0xC1, kOpBr = 0x0C,
    kOpI64Add = 0x7C, kOpI64Or = 0x84, kOpI64Shl = 0x86, kOpI64ShrU = 0x88, kOpF64Sqrt = 0x9F, kOpF64Div = 0xA3, kOpWrap = 0xA7, kOpExtendU = 0xAD,
    kOpI64FromF64 = 0xBD, kOpF64FromI64 = 0xBF,
};

enum : std::uint32_t {
    kVSplat = 0x11, kVExtract = 0x1B, kI32x4Eq = 0x37, kI32x4LtS = 0x39, kI32x4GtS = 0x3B, kI32x4LeS = 0x3D, kI32x4GeS = 0x3F, kF32x4Lt = 0x43,
    kF32x4Ge = 0x46, kVAnd = 0x4E, kVAndnot = 0x4F, kVOr = 0x50, kVBitselect = 0x52, kI32x4Neg = 0xA1, kI32x4Add = 0xAE, kI32x4MinS = 0xB6,
    kF32x4Add = 0xE4, kF32x4Sub = 0xE5, kF32x4Mul = 0xE6, kI32x4TruncF32x4S = 0xF8, kF32x4ConvertI32x4S = 0xFA,
};

enum Kind { Nop, Add, Sub, Madd, Msub, Max, Min, Mul, Abs, Ftoi, Itof, Clip, Opmula, Opmsub };
enum Src { Vec, Bc, Q, I };

struct Up {
    Kind kind = Nop;
    Src src = Vec;
    int target = -1;
    bool flags = false;
    int frac = 0;
    unsigned dest = 0, ft = 0, fs = 0, bc = 0;
};

Up decode_upper(std::uint32_t hi) {
    Up u;
    u.dest = (hi >> 21) & 15u;
    u.ft = (hi >> 16) & 31u;
    u.fs = (hi >> 11) & 31u;
    u.bc = hi & 3u;
    const unsigned fd = (hi >> 6) & 31u;
    const unsigned funct = hi & 63u;
    static const int kFrac[4] = {0, 4, 12, 15};
    const auto set = [&](Kind k, Src s, int t, bool f) {
        u.kind = k;
        u.src = s;
        u.target = t;
        u.flags = f;
    };
    if (funct < 0x3C) {
        switch (funct) {
        case 0x00: case 0x01: case 0x02: case 0x03: set(Add, Bc, static_cast<int>(fd), true); break;
        case 0x04: case 0x05: case 0x06: case 0x07: set(Sub, Bc, static_cast<int>(fd), true); break;
        case 0x08: case 0x09: case 0x0A: case 0x0B: set(Madd, Bc, static_cast<int>(fd), true); break;
        case 0x0C: case 0x0D: case 0x0E: case 0x0F: set(Msub, Bc, static_cast<int>(fd), true); break;
        case 0x10: case 0x11: case 0x12: case 0x13: set(Max, Bc, static_cast<int>(fd), false); break;
        case 0x14: case 0x15: case 0x16: case 0x17: set(Min, Bc, static_cast<int>(fd), false); break;
        case 0x18: case 0x19: case 0x1A: case 0x1B: set(Mul, Bc, static_cast<int>(fd), true); break;
        case 0x1C: set(Mul, Q, static_cast<int>(fd), true); break;
        case 0x1D: set(Max, I, static_cast<int>(fd), false); break;
        case 0x1E: set(Mul, I, static_cast<int>(fd), true); break;
        case 0x1F: set(Min, I, static_cast<int>(fd), false); break;
        case 0x20: set(Add, Q, static_cast<int>(fd), true); break;
        case 0x21: set(Madd, Q, static_cast<int>(fd), true); break;
        case 0x22: set(Add, I, static_cast<int>(fd), true); break;
        case 0x23: set(Madd, I, static_cast<int>(fd), true); break;
        case 0x24: set(Sub, Q, static_cast<int>(fd), true); break;
        case 0x25: set(Msub, Q, static_cast<int>(fd), true); break;
        case 0x26: set(Sub, I, static_cast<int>(fd), true); break;
        case 0x27: set(Msub, I, static_cast<int>(fd), true); break;
        case 0x28: set(Add, Vec, static_cast<int>(fd), true); break;
        case 0x29: set(Madd, Vec, static_cast<int>(fd), true); break;
        case 0x2A: set(Mul, Vec, static_cast<int>(fd), true); break;
        case 0x2B: set(Max, Vec, static_cast<int>(fd), false); break;
        case 0x2C: set(Sub, Vec, static_cast<int>(fd), true); break;
        case 0x2D: set(Msub, Vec, static_cast<int>(fd), true); break;
        case 0x2E: set(Opmsub, Vec, static_cast<int>(fd), true); break;
        case 0x2F: set(Min, Vec, static_cast<int>(fd), false); break;
        default: break;
        }
    } else {
        const unsigned index = ((hi >> 6) & 31u) * 4u + (funct & 3u);
        switch (index) {
        case 0x00: case 0x01: case 0x02: case 0x03: set(Add, Bc, kAcc, true); break;
        case 0x04: case 0x05: case 0x06: case 0x07: set(Sub, Bc, kAcc, true); break;
        case 0x08: case 0x09: case 0x0A: case 0x0B: set(Madd, Bc, kAcc, true); break;
        case 0x0C: case 0x0D: case 0x0E: case 0x0F: set(Msub, Bc, kAcc, true); break;
        case 0x10: case 0x11: case 0x12: case 0x13: set(Itof, Vec, static_cast<int>(u.ft), false); u.frac = kFrac[index & 3u]; break;
        case 0x14: case 0x15: case 0x16: case 0x17: set(Ftoi, Vec, static_cast<int>(u.ft), false); u.frac = kFrac[index & 3u]; break;
        case 0x18: case 0x19: case 0x1A: case 0x1B: set(Mul, Bc, kAcc, true); break;
        case 0x1C: set(Mul, Q, kAcc, true); break;
        case 0x1D: set(Abs, Vec, static_cast<int>(u.ft), false); break;
        case 0x1E: set(Mul, I, kAcc, true); break;
        case 0x1F: set(Clip, Vec, -1, false); break;
        case 0x20: set(Add, Q, kAcc, true); break;
        case 0x21: set(Madd, Q, kAcc, true); break;
        case 0x22: set(Add, I, kAcc, true); break;
        case 0x23: set(Madd, I, kAcc, true); break;
        case 0x24: set(Sub, Q, kAcc, true); break;
        case 0x25: set(Msub, Q, kAcc, true); break;
        case 0x26: set(Sub, I, kAcc, true); break;
        case 0x27: set(Msub, I, kAcc, true); break;
        case 0x28: set(Add, Vec, kAcc, true); break;
        case 0x29: set(Madd, Vec, kAcc, true); break;
        case 0x2A: set(Mul, Vec, kAcc, true); break;
        case 0x2C: set(Sub, Vec, kAcc, true); break;
        case 0x2D: set(Msub, Vec, kAcc, true); break;
        case 0x2E: set(Opmula, Vec, kAcc, true); break;
        default: break;
        }
    }
    if (u.kind == Opmula || u.kind == Opmsub) u.dest = 0xE;
    return u;
}

bool arithmetic(Kind k) { return k == Add || k == Sub || k == Mul || k == Madd || k == Msub; }
bool outer(Kind k) { return k == Opmula || k == Opmsub; }
bool normalized(Kind k) { return arithmetic(k) || outer(k); }

bool active(const Up &u, bool flags_live) {
    if (u.kind == Nop || u.kind == Clip) return false;
    return !(u.dest == 0 || (u.target == 0 && !(u.flags && flags_live)));
}

bool sets_flags(std::uint32_t hi) {
    const Up u = decode_upper(hi);
    return active(u, true) && u.flags;
}

std::vector<int> shadow_reads(std::uint32_t hi) {
    const Up u = decode_upper(hi);
    std::vector<int> regs;
    if (!active(u, true) || !normalized(u.kind)) return regs;
    if ((u.src == Vec || u.src == Bc) && u.ft) regs.push_back(static_cast<int>(u.ft));
    if (u.fs) regs.push_back(static_cast<int>(u.fs));
    if (u.kind == Madd || u.kind == Msub || u.kind == Opmsub) regs.push_back(kAcc);
    return regs;
}

struct LowerInfo {
    int writes = -1;
    bool reads_flags = false, reads_status = false;
    bool branch = false, jump = false;
    std::uint32_t target = 0;
    bool supported = true;
};

LowerInfo analyze_lower(std::uint32_t w, std::uint32_t pc) {
    LowerInfo l;
    const unsigned dest = (w >> 21) & 15u, ft = (w >> 16) & 31u;
    const std::int32_t imm11 = static_cast<std::int32_t>(w << 21) >> 21;
    if ((w >> 25) == 0x40u) {
        const unsigned funct = w & 63u;
        if (funct < 0x3C) {
            l.supported = funct == 0x30 || funct == 0x31 || funct == 0x32 || funct == 0x34 || funct == 0x35;
            return l;
        }
        const unsigned index = ((w >> 6) & 31u) * 4u + (funct & 3u);
        switch (index) {
        case 0x30: case 0x31: case 0x34: case 0x36: case 0x3D: case 0x40: case 0x41: case 0x64:
            if (ft) l.writes = static_cast<int>(ft);
            break;
        case 0x35: case 0x37: case 0x38: case 0x39: case 0x3A: case 0x3B: case 0x3C: case 0x3E: case 0x3F: case 0x42: case 0x43:
        case 0x68: case 0x69: case 0x6C: case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x78:
        case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: case 0x7E:
            break;
        default: l.supported = false; break;
        }
        (void)dest;
        return l;
    }
    const std::uint32_t target = (pc + 8u + static_cast<std::uint32_t>(imm11) * 8u) & kMask;
    switch (w >> 25) {
    case 0x00: if (ft) l.writes = static_cast<int>(ft); break;
    case 0x01: case 0x04: case 0x05: case 0x08: case 0x09: case 0x10: case 0x11: case 0x12: case 0x13: case 0x1C: break;
    case 0x14: case 0x15: case 0x16: case 0x17: l.reads_flags = l.reads_status = true; break;
    case 0x18: case 0x1A: case 0x1B: l.reads_flags = true; break;
    case 0x20: case 0x21: case 0x28: case 0x29: case 0x2C: case 0x2D: case 0x2E: case 0x2F: l.branch = true; l.target = target; break;
    case 0x24: case 0x25: l.jump = true; break;
    default: l.supported = false; break;
    }
    return l;
}

class Translator {
public:
    Translator(const std::uint8_t *code, std::uint32_t entry, const Layout &layout) : code_(code), entry_(entry & kMask), layout_(layout) {}

    bool run(std::vector<std::uint8_t> &out);

private:
    const std::uint8_t *code_;
    std::uint32_t entry_;
    Layout layout_;
    Code c_;
    std::vector<std::uint32_t> pcs_;
    std::set<std::uint32_t> in_;
    std::map<std::uint32_t, LowerInfo> lowers_;
    std::map<std::uint32_t, bool> live_in_;
    std::map<int, std::uint32_t> shadow_;
    std::vector<std::uint64_t> shadow_in_, shadow_out_;
    std::map<std::uint32_t, std::uint32_t> case_of_;
    std::uint32_t cases_ = 0, case_index_ = 0;
    std::uint32_t vi_used_ = 0, vi_written_ = 0;
    std::map<std::uint32_t, unsigned> vi_def_;
    std::map<std::uint32_t, int> delay_;
    StallPlan plan_;
    std::set<std::uint32_t> keep_bk_, set_bi_;
    int delay_mode_ = 0;
    unsigned delay_reg_ = 0;
    bool any_status_ = false, any_flags_ = false;

    std::uint32_t word(std::uint32_t at) const {
        std::uint32_t v;
        std::memcpy(&v, code_ + (at & kMask), 4);
        return v;
    }

    std::vector<std::int64_t> successors(std::uint32_t pc) {
        std::vector<std::int64_t> s{static_cast<std::int64_t>((pc + 8u) & kMask)};
        const std::uint32_t prev = (pc - 8u) & kMask;
        if (in_.count(prev)) {
            const LowerInfo &pl = lowers_[prev];
            if (pl.branch) s.push_back(pl.target);
            if (pl.jump || (word(prev + 4) & 0x40000000u)) s.push_back(-1);
        }
        return s;
    }

    bool out_live(std::uint32_t pc) {
        for (std::int64_t s : successors(pc))
            if (s < 0 || !in_.count(static_cast<std::uint32_t>(s)) || live_in_[static_cast<std::uint32_t>(s)]) return true;
        return false;
    }

    std::vector<std::uint32_t> flow(std::uint32_t pc) {
        std::vector<std::uint32_t> s;
        const std::uint32_t next = (pc + 8u) & kMask;
        if (in_.count(next)) s.push_back(next);
        const std::uint32_t prev = (pc - 8u) & kMask;
        if (in_.count(prev)) {
            const LowerInfo &pl = lowers_[prev];
            if (pl.branch && in_.count(pl.target)) s.push_back(pl.target);
        }
        return s;
    }

    unsigned vi_def(std::uint32_t pc) {
        const std::uint32_t w = word(pc);
        if ((word(pc + 4) & 0x80000000u) || !lowers_[pc].supported || branch_delay_exempt(w)) return 0;
        const unsigned dest = (w >> 21) & 15u, it = (w >> 16) & 15u, is = (w >> 11) & 15u, id = (w >> 6) & 15u;
        if ((w >> 25) == 0x40u) {
            const unsigned funct = w & 63u;
            if (funct < 0x3C) return funct == 0x32 ? it : (funct == 0x30 || funct == 0x31 || funct == 0x34 || funct == 0x35) ? id : 0;
            switch (((w >> 6) & 31u) * 4u + (funct & 3u)) {
            case 0x34: case 0x36: return is;
            case 0x35: case 0x37: case 0x3C: case 0x68: case 0x69: return it;
            case 0x3E: return dest ? it : 0;
            default: return 0;
            }
        }
        switch (w >> 25) {
        case 0x04: return dest ? it : 0;
        case 0x08: case 0x09: return it;
        default: return 0;
        }
    }

    std::uint32_t branch_reads(std::uint32_t pc) {
        if (!lowers_[pc].branch) return 0;
        const std::uint32_t w = word(pc);
        const unsigned it = (w >> 16) & 15u, is = (w >> 11) & 15u;
        switch (w >> 25) {
        case 0x2C: case 0x2D: case 0x2E: case 0x2F: return (1u << is) & ~1u;
        case 0x28: case 0x29: return ((1u << is) | (1u << it)) & ~1u;
        default: return 0;
        }
    }

    int lower_write(std::uint32_t pc) {
        const LowerInfo &li = lowers_[pc];
        return (word(pc + 4) & 0x80000000u) || !li.supported ? -1 : li.writes;
    }

    void fl() { c_.i32c(layout_.flags); }
    void store_t() {
        fl();
        c_.get(kT);
        c_.store(kOffT);
    }
    void br(std::uint32_t depth) {
        c_.byte(kOpBr);
        c_.u32(depth);
    }
    std::uint32_t fallback_depth() const { return cases_ - 1 - case_index_ + c_.depth; }
    std::uint32_t loop_depth() const { return cases_ - case_index_ + c_.depth; }
    std::uint32_t ret_depth() const { return cases_ - case_index_ + 1 + c_.depth; }

    void leave(std::uint32_t pc) {
        c_.i32c(pc & kMask);
        c_.set(kExit);
        br(fallback_depth());
    }

    void go(std::uint32_t pc) {
        const auto it = case_of_.find(pc & kMask);
        if (it == case_of_.end()) {
            leave(pc);
            return;
        }
        if (it->second > case_index_) {
            br(it->second - case_index_ - 1 + c_.depth);
            return;
        }
        c_.i32c(it->second);
        c_.set(kPcw);
        br(loop_depth());
    }

    void vfix() {
        c_.tee(kV0);
        c_.vsplatc(0x7FFFFFFFu);
        c_.simd(kVAnd);
        c_.vsplatc(0x7F7FFFFFu);
        c_.simd(kI32x4MinS);
        c_.tee(kV1);
        c_.get(kV1);
        c_.vsplatc(0x00800000u);
        c_.simd(kF32x4Ge);
        c_.simd(kVAnd);
        c_.get(kV0);
        c_.vsplatc(0x80000000u);
        c_.simd(kVAnd);
        c_.simd(kVOr);
    }

    void vkey_of(std::uint32_t local) {
        c_.get(local);
        c_.vsplatc(0x80000000u);
        c_.simd(kVAnd);
        c_.get(local);
        c_.get(local);
        c_.vsplatc(0x7F800000u);
        c_.simd(kVAnd);
        c_.vsplatc(0);
        c_.simd(kI32x4Eq);
        c_.simd(kVBitselect);
        c_.tee(kV4);
        c_.vsplatc(0x7FFFFFFFu);
        c_.simd(kVAnd);
        c_.tee(kV5);
        c_.simd(kI32x4Neg);
        c_.get(kV5);
        c_.get(kV4);
        c_.vsplatc(0);
        c_.simd(kI32x4LtS);
        c_.simd(kVBitselect);
    }

    // The order key of MAX and MINI (the same as `vkey` in vu_ops.hpp): sign and magnitude, -0 equal to +0, denormals NOT flushed
    // (Tekken 5 builds its GIF tags with `MAXi vf1, vf00, I` after a LOI of a small integer, which has to come out unchanged).
    void vorder_of(std::uint32_t local) {
        c_.get(local);
        c_.vsplatc(0x7FFFFFFFu);
        c_.simd(kVAnd);
        c_.tee(kV5);
        c_.simd(kI32x4Neg);
        c_.get(kV5);
        c_.get(local);
        c_.vsplatc(0);
        c_.simd(kI32x4LtS);
        c_.simd(kVBitselect);
    }

    void raw_vec(unsigned reg) {
        if (!reg) {
            c_.vconst(0, 0, 0, kOne);
            return;
        }
        c_.get(kR);
        c_.vload(vf_off(reg));
    }

    void raw_lane(unsigned reg, unsigned l) {
        if (!reg) {
            c_.i32c(l == 3 ? kOne : 0u);
            return;
        }
        c_.get(kR);
        c_.load(vf_off(reg) + l * 4u);
    }

    void mem_lane(unsigned reg, unsigned l) {
        c_.get(kR);
        c_.load(vf_off(reg) + l * 4u);
    }

    void shadow_vec(unsigned reg) {
        if (!reg) {
            c_.vconst(0, 0, 0, kOne);
            return;
        }
        c_.get(shadow_.at(static_cast<int>(reg)));
    }

    void vi(unsigned i) {
        if (!i) {
            c_.i32c(0);
            return;
        }
        vi_used_ |= 1u << i;
        c_.get(kVi0 + i);
    }

    void branch_vi(unsigned i) {
        if (!i || !delay_mode_ || (delay_mode_ == 1 && i != delay_reg_)) {
            vi(i);
            return;
        }
        c_.get(kBk);
        if (delay_mode_ == 1) return;
        vi(i);
        c_.get(kBi);
        c_.i32c(i);
        c_.op(kOpEq);
        c_.op(kOpSelect);
    }

    template <typename F>
    void set_vi(unsigned i, F value) {
        value();
        if (!i) {
            c_.op(kOpDrop);
            return;
        }
        vi_used_ |= 1u << i;
        vi_written_ |= 1u << i;
        c_.i32c(0xFFFFu);
        c_.op(kOpAnd);
        c_.set(kVi0 + i);
    }

    void write_back_vi() {
        for (unsigned i = 1; i < 16; ++i) {
            if (!(vi_written_ & (1u << i))) continue;
            c_.get(kR);
            c_.get(kVi0 + i);
            c_.store16(vi_off(i));
        }
    }

    template <typename F>
    void qword(F index) {
        c_.get(kMem);
        index();
        c_.i32c(1023);
        c_.op(kOpAnd);
        c_.i32c(4);
        c_.op(kOpShl);
        c_.op(kOpAdd);
    }

    void lanes_copy(unsigned dest, unsigned to) {
        if (dest == 0xF) {
            c_.get(kR);
            c_.get(kTmp);
            c_.vload(0);
            c_.vstore(vf_off(to));
            return;
        }
        for (unsigned l = 0; l < 4; ++l) {
            if (!lane(dest, static_cast<int>(l))) continue;
            c_.get(kR);
            c_.get(kTmp);
            c_.load(l * 4u);
            c_.store(vf_off(to) + l * 4u);
        }
    }

    void lanes_store(unsigned dest, unsigned from) {
        if (dest == 0xF) {
            c_.get(kTmp);
            c_.get(kR);
            c_.vload(vf_off(from));
            c_.vstore(0);
            return;
        }
        for (unsigned l = 0; l < 4; ++l) {
            if (!lane(dest, static_cast<int>(l))) continue;
            c_.get(kTmp);
            mem_lane(from, l);
            c_.store(l * 4u);
        }
    }

    void lanes_fill(unsigned dest, unsigned to, std::uint32_t value_local) {
        for (unsigned l = 0; l < 4; ++l) {
            if (!lane(dest, static_cast<int>(l))) continue;
            c_.get(kR);
            c_.get(value_local);
            c_.store(vf_off(to) + l * 4u);
        }
    }

    void lane_mask(unsigned d) {
        c_.vconst(lane(d, 0) ? ~0u : 0u, lane(d, 1) ? ~0u : 0u, lane(d, 2) ? ~0u : 0u, lane(d, 3) ? ~0u : 0u);
    }

    void flags_inline(unsigned d) {
        const auto bits = [&](unsigned shift) {
            c_.vconst(lane(d, 0) ? 8u << shift : 0u, lane(d, 1) ? 4u << shift : 0u, lane(d, 2) ? 2u << shift : 0u, lane(d, 3) ? 1u << shift : 0u);
        };
        c_.set(kV0);
        c_.get(kV0);
        c_.vsplatc(0x7F800000u);
        c_.simd(kVAnd);
        c_.vsplatc(0);
        c_.simd(kI32x4Eq);
        bits(0);
        c_.simd(kVAnd);
        c_.get(kV0);
        c_.vsplatc(0);
        c_.simd(kI32x4LtS);
        bits(4);
        c_.simd(kVAnd);
        c_.simd(kVOr);
        c_.get(kV0);
        c_.vsplatc(0x7FFFFFFFu);
        c_.simd(kVAnd);
        c_.vsplatc(0x7FFFFFFFu);
        c_.simd(kI32x4Eq);
        bits(12);
        c_.simd(kVAnd);
        c_.simd(kVOr);
        c_.tee(kV1);
        for (std::uint8_t l = 0; l < 4; ++l) {
            if (l) c_.get(kV1);
            c_.simd(kVExtract);
            c_.byte(l);
            if (l) c_.op(kOpOr);
        }
        c_.set(kMacv);
        c_.get(kR);
        c_.get(kMacv);
        c_.store(kOffMac);
        c_.get(kR);
        c_.load(kOffStatus);
        c_.i32c(0xFF0u);
        c_.op(kOpAnd);
        const std::pair<std::uint32_t, std::uint32_t> groups[3] = {{0xFu, 0}, {0xF0u, 1}, {0xF000u, 3}};
        for (const auto &[mask, shift] : groups) {
            c_.get(kMacv);
            c_.i32c(mask);
            c_.op(kOpAnd);
            c_.i32c(0);
            c_.op(kOpNe);
            if (shift) {
                c_.i32c(shift);
                c_.op(kOpShl);
            }
            c_.op(kOpOr);
        }
        c_.tee(kStat);
        c_.get(kStat);
        c_.i32c(0xFu);
        c_.op(kOpAnd);
        c_.i32c(6);
        c_.op(kOpShl);
        c_.op(kOpOr);
        c_.set(kStat);
        c_.get(kR);
        c_.get(kStat);
        c_.store(kOffStatus);
        fl();
        fl();
        c_.load(kOffMh);
        c_.tee(kSlot);
        c_.i32c(1);
        c_.op(kOpAdd);
        c_.store(kOffMh);
        c_.get(kSlot);
        c_.i32c(7);
        c_.op(kOpAnd);
        c_.i32c(2);
        c_.op(kOpShl);
        c_.tee(kSlot);
        c_.get(kT);
        c_.store(layout_.flags + kOffMacTs);
        c_.get(kSlot);
        c_.get(kMacv);
        c_.store(layout_.flags + kOffMacV);
        c_.get(kSlot);
        c_.get(kStat);
        c_.store(layout_.flags + kOffStatusV);
    }

    template <typename V, typename T>
    void ring_clip(V value, T stamp) {
        fl();
        fl();
        c_.load(kOffCh);
        c_.tee(kSlot);
        c_.i32c(1);
        c_.op(kOpAdd);
        c_.store(kOffCh);
        c_.get(kSlot);
        c_.i32c(7);
        c_.op(kOpAnd);
        c_.i32c(2);
        c_.op(kOpShl);
        c_.tee(kSlot);
        stamp();
        c_.store(layout_.flags + kOffClipTs);
        c_.get(kSlot);
        value();
        c_.store(layout_.flags + kOffClipV);
    }

    void take_q() {
        c_.get(kR);
        fl();
        c_.load(kOffQValue);
        c_.store(kOffQ);
        fl();
        c_.i32c(0);
        c_.store8(kOffQPending);
    }

    void flush_q() {
        fl();
        c_.load8(kOffQPending);
        c_.open_if();
        take_q();
        c_.end();
    }

    void sync_q() {
        fl();
        c_.load8(kOffQPending);
        c_.get(kT);
        fl();
        c_.load(kOffQReady);
        c_.op(kOpGeU);
        c_.op(kOpAnd);
        c_.open_if();
        take_q();
        c_.end();
    }

    void to_double(std::uint32_t x) {
        c_.get(x);
        c_.i32c(0x80000000u);
        c_.op(kOpAnd);
        c_.op(kOpExtendU);
        c_.i64c(32);
        c_.op(kOpI64Shl);
        c_.get(x);
        c_.i32c(0x7FFFFFFFu);
        c_.op(kOpAnd);
        c_.op(kOpExtendU);
        c_.i64c(29);
        c_.op(kOpI64Shl);
        c_.i64c(896ull << 52);
        c_.op(kOpI64Add);
        c_.i64c(0);
        c_.get(x);
        c_.i32c(0x7F800000u);
        c_.op(kOpAnd);
        c_.op(kOpSelect);
        c_.op(kOpI64Or);
        c_.op(kOpF64FromI64);
    }

    void from_double() {
        c_.op(kOpI64FromF64);
        c_.tee(kBits);
        c_.i64c(32);
        c_.op(kOpI64ShrU);
        c_.op(kOpWrap);
        c_.i32c(0x80000000u);
        c_.op(kOpAnd);
        c_.set(kSign);
        c_.get(kBits);
        c_.i64c(52);
        c_.op(kOpI64ShrU);
        c_.op(kOpWrap);
        c_.i32c(0x7FFu);
        c_.op(kOpAnd);
        c_.i32c(896);
        c_.op(kOpSub);
        c_.set(kExp);
        c_.get(kSign);
        c_.i32c(0x7FFFFFFFu);
        c_.op(kOpOr);
        c_.get(kSign);
        c_.get(kExp);
        c_.i32c(23);
        c_.op(kOpShl);
        c_.op(kOpOr);
        c_.get(kBits);
        c_.i64c(29);
        c_.op(kOpI64ShrU);
        c_.op(kOpWrap);
        c_.i32c(0x7FFFFFu);
        c_.op(kOpAnd);
        c_.op(kOpOr);
        c_.get(kExp);
        c_.i32c(255);
        c_.op(kOpGtS);
        c_.op(kOpSelect);
        c_.get(kSign);
        c_.get(kExp);
        c_.i32c(0);
        c_.op(kOpGtS);
        c_.op(kOpSelect);
    }

    template <typename F>
    void start_q(F value, std::uint32_t latency) {
        flush_q();
        fl();
        value();
        c_.store(kOffQValue);
        fl();
        c_.get(kT);
        c_.i32c(latency);
        c_.op(kOpAdd);
        c_.store(kOffQReady);
        fl();
        c_.i32c(1);
        c_.store8(kOffQPending);
    }

    void divide_status(std::uint32_t divisor) {
        c_.get(kR);
        c_.get(kR);
        c_.load(kOffStatus);
        c_.i32c(~0x30u);
        c_.op(kOpAnd);
        c_.i32c(0);
        c_.i32c(0x20u);
        c_.get(divisor);
        c_.i32c(0x7F800000u);
        c_.op(kOpAnd);
        c_.op(kOpSelect);
        c_.op(kOpOr);
        c_.store(kOffStatus);
    }

    void clip_value(const Up &u);
    void itof(const Up &u);
    void ftoi(const Up &u);
    void outer_product(const Up &u);
    void upper_pre(const Up &u, bool flags_live);
    void upper_post(const Up &u, bool flags_live);
    bool lower(std::uint32_t w, std::uint32_t pc);
    void instruction(std::uint32_t pc, std::uint32_t following);
    void prologue();
};

void Translator::clip_value(const Up &u) {
    raw_vec(u.fs);
    c_.set(kV2);
    vkey_of(kV2);
    c_.set(kV3);
    raw_lane(u.ft, 3);
    c_.i32c(0x7FFFFFFFu);
    c_.op(kOpAnd);
    c_.tee(kTmp);
    c_.i32c(0);
    c_.get(kTmp);
    c_.i32c(0x7F800000u);
    c_.op(kOpAnd);
    c_.op(kOpSelect);
    c_.simd(kVSplat);
    c_.set(kV4);
    c_.get(kV3);
    c_.get(kV4);
    c_.simd(kI32x4GtS);
    c_.vconst(1, 4, 16, 0);
    c_.simd(kVAnd);
    c_.get(kV3);
    c_.get(kV4);
    c_.simd(kI32x4Neg);
    c_.simd(kI32x4LtS);
    c_.vconst(2, 8, 32, 0);
    c_.simd(kVAnd);
    c_.simd(kVOr);
    c_.tee(kV5);
    for (std::uint8_t l = 0; l < 3; ++l) {
        if (l) c_.get(kV5);
        c_.simd(kVExtract);
        c_.byte(l);
        if (l) c_.op(kOpOr);
    }
    c_.get(kR);
    c_.load(kOffClip);
    c_.i32c(6);
    c_.op(kOpShl);
    c_.op(kOpOr);
    c_.i32c(0xFFFFFFu);
    c_.op(kOpAnd);
    c_.set(kUclip);
}

void Translator::itof(const Up &u) {
    raw_vec(u.fs);
    c_.tee(kV2);
    c_.simd(kF32x4ConvertI32x4S);
    c_.tee(kV3);
    c_.vsplatc(0x4F000000u);
    c_.simd(kF32x4Ge);
    c_.tee(kV4);
    c_.get(kV3);
    c_.get(kV4);
    c_.simd(kVAndnot);
    c_.simd(kI32x4TruncF32x4S);
    c_.set(kV5);
    c_.get(kV2);
    c_.vsplatc(0);
    c_.simd(kI32x4GeS);
    c_.get(kV5);
    c_.get(kV2);
    c_.simd(kI32x4GtS);
    c_.simd(kVAnd);
    c_.simd(kVOr);
    c_.get(kV2);
    c_.vsplatc(0);
    c_.simd(kI32x4LtS);
    c_.get(kV5);
    c_.get(kV2);
    c_.simd(kI32x4LtS);
    c_.simd(kVAnd);
    c_.simd(kVOr);
    c_.get(kV3);
    c_.simd(kI32x4Add);
    if (u.frac) {
        c_.vsplatc((127u - static_cast<std::uint32_t>(u.frac)) << 23);
        c_.simd(kF32x4Mul);
    }
    c_.set(kUv);
}

void Translator::ftoi(const Up &u) {
    raw_vec(u.fs);
    vfix();
    if (u.frac) {
        c_.vsplatc((127u + static_cast<std::uint32_t>(u.frac)) << 23);
        c_.simd(kF32x4Mul);
    }
    c_.tee(kV2);
    c_.vsplatc(0x4F000000u);
    c_.simd(kF32x4Ge);
    c_.tee(kV3);
    c_.get(kV2);
    c_.vsplatc(0xCF000000u);
    c_.simd(kF32x4Lt);
    c_.tee(kV4);
    c_.simd(kVOr);
    c_.set(kV5);
    c_.get(kV2);
    c_.get(kV5);
    c_.simd(kVAndnot);
    c_.simd(kI32x4TruncF32x4S);
    c_.get(kV5);
    c_.simd(kVAndnot);
    c_.get(kV3);
    c_.vsplatc(0x7FFFFFFFu);
    c_.simd(kVAnd);
    c_.simd(kVOr);
    c_.get(kV4);
    c_.vsplatc(0x80000000u);
    c_.simd(kVAnd);
    c_.simd(kVOr);
    c_.set(kUv);
}

void Translator::outer_product(const Up &u) {
    static const std::uint8_t kYzx[16] = {4, 5, 6, 7, 8, 9, 10, 11, 0, 1, 2, 3, 12, 13, 14, 15};
    static const std::uint8_t kZxy[16] = {8, 9, 10, 11, 0, 1, 2, 3, 4, 5, 6, 7, 12, 13, 14, 15};
    if (u.kind == Opmsub) c_.get(shadow_.at(kAcc));
    shadow_vec(u.fs);
    shadow_vec(u.fs);
    c_.shuffle(kYzx);
    shadow_vec(u.ft);
    shadow_vec(u.ft);
    c_.shuffle(kZxy);
    c_.simd(kF32x4Mul);
    vfix();
    if (u.kind == Opmsub) {
        c_.simd(kF32x4Sub);
        vfix();
    }
    c_.set(kUv);
}

void Translator::upper_pre(const Up &u, bool flags_live) {
    if (u.kind == Clip) {
        clip_value(u);
        return;
    }
    if (!active(u, flags_live)) return;
    if (outer(u.kind)) {
        outer_product(u);
        return;
    }
    if (arithmetic(u.kind)) {
        if (u.kind == Madd || u.kind == Msub) c_.get(shadow_.at(kAcc));
        shadow_vec(u.fs);
        switch (u.src) {
        case Vec: shadow_vec(u.ft); break;
        case Bc:
            if (u.ft) {
                c_.get(shadow_.at(static_cast<int>(u.ft)));
                c_.simd(kVExtract);
                c_.byte(static_cast<std::uint8_t>(u.bc));
                c_.simd(kVSplat);
            } else {
                c_.vsplatc(u.bc == 3 ? kOne : 0u);
            }
            break;
        case Q:
            c_.get(kR);
            c_.load(kOffQ);
            c_.simd(kVSplat);
            vfix();
            break;
        default:
            c_.get(kR);
            c_.load(kOffI);
            c_.simd(kVSplat);
            vfix();
            break;
        }
        switch (u.kind) {
        case Add: c_.simd(kF32x4Add); vfix(); break;
        case Sub: c_.simd(kF32x4Sub); vfix(); break;
        case Mul: c_.simd(kF32x4Mul); vfix(); break;
        case Madd:
            c_.simd(kF32x4Mul);
            vfix();
            c_.simd(kF32x4Add);
            vfix();
            break;
        default:
            c_.simd(kF32x4Mul);
            vfix();
            c_.simd(kF32x4Sub);
            vfix();
            break;
        }
        c_.set(kUv);
        return;
    }
    if (u.kind == Itof) {
        itof(u);
        return;
    }
    if (u.kind == Ftoi) {
        ftoi(u);
        return;
    }
    raw_vec(u.fs);
    if (u.kind == Abs) {
        c_.vsplatc(0x7FFFFFFFu);
        c_.simd(kVAnd);
        c_.set(kUv);
        return;
    }
    c_.set(kV2);
    switch (u.src) {
    case Vec: raw_vec(u.ft); break;
    case Bc:
        raw_lane(u.ft, u.bc);
        c_.simd(kVSplat);
        break;
    case Q:
        c_.get(kR);
        c_.load(kOffQ);
        c_.simd(kVSplat);
        break;
    default:
        c_.get(kR);
        c_.load(kOffI);
        c_.simd(kVSplat);
        break;
    }
    c_.set(kV3);
    c_.get(kV2);
    c_.get(kV3);
    vorder_of(kV2);
    vorder_of(kV3);
    c_.simd(u.kind == Max ? kI32x4GeS : kI32x4LeS);
    c_.simd(kVBitselect);
    c_.set(kUv);
}

void Translator::upper_post(const Up &u, bool flags_live) {
    if (u.kind == Clip) {
        c_.get(kR);
        c_.get(kUclip);
        c_.store(kOffClip);
        ring_clip([&] { c_.get(kUclip); }, [&] { c_.get(kT); });
        return;
    }
    if (!active(u, flags_live)) return;
    if (u.target == 0) {
        c_.get(kUv);
        flags_inline(u.dest);
        return;
    }
    const std::uint32_t dst = reg_off(u.target);
    c_.get(kR);
    c_.get(kUv);
    if (u.dest != 0xF) {
        c_.get(kR);
        c_.vload(dst);
        lane_mask(u.dest);
        c_.simd(kVBitselect);
    }
    c_.vstore(dst);
    if (u.flags && flags_live) {
        c_.get(kUv);
        flags_inline(u.dest);
    }
}

bool Translator::lower(std::uint32_t w, std::uint32_t pc) {
    const unsigned dest = (w >> 21) & 15u, it = (w >> 16) & 15u, is = (w >> 11) & 15u, id = (w >> 6) & 15u;
    const unsigned ft = (w >> 16) & 31u, fs = (w >> 11) & 31u;
    const std::int32_t imm11 = static_cast<std::int32_t>(w << 21) >> 21;
    const auto binary = [&](unsigned out, unsigned a, unsigned b, std::uint8_t op) {
        set_vi(out, [&] {
            vi(a);
            vi(b);
            c_.op(op);
        });
    };
    if ((w >> 25) == 0x40u) {
        const unsigned funct = w & 63u;
        if (funct < 0x3C) {
            switch (funct) {
            case 0x30: binary(id, is, it, kOpAdd); break;
            case 0x31: binary(id, is, it, kOpSub); break;
            case 0x32:
                set_vi(it, [&] {
                    vi(is);
                    c_.i32c(static_cast<std::uint32_t>(static_cast<std::int32_t>(((w >> 6) & 31u) << 27) >> 27));
                    c_.op(kOpAdd);
                });
                break;
            case 0x34: binary(id, is, it, kOpAnd); break;
            case 0x35: binary(id, is, it, kOpOr); break;
            default: return false;
            }
            return true;
        }
        const unsigned index = ((w >> 6) & 31u) * 4u + (funct & 3u);
        const unsigned fsf = (w >> 21) & 3u, ftf = (w >> 23) & 3u;
        switch (index) {
        case 0x30:
            if (ft)
                for (unsigned l = 0; l < 4; ++l) {
                    if (!lane(dest, static_cast<int>(l))) continue;
                    c_.get(kR);
                    mem_lane(fs, l);
                    c_.store(vf_off(ft) + l * 4u);
                }
            break;
        case 0x31:
            if (ft) {
                c_.get(kR);
                c_.vload(vf_off(fs));
                c_.set(kV2);
                for (unsigned l = 0; l < 4; ++l) {
                    if (!lane(dest, static_cast<int>(l))) continue;
                    c_.get(kR);
                    c_.get(kV2);
                    c_.simd(kVExtract);
                    c_.byte(static_cast<std::uint8_t>((l + 1) & 3u));
                    c_.store(vf_off(ft) + l * 4u);
                }
            }
            break;
        case 0x34:
            qword([&] { vi(is); });
            c_.set(kTmp);
            if (ft) lanes_copy(dest, ft);
            set_vi(is, [&] {
                vi(is);
                c_.i32c(1);
                c_.op(kOpAdd);
            });
            break;
        case 0x35:
            qword([&] { vi(it); });
            c_.set(kTmp);
            lanes_store(dest, fs);
            set_vi(it, [&] {
                vi(it);
                c_.i32c(1);
                c_.op(kOpAdd);
            });
            break;
        case 0x36:
            set_vi(is, [&] {
                vi(is);
                c_.i32c(1);
                c_.op(kOpSub);
            });
            qword([&] { vi(is); });
            c_.set(kTmp);
            if (ft) lanes_copy(dest, ft);
            break;
        case 0x37:
            set_vi(it, [&] {
                vi(it);
                c_.i32c(1);
                c_.op(kOpSub);
            });
            qword([&] { vi(it); });
            c_.set(kTmp);
            lanes_store(dest, fs);
            break;
        case 0x38:
            mem_lane(fs, fsf);
            c_.set(kTmp);
            mem_lane(ft, ftf);
            c_.set(kTmp2);
            start_q(
                [&] {
                    to_double(kTmp);
                    to_double(kTmp2);
                    c_.op(kOpF64Div);
                    from_double();
                    c_.get(kTmp);
                    c_.get(kTmp2);
                    c_.op(kOpXor);
                    c_.i32c(0x80000000u);
                    c_.op(kOpAnd);
                    c_.i32c(0x7FFFFFFFu);
                    c_.op(kOpOr);
                    c_.get(kTmp2);
                    c_.i32c(0x7F800000u);
                    c_.op(kOpAnd);
                    c_.op(kOpSelect);
                },
                7);
            divide_status(kTmp2);
            break;
        case 0x39:
            mem_lane(ft, ftf);
            c_.i32c(0x7FFFFFFFu);
            c_.op(kOpAnd);
            c_.set(kTmp2);
            start_q(
                [&] {
                    to_double(kTmp2);
                    c_.op(kOpF64Sqrt);
                    from_double();
                },
                7);
            break;
        case 0x3A:
            mem_lane(fs, fsf);
            c_.set(kTmp);
            mem_lane(ft, ftf);
            c_.i32c(0x7FFFFFFFu);
            c_.op(kOpAnd);
            c_.set(kTmp2);
            start_q(
                [&] {
                    to_double(kTmp);
                    to_double(kTmp2);
                    c_.op(kOpF64Sqrt);
                    c_.op(kOpF64Div);
                    from_double();
                    c_.get(kTmp);
                    c_.i32c(0x80000000u);
                    c_.op(kOpAnd);
                    c_.i32c(0x7FFFFFFFu);
                    c_.op(kOpOr);
                    c_.get(kTmp2);
                    c_.i32c(0x7F800000u);
                    c_.op(kOpAnd);
                    c_.op(kOpSelect);
                },
                13);
            divide_status(kTmp2);
            break;
        case 0x3B: flush_q(); break;
        case 0x3C: set_vi(it, [&] { mem_lane(fs, fsf); }); break;
        case 0x3D:
            if (ft) {
                vi(is);
                c_.op(kOpExt16);
                c_.set(kTmp);
                lanes_fill(dest, ft, kTmp);
            }
            break;
        case 0x3E:
            for (unsigned l = 0; l < 4; ++l)
                if (lane(dest, static_cast<int>(l))) {
                    set_vi(it, [&] {
                        qword([&] { vi(is); });
                        c_.load16(l * 4u);
                    });
                    break;
                }
            break;
        case 0x3F:
            qword([&] { vi(is); });
            c_.set(kTmp);
            vi(it);
            c_.set(kTmp2);
            for (unsigned l = 0; l < 4; ++l) {
                if (!lane(dest, static_cast<int>(l))) continue;
                c_.get(kTmp);
                c_.get(kTmp2);
                c_.store(l * 4u);
            }
            break;
        case 0x40:
            c_.get(kR);
            c_.get(kR);
            c_.load(kOffR);
            c_.tee(kTmp);
            c_.i32c(4);
            c_.op(kOpShrU);
            c_.get(kTmp);
            c_.i32c(22);
            c_.op(kOpShrU);
            c_.op(kOpXor);
            c_.i32c(1);
            c_.op(kOpAnd);
            c_.get(kTmp);
            c_.i32c(1);
            c_.op(kOpShl);
            c_.op(kOpOr);
            c_.i32c(0x7FFFFFu);
            c_.op(kOpAnd);
            c_.store(kOffR);
            if (ft) {
                c_.get(kR);
                c_.load(kOffR);
                c_.i32c(kOne);
                c_.op(kOpOr);
                c_.set(kTmp);
                lanes_fill(dest, ft, kTmp);
            }
            break;
        case 0x41:
            if (ft) {
                c_.get(kR);
                c_.load(kOffR);
                c_.i32c(0x7FFFFFu);
                c_.op(kOpAnd);
                c_.i32c(kOne);
                c_.op(kOpOr);
                c_.set(kTmp);
                lanes_fill(dest, ft, kTmp);
            }
            break;
        case 0x42:
            c_.get(kR);
            mem_lane(fs, fsf);
            c_.i32c(0x7FFFFFu);
            c_.op(kOpAnd);
            c_.store(kOffR);
            break;
        case 0x43:
            c_.get(kR);
            c_.get(kR);
            c_.load(kOffR);
            mem_lane(fs, fsf);
            c_.op(kOpXor);
            c_.i32c(0x7FFFFFu);
            c_.op(kOpAnd);
            c_.store(kOffR);
            break;
        case 0x64:
            if (ft) {
                c_.get(kR);
                c_.load(kOffP);
                c_.set(kTmp);
                lanes_fill(dest, ft, kTmp);
            }
            break;
        case 0x68: set_vi(it, [&] { c_.call(kTop); }); break;
        case 0x69: set_vi(it, [&] { c_.call(kItop); }); break;
        case 0x6C:
            vi(is);
            c_.call(kKick);
            break;
        case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x78: case 0x79: case 0x7A: case 0x7C: case 0x7D: case 0x7E:
            c_.get(kR);
            c_.i32c(index);
            c_.i32c(fs);
            c_.i32c(fsf);
            c_.call(kEfu);
            break;
        case 0x7B: break;
        default: return false;
        }
        return true;
    }
    const std::uint32_t imm15 = ((w >> 10) & 0x7800u) | (w & 0x7FFu);
    const std::uint32_t imm12 = ((w >> 10) & 0x800u) | (w & 0x7FFu);
    const std::uint32_t imm24 = w & 0xFFFFFFu;
    const auto status = [&] {
        store_t();
        fl();
        c_.call(kStatus);
    };
    const auto mac = [&] {
        store_t();
        fl();
        c_.call(kMac);
    };
    const auto clip = [&] {
        store_t();
        fl();
        c_.call(kClip);
    };
    const auto offset = [&](unsigned reg) {
        vi(reg);
        c_.i32c(static_cast<std::uint32_t>(imm11));
        c_.op(kOpAdd);
    };
    switch (w >> 25) {
    case 0x00:
        qword([&] { offset(is); });
        c_.set(kTmp);
        if (ft) lanes_copy(dest, ft);
        break;
    case 0x01:
        qword([&] { offset(it); });
        c_.set(kTmp);
        lanes_store(dest, fs);
        break;
    case 0x04:
        for (unsigned l = 0; l < 4; ++l)
            if (lane(dest, static_cast<int>(l))) {
                set_vi(it, [&] {
                    qword([&] { offset(is); });
                    c_.load16(l * 4u);
                });
                break;
            }
        break;
    case 0x05:
        qword([&] { offset(is); });
        c_.set(kTmp);
        vi(it);
        c_.set(kTmp2);
        for (unsigned l = 0; l < 4; ++l) {
            if (!lane(dest, static_cast<int>(l))) continue;
            c_.get(kTmp);
            c_.get(kTmp2);
            c_.store(l * 4u);
        }
        break;
    case 0x08:
        set_vi(it, [&] {
            vi(is);
            c_.i32c(imm15);
            c_.op(kOpAdd);
        });
        break;
    case 0x09:
        set_vi(it, [&] {
            vi(is);
            c_.i32c(imm15);
            c_.op(kOpSub);
        });
        break;
    case 0x10:
        set_vi(1, [&] {
            clip();
            c_.i32c(0xFFFFFFu);
            c_.op(kOpAnd);
            c_.i32c(imm24);
            c_.op(kOpEq);
        });
        break;
    case 0x11:
        c_.get(kR);
        c_.i32c(imm24);
        c_.store(kOffClip);
        ring_clip([&] { c_.i32c(imm24); },
                  [&] {
                      c_.get(kT);
                      c_.i32c(4);
                      c_.op(kOpSub);
                  });
        break;
    case 0x12:
        set_vi(1, [&] {
            clip();
            c_.i32c(imm24);
            c_.op(kOpAnd);
            c_.i32c(0);
            c_.op(kOpNe);
        });
        break;
    case 0x13:
        set_vi(1, [&] {
            clip();
            c_.i32c(imm24);
            c_.op(kOpOr);
            c_.i32c(0xFFFFFFu);
            c_.op(kOpAnd);
            c_.i32c(0xFFFFFFu);
            c_.op(kOpEq);
        });
        break;
    case 0x14:
        set_vi(it, [&] {
            status();
            c_.i32c(0xFFFu);
            c_.op(kOpAnd);
            c_.i32c(imm12);
            c_.op(kOpEq);
        });
        break;
    case 0x15:
        c_.get(kR);
        status();
        c_.i32c(0x3Fu);
        c_.op(kOpAnd);
        c_.i32c(imm12 & 0xFC0u);
        c_.op(kOpOr);
        c_.store(kOffStatus);
        fl();
        c_.get(kR);
        c_.call(kForceMac);
        break;
    case 0x16:
        set_vi(it, [&] {
            status();
            c_.i32c(imm12);
            c_.op(kOpAnd);
        });
        break;
    case 0x17:
        set_vi(it, [&] {
            status();
            c_.i32c(imm12);
            c_.op(kOpOr);
            c_.i32c(0xFFFu);
            c_.op(kOpAnd);
            c_.i32c(0xFFFu);
            c_.op(kOpEq);
        });
        break;
    case 0x18:
        set_vi(it, [&] {
            mac();
            c_.i32c(0xFFFFu);
            c_.op(kOpAnd);
            vi(is);
            c_.op(kOpEq);
        });
        break;
    case 0x1A:
        set_vi(it, [&] {
            mac();
            vi(is);
            c_.op(kOpAnd);
        });
        break;
    case 0x1B:
        set_vi(it, [&] {
            mac();
            vi(is);
            c_.op(kOpOr);
            c_.i32c(0xFFFFu);
            c_.op(kOpAnd);
            c_.i32c(0xFFFFu);
            c_.op(kOpEq);
        });
        break;
    case 0x1C:
        set_vi(it, [&] {
            clip();
            c_.i32c(0xFFFu);
            c_.op(kOpAnd);
        });
        break;
    case 0x20:
        c_.i32c(1);
        c_.set(kPend);
        break;
    case 0x21:
        set_vi(it, [&] { c_.i32c((pc + 16u) / 8u); });
        c_.i32c(1);
        c_.set(kPend);
        break;
    case 0x24:
    case 0x25:
        vi(is);
        c_.i32c(8);
        c_.op(kOpMul);
        c_.i32c(kMask);
        c_.op(kOpAnd);
        c_.set(kJpc);
        if ((w >> 25) == 0x25) set_vi(it, [&] { c_.i32c((pc + 16u) / 8u); });
        c_.i32c(1);
        c_.set(kPendj);
        break;
    case 0x28:
    case 0x29:
        branch_vi(is);
        branch_vi(it);
        c_.op((w >> 25) == 0x28 ? kOpEq : kOpNe);
        c_.set(kPend);
        break;
    case 0x2C: case 0x2D: case 0x2E: case 0x2F: {
        static const std::uint8_t kCompare[4] = {kOpLtS, kOpGtS, kOpLeS, kOpGeS};
        branch_vi(is);
        c_.op(kOpExt16);
        c_.i32c(0);
        c_.op(kCompare[(w >> 25) - 0x2C]);
        c_.set(kPend);
        break;
    }
    default: return false;
    }
    return true;
}

void Translator::instruction(std::uint32_t pc, std::uint32_t following) {
    const std::uint32_t lo = word(pc), hi = word(pc + 4);
    const Up u = decode_upper(hi);
    const bool flags_live = any_status_ ? true : (any_flags_ && out_live(pc));
    const LowerInfo &li = lowers_[pc];

    c_.get(kT);
    c_.i32c(1 + plan_.stall[pc >> 3]);
    c_.op(kOpAdd);
    c_.set(kT);
    if (reads_q(hi)) {
        const QRead kind = q_read_kind(code_, kMask, pc);
        if (kind == kQNew) flush_q();
        else if (kind == kQDynamic) sync_q();
    }
    upper_pre(u, flags_live);
    const unsigned def = vi_def_[pc];
    if (keep_bk_.count(pc)) {
        vi(def);
        c_.set(kBkNext);
    }
    const auto delay = delay_.find(pc);
    delay_mode_ = delay == delay_.end() ? 0 : delay->second;
    delay_reg_ = delay_mode_ == 1 ? vi_def_[(pc - 8u) & kMask] : 0;
    if (hi & 0x80000000u) {
        c_.get(kR);
        c_.i32c(lo);
        c_.store(kOffI);
    } else if (!li.supported || !lower(lo, pc)) {
        leave(pc);
    }
    delay_mode_ = 0;
    if (keep_bk_.count(pc)) {
        c_.get(kBkNext);
        c_.set(kBk);
    }
    if (set_bi_.count(pc)) {
        c_.i32c(def);
        c_.set(kBi);
    }
    upper_post(u, flags_live);
    const int written = lower_write(pc);
    const std::uint64_t live = shadow_out_[pc >> 3];
    if (written >= 0 && shadow_.count(written) && ((live >> written) & 1u)) {
        c_.get(kR);
        c_.vload(reg_off(written));
        vfix();
        c_.set(shadow_.at(written));
    }
    const bool target_active = active(u, flags_live) && u.target > 0;
    if (target_active && u.target != written && shadow_.count(u.target) && ((live >> u.target) & 1u)) {
        if (!normalized(u.kind)) {
            c_.get(kR);
            c_.vload(reg_off(u.target));
            vfix();
        } else if (u.dest == 0xF) {
            c_.get(kUv);
        } else {
            c_.get(kUv);
            c_.get(shadow_.at(u.target));
            lane_mask(u.dest);
            c_.simd(kVBitselect);
        }
        c_.set(shadow_.at(u.target));
    }
    if (hi & 0x40000000u) {
        c_.i32c(1);
        c_.set(kEnding);
    }

    const std::uint32_t prev = (pc - 8u) & kMask;
    if (in_.count(prev)) {
        const LowerInfo &pl = lowers_[prev];
        if (word(prev + 4) & 0x40000000u) {
            c_.get(kEnding);
            c_.open_if();
            if (pl.branch || pl.jump) {
                if (pl.branch) c_.i32c(pl.target & kMask);
                else c_.get(kJpc);
                c_.i32c((pc + 8u) & kMask);
                c_.get(pl.branch ? kPend : kPendj);
                c_.op(kOpSelect);
            } else {
                c_.i32c((pc + 8u) & kMask);
            }
            c_.set(kExit);
            br(ret_depth());
            c_.end();
        }
        if (pl.branch) {
            c_.get(kPend);
            c_.open_if();
            c_.i32c(0);
            c_.set(kPend);
            go(pl.target);
            c_.end();
        }
        if (pl.jump) {
            c_.get(kPendj);
            c_.open_if();
            c_.i32c(0);
            c_.set(kPendj);
            c_.get(kJpc);
            c_.set(kExit);
            br(fallback_depth());
            c_.end();
        }
    }
    const std::uint32_t next = (pc + 8u) & kMask;
    if (!in_.count(next)) leave(next);
    else if (next != following) go(next);
}

void Translator::prologue() {
    for (const std::uint32_t at : {kOffMacTs, kOffMacTs + 16u, kOffClipTs, kOffClipTs + 16u}) {
        fl();
        c_.vconst(0, 0, 0, 0);
        c_.vstore(at);
    }
    const std::pair<std::uint32_t, std::uint32_t> rings[3] = {{kOffMac, kOffMacV}, {kOffStatus, kOffStatusV}, {kOffClip, kOffClipV}};
    for (const auto &[from, to] : rings) {
        c_.get(kR);
        c_.load(from);
        c_.simd(kVSplat);
        c_.set(kV0);
        fl();
        c_.get(kV0);
        c_.vstore(to);
        fl();
        c_.get(kV0);
        c_.vstore(to + 16u);
    }
    fl();
    c_.i32c(0);
    c_.store(kOffMh);
    fl();
    c_.i32c(0);
    c_.store(kOffCh);
    fl();
    c_.i32c(0);
    c_.store8(kOffQPending);
    c_.i32c(16);
    c_.set(kT);
    for (unsigned i = 1; i < 16; ++i) {
        if (!(vi_used_ & (1u << i))) continue;
        c_.get(kR);
        c_.load16(vi_off(i));
        c_.set(kVi0 + i);
    }
    const std::uint64_t live = shadow_in_[entry_ >> 3];
    for (const auto &[reg, local] : shadow_) {
        if (!((live >> reg) & 1u)) continue;
        c_.get(kR);
        c_.vload(reg_off(reg));
        vfix();
        c_.set(local);
    }
    c_.i32c(case_of_.at(entry_));
    c_.set(kPcw);
}

bool Translator::run(std::vector<std::uint8_t> &out) {
    pcs_ = reachable(code_, kMask, entry_);
    if (pcs_.empty()) return false;
    // Stalls that depend on the path taken are left to the interpreter, which counts them as they happen.
    plan_ = stall_plan(code_, kMask, pcs_, entry_);
    if (!plan_.ok || plan_.any_tracked) return false;
    in_.insert(pcs_.begin(), pcs_.end());
    for (std::uint32_t pc : pcs_) {
        LowerInfo l;
        if (!(word(pc + 4) & 0x80000000u)) l = analyze_lower(word(pc), pc);
        any_status_ = any_status_ || l.reads_status;
        any_flags_ = any_flags_ || l.reads_flags;
        lowers_[pc] = l;
    }
    for (std::uint32_t pc : pcs_) live_in_[pc] = false;
    for (bool changed = true; changed;) {
        changed = false;
        for (auto it = pcs_.rbegin(); it != pcs_.rend(); ++it) {
            const std::uint32_t pc = *it;
            const bool out = out_live(pc);
            bool reader_near = false;
            for (std::uint32_t k = 1; k <= 3; ++k) {
                const std::uint32_t q = (pc + 8u * k) & kMask;
                if (in_.count(q) && (lowers_[q].reads_flags || lowers_[q].branch || lowers_[q].jump)) reader_near = true;
            }
            const bool kills = sets_flags(word(pc + 4)) && !reader_near;
            const bool v = lowers_[pc].reads_flags || (!kills && out);
            if (v != live_in_[pc]) {
                live_in_[pc] = v;
                changed = true;
            }
        }
    }

    std::set<std::uint32_t> leaders{entry_, pcs_.front()};
    for (std::uint32_t pc : pcs_) {
        if (!in_.count((pc - 8u) & kMask)) leaders.insert(pc);
        const LowerInfo &l = lowers_[pc];
        if (l.branch && in_.count(l.target)) leaders.insert(l.target);
    }
    for (std::uint32_t pc : leaders) case_of_[pc] = cases_++;
    std::map<std::uint32_t, std::vector<std::uint32_t>> preds;
    for (std::uint32_t pc : pcs_) {
        vi_def_[pc] = vi_def(pc);
        for (std::uint32_t s : flow(pc)) preds[s].push_back(pc);
    }
    for (std::uint32_t pc : pcs_) {
        const std::uint32_t reads = branch_reads(pc);
        bool hit = false;
        for (std::uint32_t p : preds[pc]) hit = hit || (vi_def_[p] && ((reads >> vi_def_[p]) & 1u));
        if (!hit) continue;
        const bool dynamic = case_of_.count(pc) != 0;
        delay_[pc] = dynamic ? 2 : 1;
        for (std::uint32_t p : preds[pc]) {
            if (vi_def_[p]) keep_bk_.insert(p);
            if (dynamic) set_bi_.insert(p);
        }
    }

    std::set<int> shadowed;
    for (std::uint32_t pc : pcs_)
        for (int reg : shadow_reads(word(pc + 4))) shadowed.insert(reg);
    std::uint32_t next_local = kShadow0;
    for (int reg : shadowed) shadow_[reg] = next_local++;
    std::vector<std::uint64_t> uses(2048, 0), defs(2048, 0);
    for (std::uint32_t pc : pcs_) {
        const std::uint32_t hi = word(pc + 4), at = pc >> 3;
        for (int reg : shadow_reads(hi)) uses[at] |= 1ull << reg;
        const int written = lower_write(pc);
        if (written >= 0 && shadow_.count(written)) defs[at] |= 1ull << written;
        const Up u = decode_upper(hi);
        if (active(u, true) && u.target > 0 && u.target != written && shadow_.count(u.target)) {
            defs[at] |= 1ull << u.target;
            if (normalized(u.kind) && u.dest != 0xF) uses[at] |= 1ull << u.target;
        }
    }
    shadow_in_.assign(2048, 0);
    shadow_out_.assign(2048, 0);
    for (bool changed = true; changed;) {
        changed = false;
        for (auto it = pcs_.rbegin(); it != pcs_.rend(); ++it) {
            const std::uint32_t at = *it >> 3;
            std::uint64_t o = 0;
            for (std::uint32_t s : flow(*it)) o |= shadow_in_[s >> 3];
            shadow_out_[at] = o;
            const std::uint64_t v = uses[at] | (o & ~defs[at]);
            if (v != shadow_in_[at]) {
                shadow_in_[at] = v;
                changed = true;
            }
        }
    }

    c_.byte(0x02);
    c_.byte(0x40);
    c_.byte(0x03);
    c_.byte(0x40);
    c_.byte(0x02);
    c_.byte(0x40);
    for (std::uint32_t k = 0; k < cases_; ++k) {
        c_.byte(0x02);
        c_.byte(0x40);
    }
    c_.get(kPcw);
    c_.byte(0x0E);
    c_.u32(cases_);
    for (std::uint32_t k = 0; k < cases_; ++k) c_.u32(k);
    c_.u32(cases_);
    for (std::size_t i = 0; i < pcs_.size(); ++i) {
        const std::uint32_t pc = pcs_[i];
        const auto it = case_of_.find(pc);
        if (it != case_of_.end()) {
            c_.byte(0x0B);
            case_index_ = it->second;
        }
        instruction(pc, i + 1 < pcs_.size() ? pcs_[i + 1] : ~0u);
    }
    c_.byte(0x0B);
    write_back_vi();
    c_.get(kR);
    c_.get(kExit);
    c_.tail(kInterpret);
    c_.byte(0x0B);
    c_.byte(0x0B);
    write_back_vi();
    flush_q();
    c_.get(kR);
    c_.get(kExit);
    c_.store(kOffTpc);
    c_.byte(0x0B);

    std::vector<std::uint8_t> body_code = std::move(c_.b);
    c_.b.clear();
    prologue();
    c_.b.insert(c_.b.end(), body_code.begin(), body_code.end());

    Out m;
    m.b = {0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00};
    std::vector<std::pair<std::uint8_t, bool>> types;
    std::vector<std::uint32_t> import_type(kImports);
    for (std::uint32_t i = 0; i < kImports; ++i) {
        const std::pair<std::uint8_t, bool> t{kImportTypes[i].params, kImportTypes[i].result};
        auto found = std::find(types.begin(), types.end(), t);
        if (found == types.end()) {
            types.push_back(t);
            found = types.end() - 1;
        }
        import_type[i] = static_cast<std::uint32_t>(found - types.begin());
    }
    const std::uint32_t run_type = static_cast<std::uint32_t>(types.size());
    Out ts;
    ts.u32(run_type + 1);
    for (const auto &[params, result] : types) {
        ts.byte(0x60);
        ts.u32(params);
        for (std::uint8_t p = 0; p < params; ++p) ts.byte(0x7F);
        ts.u32(result ? 1 : 0);
        if (result) ts.byte(0x7F);
    }
    ts.byte(0x60);
    ts.u32(2);
    ts.byte(0x7F);
    ts.byte(0x7F);
    ts.u32(0);
    m.section(1, ts);

    Out is;
    is.u32(kImports + 1);
    is.name("env");
    is.name("memory");
    is.byte(0x02);
    is.byte(0x03);
    is.u32(1);
    is.u32(65536);
    for (std::uint32_t i = 0; i < kImports; ++i) {
        is.name("env");
        is.name(kImportNames[i]);
        is.byte(0x00);
        is.u32(import_type[i]);
    }
    m.section(2, is);

    Out fs;
    fs.u32(1);
    fs.u32(run_type);
    m.section(3, fs);

    Out es;
    es.u32(1);
    es.name("run");
    es.byte(0x00);
    es.u32(kImports);
    m.section(7, es);

    Out body;
    body.u32(3);
    body.u32(kI32Count);
    body.byte(0x7F);
    body.u32(1);
    body.byte(0x7E);
    body.u32(kVecTemps + static_cast<std::uint32_t>(shadow_.size()));
    body.byte(0x7B);
    body.b.insert(body.b.end(), c_.b.begin(), c_.b.end());
    Out cs;
    cs.u32(1);
    cs.u32(static_cast<std::uint32_t>(body.b.size()));
    cs.b.insert(cs.b.end(), body.b.begin(), body.b.end());
    m.section(10, cs);

    char label[16];
    std::snprintf(label, sizeof label, "vu1_%04X", entry_);
    Out names, functions;
    functions.u32(1);
    functions.u32(kImports);
    functions.name(label);
    names.name("name");
    names.section(1, functions);
    m.section(0, names);

    out = std::move(m.b);
    return true;
}

}

bool translate(const std::uint8_t *code, std::uint32_t entry, const Layout &layout, std::vector<std::uint8_t> &out) {
    Translator t(code, entry, layout);
    return t.run(out);
}

}
