#include "vurecomp_gen.hpp"

#include "ps2/os.hpp"
#include "ps2/vu_ops.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr std::uint32_t kMask = 16383u;
constexpr int kAcc = 32;

struct Program {
    std::uint64_t key;
    std::uint32_t entry;
    std::vector<std::uint8_t> code;
    int unit = 1;
};

std::uint32_t word(const std::vector<std::uint8_t> &code, std::uint32_t at) {
    std::uint32_t v;
    std::memcpy(&v, code.data() + (at & (code.size() - 1u)), 4);
    return v;
}

std::string label(std::uint32_t pc) { return std::format("L_{:04X}", pc); }

const char *const kLane = "xyzw";

bool lane(unsigned dest, int i) { return dest & (8u >> i); }

struct Upper {
    std::string pre, post;
    bool sets_flags = false;
    int target = -1;
    unsigned mask = 0;
    bool normalized = false;
    std::vector<int> shadow_reads;
    // The lazy clean-up (generate()): `arith` is an add, subtract or multiply done as float instructions, whose result may hold a
    // denormal that stands for a zero; `raw_reads` are the registers and lanes whose bits this instruction reads as they are.
    bool arith = false;
    std::vector<std::pair<int, unsigned>> raw_reads;
};

std::string vf(unsigned i, int lane_index) {
    if (!i) return lane_index == 3 ? "0x3F800000u" : "0u";
    return std::format("r.vf[{}].w[{}]", i, lane_index);
}

std::string vreg(unsigned i) { return i ? std::format("vload(r.vf[{}])", i) : "v4u{0u, 0u, 0u, 0x3F800000u}"; }

std::string shadow(int reg) { return reg == kAcc ? "nacc" : std::format("n{}", reg); }
std::string raw(int reg) { return reg == kAcc ? "r.acc" : std::format("r.vf[{}]", reg); }

std::string lane_mask(unsigned d) {
    std::string mask = "v4u{";
    for (int i = 0; i < 4; ++i) mask += std::string(i ? ", " : "") + (lane(d, i) ? "0xFFFFFFFFu" : "0u");
    return mask + "}";
}

