#pragma once

// The set-up of one triangle for the drawing program: the words of its shape record that come from the three corners (gs.wgsl reads them, see
// draw() in gpu.cpp) and its box on the screen. Two versions that give the same bits:
//
// - triangle_scalar: the arithmetic as draw() always did it, one corner and one edge after the other. It is the reference for the test
//   (tests/draw_setup.cpp) and the path for corners outside 0 .. 2^31 - 1, which the GS never makes (its coordinates are 16 bits).
// - triangle: the same operations on vectors of four lanes, so the three corners, the three edges and the four sides of the box are each one
//   vector operation, and the edge rule has no branches (the scalar version's three data-dependent jumps were mispredicted about half the time).
//
// Why the results are the same, bit for bit:
// - every lane does the same IEEE operations as the scalar code on the same values, in the same order: the conversions, the subtraction of the
//   offset, the division by 16, the differences, the products, the multiplication by the sign. The build turns off fused multiply-add
//   (-ffp-contract=off, scripts/cxx.sh), and nothing here is written as a * b + c anyway: each product is a value of its own.
// - (x2 - x0) * (y1 - y0) is computed as (y1 - y0) * (x2 - x0): one rounding of the same exact product either way.
// - the smallest and largest corner: std::min / std::max of the scalar code can differ from the vector's only between +0 and -0, and both
//   give the integer 0 after ceil / floor and the conversion.
// - the box's maximum lanes go through the minimum's rounding too, with a factor of 1: v * 1 and v / 1 are exact and floor of a whole number
//   is that number, so they are still ceil(v).
// - the products and the area's subtraction are separate statements in the vector version and the scalar one turns contraction off
//   (PS2_SETUP_NO_FMA below), so neither fuses a multiply and an add.
// - integer to float: a corner below 2^31 converts to the same float through a signed or an unsigned conversion.

#include "../gs.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

// No fused multiply-add in either version, whatever the build's -ffp-contract says: the x86 builds pass -ffp-contract=off (scripts/cxx.sh),
// but clang's default elsewhere (arm64 macOS) fuses inside one expression, which turned the scalar area into one rounding instead of two
// there. Now every platform computes the same records.
#if defined(__clang__)
#define PS2_SETUP_NO_FMA _Pragma("clang fp contract(off)")
#else
#define PS2_SETUP_NO_FMA
#endif

