// No game data: an EE sceSifSetDma whose transfer list, or whose source packet, lives in the scratchpad (0x70000000..0x70003FFF).
// NASCAR 06 (issue 96), NASCAR Thunder 2004 and Need for Speed: Most Wanted (issue 101) keep the list of the DMA call
// that sends a SIF command in the scratchpad. The runtime read the list through the main RAM mask, so a list at 0x700031C0 became
// RAM at 0x31C0 (zeros): "source 0, destination 0, size 0", which it took for the IOP reset transfer ("IOP reboot requested with
// ''", "carries command 0x00000000, not a reset"), and the real command never reached the IOP.
// The command is a user command nobody handles, so it is counted by sif::unhandled_commands(); that is how the test sees that it arrived.
#include "iop/kernel.hpp"
#include "iop/sif.hpp"
#include "ps2/memory.hpp"

#include <cstdio>
#include <cstring>

namespace {
namespace iop = ps2::iop;
namespace sif = ps2::sif;

int failed = 0;
void check(bool ok, const char *message) {
    if (!ok) { ++failed; std::fprintf(stderr, "FAIL: %s\n", message); }
}

constexpr std::uint32_t kRamPacket = 0x00120000, kRamList = 0x00121000;  // main RAM: a command packet, a transfer list
constexpr std::uint32_t kSprPacket = 0x70000200, kSprList = 0x700031C0;  // scratchpad: the same two things

// A 16-byte command header (size 16, no data, command id `cid`), as ps2sdk's _SifSendCmd builds it.
void make_packet(std::uint32_t at, std::uint32_t cid) {
    ps2::st32(at, 16u); ps2::st32(at + 4, 0); ps2::st32(at + 8, cid); ps2::st32(at + 12, 0);
}

// One transfer: `packet` to the IOP's command buffer.
void make_list(std::uint32_t at, std::uint32_t packet) {
    ps2::st32(at, packet); ps2::st32(at + 4, sif::get_reg(2)); ps2::st32(at + 8, 16); ps2::st32(at + 12, 0x44);
}
}

int main() {
    iop::reset();
    iop::run(8192);

    // Bytes that main RAM holds at the scratchpad offsets (what the old mask read): zeros, so the old code saw a transfer to address 0.
    std::memset(ps2::g_ram + (kSprList & 0x3FFFu), 0, 16);
    std::memset(ps2::g_ram + (kSprPacket & 0x3FFFu), 0, 16);

    // 1. The list in the scratchpad, the packet in main RAM: the command reaches the IOP.
    constexpr std::uint32_t kCidA = 0x00000007u;
    const std::uint64_t a = sif::unhandled_commands(kCidA);
    make_packet(kRamPacket, kCidA);
    make_list(kSprList, kRamPacket);
    check(sif::set_dma(kSprList, 1) != 0, "sceSifSetDma takes a list in the scratchpad");
    check(sif::unhandled_commands(kCidA) == a + 1, "a command whose transfer list is in the scratchpad reaches the IOP");

    // 2. The list in main RAM, the packet in the scratchpad.
    constexpr std::uint32_t kCidB = 0x00000009u;
    const std::uint64_t b = sif::unhandled_commands(kCidB);
    make_packet(kSprPacket, kCidB);
    make_list(kRamList, kSprPacket);
    sif::set_dma(kRamList, 1);
    check(sif::unhandled_commands(kCidB) == b + 1, "a command whose packet is in the scratchpad reaches the IOP");

    // 3. Both in the scratchpad.
    constexpr std::uint32_t kCidC = 0x0000000Bu;
    const std::uint64_t c = sif::unhandled_commands(kCidC);
    make_packet(kSprPacket, kCidC);
    make_list(kSprList, kSprPacket);
    sif::set_dma(kSprList, 1);
    check(sif::unhandled_commands(kCidC) == c + 1, "a command with list and packet in the scratchpad reaches the IOP");

    iop::run(8192);
    if (failed) { std::fprintf(stderr, "%d check(s) failed\n", failed); return 1; }
    std::puts("sif scratchpad transfers: all checks passed");
    return 0;
}
