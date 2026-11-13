#pragma once

#include "cpu.hpp"
#include "fpu.hpp"

#include <cstdint>

namespace ps2::vu {

inline bool lane(unsigned dest, int i) { return dest & (8u >> i); }

template <typename F>
inline void apply(Vu0Regs &v, unsigned dest, unsigned fd, F f) {
    Reg128 r = v.vf[fd];
    for (int i = 0; i < 4; ++i)
        if (lane(dest, i)) r.w[i] = f(i);
    if (fd) v.vf[fd] = r;
}

inline void macro_flags(Vu0Regs &v, const Reg128 &r, unsigned dest) {
    std::uint32_t mac = 0;
    for (int i = 0; i < 4; ++i) {
        if (!lane(dest, i)) continue;
        const std::uint32_t bit = 3u - static_cast<std::uint32_t>(i);
        const std::uint32_t x = r.w[i];
        if (!(x & 0x7F800000u)) mac |= 1u << bit;
        if (x & 0x80000000u) mac |= 1u << (bit + 4);
        if ((x & 0x7FFFFFFFu) == 0x7FFFFFFFu) mac |= 1u << (bit + 12);
    }
    v.mac = mac;
    std::uint32_t status = v.status & 0xFF0u;
    if (mac & 0x000Fu) status |= 0x1u;
    if (mac & 0x00F0u) status |= 0x2u;
    if (mac & 0xF000u) status |= 0x8u;
    status |= (status & 0xFu) << 6;
    v.status = (status & ~0x30u) | (v.status & 0x30u);
}

template <typename F>
inline void arith(Vu0Regs &v, unsigned dest, unsigned fd, F f) {
    Reg128 r = v.vf[fd];
    for (int i = 0; i < 4; ++i)
        if (lane(dest, i)) r.w[i] = f(i);
    macro_flags(v, r, dest);
    if (fd) v.vf[fd] = r;
}

template <typename F>
inline void arith_acc(Vu0Regs &v, unsigned dest, F f) {
    Reg128 r = v.acc;
    for (int i = 0; i < 4; ++i)
        if (lane(dest, i)) r.w[i] = f(i);
    macro_flags(v, r, dest);
    v.acc = r;
}

inline void add(Vu0Regs &v, unsigned dest, unsigned fd, unsigned fs, unsigned ft) {
    arith(v, dest, fd, [&](int i) { return fp::add(v.vf[fs].w[i], v.vf[ft].w[i]); });
}
inline void addbc(Vu0Regs &v, unsigned dest, unsigned fd, unsigned fs, std::uint32_t t) {
    arith(v, dest, fd, [&](int i) { return fp::add(v.vf[fs].w[i], t); });
}
inline void sub(Vu0Regs &v, unsigned dest, unsigned fd, unsigned fs, unsigned ft) {
    arith(v, dest, fd, [&](int i) { return fp::sub(v.vf[fs].w[i], v.vf[ft].w[i]); });
}
inline void subbc(Vu0Regs &v, unsigned dest, unsigned fd, unsigned fs, std::uint32_t t) {
    arith(v, dest, fd, [&](int i) { return fp::sub(v.vf[fs].w[i], t); });
}
inline void mul(Vu0Regs &v, unsigned dest, unsigned fd, unsigned fs, unsigned ft) {
    arith(v, dest, fd, [&](int i) { return fp::mul(v.vf[fs].w[i], v.vf[ft].w[i]); });
}
inline void mulbc(Vu0Regs &v, unsigned dest, unsigned fd, unsigned fs, std::uint32_t t) {
    arith(v, dest, fd, [&](int i) { return fp::mul(v.vf[fs].w[i], t); });
}
inline void madd(Vu0Regs &v, unsigned dest, unsigned fd, unsigned fs, unsigned ft) {
    arith(v, dest, fd, [&](int i) { return fp::add(v.acc.w[i], fp::mul(v.vf[fs].w[i], v.vf[ft].w[i])); });
}
inline void maddbc(Vu0Regs &v, unsigned dest, unsigned fd, unsigned fs, std::uint32_t t) {
    arith(v, dest, fd, [&](int i) { return fp::add(v.acc.w[i], fp::mul(v.vf[fs].w[i], t)); });
}
inline void msub(Vu0Regs &v, unsigned dest, unsigned fd, unsigned fs, unsigned ft) {
    arith(v, dest, fd, [&](int i) { return fp::sub(v.acc.w[i], fp::mul(v.vf[fs].w[i], v.vf[ft].w[i])); });
}
inline void msubbc(Vu0Regs &v, unsigned dest, unsigned fd, unsigned fs, std::uint32_t t) {
    arith(v, dest, fd, [&](int i) { return fp::sub(v.acc.w[i], fp::mul(v.vf[fs].w[i], t)); });
}
inline void max(Vu0Regs &v, unsigned dest, unsigned fd, unsigned fs, unsigned ft) {
    apply(v, dest, fd, [&](int i) { return fp::vu_max(v.vf[fs].w[i], v.vf[ft].w[i]); });
}
inline void maxbc(Vu0Regs &v, unsigned dest, unsigned fd, unsigned fs, std::uint32_t t) {
    apply(v, dest, fd, [&](int i) { return fp::vu_max(v.vf[fs].w[i], t); });
}
inline void mini(Vu0Regs &v, unsigned dest, unsigned fd, unsigned fs, unsigned ft) {
    apply(v, dest, fd, [&](int i) { return fp::vu_min(v.vf[fs].w[i], v.vf[ft].w[i]); });
}
inline void minibc(Vu0Regs &v, unsigned dest, unsigned fd, unsigned fs, std::uint32_t t) {
    apply(v, dest, fd, [&](int i) { return fp::vu_min(v.vf[fs].w[i], t); });
}

inline void adda(Vu0Regs &v, unsigned dest, unsigned fs, unsigned ft) {
    arith_acc(v, dest, [&](int i) { return fp::add(v.vf[fs].w[i], v.vf[ft].w[i]); });
}
inline void addabc(Vu0Regs &v, unsigned dest, unsigned fs, std::uint32_t t) {
    arith_acc(v, dest, [&](int i) { return fp::add(v.vf[fs].w[i], t); });
}
inline void suba(Vu0Regs &v, unsigned dest, unsigned fs, unsigned ft) {
    arith_acc(v, dest, [&](int i) { return fp::sub(v.vf[fs].w[i], v.vf[ft].w[i]); });
}
inline void subabc(Vu0Regs &v, unsigned dest, unsigned fs, std::uint32_t t) {
    arith_acc(v, dest, [&](int i) { return fp::sub(v.vf[fs].w[i], t); });
}
inline void mula(Vu0Regs &v, unsigned dest, unsigned fs, unsigned ft) {
    arith_acc(v, dest, [&](int i) { return fp::mul(v.vf[fs].w[i], v.vf[ft].w[i]); });
}
inline void mulabc(Vu0Regs &v, unsigned dest, unsigned fs, std::uint32_t t) {
    arith_acc(v, dest, [&](int i) { return fp::mul(v.vf[fs].w[i], t); });
}
inline void madda(Vu0Regs &v, unsigned dest, unsigned fs, unsigned ft) {
    arith_acc(v, dest, [&](int i) { return fp::add(v.acc.w[i], fp::mul(v.vf[fs].w[i], v.vf[ft].w[i])); });
}
inline void maddabc(Vu0Regs &v, unsigned dest, unsigned fs, std::uint32_t t) {
    arith_acc(v, dest, [&](int i) { return fp::add(v.acc.w[i], fp::mul(v.vf[fs].w[i], t)); });
}
inline void msuba(Vu0Regs &v, unsigned dest, unsigned fs, unsigned ft) {
    arith_acc(v, dest, [&](int i) { return fp::sub(v.acc.w[i], fp::mul(v.vf[fs].w[i], v.vf[ft].w[i])); });
}
inline void msubabc(Vu0Regs &v, unsigned dest, unsigned fs, std::uint32_t t) {
    arith_acc(v, dest, [&](int i) { return fp::sub(v.acc.w[i], fp::mul(v.vf[fs].w[i], t)); });
}

inline void opmula(Vu0Regs &v, unsigned fs, unsigned ft) {
    const Reg128 s = v.vf[fs], t = v.vf[ft];
    v.acc.w[0] = fp::mul(s.w[1], t.w[2]);
    v.acc.w[1] = fp::mul(s.w[2], t.w[0]);
    v.acc.w[2] = fp::mul(s.w[0], t.w[1]);
    macro_flags(v, v.acc, 14u);
}

inline void opmsub(Vu0Regs &v, unsigned fd, unsigned fs, unsigned ft) {
    const Reg128 s = v.vf[fs], t = v.vf[ft];
    Reg128 r = v.vf[fd];
    r.w[0] = fp::sub(v.acc.w[0], fp::mul(s.w[1], t.w[2]));
    r.w[1] = fp::sub(v.acc.w[1], fp::mul(s.w[2], t.w[0]));
    r.w[2] = fp::sub(v.acc.w[2], fp::mul(s.w[0], t.w[1]));
    macro_flags(v, r, 14u);
    if (fd) v.vf[fd] = r;
}

inline void abs(Vu0Regs &v, unsigned dest, unsigned ft, unsigned fs) {
    apply(v, dest, ft, [&](int i) { return v.vf[fs].w[i] & fp::kAbs; });
}
inline void move(Vu0Regs &v, unsigned dest, unsigned ft, unsigned fs) {
    apply(v, dest, ft, [&](int i) { return v.vf[fs].w[i]; });
}
inline void mr32(Vu0Regs &v, unsigned dest, unsigned ft, unsigned fs) {
    const Reg128 s = v.vf[fs];
    apply(v, dest, ft, [&](int i) { return s.w[(i + 1) & 3]; });
}
inline void ftoi(Vu0Regs &v, unsigned dest, unsigned ft, unsigned fs, int fraction) {
    apply(v, dest, ft, [&](int i) { return fp::to_fixed(v.vf[fs].w[i], fraction); });
}
inline void itof(Vu0Regs &v, unsigned dest, unsigned ft, unsigned fs, int fraction) {
    apply(v, dest, ft, [&](int i) { return fp::from_fixed(v.vf[fs].sw[i], fraction); });
}

inline void clip(Vu0Regs &v, unsigned fs, unsigned ft) {
    const std::int32_t w = fp::key(v.vf[ft].w[3] & fp::kAbs);
    std::uint32_t flags = 0;
    for (int i = 0; i < 3; ++i) {
        const std::int32_t x = fp::key(v.vf[fs].w[i]);
        if (x > w) flags |= 1u << (i * 2);
        if (x < -w) flags |= 2u << (i * 2);
    }
    v.clip = ((v.clip << 6) | flags) & 0xFFFFFFu;
}

inline void div(Vu0Regs &v, std::uint32_t s, std::uint32_t t) {
    const fp::Result r = fp::div(s, t);
    v.q = r.value;
    v.status = (v.status & ~0x30u) | (r.invalid ? 0x20u : 0u);
}
inline void sqrt(Vu0Regs &v, std::uint32_t t) { v.q = fp::sqrt(t); }
inline void rsqrt(Vu0Regs &v, std::uint32_t s, std::uint32_t t) {
    const fp::Result r = fp::rsqrt(s, t);
    v.q = r.value;
    v.status = (v.status & ~0x30u) | (r.invalid ? 0x20u : 0u);
}

inline void mfir(Vu0Regs &v, unsigned dest, unsigned ft, unsigned is) {
    const std::uint32_t value = static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int16_t>(v.vi[is])));
    apply(v, dest, ft, [&](int) { return value; });
}

