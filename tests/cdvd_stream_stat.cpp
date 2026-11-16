// No game data: sceCdStStat, the stream command 6 of the N-command server 0x80000595 (public issue 44, HSX Hypersonic Xtreme, whose log
// shows 271 031 calls to that server in 4 034 frames while its intro movie waits). In ps2sdk's cdvdstm.c the command answers how many
// sectors the stream buffer holds; the runtime answered the call's lbn field, which sceCdStStat sets to 0, so a game that waits for data
// in the buffer before it reads waited for ever. Here the EE side is played with made-up packets (the call of tests/fileio_layouts.cpp):
// sceCdStInit (command 5), sceCdStStart (1), sceCdStStat (6), sceCdStStop (3).
// Build: ninja ps2cdvdstreamstattest, run build/.../ps2cdvdstreamstattest (prints "cdvd stream stat: 0 failed").
#include "iop/iop.hpp"
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
    if (!ok) { ++failed; std::printf("FAIL: %s\n", message); }
}

constexpr std::uint32_t kXor = 0xACACACACu, kNcmd = 0x80000595u;
constexpr std::uint32_t kArgs = 0x1E00200, kRecv = 0x1E00300, kList = 0x1E00000, kPacket = 0x1E00100;
constexpr std::uint32_t kSentinel = 0xFEEDFEEDu;

// sceCdStream's call: five words (lbn, sectors, buffer, command, read mode); the answer is one word.
std::uint32_t stream_call(std::uint32_t lbn, std::uint32_t sectors, std::uint32_t buffer, std::uint32_t command) {
    const std::uint32_t args[5] = {lbn, sectors, buffer, command, 0};
    std::memcpy(ps2::g_ram + kArgs, args, sizeof args);
    ps2::st32(kRecv, kSentinel);
    std::uint32_t packet[14] = {56, 0, 0x8000000Au, 0, 0, 0, 0, 0, 9, sizeof args, kRecv, 4, 0, kNcmd ^ kXor};
    std::memcpy(ps2::g_ram + kPacket, packet, sizeof packet);
    const std::uint32_t list[8] = {kArgs, 0xDEADBEF0u, sizeof args, 0, kPacket, 0x0001E000u, sizeof packet, 0};
    std::memcpy(ps2::g_ram + kList, list, sizeof list);
    sif::set_dma(kList, 2);
    iop::run(4096);
    return ps2::ld32(kRecv);
}
}  // namespace

int main() {
    iop::reset();
    check(stream_call(0, 0, 0, 6) == 0, "no stream yet: the buffer holds nothing");
    check(stream_call(80, 8, 0x100000, 5) == 1, "sceCdStInit (80 sectors in 8 banks) answers 1");
    check(stream_call(0, 0, 0, 6) == 0, "set up but not started: nothing in the buffer");
    check(stream_call(1000, 0, 0, 1) == 1, "sceCdStStart answers 1");
    check(stream_call(0, 0, 0, 6) == 70, "started: the buffer is full, 7 of its 8 banks of 10 sectors (this answered 0 before)");
    check(stream_call(0, 0, 0, 3) == 1, "sceCdStStop answers 1");
    check(stream_call(0, 0, 0, 6) == 0, "stopped: nothing in the buffer");
    check(stream_call(64, 2, 0x100000, 5) == 1, "sceCdStInit again: 64 sectors in 2 banks");
    check(stream_call(500, 0, 0, 1) == 1, "sceCdStStart answers 1");
    check(stream_call(0, 0, 0, 6) == 32, "two banks: one bank of 32 sectors full");
    std::printf("cdvd stream stat: %d failed\n", failed);
    return failed ? 1 : 0;
}
