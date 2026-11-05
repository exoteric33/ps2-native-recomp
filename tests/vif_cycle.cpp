// No game data: an UNPACK while CYCLE is 0 (the state after power-on, before any STCYCL, or STCYCL 0) takes all its data, so the
// commands behind it are still read as commands; with CL 0 and WL above 0 (Tekken 5) it takes none. Build and run: ninja ps2viftest && build/<dir>/ps2viftest
#include "hw.hpp"
#include "vif.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {
int failed = 0;
void check(bool ok, const char *what, int unit) {
    if (!ok) { ++failed; std::fprintf(stderr, "FAIL (VIF%d): %s\n", unit, what); }
}

// Data words that read as STCYCL commands: a UNPACK that takes too little data leaves them to be run, and CYCLE shows it.
constexpr std::uint32_t kData = 0x01002000u;
constexpr std::uint32_t kStcycl = 0x01000000u, kMark = 0x07000000u, kStmask = 0x20000000u, kStcol = 0x31000000u;
// V4-32 (0x6C), with the mask bit (0x10) when asked for; NUM and the address in VU memory.
std::uint32_t unpack_v4_32(std::uint32_t num, std::uint32_t addr, bool masked = false) {
    return (masked ? 0x7C000000u : 0x6C000000u) | (num << 16) | addr;
}

std::uint8_t *vu_mem(int unit) { return unit ? ps2::g_vu1_mem : ps2::g_vu0_mem; }
std::uint32_t quad_word(int unit, std::uint32_t quad, int word) {
    std::uint32_t v;
    std::memcpy(&v, vu_mem(unit) + quad * 16 + word * 4, 4);
    return v;
}
void clear_vu(int unit) { std::memset(vu_mem(unit), 0, unit ? sizeof ps2::g_vu1_mem : sizeof ps2::g_vu0_mem); }

// `data` words kData + i, so every word of the stream can be told apart in VU memory.
std::vector<std::uint32_t> stream(std::uint32_t cycle, bool set_cycle, std::uint32_t num, std::uint32_t data_words, std::uint32_t mark, bool masked = false) {
    std::vector<std::uint32_t> w;
    if (set_cycle) w.push_back(kStcycl | cycle);
    if (masked) {
        w.push_back(kStmask); w.push_back(0xAAAAAAAAu);
        w.push_back(kStcol); w.push_back(1); w.push_back(2); w.push_back(3); w.push_back(4);
    }
    w.push_back(unpack_v4_32(num, 0, masked));
    for (std::uint32_t i = 0; i < data_words; ++i) w.push_back(kData + i);
    w.push_back(kMark | mark);  // a command right behind the data: it only runs when the UNPACK took exactly its data
    return w;
}

// The stream fed in one piece, or cut after `cut` words (a DMA tag boundary inside the UNPACK).
void feed(int unit, const std::vector<std::uint32_t> &w, std::size_t cut = 0) {
    if (!cut || cut >= w.size()) { ps2::vif::transfer_words(unit, w.data(), static_cast<std::uint32_t>(w.size())); return; }
    ps2::vif::transfer_words(unit, w.data(), static_cast<std::uint32_t>(cut));
    ps2::vif::transfer_words(unit, w.data() + cut, static_cast<std::uint32_t>(w.size() - cut));
}

// An UNPACK of `num` quadwords with the data it needs and a MARK behind it: the data went where the write cycle says and the MARK ran.
void in_step(int unit, const char *what, std::uint32_t cycle, bool set_cycle, std::uint32_t num, std::uint32_t data_quads, std::size_t cut = 0) {
    ps2::vif::reset();
    clear_vu(unit);
    feed(unit, stream(cycle, set_cycle, num, data_quads * 4, 0x1234), cut);
    char msg[160];
    std::snprintf(msg, sizeof msg, "%s: the MARK behind the data ran", what);
    check(ps2::vif::read(unit, 0x30) == 0x1234, msg, unit);
    std::snprintf(msg, sizeof msg, "%s: nothing waits for data", what);
    check((ps2::vif::read(unit, 0x00) & 3u) == 0, msg, unit);
    std::snprintf(msg, sizeof msg, "%s: the data was not run as commands (CYCLE unchanged)", what);
    check(ps2::vif::read(unit, 0x40) == (set_cycle ? cycle : 0u), msg, unit);
}

