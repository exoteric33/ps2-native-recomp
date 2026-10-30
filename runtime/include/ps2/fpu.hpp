#pragma once

#include <bit>
#include <cmath>
#include <cstdint>
#include <utility>

namespace ps2::fp {

inline constexpr std::uint32_t kSign = 0x80000000u;
inline constexpr std::uint32_t kAbs = 0x7FFFFFFFu;
inline constexpr std::uint32_t kOne = 0x3F800000u;

inline std::uint32_t exponent(std::uint32_t x) { return (x >> 23) & 0xFFu; }
inline std::uint32_t flush(std::uint32_t x) { return exponent(x) ? x : (x & kSign); }
inline std::uint32_t mantissa(std::uint32_t x) { return (x & 0x7FFFFFu) | 0x800000u; }

inline std::uint32_t pack(std::uint32_t sign, int e, std::uint32_t m) {
    if (e > 255) return sign | kAbs;
    if (e <= 0) return sign;
    return sign | (static_cast<std::uint32_t>(e) << 23) | (m & 0x7FFFFFu);
}

inline std::uint32_t add(std::uint32_t a, std::uint32_t b) {
    a = flush(a);
    b = flush(b);
    std::uint32_t aa = a & kAbs, ba = b & kAbs;
    if (!ba) return aa ? a : (a & b);
    if (!aa) return b;
    if (ba > aa) {
        std::swap(a, b);
        std::swap(aa, ba);
    }
    int e = static_cast<int>(aa >> 23);
    const int diff = e - static_cast<int>(ba >> 23);
    std::uint32_t ma = mantissa(aa) << 1;
    const std::uint32_t mb = diff > 25 ? 0u : (mantissa(ba) << 1) >> diff;
    if ((a ^ b) & kSign) {
        ma -= mb;
        if (!ma) return 0;
        const int shift = std::countl_zero(ma) - 7;
        ma <<= shift;
        e -= shift;
    } else {
        ma += mb;
        if (ma & 0x2000000u) {
            ma >>= 1;
            ++e;
        }
    }
    return pack(a & kSign, e, ma >> 1);
}

inline std::uint32_t sub(std::uint32_t a, std::uint32_t b) { return add(a, b ^ kSign); }

inline std::uint32_t mul(std::uint32_t a, std::uint32_t b) {
    const std::uint32_t sign = (a ^ b) & kSign;
    const int ea = static_cast<int>(exponent(a)), eb = static_cast<int>(exponent(b));
    if (!ea || !eb) return sign;
    std::uint64_t m = static_cast<std::uint64_t>(mantissa(a)) * mantissa(b);
    int e = ea + eb - 127;
    if (m >> 47) {
        m >>= 24;
        ++e;
    } else {
        m >>= 23;
    }
    return pack(sign, e, static_cast<std::uint32_t>(m));
}

inline double to_double(std::uint32_t x) {
    if (!exponent(x)) return (x & kSign) ? -0.0 : 0.0;
    const std::uint64_t bits = (static_cast<std::uint64_t>(x & kSign) << 32) |
                               (static_cast<std::uint64_t>(exponent(x) + 1023 - 127) << 52) |
                               (static_cast<std::uint64_t>(x & 0x7FFFFFu) << 29);
    return std::bit_cast<double>(bits);
}

inline std::uint32_t from_double(double d) {
    const std::uint64_t bits = std::bit_cast<std::uint64_t>(d);
    const std::uint32_t sign = static_cast<std::uint32_t>(bits >> 32) & kSign;
    const int e = static_cast<int>((bits >> 52) & 0x7FFu);
    if (!e) return sign;
    return pack(sign, e - 1023 + 127, static_cast<std::uint32_t>(bits >> 29));
}

struct Result {
    std::uint32_t value;
    bool invalid;
};

inline Result div(std::uint32_t a, std::uint32_t b) {
    a = flush(a);
    b = flush(b);
    const std::uint32_t sign = (a ^ b) & kSign;
    if (!(b & kAbs)) return {sign | kAbs, true};
    if (!(a & kAbs)) return {sign, false};
    return {from_double(to_double(a) / to_double(b)), false};
}

inline std::uint32_t sqrt(std::uint32_t a) {
    a = flush(a) & kAbs;
    if (!a) return 0;
    return from_double(std::sqrt(to_double(a)));
}

inline Result rsqrt(std::uint32_t a, std::uint32_t b) {
    a = flush(a);
    b = flush(b) & kAbs;
    if (!b) return {(a & kSign) | kAbs, true};
    if (!(a & kAbs)) return {a & kSign, false};
    return {from_double(to_double(a) / std::sqrt(to_double(b))), false};
}

inline std::int32_t key(std::uint32_t x) {
    x = flush(x);
    const auto magnitude = static_cast<std::int32_t>(x & kAbs);
    return (x & kSign) ? -magnitude : magnitude;
}

// max.s and min.s compare the raw bits as signed integers, with the order of two negative numbers reversed. Unlike
// key(), which makes the two zeros equal, this keeps -0 below +0 (hardware: max(-0, +0) is +0 and min(+0, -0) is
// -0), and denormals and NaNs order by their bits instead of being flushed.
inline std::uint32_t max(std::uint32_t a, std::uint32_t b) {
    const auto x = static_cast<std::int32_t>(a), y = static_cast<std::int32_t>(b);
    return (x < 0 && y < 0) ? (x < y ? a : b) : (x > y ? a : b);
}

inline std::uint32_t min(std::uint32_t a, std::uint32_t b) {
    const auto x = static_cast<std::int32_t>(a), y = static_cast<std::int32_t>(b);
    return (x < 0 && y < 0) ? (x > y ? a : b) : (x < y ? a : b);
}

// The vector units' MAX and MINI keep the key() order for the zeros: emulators disagree on their -0 behaviour and no hardware test
// covers it, and the games that already run were brought up with this (-0 and +0 are equal, the first operand wins).
// Denormals are NOT flushed in this order: they compare by their bits like any other number and come out as they went in. Tekken 5
// builds its GIF tags with `MAXi vf1, vf00, I` after a LOI of a small integer (3, 14: denormals as floats), and a flush to zero
// there made every tag empty (a fight drawn as colour noise). Order of the result, not of the arithmetic: ADD, MUL and the like
// still flush their inputs.
inline std::int32_t order_key(std::uint32_t x) {
    const auto magnitude = static_cast<std::int32_t>(x & kAbs);
    return (x & kSign) ? -magnitude : magnitude;
}
inline std::uint32_t vu_max(std::uint32_t a, std::uint32_t b) { return order_key(a) >= order_key(b) ? a : b; }
inline std::uint32_t vu_min(std::uint32_t a, std::uint32_t b) { return order_key(a) <= order_key(b) ? a : b; }

inline std::uint32_t to_fixed(std::uint32_t x, int fraction) {
    const int e = static_cast<int>(exponent(x)) + fraction;
    if (e <= 126 || !exponent(x)) return 0;
    const bool negative = x & kSign;
    if (e >= 158) return negative ? 0x80000000u : 0x7FFFFFFFu;
    const int shift = e - 150;
    const std::uint32_t m = mantissa(x);
    const std::uint32_t v = shift >= 0 ? m << shift : m >> -shift;
    return negative ? 0u - v : v;
}

inline std::uint32_t from_fixed(std::int32_t v, int fraction) {
    return from_double(static_cast<double>(v) / static_cast<double>(1u << fraction));
}

inline std::uint32_t cvt_w_s(std::uint32_t x) { return to_fixed(x, 0); }
inline std::uint32_t cvt_s_w(std::uint32_t v) { return from_fixed(static_cast<std::int32_t>(v), 0); }

// The Emotion Engine's own FPU (COP1), as the hardware tests show it. The functions above are shared with the vector
// units, whose arithmetic differs in the last bit and in the flags, so they stay as they are. Here:
//   - there is no NaN and no infinity: an exponent of 255 is an ordinary number, and an overflow gives the largest
//     value (0x7FFFFFFF with the sign), an underflow or a denormal gives zero;
//   - add, sub and mul truncate; div, sqrt and rsqrt round to nearest;
//   - the multiplier drops one unit in the last place of the product when bit 1 of the second operand's significand
//     is set and the product is exact (1.0 * 0x3FFFFFFF is 0x3FFFFFFE, 0x3FFFFFFF * 1.0 is not);
//   - madd/msub add the product to the accumulator before the overflow is cut off;
//   - the flags go to FCR31: the cause bit and the sticky bit of invalid (sqrt of a negative number, 0/0), divide by
//     zero and overflow. Underflow never shows, as on the console.
// Every function takes FCR31 by reference for the flags.
namespace ee {

inline constexpr std::uint32_t kFlagInvalid = 0x20040u;
inline constexpr std::uint32_t kFlagDivide = 0x10020u;
inline constexpr std::uint32_t kFlagOverflow = 0x8010u;

// sign, biased exponent (not limited to 255: a product keeps its exponent until it is added or stored) and the 24-bit
// significand with the leading one; a significand of 0 is a zero.
struct Value {
    std::uint32_t sign;
    int e;
    std::uint32_t m;
};

inline Value unpack(std::uint32_t x) {
    const std::uint32_t e = exponent(x);
    if (!e) return {x & kSign, 0, 0};
    return {x & kSign, static_cast<int>(e), mantissa(x)};
}

inline std::uint32_t pack(std::uint32_t &fcr, const Value &v) {
    if (!v.m || v.e <= 0) return v.sign;
    if (v.e > 255) {
        fcr |= kFlagOverflow;
        return v.sign | kAbs;
    }
    return v.sign | (static_cast<std::uint32_t>(v.e) << 23) | (v.m & 0x7FFFFFu);
}

inline Value add_value(Value a, Value b) {
    if (!b.m) return a.m ? a : Value{a.sign & b.sign, 0, 0};
    if (!a.m) return b;
    if (b.e > a.e || (b.e == a.e && b.m > a.m)) std::swap(a, b);
    int e = a.e;
    const int diff = a.e - b.e;
    std::uint32_t ma = a.m << 1;
    const std::uint32_t mb = diff > 25 ? 0u : (b.m << 1) >> diff;
    if (a.sign != b.sign) {
        ma -= mb;
        if (!ma) return {0, 0, 0};
        const int shift = std::countl_zero(ma) - 7;
        ma <<= shift;
        e -= shift;
    } else {
        ma += mb;
        if (ma & 0x2000000u) {
            ma >>= 1;
            ++e;
        }
    }
    return {a.sign, e, ma >> 1};
}

inline Value mul_value(std::uint32_t a, std::uint32_t b, bool booth = true) {
    const Value x = unpack(a), y = unpack(b);
    const std::uint32_t sign = x.sign ^ y.sign;
    if (!x.m || !y.m) return {sign, 0, 0};
    std::uint64_t p = static_cast<std::uint64_t>(x.m) * y.m;
    if (booth) p -= (y.m >> 1) & 1u;
    int e = x.e + y.e - 127;
    if (p >> 47) {
        p >>= 24;
        ++e;
    } else {
        p >>= 23;
    }
    return {sign, e, static_cast<std::uint32_t>(p)};
}

inline Value negated(Value v) {
    v.sign ^= kSign;
    return v;
}

inline std::uint32_t add(std::uint32_t &fcr, std::uint32_t a, std::uint32_t b) {
    return pack(fcr, add_value(unpack(a), unpack(b)));
}
inline std::uint32_t sub(std::uint32_t &fcr, std::uint32_t a, std::uint32_t b) {
    return pack(fcr, add_value(unpack(a), negated(unpack(b))));
}
inline std::uint32_t mul(std::uint32_t &fcr, std::uint32_t a, std::uint32_t b) {
    return pack(fcr, mul_value(a, b));
}
inline std::uint32_t madd(std::uint32_t &fcr, std::uint32_t acc, std::uint32_t a, std::uint32_t b) {
    return pack(fcr, add_value(unpack(acc), mul_value(a, b)));
}
inline std::uint32_t msub(std::uint32_t &fcr, std::uint32_t acc, std::uint32_t a, std::uint32_t b) {
    return pack(fcr, add_value(unpack(acc), negated(mul_value(a, b))));
}

// Quotient of two non-zero numbers, rounded to nearest (ties to even).
inline std::uint32_t divide(std::uint32_t &fcr, std::uint32_t a, std::uint32_t b) {
    const Value x = unpack(a), y = unpack(b);
    const std::uint64_t num = static_cast<std::uint64_t>(x.m) << 25;
    std::uint64_t q = num / y.m;
    bool sticky = num % y.m != 0;
    int e = x.e - y.e + 127;
    int drop = 2;
    if (!(q >> 25)) {
        drop = 1;
        --e;
    }
    sticky = sticky || (q & ((1u << (drop - 1)) - 1)) != 0;
    const bool half = (q >> (drop - 1)) & 1u;
    q >>= drop;
    if (half && (sticky || (q & 1u))) ++q;
    if (q >> 24) {
        q >>= 1;
        ++e;
    }
    return pack(fcr, {(x.sign ^ y.sign), e, static_cast<std::uint32_t>(q)});
}

inline std::uint32_t div(std::uint32_t &fcr, std::uint32_t a, std::uint32_t b) {
    a = flush(a);
    b = flush(b);
    const std::uint32_t sign = (a ^ b) & kSign;
    if (!(b & kAbs)) {
        fcr |= (a & kAbs) ? kFlagDivide : kFlagInvalid;
        return sign | kAbs;
    }
    if (!(a & kAbs)) return sign;
    return divide(fcr, a, b);
}

// Square root of the absolute value, rounded to nearest; a negative operand raises the invalid flag.
inline std::uint32_t sqrt(std::uint32_t &fcr, std::uint32_t x) {
    if ((x & kSign) && exponent(x)) fcr |= kFlagInvalid;
    x = flush(x) & kAbs;
    if (!x) return 0;
    const int e = static_cast<int>(exponent(x)) - 127;
    const std::uint64_t n = static_cast<std::uint64_t>(mantissa(x)) << ((e & 1) ? 24 : 23);
    int re = e >> 1;
    std::uint64_t r = static_cast<std::uint64_t>(std::sqrt(static_cast<double>(n)));
    while (r * r > n) --r;
    while ((r + 1) * (r + 1) <= n) ++r;
    if (n - r * r > r) ++r;
    if (r >> 24) {
        r >>= 1;
        ++re;
    }
    return (static_cast<std::uint32_t>(re + 127) << 23) | (static_cast<std::uint32_t>(r) & 0x7FFFFFu);
}

// fs / sqrt(ft), as fs * (1 / sqrt(ft)) with the root and the reciprocal rounded to nearest and the product truncated
// (this reproduces the last bit in every case of the hardware test; a quotient of the rounded root does not).
inline std::uint32_t rsqrt(std::uint32_t &fcr, std::uint32_t a, std::uint32_t b) {
    a = flush(a);
    b = flush(b);
    if (!(b & kAbs)) {
        fcr |= (a & kAbs) ? kFlagDivide : kFlagInvalid;
        return (a & kSign) | kAbs;
    }
    const std::uint32_t root = sqrt(fcr, b);
    if (!(a & kAbs)) return a & kSign;
    return pack(fcr, mul_value(a, divide(fcr, kOne, root), false));
}

}

}