inline void set_vi(Vu0Regs &v, unsigned index, std::uint32_t value) {
    if (index) v.vi[index] = static_cast<std::uint16_t>(value);
}

inline std::uint32_t rand_next(std::uint32_t r) {
    const std::uint32_t bit = ((r >> 4) ^ (r >> 22)) & 1u;
    return ((r << 1) | bit) & 0x7FFFFFu;
}

// ---------------------------------------------------------------------------------------------------------------------
// The same instructions on four lanes at once, for the code the recompiler writes (recomp/codegen.cpp). Every function
// named <old name>4 leaves the registers, the flags and the sticky bits exactly as the helper above of the old name
// does; the helpers above stay, because the interpreter and code translated earlier call them.
//
// How the numbers are made: fp::add and fp::mul are the console's arithmetic done bit by bit. Here the PC's own float
// unit does the work and the difference is put right afterwards:
//   - a multiply is done in double precision, where the product of two floats is exact, and cut to 24 bits;
//   - an add first cuts the smaller operand at the guard bit of the larger one (the console keeps one bit below the
//     last place and drops the rest before it adds, which is what makes its subtractions differ from a PC's), adds,
//     and takes one step back when the PC rounded the sum away from zero (the rest of the sum tells: it is exact);
//   - denormals go in and come out as zeros with their sign.
// A PC float has no numbers with an exponent of 255, which are ordinary numbers on the console. A lane that meets one
// (as an operand, as a result, or the one product that cannot be told from an underflow) is marked, and an instruction
// with a marked lane it writes is handed to the old helper as a whole. So the fast path never sees an overflow, and
// its flags are only "zero" and "sign".
//
// The template argument says whether the MAC register and the low four status bits are wanted. The recompiler passes
// 0 when the next thing that touches them, on the straight line after the instruction, is another instruction that
// sets them (recomp::vu0_flags_dead). The sticky status bits are kept either way.
#if defined(__clang__) || defined(__GNUC__)
#define PS2_VU4 inline __attribute__((always_inline))
#else
#define PS2_VU4 inline
#endif
// The way out to the old helper (a lane with an exponent of 255, or the one product on the edge of underflow) is rare
// and stays a call, so the code around an instruction holds the fast path only.
#if defined(__clang__) || defined(__GNUC__)
#define PS2_VU4_COLD __attribute__((noinline, cold))
#else
#define PS2_VU4_COLD
#endif

