// The vector set-up of a triangle (runtime/src/gs/gpu/setup.hpp, prim_setup::triangle) against the scalar arithmetic draw() used before
// (prim_setup::triangle_scalar): every word of the shape record, the box and "has no area" must be the same, bit for bit.
// Random corners over the whole 16-bit range, small triangles near pixel edges, shared x or y (the edge rule's ties), no area, the
// offset and scissor at their ends, every scale, flat and smooth shading, STQ and UV, and corners at or above 2^31 (the scalar fallback).
// No game needed: ninja ps2drawsetuptest && ./ps2drawsetuptest
#include "gs/gpu/setup.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>

using ps2::gs::Vertex;
namespace ps = ps2::gs::gpu::prim_setup;

namespace {
std::uint64_t g_seed = 0x243F6A8885A308D3ull;
std::uint32_t rnd() {
    g_seed ^= g_seed << 13;
    g_seed ^= g_seed >> 7;
    g_seed ^= g_seed << 17;
    return static_cast<std::uint32_t>(g_seed >> 16);
}
std::uint32_t rnd(std::uint32_t n) { return rnd() % n; }

int g_failures = 0;
long g_cases = 0, g_drawn = 0;

void check(const Vertex *v, int ofx, int ofy, float samples, std::uint32_t scx, std::uint32_t scy, bool fst, bool iip, const char *what) {
    std::uint32_t a[28] = {}, b[28] = {};
    int ba[4] = {7, 7, 7, 7}, bb[4] = {7, 7, 7, 7};
    const bool ra = ps::triangle_scalar(a, ba, v, ofx, ofy, samples, scx, scy, fst, iip);
    const bool rb = ps::triangle(b, bb, v, ofx, ofy, samples, scx, scy, fst, iip);
    ++g_cases;
    bool same = ra == rb;
    if (same && ra) {
        ++g_drawn;
        same = std::memcmp(a, b, sizeof a) == 0 && std::memcmp(ba, bb, sizeof ba) == 0;
    }
    if (!same && g_failures++ < 10) {
        std::printf("DIFF (%s): corners %u,%u %u,%u %u,%u offset %d,%d scale %g scissor %08x %08x fst %d iip %d: drawn %d/%d box %d %d %d %d / %d %d %d %d\n", what,
                    v[0].x, v[0].y, v[1].x, v[1].y, v[2].x, v[2].y, ofx, ofy, samples, scx, scy, fst, iip, ra, rb, ba[0], ba[1], ba[2], ba[3], bb[0], bb[1], bb[2], bb[3]);
        for (int i = 0; i < 28; ++i)
            if (a[i] != b[i]) std::printf("   word %d: %08x / %08x\n", i, a[i], b[i]);
    }
}

float rnd_float() {
    switch (rnd(4)) {
    case 0: return static_cast<float>(rnd(2000)) / 7.0f - 100.0f;
    case 1: return 0.0f;
    case 2: return -0.0f;
    default: {
        std::uint32_t u = rnd();
        float f;
        std::memcpy(&f, &u, 4);
        return f;  // any bit pattern, NaN included: the texture words are copied as bits
    }
    }
}

void attrs(Vertex &v) {
    v.z = rnd(4) == 0 ? (rnd(2) ? 0xFFFFFFFFu : 0u) : rnd();
    v.r = static_cast<std::uint8_t>(rnd());
    v.g = static_cast<std::uint8_t>(rnd());
    v.b = static_cast<std::uint8_t>(rnd());
    v.a = static_cast<std::uint8_t>(rnd());
    v.q = rnd_float();
    v.s = rnd_float();
    v.t = rnd_float();
    v.u = rnd(0x4000);
    v.v = rnd(0x4000);
    v.f = static_cast<std::uint8_t>(rnd());
}

std::uint32_t scissor_word() {
    switch (rnd(4)) {
    case 0: return 0u | (2047u << 16);
    case 1: return rnd(2048) | (rnd(2048) << 16);
    case 2: return 0u | (639u << 16);
    default: return rnd(0x10000) | (rnd(0x10000) << 16);  // also first > last and beyond 2047
    }
}

void random_case(int kind) {
    Vertex v[3] = {};
    for (Vertex &p : v) attrs(p);
    const int ofx = rnd(3) == 0 ? static_cast<int>(rnd(0x10000)) : 0x8000 - 320 * 16 + static_cast<int>(rnd(64)) * 16;
    const int ofy = rnd(3) == 0 ? static_cast<int>(rnd(0x10000)) : 0x8000 - 224 * 16 + static_cast<int>(rnd(64)) * 16;
    const std::uint32_t cx = static_cast<std::uint32_t>(ofx) + rnd(640 * 16), cy = static_cast<std::uint32_t>(ofy) + rnd(448 * 16);
    for (int i = 0; i < 3; ++i) {
        switch (kind) {
        case 0:  // anywhere in the 16-bit range
            v[i].x = rnd(0x10000);
            v[i].y = rnd(0x10000);
            break;
        case 1:  // a small triangle around one point, often on pixel and sample edges (multiples of 16 and of 16 / scale)
            v[i].x = (cx + rnd(64) - 32) & 0xFFFFu;
            v[i].y = (cy + rnd(64) - 32) & 0xFFFFu;
            if (rnd(2)) v[i].x &= ~15u;
            if (rnd(2)) v[i].y &= ~15u;
            break;
        case 2:  // corners on a coarse grid: shared x or y, no area, edges along the axes
            v[i].x = (cx & ~255u) + rnd(4) * 16 * (1 + rnd(8));
            v[i].y = (cy & ~255u) + rnd(4) * 16 * (1 + rnd(8));
            break;
        default:  // the ends of the range
        {
            const std::uint32_t ends[] = {0u, 1u, 15u, 16u, 0x7FFFu, 0x8000u, 0xFFF0u, 0xFFFFu};
            v[i].x = rnd(2) ? ends[rnd(8)] : rnd(0x10000);
            v[i].y = rnd(2) ? ends[rnd(8)] : rnd(0x10000);
        }
        }
    }
    if (kind == 2 && rnd(4) == 0) v[2] = v[rnd(2)];  // two corners the same
    if (kind == 2 && rnd(4) == 0) {  // three corners on a line
        v[2].x = v[1].x + (v[1].x - v[0].x);
        v[2].y = v[1].y + (v[1].y - v[0].y);
    }
    const float samples = static_cast<float>(1 + rnd(3));
    check(v, ofx, ofy, samples, scissor_word(), scissor_word(), rnd(2), rnd(2), "random");
}

// Every triangle with corners on a 3 x 3 grid of sub-pixel positions near a pixel edge, every scale: all edge-rule ties and empty boxes.
void grid_cases() {
    const std::uint32_t base = 0x8000 + 100 * 16;
    const std::uint32_t at[3] = {base - 1, base, base + 9};
    const std::uint32_t full = 0u | (2047u << 16);
    for (int c0 = 0; c0 < 9; ++c0)
        for (int c1 = 0; c1 < 9; ++c1)
            for (int c2 = 0; c2 < 9; ++c2)
                for (int s = 1; s <= 3; ++s) {
                    Vertex v[3] = {};
                    const int c[3] = {c0, c1, c2};
                    for (int i = 0; i < 3; ++i) {
                        attrs(v[i]);
                        v[i].x = at[c[i] % 3];
                        v[i].y = at[c[i] / 3];
                    }
                    check(v, 0x8000, 0x8000, static_cast<float>(s), full, full, false, true, "grid");
                    check(v, 0x8000 - 3, 0x8000 + 5, static_cast<float>(s), full, full, true, false, "grid");
                }
}

// Corners at or above 2^31 (the GS never makes them): the vector version hands them to the scalar one.
void high_cases() {
    for (int n = 0; n < 20000; ++n) {
        Vertex v[3] = {};
        for (Vertex &p : v) {
            attrs(p);
            p.x = rnd(2) ? rnd() | 0x80000000u : rnd();
            p.y = rnd();
        }
        const std::uint32_t full = 0u | (2047u << 16);
        check(v, static_cast<int>(rnd(0x10000)), static_cast<int>(rnd(0x10000)), static_cast<float>(1 + rnd(3)), full, full, rnd(2), rnd(2), "high");
    }
}
}  // namespace

