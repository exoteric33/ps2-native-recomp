// No game data: the IPU's inverse transform (BDEC) gives the same pixels bit for bit as the transform it replaced, for every input the
// decoder can hand it. The old function is kept below as the reference, unchanged; ps2::ipu::idct (AVX2 when the build has it) and
// ps2::ipu::idct_plain (the plain C++ fallback, the one Arm Macs use) are both compared with it on millions of random and edge-case
// blocks. The decoder clamps every dequantised coefficient to -2048..2047 (and the mismatch control keeps f[63] inside it), so that is
// the range that matters; wider values up to +-2^20 are tried too (every sum stays far inside the int range there).
//
//   ninja ps2idctexacttest && ./ps2idctexacttest [random blocks, default 4000000] [seed]
#include "ipu/idct.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>

namespace {

// ---- the reference: the transform as runtime/src/ipu/ipu.cpp had it before (main 672e816), with its own copy of the cosine table
struct OldTables {
    double cosines[8][8];
    double cosines_t[8][8];
    OldTables() {
        for (int x = 0; x < 8; ++x)
            for (int u = 0; u < 8; ++u) {
                cosines[x][u] = (u ? 0.5 : 0.5 / std::sqrt(2.0)) * std::cos((2 * x + 1) * u * 3.14159265358979323846 / 16.0);
                cosines_t[u][x] = cosines[x][u];
            }
    }
};

const OldTables &old_tables() {
    static const OldTables t;
    return t;
}

void old_idct(const int *in, std::int16_t *out) {
    const OldTables &t = old_tables();
    alignas(32) double rows[8][8];
    int live[8], live_rows = 0;
    for (int v = 0; v < 8; ++v) {
        const int *row = in + v * 8;
        int at[8], count = 0;
        for (int u = 0; u < 8; ++u)
            if (row[u]) at[count++] = u;
        if (!count) continue;
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

// ---- the comparison
long long g_blocks = 0, g_failed = 0;

void check(const int *in, const char *kind) {
    alignas(32) std::int16_t want[64], fast[64], plain[64];
    std::memset(fast, 0x55, sizeof fast);
    std::memset(plain, 0x55, sizeof plain);
    old_idct(in, want);
    ps2::ipu::idct(in, fast);
    ps2::ipu::idct_plain(in, plain);
    ++g_blocks;
    const bool fast_ok = !std::memcmp(want, fast, sizeof want), plain_ok = !std::memcmp(want, plain, sizeof want);
    if (fast_ok && plain_ok) return;
    if (++g_failed <= 10) {
        std::fprintf(stderr, "FAIL (%s, %s differs) in:", kind, fast_ok ? "plain" : "fast");
        for (int i = 0; i < 64; ++i)
            if (in[i]) std::fprintf(stderr, " [%d]=%d", i, in[i]);
        std::fprintf(stderr, "\n");
    }
}

std::mt19937_64 g_rng;
int uniform(int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(g_rng); }

// a value as the decoder makes them: mostly small, sometimes large, sometimes at the limits
int coefficient() {
    switch (uniform(0, 9)) {
    case 0: return uniform(0, 1) ? 2047 : -2048;
    case 1: return uniform(-2048, 2047);
    case 2: return uniform(-256, 255);
    case 3: return uniform(-32, 32);
    default: { const int v = uniform(1, 8); return uniform(0, 1) ? v : -v; }
    }
}

}  // namespace

int main(int argc, char **argv) {
    const long long random_blocks = argc > 1 ? std::atoll(argv[1]) : 4000000;
    g_rng.seed(argc > 2 ? std::strtoull(argv[2], nullptr, 0) : 0x1DC7ull);
    alignas(32) int in[64];

    // 1. all zero, and every single position with every value -2048..2047 at position 0 and a spread of values elsewhere
    std::fill(in, in + 64, 0);
    check(in, "zero");
    for (int v = -2048; v <= 2047; ++v) {
        std::fill(in, in + 64, 0);
        in[0] = v;
        check(in, "dc");
        in[63] = 1;  // the mismatch control's usual partner of a DC-only block
        check(in, "dc+63");
        in[63] = -1;
        check(in, "dc+63");
    }
    for (int p = 1; p < 64; ++p)
        for (int v = -2048; v <= 2047; v += (v > -40 && v < 40) ? 1 : 7) {
            std::fill(in, in + 64, 0);
            in[p] = v;
            check(in, "single");
        }
    // 2. every pair of positions with the extreme values
    for (int p = 0; p < 64; ++p)
        for (int q = p + 1; q < 64; ++q)
            for (int a : {-2048, -1, 1, 2047})
                for (int b : {-2048, -1, 1, 2047}) {
                    std::fill(in, in + 64, 0);
                    in[p] = a;
                    in[q] = b;
                    check(in, "pair");
                }
    // 3. random blocks: sparse like a video block (few coefficients, mostly low positions), dense, whole rows, saturated, wide values
    for (long long n = 0; n < random_blocks; ++n) {
        std::fill(in, in + 64, 0);
        const int kind = static_cast<int>(n % 6);
        if (kind == 0 || kind == 1) {  // sparse, positions biased to the top-left corner as zig-zag order makes them
            const int count = kind == 0 ? uniform(1, 6) : uniform(1, 24);
            for (int i = 0; i < count; ++i) {
                const int p = std::min(63, static_cast<int>(std::abs(std::normal_distribution<double>(0.0, 14.0)(g_rng))));
                in[p] = coefficient();
            }
            if (uniform(0, 1)) in[63] += uniform(0, 1) ? 1 : -1;
        } else if (kind == 2) {  // dense
            for (int i = 0; i < 64; ++i) in[i] = coefficient();
        } else if (kind == 3) {  // a few whole rows or columns
            const int lines = uniform(1, 3);
            for (int l = 0; l < lines; ++l) {
                const int at = uniform(0, 7);
                const bool row = uniform(0, 1);
                for (int i = 0; i < 8; ++i) in[row ? at * 8 + i : i * 8 + at] = coefficient();
            }
        } else if (kind == 4) {  // everything at the limits (pictures that clamp to -256 and 255 everywhere)
            for (int i = 0; i < 64; ++i) in[i] = uniform(0, 3) ? 0 : (uniform(0, 1) ? 2047 : -2048);
        } else {  // wider than the decoder makes, still far inside the range where every sum is exact enough to compare
            const int count = uniform(1, 64);
            for (int i = 0; i < count; ++i) in[uniform(0, 63)] = uniform(-(1 << 20), 1 << 20);
        }
        check(in, "random");
    }
    std::printf("idct exact: %lld blocks, %lld differ (fast = %s)\n", g_blocks, g_failed,
#if defined(__AVX2__)
                "AVX2"
#else
                "plain"
#endif
    );
    return g_failed ? 1 : 0;
}
