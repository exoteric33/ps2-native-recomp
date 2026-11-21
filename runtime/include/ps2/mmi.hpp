#pragma once

#include "cpu.hpp"

#include <bit>
#include <cstdint>
#include <limits>

namespace ps2::mmi {

template <typename T>
inline T sat_add(T a, T b) {
    using W = std::conditional_t<std::is_signed_v<T>, std::int64_t, std::uint64_t>;
    const W r = static_cast<W>(a) + static_cast<W>(b);
    if (r > static_cast<W>(std::numeric_limits<T>::max())) return std::numeric_limits<T>::max();
    if constexpr (std::is_signed_v<T>)
        if (r < static_cast<W>(std::numeric_limits<T>::min())) return std::numeric_limits<T>::min();
    return static_cast<T>(r);
}

template <typename T>
inline T sat_sub(T a, T b) {
    if constexpr (std::is_signed_v<T>) {
        const std::int64_t r = static_cast<std::int64_t>(a) - static_cast<std::int64_t>(b);
        if (r > std::numeric_limits<T>::max()) return std::numeric_limits<T>::max();
        if (r < std::numeric_limits<T>::min()) return std::numeric_limits<T>::min();
        return static_cast<T>(r);
    } else {
        return a > b ? static_cast<T>(a - b) : T{0};
    }
}

#define PS2_LANES(name, field, count, expr) \
    inline void name(Reg128 &d, const Reg128 &s, const Reg128 &t) { \
        Reg128 r; \
        for (int i = 0; i < count; ++i) r.field[i] = (expr); \
        d = r; \
    }

PS2_LANES(paddw, w, 4, s.w[i] + t.w[i])
PS2_LANES(psubw, w, 4, s.w[i] - t.w[i])
PS2_LANES(paddh, h, 8, static_cast<std::uint16_t>(s.h[i] + t.h[i]))
PS2_LANES(psubh, h, 8, static_cast<std::uint16_t>(s.h[i] - t.h[i]))
PS2_LANES(paddb, b, 16, static_cast<std::uint8_t>(s.b[i] + t.b[i]))
PS2_LANES(psubb, b, 16, static_cast<std::uint8_t>(s.b[i] - t.b[i]))
PS2_LANES(paddsw, sw, 4, sat_add(s.sw[i], t.sw[i]))
PS2_LANES(psubsw, sw, 4, sat_sub(s.sw[i], t.sw[i]))
PS2_LANES(paddsh, sh, 8, sat_add(s.sh[i], t.sh[i]))
PS2_LANES(psubsh, sh, 8, sat_sub(s.sh[i], t.sh[i]))
PS2_LANES(paddsb, sb, 16, sat_add(s.sb[i], t.sb[i]))
PS2_LANES(psubsb, sb, 16, sat_sub(s.sb[i], t.sb[i]))
PS2_LANES(padduw, w, 4, sat_add(s.w[i], t.w[i]))
PS2_LANES(psubuw, w, 4, sat_sub(s.w[i], t.w[i]))
PS2_LANES(padduh, h, 8, sat_add(s.h[i], t.h[i]))
PS2_LANES(psubuh, h, 8, sat_sub(s.h[i], t.h[i]))
PS2_LANES(paddub, b, 16, sat_add(s.b[i], t.b[i]))
PS2_LANES(psubub, b, 16, sat_sub(s.b[i], t.b[i]))
PS2_LANES(pcgtw, w, 4, s.sw[i] > t.sw[i] ? 0xFFFFFFFFu : 0u)
PS2_LANES(pcgth, h, 8, static_cast<std::uint16_t>(s.sh[i] > t.sh[i] ? 0xFFFFu : 0u))
PS2_LANES(pcgtb, b, 16, static_cast<std::uint8_t>(s.sb[i] > t.sb[i] ? 0xFFu : 0u))
PS2_LANES(pceqw, w, 4, s.w[i] == t.w[i] ? 0xFFFFFFFFu : 0u)
PS2_LANES(pceqh, h, 8, static_cast<std::uint16_t>(s.h[i] == t.h[i] ? 0xFFFFu : 0u))
PS2_LANES(pceqb, b, 16, static_cast<std::uint8_t>(s.b[i] == t.b[i] ? 0xFFu : 0u))
PS2_LANES(pmaxw, sw, 4, s.sw[i] > t.sw[i] ? s.sw[i] : t.sw[i])
PS2_LANES(pminw, sw, 4, s.sw[i] < t.sw[i] ? s.sw[i] : t.sw[i])
PS2_LANES(pmaxh, sh, 8, s.sh[i] > t.sh[i] ? s.sh[i] : t.sh[i])
PS2_LANES(pminh, sh, 8, s.sh[i] < t.sh[i] ? s.sh[i] : t.sh[i])
PS2_LANES(padsbh, h, 8, static_cast<std::uint16_t>(i < 4 ? s.h[i] - t.h[i] : s.h[i] + t.h[i]))
PS2_LANES(pand, d, 2, s.d[i] & t.d[i])
PS2_LANES(por, d, 2, s.d[i] | t.d[i])
PS2_LANES(pxor, d, 2, s.d[i] ^ t.d[i])
PS2_LANES(pnor, d, 2, ~(s.d[i] | t.d[i]))
PS2_LANES(pextlw, w, 4, (i & 1) ? s.w[i >> 1] : t.w[i >> 1])
PS2_LANES(pextuw, w, 4, (i & 1) ? s.w[2 + (i >> 1)] : t.w[2 + (i >> 1)])
PS2_LANES(pextlh, h, 8, (i & 1) ? s.h[i >> 1] : t.h[i >> 1])
PS2_LANES(pextuh, h, 8, (i & 1) ? s.h[4 + (i >> 1)] : t.h[4 + (i >> 1)])
PS2_LANES(pextlb, b, 16, (i & 1) ? s.b[i >> 1] : t.b[i >> 1])
PS2_LANES(pextub, b, 16, (i & 1) ? s.b[8 + (i >> 1)] : t.b[8 + (i >> 1)])
PS2_LANES(ppacw, w, 4, i < 2 ? t.w[i * 2] : s.w[(i - 2) * 2])
PS2_LANES(ppach, h, 8, i < 4 ? t.h[i * 2] : s.h[(i - 4) * 2])
PS2_LANES(ppacb, b, 16, i < 8 ? t.b[i * 2] : s.b[(i - 8) * 2])
PS2_LANES(pinth, h, 8, (i & 1) ? s.h[4 + (i >> 1)] : t.h[i >> 1])
PS2_LANES(pinteh, h, 8, (i & 1) ? s.h[i & ~1] : t.h[i & ~1])
PS2_LANES(pcpyld, d, 2, i ? s.d[0] : t.d[0])
PS2_LANES(pcpyud, d, 2, i ? t.d[1] : s.d[1])

#undef PS2_LANES

#define PS2_UNARY(name, field, count, expr) \
    inline void name(Reg128 &d, const Reg128 &t) { \
        Reg128 r; \
        for (int i = 0; i < count; ++i) r.field[i] = (expr); \
        d = r; \
    }

PS2_UNARY(pabsw, sw, 4, t.sw[i] == INT32_MIN ? INT32_MAX : (t.sw[i] < 0 ? -t.sw[i] : t.sw[i]))
PS2_UNARY(pabsh, sh, 8, static_cast<std::int16_t>(t.sh[i] == INT16_MIN ? INT16_MAX : (t.sh[i] < 0 ? -t.sh[i] : t.sh[i])))
PS2_UNARY(pcpyh, h, 8, t.h[i & 4])
PS2_UNARY(pexch, h, 8, t.h[(i & 4) | (i & 1 ? (i & 2 ? 3 : 2) : (i & 2 ? 1 : 0))])
PS2_UNARY(pexcw, w, 4, t.w[i == 1 ? 2 : i == 2 ? 1 : i])
PS2_UNARY(pexeh, h, 8, t.h[(i & 4) | ((i & 3) == 0 ? 2 : (i & 3) == 2 ? 0 : (i & 3))])
PS2_UNARY(pexew, w, 4, t.w[i == 0 ? 2 : i == 2 ? 0 : i])
PS2_UNARY(prevh, h, 8, t.h[(i & 4) | (3 - (i & 3))])
PS2_UNARY(prot3w, w, 4, t.w[i == 0 ? 1 : i == 1 ? 2 : i == 2 ? 0 : 3])
PS2_UNARY(pext5, w, 4, ((t.w[i] & 0x1Fu) << 3) | (((t.w[i] >> 5) & 0x1Fu) << 11) |
                           (((t.w[i] >> 10) & 0x1Fu) << 19) | (((t.w[i] >> 15) & 1u) << 31))
PS2_UNARY(ppac5, w, 4, ((t.w[i] >> 3) & 0x1Fu) | (((t.w[i] >> 11) & 0x1Fu) << 5) |
                           (((t.w[i] >> 19) & 0x1Fu) << 10) | (((t.w[i] >> 31) & 1u) << 15))

#undef PS2_UNARY

inline void psllh(Reg128 &d, const Reg128 &t, unsigned sa) { for (int i = 0; i < 8; ++i) d.h[i] = static_cast<std::uint16_t>(t.h[i] << (sa & 15)); }
inline void psrlh(Reg128 &d, const Reg128 &t, unsigned sa) { for (int i = 0; i < 8; ++i) d.h[i] = static_cast<std::uint16_t>(t.h[i] >> (sa & 15)); }
inline void psrah(Reg128 &d, const Reg128 &t, unsigned sa) { for (int i = 0; i < 8; ++i) d.sh[i] = static_cast<std::int16_t>(t.sh[i] >> (sa & 15)); }
inline void psllw(Reg128 &d, const Reg128 &t, unsigned sa) { for (int i = 0; i < 4; ++i) d.w[i] = t.w[i] << (sa & 31); }
inline void psrlw(Reg128 &d, const Reg128 &t, unsigned sa) { for (int i = 0; i < 4; ++i) d.w[i] = t.w[i] >> (sa & 31); }
inline void psraw(Reg128 &d, const Reg128 &t, unsigned sa) { for (int i = 0; i < 4; ++i) d.sw[i] = t.sw[i] >> (sa & 31); }

inline void psllvw(Reg128 &d, const Reg128 &s, const Reg128 &t) {
    const std::int64_t a = static_cast<std::int32_t>(t.w[0] << (s.w[0] & 31)), b = static_cast<std::int32_t>(t.w[2] << (s.w[2] & 31));
    d.sd[0] = a;
    d.sd[1] = b;
}

inline void psrlvw(Reg128 &d, const Reg128 &s, const Reg128 &t) {
    const std::int64_t a = static_cast<std::int32_t>(t.w[0] >> (s.w[0] & 31)), b = static_cast<std::int32_t>(t.w[2] >> (s.w[2] & 31));
    d.sd[0] = a;
    d.sd[1] = b;
}

inline void psravw(Reg128 &d, const Reg128 &s, const Reg128 &t) {
    const std::int64_t a = t.sw[0] >> (s.w[0] & 31), b = t.sw[2] >> (s.w[2] & 31);
    d.sd[0] = a;
    d.sd[1] = b;
}

inline std::uint32_t leading_sign_bits(std::uint32_t x) {
    return static_cast<std::uint32_t>(std::countl_zero(static_cast<std::int32_t>(x) < 0 ? ~x : x)) - 1u;
}

inline void plzcw(Reg128 &d, const Reg128 &s) {
    const std::uint32_t a = leading_sign_bits(s.w[0]), b = leading_sign_bits(s.w[1]);
    d.w[0] = a;
    d.w[1] = b;
}

// QFSRV: the 32 bytes s:t (s the upper half) shifted right by sa & 15 bytes, the low 16 bytes kept. Done on 64-bit words with
// selects and shifts. It used to build the 32 bytes in a stack buffer and read 16 of them back at a byte offset; the processor
// cannot forward that read from the two stores just before it and waits for them (God of War II's film decoder runs it in its
// motion compensation loops, about 3 % of the emulation thread). Every input word is read before d is written (d may be s or t).
inline void qfsrv(Reg128 &d, const Reg128 &s, const Reg128 &t, std::uint32_t sa) {
    const std::uint32_t n = sa & 15u;
    const bool upper = n >= 8u;
    const std::uint64_t w0 = upper ? t.d[1] : t.d[0];
    const std::uint64_t w1 = upper ? s.d[0] : t.d[1];
    const std::uint64_t w2 = upper ? s.d[1] : s.d[0];
    const std::uint32_t b = (n & 7u) * 8u;
    // (x << 1) << (63 - b) is x << (64 - b) for b from 8 to 56 and 0 for b == 0, with no shift by 64
    const std::uint64_t r0 = (w0 >> b) | ((w1 << 1) << (63u - b));
    const std::uint64_t r1 = (w1 >> b) | ((w2 << 1) << (63u - b));
    d.d[0] = r0;
    d.d[1] = r1;
}

inline std::int64_t sext32(std::uint32_t v) { return static_cast<std::int32_t>(v); }

inline void set_hilo(Cpu &c, int pipe, std::uint64_t value) {
    c.lo.sd[pipe] = sext32(static_cast<std::uint32_t>(value));
    c.hi.sd[pipe] = sext32(static_cast<std::uint32_t>(value >> 32));
}

inline std::uint64_t hilo(const Cpu &c, int pipe) {
    return (static_cast<std::uint64_t>(c.hi.w[pipe * 2]) << 32) | c.lo.w[pipe * 2];
}

inline void mult(Cpu &c, int pipe, std::uint32_t s, std::uint32_t t) {
    set_hilo(c, pipe, static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(s)) * static_cast<std::int32_t>(t)));
}