namespace v4 {

using f64x4 = double __attribute__((vector_size(32)));
using u64x4 = std::uint64_t __attribute__((vector_size(32)));

// Lane x is bit 0 of tops() and bit 3 of a flag nibble and of `dest`, hence the table that turns four bits around.
inline constexpr std::uint8_t kRev[16] = {0, 8, 4, 12, 2, 10, 6, 14, 1, 9, 5, 13, 3, 11, 7, 15};
#define PS2_VU4_ROW(d) {(d) & 8u ? ~0u : 0u, (d) & 4u ? ~0u : 0u, (d) & 2u ? ~0u : 0u, (d) & 1u ? ~0u : 0u}
alignas(16) inline constexpr std::uint32_t kLanes[16][4] = {
    PS2_VU4_ROW(0u), PS2_VU4_ROW(1u), PS2_VU4_ROW(2u), PS2_VU4_ROW(3u), PS2_VU4_ROW(4u), PS2_VU4_ROW(5u), PS2_VU4_ROW(6u), PS2_VU4_ROW(7u),
    PS2_VU4_ROW(8u), PS2_VU4_ROW(9u), PS2_VU4_ROW(10u), PS2_VU4_ROW(11u), PS2_VU4_ROW(12u), PS2_VU4_ROW(13u), PS2_VU4_ROW(14u), PS2_VU4_ROW(15u),
};
#undef PS2_VU4_ROW

PS2_VU4 u32x4 splat(std::uint32_t x) { return u32x4{x, x, x, x}; }
PS2_VU4 u32x4 lanes(unsigned dest) {
    u32x4 m;
    __builtin_memcpy(&m, kLanes[dest & 15u], 16);
    return m;
}

// The top bit of every lane, x in bit 0.
PS2_VU4 unsigned tops(u32x4 m) {
#if defined(__SSE2__) && (defined(__x86_64__) || defined(__i386__))
    return static_cast<unsigned>(__builtin_ia32_movmskps((f32x4)m));
#else
    return __builtin_reduce_or((m >> 31) << u32x4{0u, 1u, 2u, 3u});
#endif
}

// fp::flush: a denormal is a zero with its sign.
PS2_VU4 u32x4 flush(u32x4 x) { return x & ((u32x4)((x & 0x7F800000u) != 0u) | 0x80000000u); }

// fp::add of two operands that went through flush().
PS2_VU4 u32x4 add_flushed(u32x4 a, u32x4 b, u32x4 &bad) {
    const u32x4 swap = (u32x4)((s32x4)(b & 0x7FFFFFFFu) > (s32x4)(a & 0x7FFFFFFFu));
    const u32x4 x = (a ^ b) & swap;
    const u32x4 big = a ^ x;
    u32x4 small = b ^ x;
    // fp::add shifts the smaller significand right by the difference of the exponents and keeps one bit below the last
    // place of the larger: of `small` the bits from 2^(difference - 1) up stay, and nothing from a difference of 25.
    const u32x4 diff = ((big >> 23) - (small >> 23)) & 0xFFu;
#if defined(__AVX2__)
    // With AVX2 every lane shifts by its own count, and a count of 32 or more gives 0: ~0 << diff is -(2^diff), the same
    // mask as below without its way through a float.
    const s32x4 cut = (s32x4)__builtin_ia32_psllv4si((s32x4)splat(~0u), (s32x4)diff);
    small &= (u32x4)(cut >> 1) & (u32x4)((s32x4)diff < 25);
#else
    const s32x4 d = (s32x4)__builtin_elementwise_min((s16x8)diff, (s16x8)splat(25u));
    const u32x4 pow2 = (u32x4)__builtin_convertvector((f32x4)(((u32x4)d + 127u) << 23), s32x4);
    small &= (u32x4)((s32x4)(0u - pow2) >> 1) & (u32x4)(d < 25);
#endif
    // What is left adds without losing anything but the PC's own rounding. `rest` is that sum's error, exact because
    // `big` is the larger operand: when it is not zero and points the other way than the sum, the sum was rounded away
    // from zero and the float one step below is the cut-off one.
    const f32x4 fb = (f32x4)big, fs = (f32x4)small;
    const f32x4 sum = fb + fs;
    const f32x4 rest = fs - (sum - fb);
    const u32x4 away = (u32x4)((s32x4)((u32x4)rest ^ (u32x4)sum) >> 31) & (u32x4)(rest != 0.0f);
    bad |= (u32x4)(((u32x4)sum & 0x7F800000u) == 0x7F800000u);
    return flush((u32x4)sum + away);
}

// fp::mul of two operands that went through flush(). The product of two floats is exact as a double; without its low
// 29 bits it is a float. A result of exactly 2^-126 is marked: a product just below it rounds up to it on the way back.
PS2_VU4 u32x4 mul_flushed(u32x4 a, u32x4 b, u32x4 &bad) {
    const f64x4 p = __builtin_convertvector((f32x4)a, f64x4) * __builtin_convertvector((f32x4)b, f64x4);
    const u32x4 r = (u32x4)__builtin_convertvector((f64x4)((u64x4)p & static_cast<std::uint64_t>(0xFFFFFFFFE0000000ull)), f32x4);
    const u32x4 m = r & 0x7FFFFFFFu;
    bad |= (u32x4)((s32x4)m > 0x7F7FFFFF) | (u32x4)(m == 0x00800000u);
    return flush(r);
}

PS2_VU4 u32x4 add(u32x4 a, u32x4 b, u32x4 &bad) { return add_flushed(flush(a), flush(b), bad); }
PS2_VU4 u32x4 sub(u32x4 a, u32x4 b, u32x4 &bad) { return add_flushed(flush(a), flush(b) ^ 0x80000000u, bad); }
PS2_VU4 u32x4 mul(u32x4 a, u32x4 b, u32x4 &bad) { return mul_flushed(flush(a), flush(b), bad); }
PS2_VU4 u32x4 madd(u32x4 acc, u32x4 a, u32x4 b, u32x4 &bad) {
    const u32x4 product = mul(a, b, bad);
    return add_flushed(flush(acc), product, bad);
}
PS2_VU4 u32x4 msub(u32x4 acc, u32x4 a, u32x4 b, u32x4 &bad) {
    const u32x4 product = mul(a, b, bad);
    return add_flushed(flush(acc), product ^ 0x80000000u, bad);
}

// fp::order_key, fp::vu_max and fp::vu_min.
PS2_VU4 s32x4 order(u32x4 x) {
    const s32x4 negative = (s32x4)x >> 31;
    return ((s32x4)(x & 0x7FFFFFFFu) ^ negative) - negative;
}
PS2_VU4 u32x4 max(u32x4 a, u32x4 b) {
    const u32x4 second = (u32x4)(order(a) < order(b));
    return (b & second) | (a & ~second);
}
PS2_VU4 u32x4 min(u32x4 a, u32x4 b) {
    const u32x4 second = (u32x4)(order(a) > order(b));
    return (b & second) | (a & ~second);
}

// macro_flags for a result of the fast path (zeros and ordinary numbers only, so no overflow bit). With F false only
// the sticky bits are added: the instruction that follows sets the MAC register and the low status bits anew.
template <bool F>
PS2_VU4 void flags(Vu0Regs &v, u32x4 r, unsigned dest) {
    const unsigned d = kRev[dest & 15u];
    const unsigned z = tops((u32x4)((r & 0x7F800000u) == 0u)) & d, s = tops(r) & d;
    if (F) {
        v.mac = static_cast<std::uint32_t>(kRev[z]) | (static_cast<std::uint32_t>(kRev[s]) << 4);
        const std::uint32_t status = (v.status & 0xFF0u) | (z ? 1u : 0u) | (s ? 2u : 0u);
        v.status = status | ((status & 0xFu) << 6);
    } else {
        v.status |= (z ? 0x40u : 0u) | (s ? 0x80u : 0u);
    }
}

// The end of every flag-setting instruction: the lanes of `dest` go into `to` (when `store`), with their flags. A
// marked lane among them hands the instruction to the old helper instead, before anything was written.
template <bool F, typename Slow>
PS2_VU4 void put(Vu0Regs &v, unsigned dest, Reg128 &to, bool store, u32x4 r, u32x4 bad, Slow slow) {
    const u32x4 m = lanes(dest);
    if (__builtin_expect(tops(bad & m) != 0u, 0)) {
        slow();
        return;
    }
    r = (r & m) | (to.uw4 & ~m);
    flags<F>(v, r, dest);
    if (store) to.uw4 = r;
}

// apply(): the lanes of `dest` go into vf[fd], no flags.
PS2_VU4 void keep(Vu0Regs &v, unsigned dest, unsigned fd, u32x4 r) {
    const u32x4 m = lanes(dest);
    if (fd) v.vf[fd].uw4 = (r & m) | (v.vf[fd].uw4 & ~m);
}

}

