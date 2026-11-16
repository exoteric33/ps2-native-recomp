// Checks of the late float clean-up of the compiled VU1 programs (ps2/vu_ops.hpp, "done late"). No game data.
//
//   1. vclamp and vflush_value against vfix_generic on every one of the 2^32 bit patterns: the two halves together are the old
//      clean-up, and the first half alone leaves everything but an exponent of 255 as it was.
//   1b. vclamp_result (the first half done with float MIN and MAX, as the arithmetic does it) the same way, with the "denormals
//      are zero" bit set, on every bit pattern that is not a NaN.
//   1c. The flush done as a multiply by one (vflush_mul_value), with that bit set, against the integer one on every bit pattern
//      whose exponent is not 255.
//   2. With the processor's "denormals are zero" bit set, as inside a compiled program: add, subtract, multiply, multiply-add and
//      multiply-subtract done the late way give, after the flush, the bits of the old functions that clean at once; the flags taken
//      from the unflushed result are the flags of the clean one; and a second operation fed with the unflushed result gives what it
//      gives fed with the clean one. On every pair of a pool of edge values, on products and sums aimed at the edges (just below the
//      smallest normal number, where the product rounds up to it; exact denormals; overflow) and on random bit patterns.
//
// Build and run: c++ -std=c++20 -O2 -msse4.1 -Iruntime/include tests/test_vu_cleanup.cpp -o t && ./t     (or: ninja ps2vucleanuptest)
// Without SSE4.1 the late helpers are the old ones and the test only shows that; it says which of the two it ran.
#include "ps2/vu_ops.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

using namespace ps2;
using namespace ps2::vuops;

namespace {

v4u load4(const std::uint32_t *p) {
    v4u v;
    std::memcpy(&v, p, 16);
    return v;
}

bool same(v4u a, v4u b) { return std::memcmp(&a, &b, 16) == 0; }

constexpr int kResults = 9;
const char *const kNames[kResults] = {"add", "sub", "mul", "madd", "msub", "mul of the sum", "add of the product", "madd of difference and product", "sub with Q"};

struct Out {
    v4u value[kResults];
    std::uint32_t mac[kResults], status[kResults];
};

void take_flags(Out &o, int i, v4u unflushed) {
    static Vu0Regs r;
    r.mac = 0;
    r.status = 0x555u;
    flags(r, unflushed, 0xFu);
    o.mac[i] = r.mac;
    o.status[i] = r.status;
}

// The old way: every operand cleaned, every result cleaned at once. Runs with the default MXCSR.
__attribute__((noinline)) void eager(const std::uint32_t *a, const std::uint32_t *b, const std::uint32_t *c, Out *out, std::size_t quads) {
    for (std::size_t i = 0; i < quads; ++i) {
        const v4u A = load4(a + i * 4), B = load4(b + i * 4), C = load4(c + i * 4);
        Out &o = out[i];
        o.value[0] = vadd(A, B);
        o.value[1] = vsub(A, B);
        o.value[2] = vmul(A, B);
        o.value[3] = vmadd(C, A, B);
        o.value[4] = vmsub(C, A, B);
        o.value[5] = vmul(o.value[0], C);
        o.value[6] = vadd(o.value[2], C);
        o.value[7] = vmadd(o.value[1], o.value[2], C);
        o.value[8] = vsubn(vfix(A), vfix(vsplat(c[i * 4])));
        for (int k = 0; k < kResults; ++k) take_flags(o, k, o.value[k]);
    }
}

// The late way, as the generated code does it. The caller has set "denormals are zero".
__attribute__((noinline)) void late(const std::uint32_t *a, const std::uint32_t *b, const std::uint32_t *c, Out *out, std::size_t quads) {
    Reg128 ra, rb, rc;
    const v4u all = vsplat(0xFFFFFFFFu);
    for (std::size_t i = 0; i < quads; ++i) {
        std::memcpy(&ra, a + i * 4, 16);
        std::memcpy(&rb, b + i * 4, 16);
        std::memcpy(&rc, c + i * 4, 16);
        const v4u A = vshadow(ra), B = vshadow(rb), C = vshadow(rc);
        v4u v[kResults];
        v[0] = vaddd(A, B);
        v[1] = vsubd(A, B);
        v[2] = vmuld(A, B);
        v[3] = vmaddd(C, A, B);
        v[4] = vmsubd(C, A, B);
        v[5] = vmuld(v[0], C);
        v[6] = vaddd(v[2], C);
        v[7] = vmaddd(v[1], v[2], C);
        v[8] = vsubd(A, vqd(c[i * 4]));
        Out &o = out[i];
        for (int k = 0; k < kResults; ++k) {
            take_flags(o, k, v[k]);
            o.value[k] = vflush_value(v[k], all);
        }
    }
}

int compare(const char *what, const std::vector<std::uint32_t> &a, const std::vector<std::uint32_t> &b, const std::vector<std::uint32_t> &c) {
    const std::size_t quads = a.size() / 4;
    std::vector<Out> want(quads), got(quads);
    eager(a.data(), b.data(), c.data(), want.data(), quads);
    {
        VuDaz daz;
        late(a.data(), b.data(), c.data(), got.data(), quads);
        (void)daz;
    }
    int bad = 0;
    for (std::size_t i = 0; i < quads; ++i)
        for (int k = 0; k < kResults; ++k) {
            if (same(want[i].value[k], got[i].value[k]) && want[i].mac[k] == got[i].mac[k] && want[i].status[k] == got[i].status[k]) continue;
            if (++bad > 8) continue;
            std::uint32_t w[4], g[4];
            std::memcpy(w, &want[i].value[k], 16);
            std::memcpy(g, &got[i].value[k], 16);
            std::printf("%s, %s: a %08X %08X %08X %08X  b %08X %08X %08X %08X  c %08X %08X %08X %08X\n  at once %08X %08X %08X %08X mac %04X status %03X\n  late    %08X %08X %08X %08X mac %04X status %03X\n",
                        what, kNames[k], a[i * 4], a[i * 4 + 1], a[i * 4 + 2], a[i * 4 + 3], b[i * 4], b[i * 4 + 1], b[i * 4 + 2], b[i * 4 + 3], c[i * 4], c[i * 4 + 1], c[i * 4 + 2], c[i * 4 + 3],
                        w[0], w[1], w[2], w[3], want[i].mac[k], want[i].status[k], g[0], g[1], g[2], g[3], got[i].mac[k], got[i].status[k]);
        }
    std::printf("%-28s %9zu triples, %d results differ\n", what, quads * 4, bad);
    return bad;
}

}

