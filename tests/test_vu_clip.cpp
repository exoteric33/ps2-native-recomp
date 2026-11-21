// vclip_value (the CLIP of the compiled VU programs, vector instructions) against clip_value (the scalar rule) - no game needed.
// Build and run: clang++ -O2 -msse4.1 -std=gnu++20 -Iruntime/include tests/test_vu_clip.cpp -o /tmp/vuclip && /tmp/vuclip
// Every 32-bit pattern as lane x (with its negation in y and the next pattern in z) against six limits (about two minutes), then random registers
// with random and edge limits. Prints PASS or the first difference.
#include "ps2/vu_ops.hpp"

#include <cstdio>
#include <random>

using namespace ps2;

static bool check(const Reg128 &s, std::uint32_t w, std::uint32_t before) {
    Vu0Regs r{};
    r.clip = before;
    const std::uint32_t a = vuops::clip_value(r, s, w), b = vuops::vclip_value(r, s, w);
    if (a == b) return true;
    std::printf("FAIL: %08X %08X %08X w %08X: scalar %06X vector %06X\n", s.w[0], s.w[1], s.w[2], w, a, b);
    return false;
}

int main(int argc, char **argv) {
    const bool quick = argc > 1 && argv[1][0] == 'q';  // "quick": the random part only
    static const std::uint32_t edges[] = {0x00000000, 0x80000000, 0x00000001, 0x807FFFFF, 0x00800000, 0x80800000, 0x3F800000, 0xBF800000,
                                          0x3F800001, 0x3F7FFFFF, 0x7F7FFFFF, 0xFF7FFFFF, 0x7F800000, 0xFF800000, 0x7FFFFFFF, 0xFFFFFFFF,
                                          0x7FC00000, 0x40000000, 0xC0000000, 0x00400000, 0x4B000000, 0x0F000000};
    static const std::uint32_t limits[] = {0x00000000, 0x007FFFFF, 0x3F800000, 0xBF800001, 0x7F7FFFFF, 0xFFFFFFFF};
    for (std::uint32_t w : limits) {
        if (quick) break;
        std::uint32_t x = 0;
        do {
            Reg128 s{};
            s.w[0] = x;
            s.w[1] = x ^ 0x80000000u;
            s.w[2] = x + 1u;
            s.w[3] = ~x;
            if (!check(s, w, x & 0xFFFFFFu)) return 1;
        } while (++x != 0);
    }
    std::mt19937 rng(7);
    for (int i = 0; i < 50000000; ++i) {
        Reg128 s{};
        for (int k = 0; k < 4; ++k) s.w[k] = (rng() & 3) ? rng() : edges[rng() % (sizeof edges / sizeof edges[0])];
        const std::uint32_t w = (rng() & 1) ? rng() : (rng() & 1) ? s.w[rng() % 3] : edges[rng() % (sizeof edges / sizeof edges[0])];
        if (!check(s, w, rng())) return 1;
    }
    std::puts("PASS: vclip_value equals clip_value");
    return 0;
}
