// QFSRV (mmi.hpp) against the byte-buffer form it replaced: every shift amount, random and patterned inputs, and the result
// written over either input (the generated code passes the same register as d and s or t). No game needed: ninja ps2mmitest
#include "ps2/mmi.hpp"

#include <cstdio>
#include <cstring>
#include <random>

namespace {

void reference(ps2::Reg128 &d, const ps2::Reg128 &s, const ps2::Reg128 &t, std::uint32_t sa) {
    std::uint8_t bytes[32];
    std::memcpy(bytes, &t, 16);
    std::memcpy(bytes + 16, &s, 16);
    ps2::Reg128 r;
    std::memcpy(&r, bytes + (sa & 15), 16);
    d = r;
}

bool same(const ps2::Reg128 &a, const ps2::Reg128 &b) { return std::memcmp(&a, &b, 16) == 0; }

}  // namespace

int main() {
    std::mt19937_64 rng(20261121);
    long checks = 0, wrong = 0;
    for (int round = 0; round < 200000; ++round) {
        ps2::Reg128 s, t;
        for (int i = 0; i < 2; ++i) {
            s.d[i] = rng();
            t.d[i] = rng();
        }
        if (round < 256) {  // byte patterns that show which byte went where
            for (int i = 0; i < 16; ++i) {
                t.b[i] = static_cast<std::uint8_t>(i + round);
                s.b[i] = static_cast<std::uint8_t>(0x80 + i + round);
            }
        }
        const std::uint32_t sa = round < 256 ? static_cast<std::uint32_t>(round) : static_cast<std::uint32_t>(rng());
        ps2::Reg128 want, got;
        reference(want, s, t, sa);
        ps2::mmi::qfsrv(got, s, t, sa);
        ps2::Reg128 over_s = s, over_t = t, both = s;
        ps2::mmi::qfsrv(over_s, over_s, t, sa);
        ps2::mmi::qfsrv(over_t, s, over_t, sa);
        ps2::Reg128 want_both;
        reference(want_both, s, s, sa);
        ps2::mmi::qfsrv(both, both, both, sa);
        checks += 4;
        const int bad = !same(want, got) + !same(want, over_s) + !same(want, over_t) + !same(want_both, both);
        if (bad && wrong < 5) std::printf("sa %u: %d wrong\n", sa & 15, bad);
        wrong += bad;
    }
    std::printf("qfsrv: %ld checks, %ld wrong: %s\n", checks, wrong, wrong ? "FAIL" : "PASS");
    return wrong ? 1 : 0;
}