#define PS2_VU4_FD(name, expr) \
    template <bool F> PS2_VU4 void name##4(Vu0Regs &v, unsigned dest, unsigned fd, unsigned fs, unsigned ft) { \
        u32x4 bad{}; \
        const u32x4 s = v.vf[fs].uw4, t = v.vf[ft].uw4; \
        const u32x4 r = expr; \
        v4::put<F>(v, dest, v.vf[fd], fd != 0u, r, bad, [&]() PS2_VU4_COLD { name(v, dest, fd, fs, ft); }); \
    } \
    template <bool F> PS2_VU4 void name##bc4(Vu0Regs &v, unsigned dest, unsigned fd, unsigned fs, std::uint32_t bc) { \
        u32x4 bad{}; \
        const u32x4 s = v.vf[fs].uw4, t = v4::splat(bc); \
        const u32x4 r = expr; \
        v4::put<F>(v, dest, v.vf[fd], fd != 0u, r, bad, [&]() PS2_VU4_COLD { name##bc(v, dest, fd, fs, bc); }); \
    }
#define PS2_VU4_ACC(name, expr) \
    template <bool F> PS2_VU4 void name##a4(Vu0Regs &v, unsigned dest, unsigned fs, unsigned ft) { \
        u32x4 bad{}; \
        const u32x4 s = v.vf[fs].uw4, t = v.vf[ft].uw4; \
        const u32x4 r = expr; \
        v4::put<F>(v, dest, v.acc, true, r, bad, [&]() PS2_VU4_COLD { name##a(v, dest, fs, ft); }); \
    } \
    template <bool F> PS2_VU4 void name##abc4(Vu0Regs &v, unsigned dest, unsigned fs, std::uint32_t bc) { \
        u32x4 bad{}; \
        const u32x4 s = v.vf[fs].uw4, t = v4::splat(bc); \
        const u32x4 r = expr; \
        v4::put<F>(v, dest, v.acc, true, r, bad, [&]() PS2_VU4_COLD { name##abc(v, dest, fs, bc); }); \
    }
PS2_VU4_FD(add, v4::add(s, t, bad))
PS2_VU4_FD(sub, v4::sub(s, t, bad))
PS2_VU4_FD(mul, v4::mul(s, t, bad))
PS2_VU4_FD(madd, v4::madd(v.acc.uw4, s, t, bad))
PS2_VU4_FD(msub, v4::msub(v.acc.uw4, s, t, bad))
PS2_VU4_ACC(add, v4::add(s, t, bad))
PS2_VU4_ACC(sub, v4::sub(s, t, bad))
PS2_VU4_ACC(mul, v4::mul(s, t, bad))
PS2_VU4_ACC(madd, v4::madd(v.acc.uw4, s, t, bad))
PS2_VU4_ACC(msub, v4::msub(v.acc.uw4, s, t, bad))
#undef PS2_VU4_FD
#undef PS2_VU4_ACC

template <bool F> PS2_VU4 void opmula4(Vu0Regs &v, unsigned fs, unsigned ft) {
    u32x4 bad{};
    const u32x4 s = v.vf[fs].uw4, t = v.vf[ft].uw4;
    const u32x4 r = v4::mul(__builtin_shufflevector(s, s, 1, 2, 0, 3), __builtin_shufflevector(t, t, 2, 0, 1, 3), bad);
    v4::put<F>(v, 14u, v.acc, true, r, bad, [&]() PS2_VU4_COLD { opmula(v, fs, ft); });
}
template <bool F> PS2_VU4 void opmsub4(Vu0Regs &v, unsigned fd, unsigned fs, unsigned ft) {
    u32x4 bad{};
    const u32x4 s = v.vf[fs].uw4, t = v.vf[ft].uw4;
    const u32x4 r = v4::msub(v.acc.uw4, __builtin_shufflevector(s, s, 1, 2, 0, 3), __builtin_shufflevector(t, t, 2, 0, 1, 3), bad);
    v4::put<F>(v, 14u, v.vf[fd], fd != 0u, r, bad, [&]() PS2_VU4_COLD { opmsub(v, fd, fs, ft); });
}

PS2_VU4 void max4(Vu0Regs &v, unsigned dest, unsigned fd, unsigned fs, unsigned ft) { v4::keep(v, dest, fd, v4::max(v.vf[fs].uw4, v.vf[ft].uw4)); }
PS2_VU4 void maxbc4(Vu0Regs &v, unsigned dest, unsigned fd, unsigned fs, std::uint32_t bc) { v4::keep(v, dest, fd, v4::max(v.vf[fs].uw4, v4::splat(bc))); }
PS2_VU4 void mini4(Vu0Regs &v, unsigned dest, unsigned fd, unsigned fs, unsigned ft) { v4::keep(v, dest, fd, v4::min(v.vf[fs].uw4, v.vf[ft].uw4)); }
PS2_VU4 void minibc4(Vu0Regs &v, unsigned dest, unsigned fd, unsigned fs, std::uint32_t bc) { v4::keep(v, dest, fd, v4::min(v.vf[fs].uw4, v4::splat(bc))); }
PS2_VU4 void abs4(Vu0Regs &v, unsigned dest, unsigned ft, unsigned fs) { v4::keep(v, dest, ft, v.vf[fs].uw4 & 0x7FFFFFFFu); }
PS2_VU4 void move4(Vu0Regs &v, unsigned dest, unsigned ft, unsigned fs) { v4::keep(v, dest, ft, v.vf[fs].uw4); }
PS2_VU4 void mr32_4(Vu0Regs &v, unsigned dest, unsigned ft, unsigned fs) {
    const u32x4 s = v.vf[fs].uw4;
    v4::keep(v, dest, ft, __builtin_shufflevector(s, s, 1, 2, 3, 0));
}
// ftoi(), fp::to_fixed on four lanes (no flags). A lane whose exponent plus `fraction` is 158 or more becomes the largest
// number of its sign; a zero, a denormal, or a value that is below one after the scaling becomes 0. Every other lane is
// the value times 2^fraction (exact: a power of two times a normal float, below 2^31) cut towards zero by the PC's own
// conversion, which is what to_fixed's shift of the significand does. The lanes of the first two kinds are zeroed before
// the conversion, so it never sees a number it cannot convert.
PS2_VU4 void ftoi4(Vu0Regs &v, unsigned dest, unsigned ft, unsigned fs, int fraction) {
    const u32x4 x = v.vf[fs].uw4;
    const s32x4 e = (s32x4)((x >> 23) & 0xFFu) + fraction;
    const u32x4 big = (u32x4)(e >= 158);
    const u32x4 none = (u32x4)(e <= 126) | (u32x4)((x & 0x7F800000u) == 0u);
    const f32x4 scaled = (f32x4)(x & ~(big | none)) * (f32x4)v4::splat((127u + static_cast<std::uint32_t>(fraction)) << 23);
    const u32x4 whole = (u32x4)__builtin_convertvector(scaled, s32x4);
    const u32x4 most = (x >> 31) + 0x7FFFFFFFu;
    v4::keep(v, dest, ft, (whole & ~big) | (most & big));
}

}