inline void multu(Cpu &c, int pipe, std::uint32_t s, std::uint32_t t) {
    set_hilo(c, pipe, static_cast<std::uint64_t>(s) * t);
}

inline void madd(Cpu &c, int pipe, std::uint32_t s, std::uint32_t t) {
    set_hilo(c, pipe, hilo(c, pipe) + static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(s)) * static_cast<std::int32_t>(t)));
}

inline void maddu(Cpu &c, int pipe, std::uint32_t s, std::uint32_t t) {
    set_hilo(c, pipe, hilo(c, pipe) + static_cast<std::uint64_t>(s) * t);
}

inline void div(Cpu &c, int pipe, std::uint32_t s, std::uint32_t t) {
    const auto n = static_cast<std::int32_t>(s), m = static_cast<std::int32_t>(t);
    if (m == 0) {
        c.lo.sd[pipe] = n < 0 ? 1 : -1;
        c.hi.sd[pipe] = n;
    } else if (n == INT32_MIN && m == -1) {
        c.lo.sd[pipe] = INT32_MIN;
        c.hi.sd[pipe] = 0;
    } else {
        c.lo.sd[pipe] = n / m;
        c.hi.sd[pipe] = n % m;
    }
}

inline void divu(Cpu &c, int pipe, std::uint32_t s, std::uint32_t t) {
    if (t == 0) {
        c.lo.sd[pipe] = -1;
        c.hi.sd[pipe] = sext32(s);
    } else {
        c.lo.sd[pipe] = sext32(s / t);
        c.hi.sd[pipe] = sext32(s % t);
    }
}