Upper upper(std::uint32_t hi, bool flags_live, bool exact = false, bool late_flags = false) {
    Upper u;
    // late_flags: the program reads the MAC flags but never the status (see FlagPipeV in vu_ops.hpp): a live flag site keeps its
    // result in the flag pipe, and the MAC and status are worked out where a reader or a way out of the program needs them.
    const auto set_flags = [&](const std::string &value, unsigned d) {
        return late_flags ? std::format("fl.push<{}u>({}); ", d, value) : std::format("flags(r, {}, {}u); fl.set_mac(r.mac, r.status); ", value, d);
    };
    const unsigned dest = (hi >> 21) & 15u, ft = (hi >> 16) & 31u, fs = (hi >> 11) & 31u, fd = (hi >> 6) & 31u, bc = hi & 3u;
    const unsigned funct = hi & 63u;
    enum Kind { Nop, Add, Sub, Madd, Msub, Max, Min, Mul, Abs, Ftoi, Itof, Clip, Opmula, Opmsub } kind = Nop;
    enum Src { Vec, Bc, Q, I } src = Vec;
    int target = -1;
    bool flags = false;
    int frac = 0;
    static const int kFrac[4] = {0, 4, 12, 15};
    const auto set = [&](Kind k, Src s, int t, bool f) {
        kind = k;
        src = s;
        target = t;
        flags = f;
    };
    if (funct < 0x3C) {
        switch (funct) {
        case 0x00: case 0x01: case 0x02: case 0x03: set(Add, Bc, fd, true); break;
        case 0x04: case 0x05: case 0x06: case 0x07: set(Sub, Bc, fd, true); break;
        case 0x08: case 0x09: case 0x0A: case 0x0B: set(Madd, Bc, fd, true); break;
        case 0x0C: case 0x0D: case 0x0E: case 0x0F: set(Msub, Bc, fd, true); break;
        case 0x10: case 0x11: case 0x12: case 0x13: set(Max, Bc, fd, false); break;
        case 0x14: case 0x15: case 0x16: case 0x17: set(Min, Bc, fd, false); break;
        case 0x18: case 0x19: case 0x1A: case 0x1B: set(Mul, Bc, fd, true); break;
        case 0x1C: set(Mul, Q, fd, true); break;
        case 0x1D: set(Max, I, fd, false); break;
        case 0x1E: set(Mul, I, fd, true); break;
        case 0x1F: set(Min, I, fd, false); break;
        case 0x20: set(Add, Q, fd, true); break;
        case 0x21: set(Madd, Q, fd, true); break;
        case 0x22: set(Add, I, fd, true); break;
        case 0x23: set(Madd, I, fd, true); break;
        case 0x24: set(Sub, Q, fd, true); break;
        case 0x25: set(Msub, Q, fd, true); break;
        case 0x26: set(Sub, I, fd, true); break;
        case 0x27: set(Msub, I, fd, true); break;
        case 0x28: set(Add, Vec, fd, true); break;
        case 0x29: set(Madd, Vec, fd, true); break;
        case 0x2A: set(Mul, Vec, fd, true); break;
        case 0x2B: set(Max, Vec, fd, false); break;
        case 0x2C: set(Sub, Vec, fd, true); break;
        case 0x2D: set(Msub, Vec, fd, true); break;
        case 0x2E: set(Opmsub, Vec, fd, true); break;
        case 0x2F: set(Min, Vec, fd, false); break;
        default: break;
        }
    } else {
        const unsigned index = ((hi >> 6) & 31u) * 4u + (funct & 3u);
        switch (index) {
        case 0x00: case 0x01: case 0x02: case 0x03: set(Add, Bc, kAcc, true); break;
        case 0x04: case 0x05: case 0x06: case 0x07: set(Sub, Bc, kAcc, true); break;
        case 0x08: case 0x09: case 0x0A: case 0x0B: set(Madd, Bc, kAcc, true); break;
        case 0x0C: case 0x0D: case 0x0E: case 0x0F: set(Msub, Bc, kAcc, true); break;
        case 0x10: case 0x11: case 0x12: case 0x13: set(Itof, Vec, static_cast<int>(ft), false); frac = kFrac[index & 3u]; break;
        case 0x14: case 0x15: case 0x16: case 0x17: set(Ftoi, Vec, static_cast<int>(ft), false); frac = kFrac[index & 3u]; break;
        case 0x18: case 0x19: case 0x1A: case 0x1B: set(Mul, Bc, kAcc, true); break;
        case 0x1C: set(Mul, Q, kAcc, true); break;
        case 0x1D: set(Abs, Vec, static_cast<int>(ft), false); break;
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
    if (kind == Nop) return u;
    if (kind == Clip) {
        u.pre = std::format("const std::uint32_t uclip = clip_value(r, r.vf[{}], r.vf[{}].w[3]); ", fs, ft);
        u.post = "r.clip = uclip; fl.set_clip(uclip, fl.t); ";
        return u;
    }
    unsigned d = dest;
    if (kind == Opmula || kind == Opmsub) d = 0xE;
    if ((d == 0 && !(exact && flags)) || (target == 0 && !(flags && flags_live))) return u;
    const auto b = [&](int i) -> std::string {
        switch (src) {
        case Vec: return vf(ft, i);
        case Bc: return vf(ft, static_cast<int>(bc));
        case Q: return "r.q";
        default: return "r.i";
        }
    };
    const std::string dst = target == kAcc ? "r.acc" : std::format("r.vf[{}]", target);
    const bool vector = !exact && (kind == Add || kind == Sub || kind == Mul || kind == Madd || kind == Msub || kind == Max || kind == Min || kind == Abs || kind == Ftoi || kind == Itof);
    const bool arithmetic = kind == Add || kind == Sub || kind == Mul || kind == Madd || kind == Msub;
    if (target != 0) {
        u.target = target;
        u.mask = d;
        u.normalized = vector && arithmetic;
    }
    if (arithmetic && !exact) {
        u.arith = true;
        const auto operand = [&](unsigned reg) -> std::string {
            if (!reg) return vreg(0);
            u.shadow_reads.push_back(static_cast<int>(reg));
            return shadow(static_cast<int>(reg));
        };
        std::string nb;
        switch (src) {
        case Vec: nb = operand(ft); break;
        case Bc:
            if (ft) u.shadow_reads.push_back(static_cast<int>(ft));
            nb = ft ? std::format("vsplat(n{}[{}])", ft, bc) : std::format("vsplat({})", vf(0, static_cast<int>(bc)));
            break;
        case Q: nb = "vqd(r.q)"; break;
        default: nb = "vfix(vsplat(r.i))"; break;
        }
        const std::string ns = operand(fs);
        if (kind == Madd || kind == Msub) u.shadow_reads.push_back(kAcc);
        // vf00 is the constant (0, 0, 0, 1), and its broadcast lanes are 0 or 1. Both operands have been through vfix, so a sum, difference
        // or product with such a constant needs no vfix of its own: x +- 0 is x (or +0 for -0), x +- 1 stays clear of the largest
        // float (it rounds back to it) and of the denormals (a sum with 1 is 0 or at least 2^-24 away from it), x * 1 is x and x * 0 is
        // a signed zero. Programs are full of ADD/MULA/MADD with vf00 (the matrix
        // translation row, the w lane), which this makes as cheap as the float operation.
        const auto vf0_operand = [&](bool second) { return second ? ((src == Vec || src == Bc) && !ft) : !fs; };
        // (vf00 as one of the two registers makes every lane constant, so the lanes written do not matter here.)
        const bool constant_lanes = vf0_operand(false) || vf0_operand(true);
        switch (kind) {
        case Add: u.pre += constant_lanes ? std::format("const v4u uv = (v4u)((v4f){} + (v4f){}); ", ns, nb) : std::format("const v4u uv = vaddd({}, {}); ", ns, nb); break;
        case Sub: u.pre += constant_lanes ? std::format("const v4u uv = (v4u)((v4f){} - (v4f){}); ", ns, nb) : std::format("const v4u uv = vsubd({}, {}); ", ns, nb); break;
        case Mul: u.pre += constant_lanes ? std::format("const v4u uv = (v4u)((v4f){} * (v4f){}); ", ns, nb) : std::format("const v4u uv = vmuld({}, {}); ", ns, nb); break;
        case Madd: u.pre += constant_lanes ? std::format("const v4u uv = vaddd(nacc, (v4u)((v4f){} * (v4f){})); ", ns, nb) : std::format("const v4u uv = vmaddd(nacc, {}, {}); ", ns, nb); break;
        default: u.pre += constant_lanes ? std::format("const v4u uv = vsubd(nacc, (v4u)((v4f){} * (v4f){})); ", ns, nb) : std::format("const v4u uv = vmsubd(nacc, {}, {}); ", ns, nb); break;
        }
    }
    if (vector) {
        std::string vb;
        switch (src) {
        case Vec: vb = vreg(ft); break;
        case Bc: vb = std::format("vsplat({})", vf(ft, static_cast<int>(bc))); break;
        case Q: vb = "vsplat(r.q)"; break;
        default: vb = "vsplat(r.i)"; break;
        }
        const std::string vs = vreg(fs);
        // MAX, MINI, ABS and ITOF take the bits as they are (FTOI cleans its operand itself, so it does not care).
        const auto reads = [&](unsigned reg, unsigned lanes) {
            if (reg && lanes) u.raw_reads.emplace_back(static_cast<int>(reg), lanes);
        };
        if (kind == Max || kind == Min) {
            reads(fs, d);
            if (src == Vec) reads(ft, d);
            else if (src == Bc) reads(ft, 8u >> bc);
        }
        if (kind == Abs || kind == Itof) reads(fs, d);
        std::string e;
        switch (kind) {
        case Max: e = std::format("vmax({}, {})", vs, vb); break;
        case Min: e = std::format("vmin({}, {})", vs, vb); break;
        case Abs: e = std::format("vabs({})", vs); break;
        case Ftoi: e = std::format("vftoi({}, {})", vs, frac); break;
        case Itof: e = std::format("vitof({}, {})", vs, frac); break;
        default: break;
        }
        if (!e.empty()) u.pre += std::format("const v4u uv = {}; ", e);
        if (target == 0) {
            u.post = "{ " + set_flags("uv", d) + "} ";
            u.sets_flags = true;
            return u;
        }
        if (d == 0xF) u.post += std::format("vstore({}, uv); ", dst);
        else u.post += std::format("vstore({0}, vsel(vload({0}), uv, {1})); ", dst, lane_mask(d));
        u.sets_flags = flags;
        if (flags && flags_live) u.post += set_flags("uv", d);
        return u;
    }
    for (int i = 0; i < 4; ++i) {
        if (!lane(d, i)) continue;
        std::string e;
        switch (kind) {
        case Add: e = std::format("{}({}, {})", exact ? "fp::add" : "add", vf(fs, i), b(i)); break;
        case Sub: e = std::format("{}({}, {})", exact ? "fp::sub" : "sub", vf(fs, i), b(i)); break;
        case Mul: e = std::format("{}({}, {})", exact ? "fp::mul" : "mul", vf(fs, i), b(i)); break;
        case Madd: e = (exact ? std::format("fp::add(r.acc.w[{}], fp::mul({}, {}))", i, vf(fs, i), b(i)) : std::format("madd(r.acc.w[{}], {}, {})", i, vf(fs, i), b(i))); break;
        case Msub: e = (exact ? std::format("fp::sub(r.acc.w[{}], fp::mul({}, {}))", i, vf(fs, i), b(i)) : std::format("msub(r.acc.w[{}], {}, {})", i, vf(fs, i), b(i))); break;
        case Max: e = std::format("{}({}, {})", exact ? "fp::max" : "max", vf(fs, i), b(i)); break;
        case Min: e = std::format("{}({}, {})", exact ? "fp::min" : "min", vf(fs, i), b(i)); break;
        case Abs: e = std::format("({} & 0x7FFFFFFFu)", vf(fs, i)); break;
        case Ftoi: e = std::format("fp::to_fixed({}, {})", vf(fs, i), frac); break;
        case Itof: e = std::format("fp::from_fixed(r.vf[{}].sw[{}], {})", fs, i, frac); break;
        case Opmula: {
            static const int a[3] = {1, 2, 0}, c[3] = {2, 0, 1};
            e = std::format("{}({}, {})", exact ? "fp::mul" : "mul", vf(fs, a[i]), vf(ft, c[i]));
            break;
        }
        case Opmsub: {
            static const int a[3] = {1, 2, 0}, c[3] = {2, 0, 1};
            e = (exact ? std::format("fp::sub(r.acc.w[{}], fp::mul({}, {}))", i, vf(fs, a[i]), vf(ft, c[i])) : std::format("msub(r.acc.w[{}], {}, {})", i, vf(fs, a[i]), vf(ft, c[i])));
            break;
        }
        default: break;
        }
        u.pre += std::format("const std::uint32_t u{} = {}; ", kLane[i], e);
    }
    if (target == 0) {
        u.post = "{ Reg128 uf{}; ";
        for (int i = 0; i < 4; ++i)
            if (lane(d, i)) u.post += std::format("uf.w[{}] = u{}; ", i, kLane[i]);
        u.post += set_flags(late_flags ? "vload(uf)" : "uf", d) + "} ";
        u.sets_flags = true;
        return u;
    }
    for (int i = 0; i < 4; ++i)
        if (lane(d, i)) u.post += std::format("{}.w[{}] = u{}; ", dst, i, kLane[i]);
    u.sets_flags = flags;
    if (flags && flags_live) u.post += set_flags(late_flags ? std::format("vload({})", dst) : dst, d);
    return u;
}

std::string lanes_copy(unsigned dest, const std::string &to, const std::string &from_qword) {
    if (dest == 0xF) return std::format("std::memcpy(&{}, {}, 16); ", to, from_qword);
    if (dest == 0) return {};
    // A few lanes of a register: one blend of the whole register. Separate 4-byte stores into the register would make the next
    // 16-byte read of it wait until they have all reached the cache (no store forwarding from several small stores to one big load).
    return std::format("vstore({0}, vsel(vload({0}), vload_ptr({1}), {2})); ", to, from_qword, lane_mask(dest));
}

std::string lanes_store(unsigned dest, const std::string &qword_expr, const std::string &from) {
    if (dest == 0xF) return std::format("std::memcpy({}, &{}, 16); ", qword_expr, from);
    std::string s;
    for (int i = 0; i < 4; ++i)
        if (lane(dest, i)) s += std::format("std::memcpy({} + {}, &{}.w[{}], 4); ", qword_expr, i * 4, from, i);
    return s;
}

struct Lower {
    std::string code;
    // Runs after the upper instruction of the pair has committed its flags (FSSET, upstream b7371e8)
    std::string after_upper;
    int writes = -1;
    bool reads_flags = false, reads_status = false;
    bool branch = false, jump = false;
    std::uint32_t target = 0;
    bool supported = true;
    // Registers and lanes whose bits the instruction reads as they are (see Upper::raw_reads).
    std::vector<std::pair<int, unsigned>> raw_reads;
};

Lower lower(std::uint32_t w, std::uint32_t pc, std::uint32_t code_mask = kMask, bool unit0 = false) {
    const std::uint32_t kMask = code_mask;
    Lower l;
    const unsigned dest = (w >> 21) & 15u, it = (w >> 16) & 15u, is = (w >> 11) & 15u, id = (w >> 6) & 15u;
    const unsigned ft = (w >> 16) & 31u, fs = (w >> 11) & 31u;
    const std::int32_t imm11 = static_cast<std::int32_t>(w << 21) >> 21;
    std::string &c = l.code;
    const auto reads = [&](unsigned reg, unsigned lanes) {
        if (reg && lanes) l.raw_reads.emplace_back(static_cast<int>(reg), lanes);
    };
    if ((w >> 25) == 0x40u) {
        const unsigned funct = w & 63u;
        if (funct < 0x3C) {
            switch (funct) {
            case 0x30: c = std::format("set_vi(r, {}, r.vi[{}] + r.vi[{}]); ", id, is, it); break;
            case 0x31: c = std::format("set_vi(r, {}, r.vi[{}] - r.vi[{}]); ", id, is, it); break;
            case 0x32: c = std::format("set_vi(r, {}, r.vi[{}] + {}u); ", it, is, static_cast<std::uint32_t>(static_cast<std::int32_t>(((w >> 6) & 31u) << 27) >> 27)); break;
            case 0x34: c = std::format("set_vi(r, {}, r.vi[{}] & r.vi[{}]); ", id, is, it); break;
            case 0x35: c = std::format("set_vi(r, {}, r.vi[{}] | r.vi[{}]); ", id, is, it); break;
            default: l.supported = false; break;
            }
            return l;
        }
        const unsigned index = ((w >> 6) & 31u) * 4u + (funct & 3u);
        const unsigned fsf = (w >> 21) & 3u, ftf = (w >> 23) & 3u;
        switch (index) {
        case 0x30:
            if (ft) l.writes = static_cast<int>(ft);
            if (ft && dest) c = std::format("vstore(r.vf[{0}], vsel(vload(r.vf[{0}]), vload(r.vf[{1}]), {2})); ", ft, fs, lane_mask(dest));
            if (ft && dest) reads(fs, dest);
            break;
        case 0x31:
            if (ft) l.writes = static_cast<int>(ft);
            if (ft && dest) reads(fs, 0xFu);
            if (ft && dest) {
                // the lanes turned by one place: x gets y, y gets z, z gets w, w gets x
                c = std::format("{{ const v4u m = vload(r.vf[{1}]); vstore(r.vf[{0}], vsel(vload(r.vf[{0}]), v4u{{m[1], m[2], m[3], m[0]}}, {2})); }} ", ft, fs, lane_mask(dest));
            }
            break;
        case 0x34:
            if (ft) l.writes = static_cast<int>(ft);
            c = std::format("{{ const std::uint8_t *q = qword(mem, r.vi[{}]); ", is);
            if (ft) c += lanes_copy(dest, std::format("r.vf[{}]", ft), "q");
            c += std::format("set_vi(r, {}, r.vi[{}] + 1u); }} ", is, is);
            break;
        case 0x35:
            reads(fs, dest);
            c = std::format("{{ std::uint8_t *q = qword(mem, r.vi[{}]); ", it) + lanes_store(dest, "q", std::format("r.vf[{}]", fs)) +
                std::format("set_vi(r, {}, r.vi[{}] + 1u); }} ", it, it);
            break;
        case 0x36:
            if (ft) l.writes = static_cast<int>(ft);
            c = std::format("{{ set_vi(r, {}, r.vi[{}] - 1u); const std::uint8_t *q = qword(mem, r.vi[{}]); ", is, is, is);
            if (ft) c += lanes_copy(dest, std::format("r.vf[{}]", ft), "q");
            c += "} ";
            break;
        case 0x37:
            reads(fs, dest);
            c = std::format("{{ set_vi(r, {}, r.vi[{}] - 1u); std::uint8_t *q = qword(mem, r.vi[{}]); ", it, it, it) +
                lanes_store(dest, "q", std::format("r.vf[{}]", fs)) + "} ";
            break;
        case 0x38: c = std::format("{{ const std::uint32_t qb = fl.begin_q(r); vu::div(r, r.vf[{}].w[{}], r.vf[{}].w[{}]); fl.end_q(r, qb, 7u); }} ", fs, fsf, ft, ftf); break;
        case 0x39: c = std::format("{{ const std::uint32_t qb = fl.begin_q(r); vu::sqrt(r, r.vf[{}].w[{}]); fl.end_q(r, qb, 7u); }} ", ft, ftf); break;
        case 0x3A: c = std::format("{{ const std::uint32_t qb = fl.begin_q(r); vu::rsqrt(r, r.vf[{}].w[{}], r.vf[{}].w[{}]); fl.end_q(r, qb, 13u); }} ", fs, fsf, ft, ftf); break;
        case 0x3B: c = "fl.flush_q(r); "; break;
        case 0x3C: reads(fs, 8u >> fsf); c = std::format("set_vi(r, {}, r.vf[{}].w[{}]); ", it, fs, fsf); break;
        case 0x3D:
            if (ft) l.writes = static_cast<int>(ft);
            if (ft) {
                c = std::format("{{ const std::uint32_t v = static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int16_t>(r.vi[{}]))); ", is);
                if (dest) c += std::format("vstore(r.vf[{0}], vsel(vload(r.vf[{0}]), vsplat(v), {1})); ", ft, lane_mask(dest));
                c += "} ";
            }
            break;
        case 0x3E:
            for (int i = 0; i < 4; ++i)
                if (lane(dest, i)) {
                    c = std::format("{{ std::uint16_t v; std::memcpy(&v, qword(mem, r.vi[{}]) + {}, 2); set_vi(r, {}, v); }} ", is, i * 4, it);
                    break;
                }
            break;
        case 0x3F:
            c = std::format("{{ std::uint8_t *q = qword(mem, r.vi[{}]); const std::uint32_t v = r.vi[{}]; ", is, it);
            for (int i = 0; i < 4; ++i) if (lane(dest, i)) c += std::format("std::memcpy(q + {}, &v, 4); ", i * 4);
            c += "} ";
            break;
        case 0x40:
            if (ft) l.writes = static_cast<int>(ft);
            c = "r.r = vu::rand_next(r.r); ";
            if (ft) for (int i = 0; i < 4; ++i) if (lane(dest, i)) c += std::format("r.vf[{}].w[{}] = r.r | fp::kOne; ", ft, i);
            break;
        case 0x41:
            if (ft) l.writes = static_cast<int>(ft);
            if (ft) for (int i = 0; i < 4; ++i) if (lane(dest, i)) c += std::format("r.vf[{}].w[{}] = (r.r & 0x7FFFFFu) | fp::kOne; ", ft, i);
            break;
        case 0x42: reads(fs, 8u >> fsf); c = std::format("r.r = r.vf[{}].w[{}] & 0x7FFFFFu; ", fs, fsf); break;
        case 0x43: reads(fs, 8u >> fsf); c = std::format("r.r = (r.r ^ r.vf[{}].w[{}]) & 0x7FFFFFu; ", fs, fsf); break;
        case 0x64:
            if (ft) l.writes = static_cast<int>(ft);
            if (ft) for (int i = 0; i < 4; ++i) if (lane(dest, i)) c += std::format("r.vf[{}].w[{}] = r.p; ", ft, i); break;
        // Match the interpreter's lower(): VU0 has no TOP and reads ITOP from
        // VIF0; the no-argument helpers expose VU1's latched values.
        case 0x68: c = std::format("set_vi(r, {}, {}); ", it, unit0 ? "0u" : "vu::top()"); break;
        case 0x69: c = std::format("set_vi(r, {}, {}); ", it, unit0 ? "vu::itop(0)" : "vu::itop()"); break;
        case 0x6C: c = std::format("vu::kick(1, r.vi[{}]); ", is); break;
        case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x78: case 0x79: case 0x7A: case 0x7C: case 0x7D: case 0x7E:
            reads(fs, 0xFu);
            c = std::format("efu(r, {}u, {}u, {}u); ", index, fs, fsf);
            break;
        case 0x7B: break;
        default: l.supported = false; break;
        }
        return l;
    }
    const std::uint32_t imm15 = ((w >> 10) & 0x7800u) | (w & 0x7FFu);
    const std::uint32_t imm12 = ((w >> 10) & 0x800u) | (w & 0x7FFu);
    const std::uint32_t target = (pc + 8u + static_cast<std::uint32_t>(imm11) * 8u) & kMask;
    switch (w >> 25) {
    case 0x00:
        if (ft) l.writes = static_cast<int>(ft);
        c = std::format("{{ const std::uint8_t *q = qword(mem, r.vi[{}] + {}u); ", is, static_cast<std::uint32_t>(imm11));
        if (ft) c += lanes_copy(dest, std::format("r.vf[{}]", ft), "q");
        c += "} ";
        break;
    case 0x01:
        reads(fs, dest);
        c = std::format("{{ std::uint8_t *q = qword(mem, r.vi[{}] + {}u); ", it, static_cast<std::uint32_t>(imm11)) + lanes_store(dest, "q", std::format("r.vf[{}]", fs)) + "} ";
        break;
    case 0x04:
        for (int i = 0; i < 4; ++i)
            if (lane(dest, i)) {
                c = std::format("{{ std::uint16_t v; std::memcpy(&v, qword(mem, r.vi[{}] + {}u) + {}, 2); set_vi(r, {}, v); }} ", is, static_cast<std::uint32_t>(imm11), i * 4, it);
                break;
            }
        break;
    case 0x05:
        c = std::format("{{ std::uint8_t *q = qword(mem, r.vi[{}] + {}u); const std::uint32_t v = r.vi[{}]; ", is, static_cast<std::uint32_t>(imm11), it);
        for (int i = 0; i < 4; ++i) if (lane(dest, i)) c += std::format("std::memcpy(q + {}, &v, 4); ", i * 4);
        c += "} ";
        break;
    case 0x08: c = std::format("set_vi(r, {}, r.vi[{}] + {}u); ", it, is, imm15); break;
    case 0x09: c = std::format("set_vi(r, {}, r.vi[{}] - {}u); ", it, is, imm15); break;
    case 0x10: c = std::format("set_vi(r, 1, (fl.clip() & 0xFFFFFFu) == {}u ? 1u : 0u); ", w & 0xFFFFFFu); break;
    case 0x11: c = std::format("r.clip = {}u; fl.set_clip(r.clip, fl.t - 4u); ", w & 0xFFFFFFu); break;
    case 0x12: c = std::format("set_vi(r, 1, (fl.clip() & {}u) ? 1u : 0u); ", w & 0xFFFFFFu); break;
    case 0x13: c = std::format("set_vi(r, 1, ((fl.clip() | {}u) & 0xFFFFFFu) == 0xFFFFFFu ? 1u : 0u); ", w & 0xFFFFFFu); break;
    case 0x14: c = std::format("set_vi(r, {}, (fl.status() & 0xFFFu) == {}u ? 1u : 0u); ", it, imm12); l.reads_flags = l.reads_status = true; break;
    case 0x15: l.after_upper = std::format("r.status = (r.status & 0x3Fu) | {}u; fl.force_mac(r.mac, r.status); ", imm12 & 0xFC0u); l.reads_flags = l.reads_status = true; break;
    case 0x16: c = std::format("set_vi(r, {}, fl.status() & {}u); ", it, imm12); l.reads_flags = l.reads_status = true; break;
    case 0x17: c = std::format("set_vi(r, {}, (fl.status() & 0xFFFu) | {}u); ", it, imm12); l.reads_flags = l.reads_status = true; break;
    case 0x18: c = std::format("set_vi(r, {}, (fl.mac() & 0xFFFFu) == r.vi[{}] ? 1u : 0u); ", it, is); l.reads_flags = true; break;
    case 0x1A: c = std::format("set_vi(r, {}, fl.mac() & r.vi[{}]); ", it, is); l.reads_flags = true; break;
    case 0x1B: c = std::format("set_vi(r, {}, (fl.mac() & 0xFFFFu) | r.vi[{}]); ", it, is); l.reads_flags = true; break;
    case 0x1C: c = std::format("set_vi(r, {}, fl.clip() & 0xFFFu); ", it); break;
    case 0x20: c = "pend = true; "; l.branch = true; l.target = target; break;
    case 0x21: c = std::format("set_vi(r, {}, {}u); pend = true; ", it, (pc + 16u) / 8u); l.branch = true; l.target = target; break;
    case 0x24: c = std::format("jpc = (static_cast<std::uint32_t>(r.vi[{}]) * 8u) & {}u; pendj = true; ", is, kMask); l.jump = true; break;
    case 0x25: c = std::format("jpc = (static_cast<std::uint32_t>(r.vi[{}]) * 8u) & {}u; set_vi(r, {}, {}u); pendj = true; ", is, kMask, it, (pc + 16u) / 8u); l.jump = true; break;
    case 0x28: c = std::format("pend = r.vi[{}] == r.vi[{}]; ", is, it); l.branch = true; l.target = target; break;
    case 0x29: c = std::format("pend = r.vi[{}] != r.vi[{}]; ", is, it); l.branch = true; l.target = target; break;
    case 0x2C: c = std::format("pend = static_cast<std::int16_t>(r.vi[{}]) < 0; ", is); l.branch = true; l.target = target; break;
    case 0x2D: c = std::format("pend = static_cast<std::int16_t>(r.vi[{}]) > 0; ", is); l.branch = true; l.target = target; break;
    case 0x2E: c = std::format("pend = static_cast<std::int16_t>(r.vi[{}]) <= 0; ", is); l.branch = true; l.target = target; break;
    case 0x2F: c = std::format("pend = static_cast<std::int16_t>(r.vi[{}]) >= 0; ", is); l.branch = true; l.target = target; break;
    default: l.supported = false; break;
    }
    return l;
}

bool upper_sets_flags(std::uint32_t hi) { return upper(hi, true).sets_flags; }

void replace_all(std::string &s, const std::string &from, const std::string &to) {
    for (std::size_t at = s.find(from); at != std::string::npos; at = s.find(from, at + to.size())) s.replace(at, from.size(), to);
}

std::string local_vi(std::string o) {
    std::string declare, sync;
    replace_all(o, "set_vi(r, 0, ", "drop_vi(");
    replace_all(o, "r.vi[0]", "0u");
    for (int n = 1; n < 16; ++n) {
        const std::string set = std::format("set_vi(r, {}, ", n), get = std::format("r.vi[{}]", n);
        const bool written = o.find(set) != std::string::npos;
        if (!written && o.find(get) == std::string::npos) continue;
        replace_all(o, set, std::format("set_vi(i{}, ", n));
        replace_all(o, get, std::format("i{}", n));
        declare += std::format("    std::uint16_t i{} = r.vi[{}];\n", n, n);
        if (written) sync += std::format("r.vi[{}] = i{}; ", n, n);
    }
    const std::size_t body = o.find("    goto L_");
    o.insert(body, declare);
    if (!sync.empty()) {
        replace_all(o, "vu::interpret(", sync + "vu::interpret(");
        replace_all(o, "r.tpc = ", sync + "r.tpc = ");
    }
    return o;
}

// The integer register a lower instruction's code writes, or -1.
int vi_written(const std::string &code) {
    const std::size_t at = code.find("set_vi(r, ");
    if (at == std::string::npos) return -1;
    return std::atoi(code.c_str() + at + 10);
}

// Tests only: the MAC flags of every flag site worked out at once, as before vurecomp 11 (the reference for FlagPipeV).
bool g_eager_flags = false;

std::string generate(const Program &p, std::string &name) {
    name.clear();
    const std::uint32_t kMask = p.unit == 0 ? 4095u : 16383u;
    const std::vector<std::uint32_t> pcs = ps2::vu::reachable(p.code.data(), kMask, p.entry);
    const ps2::vu::StallPlan plan = ps2::vu::stall_plan(p.code.data(), kMask, pcs, p.entry);
    if (!plan.ok) return {};
    std::set<std::uint32_t> in(pcs.begin(), pcs.end());
    std::map<std::uint32_t, Lower> lowers;
    bool any_status = p.unit == 0, any_flags = false, any_q = false;
    for (std::uint32_t pc : pcs) {
        const std::uint32_t lo = word(p.code, pc), hi = word(p.code, pc + 4);
        Lower l;
        if (!(hi & 0x80000000u)) l = lower(lo, pc, kMask, p.unit == 0);
        if (ps2::vu::branch_in_delay_slot(p.code.data(), kMask, pc)) l.supported = false;
        any_status = any_status || l.reads_status;
        any_flags = any_flags || l.reads_flags;
        any_q = any_q || l.code.find("fl.end_q(") != std::string::npos;
        if (p.unit == 0 && (!l.supported || l.jump || l.code.find("vu::kick(") != std::string::npos)) return {};
        lowers[pc] = l;
    }
    std::map<std::uint32_t, bool> live_in;
    const auto successors = [&](std::uint32_t pc) {
        std::vector<std::int64_t> s{static_cast<std::int64_t>((pc + 8u) & kMask)};
        const std::uint32_t prev = (pc - 8u) & kMask;
        if (in.count(prev)) {
            const Lower &pl = lowers[prev];
            const std::uint32_t phi = word(p.code, prev + 4);
            if (pl.branch) s.push_back(pl.target);
            if (pl.jump || (phi & 0x40000000u)) s.push_back(-1);
        }
        return s;
    };
    for (std::uint32_t pc : pcs) live_in[pc] = false;
    for (bool changed = true; changed;) {
        changed = false;
        for (auto it = pcs.rbegin(); it != pcs.rend(); ++it) {
            const std::uint32_t pc = *it;
            bool out = false;
            for (std::int64_t s : successors(pc)) out = out || s < 0 || !in.count(static_cast<std::uint32_t>(s)) || live_in[static_cast<std::uint32_t>(s)];
            const std::uint32_t hi = word(p.code, pc + 4);
            bool reader_near = false;
            for (std::uint32_t k = 1; k <= 3; ++k) {
                const std::uint32_t q = (pc + 8u * k) & kMask;
                if (in.count(q) && (lowers[q].reads_flags || lowers[q].branch || lowers[q].jump)) reader_near = true;
            }
            const bool kills = upper_sets_flags(hi) && !reader_near;
            const bool v = lowers[pc].reads_flags || (!kills && out);
            if (v != live_in[pc]) {
                live_in[pc] = v;
                changed = true;
            }
        }
    }
    std::map<std::uint32_t, int> def;
    std::map<std::uint32_t, std::vector<std::uint32_t>> preds;
    std::set<std::uint32_t> targets;
    for (std::uint32_t pc : pcs) {
        const std::uint32_t lo = word(p.code, pc), hi = word(p.code, pc + 4);
        def[pc] = (hi & 0x80000000u) || ps2::vu::branch_delay_exempt(lo) ? -1 : vi_written(lowers[pc].code);
        if (lowers[pc].branch) targets.insert(lowers[pc].target & kMask);
        for (std::int64_t s : successors(pc))
            if (s >= 0 && in.count(static_cast<std::uint32_t>(s))) preds[static_cast<std::uint32_t>(s)].push_back(pc);
    }
    std::map<std::uint32_t, int> delay, keep_bk;
    std::set<std::uint32_t> set_bi;
    for (std::uint32_t pc : pcs) {
        if (!lowers[pc].branch) continue;
        const std::uint32_t lo = word(p.code, pc), op = lo >> 25, it = (lo >> 16) & 15u, is = (lo >> 11) & 15u;
        std::uint32_t reads = 0;
        if (op == 0x28 || op == 0x29) reads = (1u << is) | (1u << it);
        else if (op >= 0x2C && op <= 0x2F) reads = 1u << is;
        reads &= ~1u;
        bool hit = false;
        for (std::uint32_t q : preds[pc]) hit = hit || (def[q] > 0 && ((reads >> def[q]) & 1u));
        if (!hit) continue;
        const bool dynamic = pc == (p.entry & kMask) || targets.count(pc) || !in.count((pc - 8u) & kMask);
        delay[pc] = dynamic ? 2 : 1;
        for (std::uint32_t q0 : preds[pc]) {
            const int reg = def[q0];
            if (dynamic) set_bi.insert(q0);
            if (reg <= 0 || !((reads >> reg) & 1u)) continue;
            auto walk = [&](auto &self, std::uint32_t u, int depth) -> void {
                bool extended = false;
                if (depth < 3) {
                    for (std::uint32_t pred : preds[u]) {
                        if (def[pred] == reg) {
                            extended = true;
                            self(self, pred, depth + 1);
                        }
                    }
                }
                if (extended) {
                    for (std::uint32_t pred : preds[u]) {
                        if (def[pred] != reg) keep_bk[pred] = reg;
                    }
                } else {
                    keep_bk[u] = reg;
                }
            };
            walk(walk, q0, 0);
        }
    }
    std::set<int> shadowed;
    for (std::uint32_t pc : pcs)
        for (int reg : upper(word(p.code, pc + 4), true, p.unit == 0).shadow_reads) shadowed.insert(reg);
    // The lazy clean-up of unit 1 (ps2/vu_ops.hpp, "done late"): an add, subtract or multiply leaves its result clamped but possibly
    // with a denormal in it, in the register as in its shadow. `a_in`/`a_out` say for every instruction pair which lanes of which
    // register hold such a result for certain (on every path); where a pair reads the bits of such a lane, and where the program
    // leaves, a vflush comes first. Where two paths meet and only one of them has the lane in that state, that path flushes it on its
    // way, unless nothing can read the lane before it is written again (`raw_live`): a register a loop computes anew every round is
    // then flushed once, after the loop.
    using Lanes = std::array<std::uint8_t, 33>;  // by register (1 to 31, 32 = ACC); bit 8 is x, bit 1 is w
    struct Effect {
        Lanes observed{}, kill{}, gen{};
        bool stops = false;
    };
    // Setting the bit and putting it back are two writes of MXCSR per call, which cost far more than an instruction: the processor
    // waits at each of them (measured on a Zen 3: a short program of 15 multiplies and adds took more than twice as long with them).
    // So a program gets the late clean-up only where its arithmetic makes up for that: an estimate of the adds, subtracts and
    // multiplies one call runs, each one in a loop counted as kLoopRounds. Every other program of unit 1 cleans each result at once,
    // as before (the same code as without SSE4.1; both ways give the same bits, so the choice only changes the speed).
    constexpr int kLoopRounds = 8, kDazWorth = 40;
    int worth = 0;
    if (p.unit != 0) {
        for (std::uint32_t pc : pcs) {
            const std::uint32_t hi = word(p.code, pc + 4);
            const bool has_lower = !(hi & 0x80000000u);
            if (has_lower && !lowers[pc].supported) continue;
            if (!upper(hi, true, false).arith) continue;
            // in a loop when the pair can reach itself again
            std::set<std::uint32_t> seen;
            std::vector<std::uint32_t> todo{pc};
            bool loop = false;
            while (!todo.empty() && !loop) {
                const std::uint32_t q = todo.back();
                todo.pop_back();
                for (std::int64_t s : successors(q)) {
                    if (s < 0 || !in.count(static_cast<std::uint32_t>(s))) continue;
                    if (static_cast<std::uint32_t>(s) == pc) loop = true;
                    else if (seen.insert(static_cast<std::uint32_t>(s)).second) todo.push_back(static_cast<std::uint32_t>(s));
                }
            }
            worth += loop ? kLoopRounds : 1;
        }
    }
    const bool lazy = p.unit != 0 && worth >= kDazWorth;
    bool any_arith = false;
    std::map<std::uint32_t, Effect> effect;
    std::map<std::uint32_t, Lanes> a_in, a_out, raw_live;
    if (lazy) {
        for (std::uint32_t pc : pcs) {
            const std::uint32_t lo = word(p.code, pc), hi = word(p.code, pc + 4);
            const Upper u = upper(hi, true, false);
            const Lower &l = lowers[pc];
            const bool has_lower = !(hi & 0x80000000u);
            Effect e;
            if (has_lower && !l.supported) {
                // the pair is handed to the interpreter as it is: everything is read, nothing is done here
                e.observed.fill(0xF);
                e.stops = true;
            } else {
                any_arith = any_arith || u.arith;
                for (const auto &[reg, lanes] : u.raw_reads) e.observed[reg] |= lanes;
                if (has_lower)
                    for (const auto &[reg, lanes] : l.raw_reads) e.observed[reg] |= lanes;
                // the store of the lower instruction comes first in the generated code, the store of the upper one after it
                if (has_lower && l.writes > 0) e.kill[l.writes] |= (lo >> 21) & 15u;
                if (u.target > 0) {
                    e.kill[u.target] |= u.mask;
                    if (u.normalized) e.gen[u.target] |= u.mask;
                }
            }
            effect[pc] = e;
            raw_live[pc] = Lanes{};
            a_in[pc] = Lanes{};
            a_out[pc].fill(0xF);
        }
        for (bool changed = true; changed;) {
            changed = false;
            for (auto it = pcs.rbegin(); it != pcs.rend(); ++it) {
                const Effect &e = effect[*it];
                Lanes out{};
                for (std::int64_t s : successors(*it)) {
                    if (s < 0 || !in.count(static_cast<std::uint32_t>(s))) out.fill(0xF);
                    else
                        for (int reg = 1; reg <= 32; ++reg) out[reg] |= raw_live[static_cast<std::uint32_t>(s)][reg];
                }
                Lanes v{};
                for (int reg = 1; reg <= 32; ++reg) v[reg] = e.stops ? 0xF : static_cast<std::uint8_t>(e.observed[reg] | (out[reg] & ~e.kill[reg]));
                if (v != raw_live[*it]) {
                    raw_live[*it] = v;
                    changed = true;
                }
            }
        }
        for (bool changed = true; changed;) {
            changed = false;
            for (std::uint32_t pc : pcs) {
                Lanes v{};
                if (pc != (p.entry & kMask) && !preds[pc].empty()) {
                    v.fill(0xF);
                    for (std::uint32_t q : preds[pc])
                        for (int reg = 1; reg <= 32; ++reg) v[reg] &= a_out[q][reg];
                }
                const Effect &e = effect[pc];
                Lanes out{};
                for (int reg = 1; reg <= 32; ++reg) out[reg] = static_cast<std::uint8_t>(((v[reg] & ~e.observed[reg]) & ~e.kill[reg]) | e.gen[reg]);
                if (v != a_in[pc] || out != a_out[pc]) {
                    a_in[pc] = v;
                    a_out[pc] = out;
                    changed = true;
                }
            }
        }
    }
    // The last XGKICK of a run. A kick leaves the body with MXCSR put back (vkick) and sets the bit again afterwards, for the float
    // arithmetic still to come. Where none can come any more (`daz_out`: no add, subtract, multiply or EFU call on any way from the
    // pair to the end of the program; everything else does not care about the bit, see ps2/vu_ops.hpp), MXCSR simply stays as the
    // caller has it (vkick_last) and the function around the body has nothing to put back: two writes of MXCSR less per call.
    // Every result of the arithmetic before the kick is in the register file by then (each pair stores what it computes), so no
    // float instruction of it can be left to run after the kick. The pair after one with the E bit ends the program when it can
    // only be reached through that pair; a jump (JR, JALR) and a pair outside the program leave the body.
    std::map<std::uint32_t, bool> daz_use, daz_in, daz_out;
    const auto ends_after = [&](std::uint32_t pc) {
        const std::uint32_t prev = (pc - 8u) & kMask;
        return in.count(prev) && (word(p.code, prev + 4) & 0x40000000u) && pc != (p.entry & kMask) && preds[pc] == std::vector<std::uint32_t>{prev};
    };
    if (lazy) {
        for (std::uint32_t pc : pcs) {
            const std::uint32_t hi = word(p.code, pc + 4);
            const bool has_lower = !(hi & 0x80000000u);
            daz_use[pc] = upper(hi, true, false).arith || (has_lower && lowers[pc].code.find("efu(r, ") != std::string::npos);
            daz_in[pc] = daz_out[pc] = false;
        }
        for (bool changed = true; changed;) {
            changed = false;
            for (auto it = pcs.rbegin(); it != pcs.rend(); ++it) {
                bool out = false;
                if (!ends_after(*it))
                    for (std::int64_t s : successors(*it)) out = out || (s >= 0 && in.count(static_cast<std::uint32_t>(s)) && daz_in[static_cast<std::uint32_t>(s)]);
                const bool v = daz_use[*it] || out;
                if (out != daz_out[*it] || v != daz_in[*it]) {
                    daz_out[*it] = out;
                    daz_in[*it] = v;
                    changed = true;
                }
            }
        }
    }
    // A flush at the start of pair `pc`, or right after it. Where `daz_in[pc]` holds, the "denormals are zero" bit is set there for
    // certain: the bit is set when the body starts, and only a vkick_last clears it, at a pair q with !daz_in[q]; any pair reached
    // after q (the program not having ended) is a successor of a successor ... of q, and daz_in[q] is true when it is true for any of
    // them. The pair `pc` itself has no vkick_last then either. There the processor does the flush (vflushd, a multiply by one);
    // elsewhere it is done with integer instructions.
    const auto flush = [&](const Lanes &lanes, std::uint32_t pc) {
        const bool daz = any_arith && daz_in.count(pc) && daz_in[pc];
        std::string text;
        for (int reg = 1; reg <= 32; ++reg)
            if (lanes[reg]) text += std::format("{}({}, {}); ", daz ? "vflushd" : "vflush", raw(reg), lane_mask(lanes[reg]));
        return text;
    };
    // What has to be flushed where a pair leaves for the interpreter or ends the program, and on the way from one pair to another.
    const auto leave = [&](std::uint32_t pc) { return lazy ? flush(a_out[pc], pc) : std::string(); };
    const auto meet = [&](std::uint32_t from, std::uint32_t to) {
        if (!lazy || !in.count(to)) return std::string();
        Lanes lanes{};
        for (int reg = 1; reg <= 32; ++reg) lanes[reg] = static_cast<std::uint8_t>(a_out[from][reg] & ~a_in[to][reg] & raw_live[to][reg]);
        return flush(lanes, from);
    };
    name = std::format("vu{}_{:016X}", p.unit, p.key);
    // Unit 1: the program is a body that every float instruction lives in, and a small function around it that sets the "denormals
    // are zero" bit of the processor for the body (when the program has float arithmetic at all) and does a hop to the interpreter
    // after it.
    const std::string head = !lazy ? std::format("void {}(Vu0Regs &r, std::uint8_t *mem) {{\n", name)
                             : any_arith ? std::format("PS2_VU_BODY void {}_b(Vu0Regs &r, std::uint8_t *mem, VuDaz &daz, std::uint32_t &hop) {{\n    (void)daz;\n", name)
                                         : std::format("static inline __attribute__((always_inline)) void {}_b(Vu0Regs &r, std::uint8_t *mem, std::uint32_t &hop) {{\n", name);
    // The MAC flags worked out where they are read (vu-sticky): for VU1 programs that read the MAC flags but not the status. The
    // flag sites stay the same (live_in above), so the reader, the next program and the interpreter see the same bits.
    const bool late_flags = p.unit != 0 && any_flags && !any_status && !g_eager_flags;
    std::string o = head + std::string(late_flags ? "    FlagPipeV fl;\n" : "    FlagPipe fl;\n") + std::string("    fl.start(r);\n    bool pend = false, pendj = false, ending = false;\n    std::uint32_t jpc = 0;\n    (void)pend;\n    (void)pendj;\n    (void)ending;\n    (void)jpc;\n");
    if (p.unit == 0) o += "    std::uint32_t steps = 0;\n";
    if (!delay.empty()) o += "    std::uint16_t bk = 0, next_bk = 0;\n    unsigned bi = 0;\n    (void)bk;\n    (void)next_bk;\n    (void)bi;\n";
    // An instruction whose stall depends on the path that led to it reads the pipeline state index `st` the edges set.
    const auto list = [](const std::vector<std::uint8_t> &v) {
        std::string text;
        for (std::uint8_t x : v) text += std::format("{}{}", text.empty() ? "" : ", ", x);
        return text;
    };
    if (plan.any_tracked) {
        o += std::format("    unsigned st = {}u;\n", plan.start);
        for (std::uint32_t pc : pcs)
            if (plan.tracked[pc >> 3]) o += std::format("    static constexpr std::uint8_t ks_{:04X}[] = {{{}}};\n", pc, list(plan.by_state[pc >> 3]));
        for (const auto &e : plan.edges)
            if (plan.tracked[e.from >> 3]) o += std::format("    static constexpr std::uint8_t ke_{:04X}_{:04X}[] = {{{}}};\n", e.from, e.to, list(e.next));
    }
    for (int reg : shadowed) o += std::format("    v4u {} = vshadow({});\n", shadow(reg), raw(reg));
    o += std::format("    goto {};\n", label(p.entry & kMask));
    for (std::uint32_t pc : pcs) {
        const std::uint32_t lo = word(p.code, pc), hi = word(p.code, pc + 4);
        bool out_live = false;
        for (std::int64_t s : successors(pc)) out_live = out_live || s < 0 || !in.count(static_cast<std::uint32_t>(s)) || live_in[static_cast<std::uint32_t>(s)];
        const Upper u = upper(hi, any_status ? true : (any_flags && out_live), p.unit == 0, late_flags);
        const Lower &l = lowers[pc];
        std::string body;
        if (lazy) {
            Lanes lanes{};
            for (int reg = 1; reg <= 32; ++reg) lanes[reg] = a_in[pc][reg] & effect[pc].observed[reg];
            body += flush(lanes, pc);
        }
        if (const auto kb = keep_bk.find(pc); kb != keep_bk.end()) body += std::format("next_bk = r.vi[{}]; ", kb->second);
        body += u.pre;
        std::string lcode = l.code;
        if (const auto d = delay.find(pc); d != delay.end()) {
            for (int reg = 1; reg < 16; ++reg) {
                if (d->second == 2) replace_all(lcode, std::format("r.vi[{}]", reg), std::format("(bi == {} ? bk : r.vi[{}])", reg, reg));
                else if (reg == def[(pc - 8u) & kMask]) replace_all(lcode, std::format("r.vi[{}]", reg), "bk");
            }
        }
        if (hi & 0x80000000u) body += std::format("r.i = {}u; ", lo);
        else if (!l.supported) body += std::format("vu::interpret(1, {}u); return; ", pc);
        else {
            if (lazy && any_arith && !daz_in[pc]) replace_all(lcode, "vu::kick(1, ", "vkick_last(daz, 1, ");
            body += lcode;
        }
        if (keep_bk.count(pc)) body += "bk = next_bk; ";
        if (set_bi.count(pc)) body += std::format("bi = {}u; ", std::max(def[pc], 0));
        body += u.post;
        if (!(hi & 0x80000000u) && l.supported) body += l.after_upper;
        std::string stop;
        if (p.unit == 0) {
            stop = std::format("if (steps >= 1000000u) {{ std::fprintf(stderr, \"[vu0] program at 0x{:04X} did not end\\n\"); return; }} ", p.entry);
            stop += "if (!(steps & 255u) && vu::stopped()) return; ++steps; ";
        }
        const unsigned stall = plan.stall[pc >> 3];
        if (plan.tracked[pc >> 3]) o += std::format("{}: {{ {}fl.t += 1u + ks_{:04X}[st]; ", label(pc), stop, pc);
        else if (stall) o += std::format("{}: {{ {}fl.t += {}u; ", label(pc), stop, 1 + stall);
        else o += std::format("{}: {{ {}++fl.t; ", label(pc), stop);
        if (any_q && ps2::vu::reads_q(hi)) {
            const ps2::vu::QRead kind = ps2::vu::q_read_kind(p.code.data(), kMask, pc);
            if (kind == ps2::vu::kQNew) o += "fl.flush_q(r); ";
            else if (kind == ps2::vu::kQDynamic) o += "fl.sync_q(r); ";
        }
        o += body;
        const int lower_write = (hi & 0x80000000u) || !l.supported ? -1 : l.writes;
        if (lower_write >= 0 && shadowed.count(lower_write)) o += std::format("{} = vshadow({}); ", shadow(lower_write), raw(lower_write));
        if (u.target >= 0 && u.target != lower_write && shadowed.count(u.target)) {
            if (!u.normalized) o += std::format("{} = vshadow({}); ", shadow(u.target), raw(u.target));
            else if (u.mask == 0xF) o += std::format("{} = uv; ", shadow(u.target));
            else o += std::format("{0} = vsel({0}, uv, {1}); ", shadow(u.target), lane_mask(u.mask));
        }
        if (hi & 0x40000000u) o += "ending = true; ";
        o += "}\n";
        const auto edge = [&](std::uint32_t to) -> std::string {
            const auto *e = plan.edge(pc, to);
            if (!e) return {};
            if (plan.tracked[pc >> 3]) return std::format("st = ke_{:04X}_{:04X}[st]; ", pc, to);
            return std::format("st = {}u; ", e->next[0]);
        };
        const std::uint32_t prev = (pc - 8u) & kMask;
        if (in.count(prev)) {
            const Lower &pl = lowers[prev];
            const std::uint32_t phi = word(p.code, prev + 4);
            if (phi & 0x40000000u) {
                const std::string after = std::format("{}u", (pc + 8u) & kMask);
                const std::string tpc = pl.branch ? std::format("pend ? {}u : {}", pl.target & kMask, after) : pl.jump ? "pendj ? jpc : " + after : after;
                o += std::format("    if (ending) {{ {}r.tpc = {}; return; }}\n", leave(pc), tpc);
            }
            if (pl.branch) o += std::format("    if (pend) {{ pend = false; {}{}goto {}; }}\n", meet(pc, pl.target & kMask), edge(pl.target & kMask), label(pl.target));
            if (pl.jump) o += std::format("    if (pendj) {{ pendj = false; {}goto dispatch; }}\n", leave(pc));
        }
        const std::uint32_t next = (pc + 8u) & kMask;
        if (!in.count(next)) o += std::format("    {}vu::interpret(1, {}u);\n    return;\n", leave(pc), next);
        else if (const std::string e = meet(pc, next) + edge(next); !e.empty()) o += "    " + e + "\n";
    }
    o += "dispatch:\n    vu::interpret(1, jpc);\n    return;\n}\n\n";
    if (late_flags) {
        // every way out: the end of the program, a hop to the interpreter (an unknown jump, an unsupported instruction, code
        // outside the program) leaves the MAC and status in the registers
        replace_all(o, "vu::interpret(", "fl.finish(r); vu::interpret(");
        replace_all(o, "r.tpc = ", "fl.finish(r); r.tpc = ");
    }
    if (any_q) {
        replace_all(o, "vu::interpret(", "fl.flush_q(r); vu::interpret(");
        replace_all(o, "r.tpc = ", "fl.flush_q(r); r.tpc = ");
    }
    if (p.unit == 0) {
        replace_all(o, "qword(mem,", "qword<255u>(mem,");
        replace_all(o, "vu::kick(1,", "vu::kick(0,");
        replace_all(o, "vu::interpret(1,", "vu::interpret(0,");
    }
    // Keep VI state in the register file for VU0 early-stop/budget exits.
    if (p.unit == 0) return o;
    if (!lazy) {
        // the clean-up at once (see `worth` above): every operand and every result goes through the whole vfix
        for (const auto &[from, to] : std::initializer_list<std::pair<const char *, const char *>>{
                 {"vaddd(", "vaddn("}, {"vsubd(", "vsubn("}, {"vmuld(", "vmuln("}, {"vmaddd(", "vmaddn("}, {"vmsubd(", "vmsubn("}, {"vshadow(", "vshadow_eager("}, {"vqd(", "vqd_eager("}})
            replace_all(o, from, to);
        return local_vi(o);
    }
    o = local_vi(o);
    replace_all(o, "vu::interpret(1, ", "vhop(hop, ");
    if (any_arith) {
        replace_all(o, "vu::kick(1, ", "vkick(daz.csr, 1, ");
        replace_all(o, "efu(r, ", "vefu(daz.csr, r, ");
    }
    o.erase(o.size() - 1);  // one empty line after the two functions, not between them
    o += std::format("void {0}(Vu0Regs &r, std::uint8_t *mem) {{\n    std::uint32_t hop = kNoHop;\n    {1}\n    if (hop != kNoHop) vu::interpret(1, hop);\n}}\n\n", name,
                     any_arith ? std::format("{{ VuDaz daz; {}_b(r, mem, daz, hop); }}", name) : std::format("{}_b(r, mem, hop);", name));
    return o;
}

}

namespace ps2::vurecomp {

// Bump this when the generated code changes in a way the headers do not show (a change in this file): libraries made earlier
// are then left alone and built again (vu_live.cpp).
const char *const kGeneratorVersion = "vurecomp 11";

Result generate_files(std::vector<Input> inputs, const fs::path &out_dir, std::uint64_t live_stamp, std::size_t per_file, bool with_targets) {
    std::vector<Program> programs;
    std::set<std::pair<int, std::uint64_t>> recorded_keys;
    for (Input &in : inputs) {
        Program p{in.key, in.entry, std::move(in.code), in.unit};
        const std::uint32_t kMask = p.unit == 0 ? 4095u : 16383u;
        if ((p.unit != 0 && p.unit != 1) || p.entry > kMask || (p.entry & 7u) || p.code.size() != kMask + 1u || (live_stamp && p.unit == 0)) {
            std::fprintf(stderr, "vu%d %016llX at %04X: invalid input or unsupported VU0 live-library request, skipped\n", p.unit, static_cast<unsigned long long>(p.key), p.entry);
            continue;
        }
        const auto pcs = ps2::vu::reachable(p.code.data(), kMask, p.entry);
        if (ps2::vu::program_key(p.code.data(), pcs, p.entry) != p.key) {
            std::fprintf(stderr, "%016llX_%04X: key mismatch, skipped\n", static_cast<unsigned long long>(p.key), p.entry);
            continue;
        }
        if (ps2::vu::branch_in_delay_slot(p.code.data(), kMask, p.entry)) continue;
        if (recorded_keys.emplace(p.unit, p.key).second) programs.push_back(std::move(p));
    }
    std::set<std::pair<int, std::uint64_t>> keys;
    for (const Program &p : programs) keys.emplace(p.unit, p.key);
    const std::size_t recorded = programs.size();
    for (std::size_t i = 0; i < (with_targets ? recorded : 0); ++i) {
        if (programs[i].unit == 0) continue;  // VU0 entries are called by the EE and recorded individually.
        const std::uint32_t kMask = 16383u;
        std::set<std::uint32_t> targets;
        for (std::uint32_t pc : ps2::vu::reachable(programs[i].code.data(), kMask, programs[i].entry)) {
            const std::uint32_t lo = word(programs[i].code, pc), hi = word(programs[i].code, pc + 4);
            if (hi & 0x80000000u) continue;
            const std::uint32_t op = lo >> 25;
            if (op == 0x21 || op == 0x25) targets.insert((pc + 16u) & kMask);
            if (op == 0x08 && ((lo >> 11) & 15u) == 0) targets.insert(((((lo >> 10) & 0x7800u) | (lo & 0x7FFu)) * 8u) & kMask);
        }
        for (std::uint32_t target : targets) {
            const std::uint64_t key = ps2::vu::program_key(programs[i].code.data(), ps2::vu::reachable(programs[i].code.data(), kMask, target), target);
            if (ps2::vu::branch_in_delay_slot(programs[i].code.data(), kMask, target)) continue;
            if (keys.emplace(programs[i].unit, key).second) programs.push_back(Program{key, target, programs[i].code, programs[i].unit});
        }
    }
    std::sort(programs.begin(), programs.end(), [](const Program &a, const Program &b) { return a.key < b.key; });
    fs::create_directories(out_dir);
    for (const auto &e : fs::directory_iterator(out_dir))
        if (ps2::os::utf8(e.path().filename()).rfind("vu1_", 0) == 0) fs::remove(e.path());
    std::vector<std::string> names, bodies;
    for (std::size_t i = 0; i < programs.size(); ++i) {
        std::string name, body = generate(programs[i], name);
        if (name.empty()) {
            std::fprintf(stderr, "vu%d %016llX at %04X: unsupported control flow or pipeline plan; using interpreter\n", programs[i].unit, static_cast<unsigned long long>(programs[i].key), programs[i].entry);
            continue;
        }
        if (names.size() != i) programs[names.size()] = std::move(programs[i]);
        names.push_back(std::move(name));
        bodies.push_back(std::move(body));
    }
    Result result;
    result.skipped = programs.size() - names.size();
    result.jump_targets = programs.size() - recorded;
    programs.resize(names.size());
    for (std::size_t first = 0; first < programs.size(); first += per_file) {
        std::string text = "#include \"ps2/vu_ops.hpp\"\n#include <cstdio>\n\nnamespace gen {\n\nusing namespace ps2;\nusing namespace ps2::vuops;\n\n";
        for (std::size_t i = first; i < std::min(programs.size(), first + per_file); ++i) text += bodies[i];
        text += "}\n";
        std::ofstream(out_dir / std::format("vu1_{:03}.cpp", first / per_file), std::ios::binary) << text;
    }
    std::string table = "#include \"ps2/vu_ops.hpp\"\n\nnamespace gen {\n";
    for (const auto &n : names) table += std::format("void {}(ps2::Vu0Regs &r, std::uint8_t *mem);\n", n);
    table += "\nconst ps2::vu::Program kVuPrograms[] = {\n";
    for (std::size_t i = 0; i < programs.size(); ++i) table += std::format("    {{0x{:016X}ull, {}, {}}},\n", programs[i].key, names[i], programs[i].unit);
    if (programs.empty()) table += "    {0, nullptr},\n";
    table += "};\n\n}\n\n";
    if (live_stamp == 0) {
        table += std::format("void register_generated_vu() {{ ps2::vu::register_programs(gen::kVuPrograms, {}u); }}\n", programs.size());
    } else {
        // A loadable library. The runner is not linked to it, so the few functions the generated code calls are reached through
        // the table the runner hands over (ps2::vu::LiveHost); nothing but the two functions below is visible from outside.
        table += std::format(R"(#if defined(_WIN32)
#define PS2_LIVE_EXPORT extern "C" __declspec(dllexport)
#else
#define PS2_LIVE_EXPORT extern "C" __attribute__((visibility("default")))
#endif

namespace {{
ps2::vu::LiveHost g_host;
}}

namespace ps2::vu {{
void interpret(int unit, std::uint32_t pc) {{ g_host.interpret(unit, pc); }}
void kick(int unit, std::uint32_t address) {{ g_host.kick(unit, address); }}
std::uint32_t top() {{ return g_host.top(); }}
std::uint32_t itop() {{ return g_host.itop(); }}
}}

PS2_LIVE_EXPORT std::uint64_t ps2_vu_live_stamp() {{ return 0x{:016X}ull; }}

PS2_LIVE_EXPORT const ps2::vu::Program *ps2_vu_live_init(const ps2::vu::LiveHost *host, std::uint32_t *count) {{
    g_host = *host;
    *count = {}u;
    return gen::kVuPrograms;
}}
)", live_stamp, programs.size());
    }
    std::ofstream(out_dir / "vu1_table.cpp", std::ios::binary) << table;
    result.programs = programs.size();
    for (const Program &p : programs) result.keys.push_back(p.key);
    return result;
}

}  // namespace ps2::vurecomp