__attribute__((noinline)) int clamp_of_results_body() {
    const v4u all = vsplat(0xFFFFFFFFu);
    std::uint64_t wrong = 0, done = 0;
    for (std::uint64_t x = 0; x < 0x100000000ull; x += 4) {
        const std::uint32_t x0 = static_cast<std::uint32_t>(x);
        if ((x0 & 0x7F800000u) == 0x7F800000u && (x0 & 0x7FFFFCu)) continue;  // a quad of NaNs
        v4u v = v4u{x0, x0 + 1u, x0 + 2u, x0 + 3u};
        if ((x0 & 0x7FFFFFFCu) == 0x7F800000u) v = vsplat(x0);  // the quad with an infinity in it: the infinity alone
        const v4u clamped = vclamp_result((v4f)vopaque(v));
        if (!same(vflush_value(clamped, all), vfix_generic(v))) ++wrong;
#if PS2_VU_DAZ
        const v4u high = (v4u)((v & 0x7F800000u) == 0x7F800000u), tiny = (v4u)((v & 0x7F800000u) == 0u);
        // only an infinity changes; a denormal may come out as itself or as the zero it stands for
        if (!same(clamped | tiny, ((vfix_generic(v) & high) | (v & ~high)) | tiny)) ++wrong;
        if (!same(clamped & tiny & 0x80000000u, v & tiny & 0x80000000u)) ++wrong;
#endif
        done += 4;
    }
    std::printf("%-28s %9llu values, %llu quads differ\n", "float MIN/MAX against vfix", static_cast<unsigned long long>(done), static_cast<unsigned long long>(wrong));
    return wrong != 0;
}