// `ps2drawsetuptest bench`: the time per triangle of both versions on a strip of small triangles fed the way the GS feeds draw(): each corner's
// words stored one by one into a queue of three just before the set-up reads them, the record then copied to an array. Speed only, no check.
template <bool kVector>
[[gnu::noinline]] double bench_strip(int n) {
    static Vertex queue[3];
    static std::uint32_t out[4096 * 28];
    std::uint32_t seed = 12345;
    const auto next = [&seed] { seed = seed * 1664525u + 1013904223u; return seed >> 8; };
    const std::uint32_t full = 0u | (2047u << 16);
    int count = 0;
    std::uint64_t sum = 0;
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < n; ++i) {
        if (count >= 3) {
            queue[0] = queue[1];
            queue[1] = queue[2];
            count = 2;
        }
        Vertex &v = queue[count++];
        const std::uint32_t r = next();
        v.x = 0x8000 + 300 * 16 + (r & 511);
        v.y = 0x8000 + 200 * 16 + ((r >> 9) & 511);
        v.z = r * 77u;
        v.f = static_cast<std::uint8_t>(r);
        v.r = v.g = v.b = v.a = static_cast<std::uint8_t>(r >> 3);
        v.s = static_cast<float>(r & 255) / 256.0f;
        v.t = static_cast<float>((r >> 8) & 255) / 256.0f;
        v.q = 1.0f;
        if (count < 3) continue;
        std::uint32_t rec[28] = {};
        int box[4];
        const bool ok = kVector ? ps::triangle(rec, box, queue, 0x8000, 0x8000, 1.0f, full, full, false, true)
                                : ps::triangle_scalar(rec, box, queue, 0x8000, 0x8000, 1.0f, full, full, false, true);
        if (ok) std::memcpy(out + (i & 4095) * 28, rec, sizeof rec);
        sum += static_cast<std::uint32_t>(box[0]);
    }
    const double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / n;
    if (sum == 1) std::printf(" ");
    return ns;
}

int main(int argc, char **argv) {
    if (argc > 1 && std::strcmp(argv[1], "bench") == 0) {
        for (int round = 0; round < 3; ++round)
            std::printf("ns per triangle: scalar %.2f vector %.2f\n", bench_strip<false>(20000000), bench_strip<true>(20000000));
        return 0;
    }
    for (int n = 0; n < 4000000; ++n) random_case(n & 3);
    grid_cases();
    high_cases();
    std::printf("draw set-up: %ld triangles (%ld with area), %d differ\n", g_cases, g_drawn, g_failures);
    std::printf(g_failures ? "FAIL\n" : "PASS\n");
    return g_failures ? 1 : 0;
}
