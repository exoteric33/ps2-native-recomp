#include "idct.hpp"

#include <algorithm>
#include <cmath>

#if defined(__AVX2__)
#include <immintrin.h>
#endif

namespace ps2::ipu {

namespace {

struct Cosines {
    alignas(32) double cosines[8][8];    // [x][u]
    alignas(32) double cosines_t[8][8];  // the same values, indexed [u][x]: the transform adds one coefficient to all eight outputs at a time

    Cosines() {
        for (int x = 0; x < 8; ++x)
            for (int u = 0; u < 8; ++u) {
                cosines[x][u] = (u ? 0.5 : 0.5 / std::sqrt(2.0)) * std::cos((2 * x + 1) * u * 3.14159265358979323846 / 16.0);
                cosines_t[u][x] = cosines[x][u];
            }
    }
};

const Cosines &tables() {
    static const Cosines t;
    return t;
}

}  // namespace

void idct_plain(const int *in, std::int16_t *out) {
    // Same sums in the same order as the plain 8 x 8 x 8 version, but the terms whose coefficient is zero are left out: such a term adds
    // +0.0 or -0.0, and a sum that starts at +0.0 is never -0.0 (round to nearest), so the sum and the picture stay bit for bit the
    // same. A video block has few non-zero coefficients, so most of the 1024 multiplications are gone.
    // The eight outputs of a row are summed side by side (one coefficient added to all eight, in the order of the coefficients), which
    // is the same sum for every output as one at a time, only the compiler can now do four or two of them in one instruction.
    const Cosines &t = tables();
    alignas(32) double rows[8][8];
    int live[8], live_rows = 0;
    for (int v = 0; v < 8; ++v) {
        const int *row = in + v * 8;
        int at[8], count = 0;
        for (int u = 0; u < 8; ++u)
            if (row[u]) at[count++] = u;
        if (!count) continue;  // an all-zero row adds nothing in the second pass either
        live[live_rows++] = v;
        double acc[8] = {};
        for (int k = 0; k < count; ++k) {
            const double w = row[at[k]];
            const double *ct = t.cosines_t[at[k]];
            for (int x = 0; x < 8; ++x) acc[x] += ct[x] * w;
        }
        for (int x = 0; x < 8; ++x) rows[v][x] = acc[x];
    }
    for (int y = 0; y < 8; ++y) {
        double acc[8] = {};
        for (int k = 0; k < live_rows; ++k) {
            const double cy = t.cosines[y][live[k]];
            const double *r = rows[live[k]];
            for (int x = 0; x < 8; ++x) acc[x] += cy * r[x];
        }
        for (int x = 0; x < 8; ++x) out[y * 8 + x] = static_cast<std::int16_t>(std::clamp(static_cast<int>(std::floor(acc[x] + 0.5)), -256, 255));
    }
}

#if defined(__AVX2__)

// The same arithmetic as idct_plain, written out in AVX2: every output is the same chain of double multiplications and additions in the
// same order (a multiplication and then an addition, never a fused one: the build uses -ffp-contract=off, and the intrinsics below are
// separate instructions anyway), started from +0.0, so every sum is the same double. What is faster:
// - the zero test of a row is one compare of its eight coefficients and a bit mask, walked lowest bit first (the order of the coefficients);
// - each sum of eight lives in two registers for the whole chain instead of going through memory;
// - the second pass does two output rows at a time, so each loaded row of the first pass serves both;
// - the rounding, clamping and narrowing to 16 bits are done four pixels at a time: floor(x + 0.5) is the same instruction family as
//   std::floor, and clamping the whole-number double to -256..255 before turning it into an integer gives the same number as turning
//   it into an integer and clamping after (every sum is far inside the int range: 64 terms of at most 2048 x 0.5).
void idct(const int *in, std::int16_t *out) {
    const Cosines &t = tables();
    alignas(32) double rows[8][8];
    int live[8], live_rows = 0;
    const __m256i zero = _mm256_setzero_si256();
    for (int v = 0; v < 8; ++v) {
        const int *row = in + v * 8;
        const __m256i r = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(row));
        unsigned mask = ~static_cast<unsigned>(_mm256_movemask_ps(_mm256_castsi256_ps(_mm256_cmpeq_epi32(r, zero)))) & 0xFFu;
        if (!mask) continue;  // an all-zero row adds nothing in the second pass either
        live[live_rows++] = v;
        __m256d lo = _mm256_setzero_pd(), hi = _mm256_setzero_pd();
        do {
            const int u = __builtin_ctz(mask);
            mask &= mask - 1;
            const __m256d w = _mm256_set1_pd(static_cast<double>(row[u]));
            lo = _mm256_add_pd(lo, _mm256_mul_pd(_mm256_load_pd(t.cosines_t[u]), w));
            hi = _mm256_add_pd(hi, _mm256_mul_pd(_mm256_load_pd(t.cosines_t[u] + 4), w));
        } while (mask);
        _mm256_store_pd(rows[v], lo);
        _mm256_store_pd(rows[v] + 4, hi);
    }
    if (!live_rows) {  // every sum is +0.0, and floor(0.0 + 0.5) is 0
        std::fill(out, out + 64, std::int16_t{0});
        return;
    }
    const __m256d half = _mm256_set1_pd(0.5), low = _mm256_set1_pd(-256.0), high = _mm256_set1_pd(255.0);
    const auto finish = [&](__m256d a, __m256d b, std::int16_t *dst) {
        a = _mm256_min_pd(_mm256_max_pd(_mm256_floor_pd(_mm256_add_pd(a, half)), low), high);
        b = _mm256_min_pd(_mm256_max_pd(_mm256_floor_pd(_mm256_add_pd(b, half)), low), high);
        const __m128i packed = _mm_packs_epi32(_mm256_cvttpd_epi32(a), _mm256_cvttpd_epi32(b));
        _mm_storeu_si128(reinterpret_cast<__m128i *>(dst), packed);
    };
    for (int y = 0; y < 8; y += 2) {
        __m256d lo0 = _mm256_setzero_pd(), hi0 = _mm256_setzero_pd(), lo1 = _mm256_setzero_pd(), hi1 = _mm256_setzero_pd();
        for (int k = 0; k < live_rows; ++k) {
            const int v = live[k];
            const __m256d rlo = _mm256_load_pd(rows[v]), rhi = _mm256_load_pd(rows[v] + 4);
            const __m256d c0 = _mm256_set1_pd(t.cosines[y][v]), c1 = _mm256_set1_pd(t.cosines[y + 1][v]);
            lo0 = _mm256_add_pd(lo0, _mm256_mul_pd(c0, rlo));
            hi0 = _mm256_add_pd(hi0, _mm256_mul_pd(c0, rhi));
            lo1 = _mm256_add_pd(lo1, _mm256_mul_pd(c1, rlo));
            hi1 = _mm256_add_pd(hi1, _mm256_mul_pd(c1, rhi));
        }
        finish(lo0, hi0, out + y * 8);
        finish(lo1, hi1, out + y * 8 + 8);
    }
}

#else

void idct(const int *in, std::int16_t *out) { idct_plain(in, out); }

#endif

}  // namespace ps2::ipu