namespace ps2::gs::gpu::prim_setup {

inline std::uint32_t bits_of(float f) {
    std::uint32_t v;
    std::memcpy(&v, &f, 4);
    return v;
}

// The corners' colour, texture coordinates and fog for words 11 to 23 (flat shading: `color` is the last corner's).
inline void corner_attrs(std::uint32_t *rec, int n, const Vertex &v, bool fst, const Vertex *color) {
    const Vertex &c = color ? *color : v;
    rec[12 + n] = c.r | (static_cast<std::uint32_t>(c.g) << 8) | (static_cast<std::uint32_t>(c.b) << 16) | (static_cast<std::uint32_t>(c.a) << 24);
    rec[15 + n * 3] = bits_of(fst ? static_cast<float>(v.u) : v.s);
    rec[16 + n * 3] = bits_of(fst ? static_cast<float>(v.v) : v.t);
    rec[17 + n * 3] = bits_of(fst ? 1.0f : v.q);
    rec[11] |= static_cast<std::uint32_t>(v.f) << (8 * n);
}

// Words 1, 4 to 27 of a triangle's record (the caller zeroed it) and its box (first and last pixel, inside the scissor and 0 .. 2047).
// False: the triangle has no area and draws nothing (the box and the record are then not used).
// `scissor_x` / `scissor_y` are the setup's words 10 and 11 (first | last << 16), `samples` the scale (1, 2 or 3).
inline bool triangle_scalar(std::uint32_t *rec, int box[4], const Vertex *v, int ofx, int ofy, float samples, std::uint32_t scissor_x,
                            std::uint32_t scissor_y, bool fst, bool iip) {
    PS2_SETUP_NO_FMA
    const int scax0 = static_cast<int>(scissor_x & 0xFFFFu), scax1 = static_cast<int>(scissor_x >> 16);
    const int scay0 = static_cast<int>(scissor_y & 0xFFFFu), scay1 = static_cast<int>(scissor_y >> 16);
    const auto sx = [&](const Vertex &p) { return (static_cast<float>(p.x) - static_cast<float>(ofx)) / 16.0f; };
    const auto sy = [&](const Vertex &p) { return (static_cast<float>(p.y) - static_cast<float>(ofy)) / 16.0f; };
    const auto first = [&](float lo) { return static_cast<int>(std::floor(std::ceil(lo * samples) / samples)); };
    const float x0 = sx(v[0]), y0 = sy(v[0]), x1 = sx(v[1]), y1 = sy(v[1]), x2 = sx(v[2]), y2 = sy(v[2]);
    const float area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
    if (area == 0.0f) return false;
    const int minx = std::max(scax0, first(std::min({x0, x1, x2})));
    const int maxx = std::min(scax1, static_cast<int>(std::ceil(std::max({x0, x1, x2}))) - 1);
    const int miny = std::max(scay0, first(std::min({y0, y1, y2})));
    const int maxy = std::min(scay1, static_cast<int>(std::ceil(std::max({y0, y1, y2}))) - 1);
    box[0] = std::max(minx, 0);
    box[1] = std::max(miny, 0);
    box[2] = std::min(maxx, 2047);
    box[3] = std::min(maxy, 2047);
    rec[1] = 0;
    const float xy[6] = {x0, y0, x1, y1, x2, y2};
    for (int i = 0; i < 6; ++i) rec[4 + i] = bits_of(xy[i]);
    rec[10] = bits_of(1.0f / area);
    const float sg = area > 0.0f ? 1.0f : -1.0f;
    const auto inclusive = [&](float ax, float ay, float bx, float by) {
        const float gx = (ay - by) * sg, gy = (bx - ax) * sg;
        return gx > 0.0f || (gx == 0.0f && gy > 0.0f);
    };
    rec[27] = (inclusive(x1, y1, x2, y2) ? 1u : 0u) | (inclusive(x2, y2, x0, y0) ? 2u : 0u) | (inclusive(x0, y0, x1, y1) ? 4u : 0u);
    for (int i = 0; i < 3; ++i) corner_attrs(rec, i, v[i], fst, iip ? nullptr : &v[2]);
    rec[24] = v[0].z;
    rec[25] = bits_of(static_cast<float>(static_cast<std::int64_t>(v[1].z) - static_cast<std::int64_t>(v[0].z)));
    rec[26] = bits_of(static_cast<float>(static_cast<std::int64_t>(v[2].z) - static_cast<std::int64_t>(v[0].z)));
    return true;
}

namespace vec {
typedef float F4 __attribute__((vector_size(16)));
typedef std::int32_t I4 __attribute__((vector_size(16)));
typedef std::uint32_t U4 __attribute__((vector_size(16)));

inline F4 ceil4(F4 a) {
#if defined(__has_builtin) && __has_builtin(__builtin_elementwise_ceil)
    return __builtin_elementwise_ceil(a);
#else
    return F4{std::ceil(a[0]), std::ceil(a[1]), std::ceil(a[2]), std::ceil(a[3])};
#endif
}
inline F4 floor4(F4 a) {
#if defined(__has_builtin) && __has_builtin(__builtin_elementwise_floor)
    return __builtin_elementwise_floor(a);
#else
    return F4{std::floor(a[0]), std::floor(a[1]), std::floor(a[2]), std::floor(a[3])};
#endif
}
// std::min (b < a ? b : a) and std::max (a < b ? b : a), lane by lane
inline F4 min4(F4 a, F4 b) {
    const I4 m = b < a;
    return (F4)((m & (I4)b) | (~m & (I4)a));
}
inline F4 max4(F4 a, F4 b) {
    const I4 m = a < b;
    return (F4)((m & (I4)b) | (~m & (I4)a));
}
inline I4 imin4(I4 a, I4 b) {
    const I4 m = b < a;
    return (m & b) | (~m & a);
}
inline I4 imax4(I4 a, I4 b) {
    const I4 m = a < b;
    return (m & b) | (~m & a);
}
}  // namespace vec

// corner_attrs for the three corners, with whole words: the colour bytes r, g, b, a lie one after the other in a Vertex, so on a
// little-endian machine (all the targets) they are the colour word as it is; q, s, t lie one after the other too and become s, t, q with one
// shuffle. Each corner stores four texture words, the fourth is overwritten by the next corner and, after the last one, by the depth word 24.
inline void corner_attrs3(std::uint32_t *rec, const Vertex *v, bool fst, bool iip) {
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    static_assert(offsetof(Vertex, g) == offsetof(Vertex, r) + 1 && offsetof(Vertex, b) == offsetof(Vertex, r) + 2 && offsetof(Vertex, a) == offsetof(Vertex, r) + 3);
    static_assert(offsetof(Vertex, s) == offsetof(Vertex, q) + 4 && offsetof(Vertex, t) == offsetof(Vertex, q) + 8 && offsetof(Vertex, u) == offsetof(Vertex, q) + 12);
    static_assert(offsetof(Vertex, v) == offsetof(Vertex, u) + 4);
    using vec::F4;
    using vec::U4;
    for (int n = 0; n < 3; ++n) {
        const Vertex &c = iip ? v[n] : v[2];
        std::memcpy(rec + 12 + n, &c.r, 4);
        F4 stq;
        if (fst) {
            std::uint32_t uv[2];
            std::memcpy(uv, &v[n].u, 8);
            const U4 w = {uv[0], uv[1], 0u, 0u};
            stq = __builtin_convertvector(w, F4);
            stq[2] = 1.0f;
        } else {
            F4 qstu;
            std::memcpy(&qstu, &v[n].q, 16);
            stq = __builtin_shufflevector(qstu, qstu, 1, 2, 0, 3);
        }
        std::memcpy(rec + 15 + n * 3, &stq, 16);
    }
    rec[11] = static_cast<std::uint32_t>(v[0].f) | (static_cast<std::uint32_t>(v[1].f) << 8) | (static_cast<std::uint32_t>(v[2].f) << 16);
#else
    for (int n = 0; n < 3; ++n) corner_attrs(rec, n, v[n], fst, iip ? nullptr : &v[2]);
#endif
}

// Always inlined into draw(): the call passed ten arguments, some on the stack, for every triangle.
[[gnu::always_inline]] inline bool triangle(std::uint32_t *rec, int box[4], const Vertex *v, int ofx, int ofy, float samples, std::uint32_t scissor_x, std::uint32_t scissor_y,
                     bool fst, bool iip) {
    PS2_SETUP_NO_FMA
    using vec::F4;
    using vec::I4;
    // The corners one 32-bit word at a time: the GS wrote x and y with two 32-bit stores just before (vertex() in gs.cpp), and one 64-bit
    // load across both cannot take them from the store buffer, it waits until they reach the cache. A relaxed atomic load is a plain load
    // that the compiler does not merge with its neighbour.
    const auto word = [](const std::uint32_t &w) { return __atomic_load_n(&w, __ATOMIC_RELAXED); };
    const std::uint32_t x0 = word(v[0].x), y0 = word(v[0].y), x1 = word(v[1].x), y1 = word(v[1].y), x2 = word(v[2].x), y2 = word(v[2].y);
    if ((x0 | y0 | x1 | y1 | x2 | y2) >> 31) return triangle_scalar(rec, box, v, ofx, ofy, samples, scissor_x, scissor_y, fst, iip);
    // a = {x0, y0, x1, y1}, b = {x2, y2, x2, y2}: the corners in pixels
    const I4 ia = {static_cast<std::int32_t>(x0), static_cast<std::int32_t>(y0), static_cast<std::int32_t>(x1), static_cast<std::int32_t>(y1)};
    const I4 ib = {static_cast<std::int32_t>(x2), static_cast<std::int32_t>(y2), static_cast<std::int32_t>(x2), static_cast<std::int32_t>(y2)};
    const float fx = static_cast<float>(ofx), fy = static_cast<float>(ofy);
    const F4 off = {fx, fy, fx, fy};
    const F4 sixteen = {16.0f, 16.0f, 16.0f, 16.0f};
    const F4 a = (__builtin_convertvector(ia, F4) - off) / sixteen, b = (__builtin_convertvector(ib, F4) - off) / sixteen;
    // d = {x1 - x0, y1 - y0, x2 - x0, y2 - y0}; area = d0 * d3 - d2 * d1
    const F4 d = __builtin_shufflevector(a, b, 2, 3, 4, 5) - __builtin_shufflevector(a, a, 0, 1, 0, 1);
    const F4 p = d * __builtin_shufflevector(d, d, 3, 2, 1, 0);
    const float area = p[0] - p[1];
    if (area == 0.0f) return false;
    // the box: {min x, min y, max x, max y}, rounded as the scalar code does (first pixel: floor(ceil(v * samples) / samples), last: ceil(v) - 1)
    const F4 lo2 = vec::min4(a, b), hi2 = vec::max4(a, b);  // {x0 ? x2, y0 ? y2, x1 ? x2, y1 ? y2}
    const F4 lo = vec::min4(lo2, __builtin_shufflevector(lo2, lo2, 2, 3, 0, 1)), hi = vec::max4(hi2, __builtin_shufflevector(hi2, hi2, 2, 3, 0, 1));
    const F4 ext = __builtin_shufflevector(lo, hi, 0, 1, 4, 5);
    const F4 scale = {samples, samples, 1.0f, 1.0f};
    const I4 whole = __builtin_convertvector(vec::floor4(vec::ceil4(ext * scale) / scale), I4) + I4{0, 0, -1, -1};
    const int scax0 = static_cast<int>(scissor_x & 0xFFFFu), scax1 = static_cast<int>(scissor_x >> 16);
    const int scay0 = static_cast<int>(scissor_y & 0xFFFFu), scay1 = static_cast<int>(scissor_y >> 16);
    const I4 low = {std::max(scax0, 0), std::max(scay0, 0), INT32_MIN, INT32_MIN}, high = {INT32_MAX, INT32_MAX, std::min(scax1, 2047), std::min(scay1, 2047)};
    const I4 clamped = vec::imin4(vec::imax4(whole, low), high);
    std::memcpy(box, &clamped, 16);
    rec[1] = 0;
    std::memcpy(rec + 4, &a, 16);
    std::memcpy(rec + 8, &b, 8);
    rec[10] = bits_of(1.0f / area);
    // the edge rule: edge e runs from corner A to corner B, {1, 2}, {2, 0}, {0, 1} (lane 3 repeats lane 2 and is not used)
    const float sg = area > 0.0f ? 1.0f : -1.0f;
    const F4 sgv = {sg, sg, sg, sg}, zero = {0.0f, 0.0f, 0.0f, 0.0f};
    const F4 xa = __builtin_shufflevector(a, b, 2, 4, 0, 0), xb = __builtin_shufflevector(a, b, 4, 0, 2, 2);
    const F4 ya = __builtin_shufflevector(a, b, 3, 5, 1, 1), yb = __builtin_shufflevector(a, b, 5, 1, 3, 3);
    const F4 gx = (ya - yb) * sgv, gy = (xb - xa) * sgv;
    const I4 inclusive = (gx > zero) | ((gx == zero) & (gy > zero));
    const I4 lane_bits = inclusive & I4{1, 2, 4, 0};
    rec[27] = static_cast<std::uint32_t>(lane_bits[0] | lane_bits[1] | lane_bits[2]);
    corner_attrs3(rec, v, fst, iip);
    rec[24] = v[0].z;
    rec[25] = bits_of(static_cast<float>(static_cast<std::int64_t>(v[1].z) - static_cast<std::int64_t>(v[0].z)));
    rec[26] = bits_of(static_cast<float>(static_cast<std::int64_t>(v[2].z) - static_cast<std::int64_t>(v[0].z)));
    return true;
}

}  // namespace ps2::gs::gpu::prim_setup
