// No game data: an XGKICK whose packet does not end within VU1's data memory (a "tag" that is really vertex data, with a large NLOOP)
// must not swallow the next kick's packet, which on the console starts with a tag of its own; and writes to GS registers that do
// not exist are counted (public issue #69). Build and run: ninja ps2gifrunawaytest && build/<dir>/ps2gifrunawaytest
#include "gs/gif.hpp"
#include "gs/gs.hpp"
#include "hw.hpp"
#include "ps2/vu_ops.hpp"
#include "vu_micro.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace {
int failed = 0;
void check(bool ok, const char *what) {
    if (!ok) { ++failed; std::fprintf(stderr, "FAIL: %s\n", what); }
}

void put(std::uint32_t quad, std::uint64_t lo, std::uint64_t hi) {
    std::memcpy(ps2::g_vu1_mem + quad * 16, &lo, 8);
    std::memcpy(ps2::g_vu1_mem + quad * 16 + 8, &hi, 8);
}

// A GIF tag: NLOOP, EOP, FLG 0 (PACKED), NREG and the register descriptors.
std::uint64_t tag(std::uint32_t nloop, bool eop, std::uint32_t nreg) {
    return nloop | (eop ? 1ull << 15 : 0) | (static_cast<std::uint64_t>(nreg & 15u) << 60);
}

// One A+D write of FRAME_1 at `at`, as the packet a kick sends.
void frame_packet(std::uint32_t at, std::uint64_t value) {
    put(at, tag(1, true, 1), 0xE);
    put(at + 1, value, 0x4C);
}

void runaway(std::uint32_t at) {
    // NLOOP 0x7FFF of one NOP descriptor each: 32767 quadwords, far more than the 1024 of VU1's memory.
    std::memset(ps2::g_vu1_mem, 0, sizeof ps2::g_vu1_mem);
    put(at, tag(0x7FFF, true, 1), 0xF);
}
}

int main() {
    ps2::gif::render_reset();

    frame_packet(0, 0x1111);
    ps2::vu::kick(1, 0);
    check(ps2::gs::g_state.ctx[0].frame == 0x1111, "a normal kick sets FRAME_1");

    // The bad packet at 0x200 runs round the memory; the next kick's packet must still be read as a packet.
    runaway(0x200);
    ps2::vu::kick(1, 0x200);
    frame_packet(0x10, 0x2222);
    ps2::vu::kick(1, 0x10);
    check(ps2::gs::g_state.ctx[0].frame == 0x2222, "the kick after one without an end sets FRAME_1");

    // A real tag, then one whose data is more than the memory holds: the first is sent, the second and what follows it are not (they
    // would spray the register its body writes, here FRAME_1).
    std::memset(ps2::g_vu1_mem, 0, sizeof ps2::g_vu1_mem);
    put(0x380, tag(1, false, 1), 0xE);
    put(0x381, 0x7777, 0x4C);               // FRAME_1 = 0x7777 by the first tag
    put(0x382, tag(0x7FFF, true, 1), 0xE);  // vertex data where the second tag should be
    for (std::uint32_t i = 0x383; i < 0x3A0; ++i) put(i, 0x9999, 0x4C);
    ps2::vu::kick(1, 0x380);
    check(ps2::gs::g_state.ctx[0].frame == 0x7777, "the whole tags before a tag that cannot be one are sent, what follows is not");

    // The same without EOP: tags of NLOOP 0 one after another all round the memory never end the packet.
    std::memset(ps2::g_vu1_mem, 0, sizeof ps2::g_vu1_mem);
    ps2::vu::kick(1, 0);
    frame_packet(0x20, 0x3333);
    ps2::vu::kick(1, 0x20);
    check(ps2::gs::g_state.ctx[0].frame == 0x3333, "the kick after a memory of empty tags sets FRAME_1");

    // A packet that wraps round the end of the memory but ends is one packet, as before.
    put(1023, tag(1, true, 1), 0xE);
    put(0, 0x4444, 0x4C);
    ps2::vu::kick(1, 1023);
    check(ps2::gs::g_state.ctx[0].frame == 0x4444, "a packet across the end of the memory is still read whole");

    // The report of a packet without an end: counted, and the tags it walks through are described (what the log of a player shows).
    const std::uint64_t before = ps2::vu::runaway_kicks();
    std::memset(ps2::g_vu1_mem, 0, sizeof ps2::g_vu1_mem);
    put(0x300, tag(2, false, 1), 0xF);       // a real tag without EOP, 2 qwords of data
    put(0x303, tag(0x7FFF, true, 1), 0xF);   // where the next tag should be lies something that is none
    ps2::vu::kick(1, 0x300);
    check(ps2::vu::runaway_kicks() == before + 1, "a packet that does not end is counted once");
    frame_packet(0x10, 0x5555);
    ps2::vu::kick(1, 0x10);
    check(ps2::vu::runaway_kicks() == before + 1, "a packet that ends is not counted");
    const std::string walk = ps2::vu::describe_packet(0x300);
    check(walk.find("tag at 0x300") != std::string::npos && walk.find("NLOOP 2 EOP 0") != std::string::npos, "the walk shows the first tag");
    check(walk.find("next tag at 0x303") != std::string::npos, "the first tag says where the next one lies");
    check(walk.find("tag at 0x303") != std::string::npos && walk.find("NLOOP 32767 EOP 1") != std::string::npos && walk.find("past the end of the memory") != std::string::npos,
          "the second tag is shown with its size, which runs past the memory");
    check(ps2::vu::describe_packet(0x10).find("EOP 1") != std::string::npos && ps2::vu::describe_packet(0x10).find("tag at 0x011") == std::string::npos,
          "the walk of a packet that ends stops at its EOP tag");
    std::fputs(walk.c_str(), stderr);

    // An A+D write to an address that is no register (0x30), and the PACKED descriptor 0xB: both counted and shown by stats().
    put(0x40, tag(1, true, 2), 0xBE);
    put(0x41, 0, 0x30);
    put(0x42, 0, 0);
    ps2::vu::kick(1, 0x40);
    ps2::gif::stats();

    if (failed) { std::fprintf(stderr, "%d checks failed\n", failed); return 1; }
    std::puts("gif runaway: all checks passed");
    return 0;
}