inline void pmultw(Cpu &c, Reg128 &d, const Reg128 &s, const Reg128 &t) {
    Reg128 r;
    for (int i = 0; i < 2; ++i) {
        const std::int64_t p = static_cast<std::int64_t>(s.sw[i * 2]) * t.sw[i * 2];
        r.sd[i] = p;
        set_hilo(c, i, static_cast<std::uint64_t>(p));
    }
    d = r;
}

inline void pmultuw(Cpu &c, Reg128 &d, const Reg128 &s, const Reg128 &t) {
    Reg128 r;
    for (int i = 0; i < 2; ++i) {
        const std::uint64_t p = static_cast<std::uint64_t>(s.w[i * 2]) * t.w[i * 2];
        r.d[i] = p;
        set_hilo(c, i, p);
    }
    d = r;
}

inline void pmaddw(Cpu &c, Reg128 &d, const Reg128 &s, const Reg128 &t, bool subtract) {
    Reg128 r;
    for (int i = 0; i < 2; ++i) {
        const std::int64_t p = static_cast<std::int64_t>(s.sw[i * 2]) * t.sw[i * 2];
        const std::uint64_t acc = hilo(c, i) + static_cast<std::uint64_t>(subtract ? -p : p);
        r.d[i] = acc;
        set_hilo(c, i, acc);
    }
    d = r;
}

