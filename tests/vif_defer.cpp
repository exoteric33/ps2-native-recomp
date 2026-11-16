// No game data: VIF1 holds a program it starts with MSCAL (double buffer on, OFFSET not 0) until the commands behind it that fill the other
// buffer have run, and PS2_VIF_DEFER=0 starts it at the MSCAL as before (public issue #69, Nickelodeon Barnyard, to tell whether the
// hold-back is what a game's trouble comes from). Run it twice: ps2vifdefertest, then ps2vifdefertest off.
// Build and run: ninja ps2vifdefertest && build/<dir>/ps2vifdefertest && build/<dir>/ps2vifdefertest off
#include "hw.hpp"
#include "vif.hpp"
#include "ps2/vu_ops.hpp"
#include "vu_micro.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {
int failed = 0;
void check(bool ok, const char *what) {
    if (!ok) { ++failed; std::fprintf(stderr, "FAIL: %s\n", what); }
}

constexpr std::uint32_t kOffset = 0x02000000u, kBase = 0x03000000u, kItop = 0x04000000u, kMscal = 0x14000000u, kMark = 0x07000000u;
constexpr std::uint32_t kStcycl = 0x01000000u, kStmask = 0x20000000u, kStrow = 0x30000000u;
constexpr std::uint32_t kUnpackV4_32 = 0x6C000000u, kFlg = 0x8000u;

void feed(const std::vector<std::uint32_t> &w) { ps2::vif::transfer_words(1, w.data(), static_cast<std::uint32_t>(w.size())); }

std::uint32_t quad_word(std::uint32_t quad, int word) {
    std::uint32_t v;
    std::memcpy(&v, ps2::g_vu1_mem + quad * 16 + word * 4, 4);
    return v;
}
}

int main(int argc, char **argv) {
    const bool off = argc > 1 && std::strcmp(argv[1], "off") == 0;
    if (off) {
#ifdef _WIN32
        _putenv_s("PS2_VIF_DEFER", "0");
#else
        setenv("PS2_VIF_DEFER", "0", 1);
#endif
    }

    ps2::vif::reset();
    ps2::vu::reset();
    std::memset(ps2::g_vu1_mem, 0, sizeof ps2::g_vu1_mem);
    std::memset(ps2::g_vu1_code, 0, sizeof ps2::g_vu1_code);
    // A program that does nothing and ends: a NOP with the E bit and the NOP of its delay slot.
    const std::uint32_t program[4] = {0x8000033Cu, 0x400002FFu, 0x8000033Cu, 0x000002FFu};
    std::memcpy(ps2::g_vu1_code, program, sizeof program);
    ps2::vu::code_changed(1);

    const std::uint64_t calls = ps2::vu::interpreted_calls();
    feed({kOffset | 0x100, kBase | 0, kItop | 7, kMscal | 0});
    if (off) {
        check(ps2::vu::itop() == 7, "off: the program started at its MSCAL (it saw ITOP 7)");
        check(ps2::vu::interpreted_calls() == calls + 1, "off: the program ran at its MSCAL");
    } else {
        check(ps2::vu::itop() == 0, "the program is held back at its MSCAL (double buffer on)");
        check(ps2::vu::interpreted_calls() == calls, "the program has not run yet");
    }

    // The next batch: a skipping write (CL 2, WL 1) with a mask and a row, then an UNPACK with the FLG bit into the other buffer.
    feed({kStcycl | 0x0102, kStmask, 0, kStrow, 1, 2, 3, 4, kUnpackV4_32 | (1u << 16) | kFlg, 0xAAAA0001u, 0xAAAA0002u, 0xAAAA0003u, 0xAAAA0004u});
    check(quad_word(0x100, 0) == 0xAAAA0001u && quad_word(0x100, 3) == 0xAAAA0004u, "the UNPACK with FLG went into the other buffer (TOPS 0x100)");
    if (!off) {
        check(ps2::vu::itop() == 0 && ps2::vu::interpreted_calls() == calls, "the commands behind the MSCAL that fill the other buffer do not start it");
        feed({kMark | 1});
    }
    check(ps2::vu::itop() == 7, "the program has started by now and saw ITOP 7");
    check(ps2::vu::interpreted_calls() == calls + 1, "the program ran exactly once");
    ps2::vif::stats();
    ps2::vif::describe_vu1();

    if (failed) { std::fprintf(stderr, "%d checks failed\n", failed); return 1; }
    std::puts(off ? "vif defer (off): all checks passed" : "vif defer: all checks passed");
    return 0;
}