void run(int unit) {
    // Before any STCYCL (CYCLE is 0 after power-on): the 8 words of a 2-quadword V4-32 UNPACK are data, not commands (the FIFA Street 2 case).
    in_step(unit, "first UNPACK before any STCYCL", 0, false, 2, 2);
    check(quad_word(unit, 0, 0) == kData && quad_word(unit, 0, 3) == kData + 3 && quad_word(unit, 1, 0) == kData + 4 && quad_word(unit, 1, 3) == kData + 7,
          "first UNPACK before any STCYCL: both quadwords written, in order", unit);
    check(quad_word(unit, 2, 0) == 0, "first UNPACK before any STCYCL: nothing written behind", unit);
    in_step(unit, "first UNPACK before any STCYCL, cut inside the data", 0, false, 2, 2, 4);

    // The same with the large NUM the Ridge Racer V case has, on both unit sizes, and the cut anywhere.
    in_step(unit, "NUM 91 before any STCYCL", 0, false, 91, 91);
    in_step(unit, "NUM 91 before any STCYCL, cut", 0, false, 91, 91, 100);

    // STCYCL 0 (CL 0, WL 0) sent by the game: the same as the state after power-on.
    in_step(unit, "STCYCL 0", 0x0000, true, 8, 8);
    check(quad_word(unit, 7, 0) == kData + 7 * 4, "STCYCL 0: eight quadwords written back to back", unit);

    // The cycle register is replaced by each STCYCL, not merged: 4,4, then 0 again.
    ps2::vif::reset();
    clear_vu(unit);
    feed(unit, stream(0x0404, true, 4, 16, 1));
    feed(unit, stream(0x0000, true, 4, 16, 2));
    check(ps2::vif::read(unit, 0x30) == 2 && ps2::vif::read(unit, 0x40) == 0x0000, "STCYCL 4,4 then STCYCL 0: both UNPACKs in step", unit);

    // With the mask: the column register by write cycle, 1 2 3 4 4 4 4 4 for WL 0 (the layout that dma/vif/stcycl measures).
    ps2::vif::reset();
    clear_vu(unit);
    feed(unit, stream(0x0000, true, 8, 32, 0x77, true));
    bool layout = true;
    for (std::uint32_t q = 0; q < 8; ++q) layout = layout && quad_word(unit, q, 0) == (q < 4 ? q + 1 : 4);
    check(layout, "CYCLE 0 with all-column mask: 1 2 3 4 4 4 4 4", unit);
    check(ps2::vif::read(unit, 0x30) == 0x77, "CYCLE 0 with all-column mask: the MARK ran", unit);

    // CL 0 with WL above 0 is a filling write that takes no data (Tekken 5: STCYCL 0x0500, STMASK, then UNPACK headers back to back,
    // each filling 6 quadwords from the column register). The next headers must run as commands, and the stream must be in step.
    for (std::uint32_t wl : {1u, 5u}) {
        ps2::vif::reset();
        clear_vu(unit);
        std::vector<std::uint32_t> w = {kStcycl | (wl << 8), kStmask, 0xAAAAAAAAu, kStcol, 1, 2, 3, 4};
        for (std::uint32_t at : {1u, 0x5Bu, 0xB5u}) w.push_back(0x70060000u | at);  // S-32 with the mask, NUM 6, no data behind
        w.push_back(kMark | 0x55);
        feed(unit, w);
        char msg[120];
        std::snprintf(msg, sizeof msg, "CL 0 WL %u takes no data: the UNPACKs behind each other and the MARK ran", wl);
        check(ps2::vif::read(unit, 0x30) == 0x55 && (ps2::vif::read(unit, 0x00) & 3u) == 0, msg, unit);
        std::snprintf(msg, sizeof msg, "CL 0 WL %u takes no data: the quadwords hold the column values", wl);
        check(quad_word(unit, 1, 0) == 1 && quad_word(unit, 0x5B, 0) == 1 && quad_word(unit, 0xB5, 0) == 1 && quad_word(unit, 0xB5 + 4, 0) == (wl == 5 ? 4u : 1u), msg, unit);
    }

    // What must not change: a filling write (CL 1, WL 4) takes one quadword per cycle, a skipping write (CL 4, WL 2) takes one quadword per output
    // and leaves the other quadwords of each CL untouched.
    in_step(unit, "filling write CL 1 WL 4, NUM 8 takes 2 quadwords", 0x0401, true, 8, 2);
    in_step(unit, "filling write CL 1 WL 0, NUM 8 takes 1 quadword", 0x0001, true, 8, 1);
    in_step(unit, "skipping write CL 4 WL 2, NUM 4 takes 4 quadwords", 0x0204, true, 4, 4);
    check(quad_word(unit, 4, 0) == kData + 2 * 4 && quad_word(unit, 2, 0) == 0 && quad_word(unit, 3, 0) == 0,
          "skipping write CL 4 WL 2: quadwords 2 and 3 untouched, the third written at 4", unit);
    in_step(unit, "normal write CL 4 WL 4", 0x0404, true, 8, 8);
}
}  // namespace

int main() {
    run(0);
    run(1);
    ps2::vif::reset();
    if (failed) { std::fprintf(stderr, "%d checks failed\n", failed); return 1; }
    std::puts("vif cycle: all checks passed");
    return 0;
}