inline void pmadduw(Cpu &c, Reg128 &d, const Reg128 &s, const Reg128 &t) {
    Reg128 r;
    for (int i = 0; i < 2; ++i) {
        const std::uint64_t acc = hilo(c, i) + static_cast<std::uint64_t>(s.w[i * 2]) * t.w[i * 2];
        r.d[i] = acc;
        set_hilo(c, i, acc);
    }
    d = r;
}

inline void pdivw(Cpu &c, const Reg128 &s, const Reg128 &t) {
    for (int i = 0; i < 2; ++i) div(c, i, s.w[i * 2], t.w[i * 2]);
}

inline void pdivuw(Cpu &c, const Reg128 &s, const Reg128 &t) {
    for (int i = 0; i < 2; ++i) divu(c, i, s.w[i * 2], t.w[i * 2]);
}

inline void pdivbw(Cpu &c, const Reg128 &s, const Reg128 &t) {
    const std::int32_t m = t.sh[0];
    for (int i = 0; i < 4; ++i) {
        const std::int32_t n = s.sw[i];
        if (m == 0) {
            c.lo.sw[i] = n < 0 ? 1 : -1;
            c.hi.sw[i] = n;
        } else if (n == INT32_MIN && m == -1) {
            c.lo.sw[i] = INT32_MIN;
            c.hi.sw[i] = 0;
        } else {
            c.lo.sw[i] = n / m;
            c.hi.sw[i] = static_cast<std::int16_t>(n % m);
        }
    }
}