// 1c. The flush done by the processor (vflush_mul_value, a multiply by one with "denormals are zero" set) against the one done with
// integer instructions, for all lanes and for some, on every bit pattern whose exponent is not 255 (what the lanes it is used on
// can hold: results of vclamp_result).
__attribute__((noinline)) int flush_by_multiply_body() {
    const v4u all = vsplat(0xFFFFFFFFu), xz = v4u{0xFFFFFFFFu, 0u, 0xFFFFFFFFu, 0u}, w = v4u{0u, 0u, 0u, 0xFFFFFFFFu};
    std::uint64_t wrong = 0, done = 0;
    for (std::uint64_t x = 0; x < 0x100000000ull; x += 4) {
        const std::uint32_t x0 = static_cast<std::uint32_t>(x);
        if ((x0 & 0x7F800000u) == 0x7F800000u) continue;  // a quad with exponent 255
        const v4u v = vopaque(v4u{x0, x0 + 1u, x0 + 2u, x0 + 3u});
        if (!same(vflush_mul_value(v, all), vflush_value(v, all))) ++wrong;
        if (!same(vflush_mul_value(v, xz), vflush_value(v, xz))) ++wrong;
        if (!same(vflush_mul_value(v, w), vflush_value(v, w))) ++wrong;
        done += 4;
    }
    std::printf("%-28s %9llu values, %llu quads differ\n", "flush by multiply", static_cast<unsigned long long>(done), static_cast<unsigned long long>(wrong));
    return wrong != 0;
}

int flush_by_multiply() {
    VuDaz daz;
    return flush_by_multiply_body();
}

int clamp_of_results() {
    VuDaz daz;
    return clamp_of_results_body();
}

int main() {
    int bad = 0;
    std::printf("the late clean-up is %s in this build\n", PS2_VU_DAZ ? "on (SSE4.1, denormals are zero)" : "off (the old functions)");

    // 1. the two halves on every bit pattern
    {
        const v4u all = vsplat(0xFFFFFFFFu), none = vsplat(0u), xz = v4u{0xFFFFFFFFu, 0u, 0xFFFFFFFFu, 0u};
        std::uint64_t wrong = 0;
        for (std::uint64_t x = 0; x < 0x100000000ull; x += 4) {
            const std::uint32_t x0 = static_cast<std::uint32_t>(x);
            const v4u v = v4u{x0, x0 + 1u, x0 + 2u, x0 + 3u};
            const v4u want = vfix_generic(v), clamped = vclamp(v);
            if (!same(vflush_value(clamped, all), want)) ++wrong;
#if PS2_VU_DAZ
            // the first half alone: only an exponent of 255 changes
            const v4u high = (v4u)((v & 0x7F800000u) == 0x7F800000u);
            if (!same(clamped, (want & high) | (v & ~high))) ++wrong;
            // the second half only touches the lanes it is told to
            if (!same(vflush_value(clamped, none), clamped)) ++wrong;
            if (!same(vflush_value(clamped, xz), (want & xz) | (clamped & ~xz))) ++wrong;
#endif
        }
        std::printf("%-28s %9s values, %llu quads differ\n", "the two halves against vfix", "2^32", static_cast<unsigned long long>(wrong));
        bad += wrong != 0;
    }

    // 1b. the first half as the arithmetic does it (float MIN and MAX), with "denormals are zero" set as inside a program, on every
    // bit pattern that is not a NaN (a sum or product of numbers without an exponent of 255 never is one)
    bad += clamp_of_results();
    bad += flush_by_multiply();

    // 2. arithmetic with "denormals are zero"
    static const std::uint32_t kEdge[] = {
        0x00000000, 0x00000001, 0x00000002, 0x00000005, 0x00400000, 0x007FFFFE, 0x007FFFFF, 0x00800000, 0x00800001, 0x00800002, 0x00C00000, 0x00FFFFFF,
        0x01000000, 0x01000001, 0x017FFFFF, 0x1F000000, 0x1F7FFFFF, 0x1F800000, 0x1F800001, 0x1FFFFFFF, 0x20000000, 0x33800000, 0x34000000, 0x3EFFFFFF,
        0x3F000000, 0x3F400000, 0x3F7FFFFE, 0x3F7FFFFF, 0x3F800000, 0x3F800001, 0x3FC00000, 0x3FFFFFFF, 0x40000000, 0x40400000, 0x4B000000, 0x4B800000,
        0x5F000000, 0x5F7FFFFF, 0x5F800000, 0x60000000, 0x7E800000, 0x7EFFFFFF, 0x7F000000, 0x7F7FFFFE, 0x7F7FFFFF, 0x7F800000, 0x7F800001, 0x7FC00000,
        0x7FFFFFFF};
    std::vector<std::uint32_t> pool;
    for (std::uint32_t x : kEdge) {
        pool.push_back(x);
        pool.push_back(x | 0x80000000u);
    }
    const auto pad = [](std::vector<std::uint32_t> &v) {
        while (v.size() % 4) v.push_back(0x3F800000u);
    };
    {
        std::vector<std::uint32_t> a, b, c;
        for (std::size_t i = 0; i < pool.size(); ++i)
            for (std::size_t j = 0; j < pool.size(); ++j)
                for (std::size_t k = 0; k < pool.size(); k += 3) {
                    a.push_back(pool[i]);
                    b.push_back(pool[j]);
                    c.push_back(pool[(k + i + j) % pool.size()]);
                }
        pad(a), pad(b), pad(c);
        bad += compare("pairs of the edge values", a, b, c);
    }
    std::mt19937 g(20261110u);
    {
        // products and sums aimed at the edges: exponents that add up to the smallest and the largest ones, random mantissas, and the
        // mantissas of all ones and of one that put a product a hair below a power of two
        std::vector<std::uint32_t> a, b, c;
        static const std::uint32_t kMantissa[] = {0x000000, 0x000001, 0x7FFFFF, 0x7FFFFE, 0x400000, 0x3FFFFF};
        for (int n = 0; n < 6000000; ++n) {
            const auto mantissa = [&]() { return g() % 3 ? (g() & 0x7FFFFFu) : kMantissa[g() % 6]; };
            const int target = g() % 2 ? static_cast<int>(g() % 6) - 2 : 252 + static_cast<int>(g() % 6);  // biased exponent the product aims at
            const int ea = 1 + static_cast<int>(g() % 254);
            int eb = target + 127 - ea + static_cast<int>(g() % 3) - 1;
            eb = eb < 0 ? 0 : eb > 255 ? 255 : eb;
            a.push_back((g() & 0x80000000u) | (static_cast<std::uint32_t>(ea) << 23) | mantissa());
            b.push_back((g() & 0x80000000u) | (static_cast<std::uint32_t>(eb) << 23) | mantissa());
            // the third value is near the product in size (so the multiply-add cancels), tiny, or huge
            const int ec = g() % 2 ? target + static_cast<int>(g() % 5) - 2 : static_cast<int>(g() % 256);
            c.push_back((g() & 0x80000000u) | (static_cast<std::uint32_t>(ec < 0 ? 0 : ec > 255 ? 255 : ec) << 23) | mantissa());
        }
        pad(a), pad(b), pad(c);
        bad += compare("aimed at the edges", a, b, c);
    }
    {
        // sums and differences of small numbers (exact denormals) and of the largest ones (overflow)
        std::vector<std::uint32_t> a, b, c;
        for (int n = 0; n < 3000000; ++n) {
            const std::uint32_t e = g() % 2 ? g() % 4 : 252 + g() % 4;
            a.push_back((g() & 0x80000000u) | (e << 23) | (g() & 0x7FFFFFu));
            b.push_back((g() & 0x80000000u) | ((g() % 3 ? e : (e ^ 1u)) << 23) | (g() % 4 ? (a.back() & 0x7FFFFFu) ^ (g() & 0xFu) : g() & 0x7FFFFFu));
            c.push_back(g() % 2 ? pool[g() % pool.size()] : g());
        }
        pad(a), pad(b), pad(c);
        bad += compare("small and large sums", a, b, c);
    }
    {
        std::vector<std::uint32_t> a, b, c;
        for (int n = 0; n < 6000000; ++n) {
            a.push_back(g());
            b.push_back(g());
            c.push_back(g());
        }
        bad += compare("random bit patterns", a, b, c);
    }
    std::puts(bad ? "FAILED" : "the late clean-up gives the bits and flags of the old one");
    return bad ? 1 : 0;
}