inline void pmulth(Cpu &c, Reg128 &d, const Reg128 &s, const Reg128 &t, int mode) {
    std::int32_t p[8];
    for (int i = 0; i < 8; ++i) p[i] = static_cast<std::int32_t>(s.sh[i]) * t.sh[i];
    constexpr int kLo[4] = {0, 1, 4, 5}, kHi[4] = {2, 3, 6, 7};
    for (int k = 0; k < 4; ++k) {
        if (mode == 0) {
            c.lo.sw[k] = p[kLo[k]];
            c.hi.sw[k] = p[kHi[k]];
        } else if (mode == 1) {
            c.lo.sw[k] += p[kLo[k]];
            c.hi.sw[k] += p[kHi[k]];
        } else {
            c.lo.sw[k] -= p[kLo[k]];
            c.hi.sw[k] -= p[kHi[k]];
        }
    }
    Reg128 r;
    r.w[0] = c.lo.w[0];
    r.w[1] = c.hi.w[0];
    r.w[2] = c.lo.w[2];
    r.w[3] = c.hi.w[2];
    d = r;
}

inline void phmadh(Cpu &c, Reg128 &d, const Reg128 &s, const Reg128 &t, bool subtract) {
    std::uint32_t p[8];
    for (int i = 0; i < 8; ++i) p[i] = static_cast<std::uint32_t>(static_cast<std::int32_t>(s.sh[i]) * t.sh[i]);
    // Every LO/HI doubleword takes the sum (phmadh) or difference (phmsbh) of an even/odd
    // product pair in its low word, and the odd product (inverted for phmsbh) in its high word.
    const auto pair = [&](Reg128 &dst, int word, int k) {
        dst.w[word] = subtract ? p[k + 1] - p[k] : p[k + 1] + p[k];
        dst.w[word + 1] = subtract ? ~p[k + 1] : p[k + 1];
    };
    pair(c.lo, 0, 0);
    pair(c.hi, 0, 2);
    pair(c.lo, 2, 4);
    pair(c.hi, 2, 6);
    Reg128 r;
    r.w[0] = c.lo.w[0];
    r.w[1] = c.hi.w[0];
    r.w[2] = c.lo.w[2];
    r.w[3] = c.hi.w[2];
    d = r;
}

inline std::int32_t sat32(std::int64_t v) {
    return v > INT32_MAX ? INT32_MAX : v < INT32_MIN ? INT32_MIN : static_cast<std::int32_t>(v);
}

inline std::int16_t sat16(std::int32_t v) {
    return v > INT16_MAX ? INT16_MAX : v < INT16_MIN ? INT16_MIN : static_cast<std::int16_t>(v);
}

inline void pmfhl(const Cpu &c, Reg128 &d, unsigned mode) {
    Reg128 r = d;
    const Reg128 &lo = c.lo, &hi = c.hi;
    switch (mode) {
    case 0:
        r.w[0] = lo.w[0]; r.w[1] = hi.w[0]; r.w[2] = lo.w[2]; r.w[3] = hi.w[2];
        break;
    case 1:
        r.w[0] = lo.w[1]; r.w[1] = hi.w[1]; r.w[2] = lo.w[3]; r.w[3] = hi.w[3];
        break;
    case 2:
        for (int i = 0; i < 2; ++i)
            r.sd[i] = sat32(static_cast<std::int64_t>((static_cast<std::uint64_t>(hi.w[i * 2]) << 32) | lo.w[i * 2]));
        break;
    case 3:
        r.h[0] = lo.h[0]; r.h[1] = lo.h[2]; r.h[2] = hi.h[0]; r.h[3] = hi.h[2];
        r.h[4] = lo.h[4]; r.h[5] = lo.h[6]; r.h[6] = hi.h[4]; r.h[7] = hi.h[6];
        break;
    default:
        r.sh[0] = sat16(lo.sw[0]); r.sh[1] = sat16(lo.sw[1]); r.sh[2] = sat16(hi.sw[0]); r.sh[3] = sat16(hi.sw[1]);
        r.sh[4] = sat16(lo.sw[2]); r.sh[5] = sat16(lo.sw[3]); r.sh[6] = sat16(hi.sw[2]); r.sh[7] = sat16(hi.sw[3]);
        break;
    }
    d = r;
}

inline void pmthl(Cpu &c, const Reg128 &s) {
    c.lo.w[0] = s.w[0];
    c.hi.w[0] = s.w[1];
    c.lo.w[2] = s.w[2];
    c.hi.w[2] = s.w[3];
}

}
