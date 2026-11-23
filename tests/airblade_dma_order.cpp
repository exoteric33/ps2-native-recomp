// No game data: the start-up sequence of issue #110 (Airblade, USA), black screen. From the tester's PS2_TRACE_DMA and PS2_TRACE_INTC
// runs: the graphics library installs one handler for DMA channels 1 (VIF1) and 2 (GIF) and enables both; a GIF transfer of 17 quadwords
// starts and ends; the program writes D_STAT 0x0000FF1F (acknowledge every channel but the SIF ones) and then D_STAT & 0xFF1F0000, which
// toggles the enabled VIF1 and GIF masks off (0x00060000); then a small VIF1 chain starts (NOPs, STCYCL, OFFSET, BASE, ITOP, STMOD,
// MSKPATH3, STMASK, FLUSHE: a path reset); its end finds the mask off. The handler ran for the GIF end that was pending from before the
// handlers were installed, and never again.
//
// What this checks:
// 1. When each end comes. Our DMAC ends a VIF1 chain or a GIF transfer inside the CHCR write that starts it (the data is moved at once), so
//    no end can come later than the program's next instruction; a console ends it later (a channel ends after its last quadword is taken),
//    never earlier. So the VIF1 end cannot be "too late" here: in the tester's run it already finds the mask off because the program
//    turned the mask off before it started the chain, and that is the same on the console.
// 2. MSKPATH3 does not hold either end: a GIF transfer started behind a VIF1 chain that masks PATH3 still ends at its own start (the GIF
//    keeps its data until the mask drops), and the VIF1 chain ends at its start.
// 3. A masked VIF1 end is not lost: its bit waits in D_STAT, and enabling the channel runs the handler.
// 4. The GIF end with the channel enabled and interrupts on. On the console the processor takes that interrupt as soon as the channel
//    ends, so the program cannot acknowledge it before the handler has run. Here the handler waits for the next safe point (a backward
//    branch once the time slice is used up, EI, or a system call), and the program's D_STAT write in between clears the bit: the handler
//    call is lost. That is the one place where this sequence goes differently from the console. The fix is outside the DMA code (the
//    D_STAT write in hw.cpp would take what is pending first, like EI does), so until it is in, step 4 prints a "gap" line instead of
//    failing; with the fix, `kGapFixed` turns it into a check.
#include "hw.hpp"
#include "kernel.hpp"
#include "ps2/cpu.hpp"
#include "ps2/generated.hpp"
#include "ps2/memory.hpp"
#include "ps2/system.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {
namespace hw = ps2::hw;

constexpr bool kGapFixed = true;  // the D_STAT write takes a pending, enabled channel end first (hw.cpp), so step 4 is a check

int failed = 0;
void check(bool ok, const char *message) {
    if (!ok) { ++failed; std::fprintf(stdout, "FAIL: %s\n", message); }
}

constexpr std::uint32_t kEie = 0x10000u, kIe = 0x1u;
constexpr std::uint32_t kCaller = 0x00100000u;
constexpr std::uint32_t kDStat = 0x1000E010u, kDCtrl = 0x1000E000u;
constexpr std::uint32_t kD1Chcr = 0x10009000u, kD1Qwc = 0x10009020u, kD1Tadr = 0x10009030u;
constexpr std::uint32_t kD2Chcr = 0x1000A000u, kD2Madr = 0x1000A010u, kD2Qwc = 0x1000A020u;
constexpr std::uint32_t kGifPacket = 0x00130000u, kVifReset = 0x00131000u, kVifMask = 0x00132000u;

void syscall_with(std::uint32_t n, std::uint32_t a0, std::uint32_t a1 = 0, std::uint32_t a2 = 0, std::uint32_t a3 = 0) {
    ps2::Cpu &c = ps2::g_cpu;
    c.r[3].sd[0] = n;
    c.r[4].sd[0] = a0;
    c.r[5].sd[0] = a1;
    c.r[6].sd[0] = a2;
    c.r[7].sd[0] = a3;
    c.pc = kCaller + 4;
    ps2::syscall(c, kCaller);
    c.pc &= ~ps2::kUnwind;
}

// The next safe point: whatever is pending and enabled runs now.
void safe_point() {
    ps2::Cpu &c = ps2::g_cpu;
    c.pc = kCaller + 4;
    ps2::kernel::service(c);
    c.pc &= ~ps2::kUnwind;
}

bool busy(std::uint32_t chcr) { return (hw::read32(chcr) & 0x100u) != 0; }
bool status_set(unsigned ch) { return (hw::read32(kDStat) >> ch) & 1u; }
bool enabled(unsigned ch) { return (hw::read32(kDStat) >> (16 + ch)) & 1u; }

// How often the handler of a channel ran, from the interrupt report (the trace is on): "DMAC 2 (GIF), in the order they run: [id .. ran N times".
unsigned long ran(unsigned ch) {
    const std::string report = ps2::kernel::interrupt_report("");
    const std::string head = "DMAC " + std::to_string(ch) + " (";
    const std::size_t at = report.find(head);
    if (at == std::string::npos) return 0;
    const std::size_t line_end = report.find('\n', at);
    const std::size_t r = report.find("ran ", at);
    if (r == std::string::npos || r > line_end) return 0;
    return std::strtoul(report.c_str() + r + 4, nullptr, 10);
}

void put_words(std::uint32_t address, const std::uint32_t *words, std::size_t count) {
    std::memcpy(ps2::g_ram + address, words, count * 4);
}

// One GIF packet of 17 quadwords: a PACKED tag (NLOOP 16, EOP, one A+D register) and 16 writes of TEXFLUSH.
void make_gif_packet() {
    std::uint32_t q[17 * 4] = {};
    q[0] = 16u | 0x8000u;  // NLOOP 16, EOP
    q[1] = 1u << 28;       // FLG PACKED, NREG 1
    q[2] = 0xEu;           // REGS: A+D
    for (int i = 1; i < 17; ++i) q[i * 4 + 2] = 0x3Fu;  // A+D: data 0, register TEXFLUSH
    put_words(kGifPacket, q, 17 * 4);
}

// A VIF1 chain of one END tag and three quadwords of codes: the path reset the tester's run sends (MSKPATH3 with `mask`).
void make_vif_chain(std::uint32_t at, bool mask) {
    const std::uint32_t q[4 * 4] = {
        3u | (7u << 28), 0, 0, 0,                               // DMAtag: QWC 3, ID END (data follows the tag)
        0x00000000u, 0x00000000u, 0x01000404u, 0x02000000u,     // NOP, NOP, STCYCL 4/4, OFFSET 0
        0x03000000u, 0x04000000u, 0x05000000u, 0x06000000u | (mask ? 0x8000u : 0u),  // BASE 0, ITOP 0, STMOD 0, MSKPATH3
        0x20000000u, 0x00000000u, 0x10000000u, 0x00000000u,     // STMASK + its word, FLUSHE, NOP
    };
    put_words(at, q, 4 * 4);
}

void start_gif() {
    hw::write32(kD2Madr, kGifPacket);
    hw::write32(kD2Qwc, 17);
    hw::write32(kD2Chcr, 0x101u);  // from memory, normal mode, STR
}

void start_vif(std::uint32_t chain) {
    hw::write32(kD1Qwc, 0);
    hw::write32(kD1Tadr, chain);
    hw::write32(kD1Chcr, 0x105u);  // from memory, chain mode, STR (the tester's CHCR)
}

void tester_sequence() {
    ps2::Cpu &c = ps2::g_cpu;
    // The library's set-up: one handler for both channels, both enabled, interrupts on.
    syscall_with(0x12, 1, ps2::kHandlerReturn, 0, 0x15D8);  // AddDmacHandler(1, handler, 0, arg)
    syscall_with(0x12, 2, ps2::kHandlerReturn, 0, 0x15D8);  // AddDmacHandler(2, handler, 0, arg)
    syscall_with(0x16, 1);                                   // EnableDmac(1)
    syscall_with(0x16, 2);                                   // EnableDmac(2)
    c.cop0[12] = kEie | kIe;
    safe_point();
    check(enabled(1) && enabled(2) && !status_set(1) && !status_set(2), "set-up: both channels enabled, nothing pending");

    // 1. The GIF transfer ends inside the write that starts it.
    start_gif();
    check(!busy(kD2Chcr), "the GIF transfer has ended when the CHCR write returns (our DMAC moves the data at once)");
    const bool gif_pending = status_set(2) && enabled(2);
    check(gif_pending || ran(2) == 1, "its end is raised at once");

    // 4. The program's next writes: acknowledge, then turn the enabled VIF1/GIF masks off.
    hw::write32(kDStat, 0x0000FF1Fu);
    const std::uint32_t toggle = hw::read32(kDStat) & 0xFF1F0000u;
    check(toggle == 0x00060000u, "D_STAT & 0xFF1F0000 is 0x00060000, the tester's second write");
    hw::write32(kDStat, toggle);
    check(!enabled(1) && !enabled(2), "the second write turns the VIF1 and GIF masks off");
    safe_point();
    const unsigned long gif_calls = ran(2);
    if (kGapFixed) {
        check(gif_calls == 1, "the GIF end's handler ran before the program's acknowledge (the console takes it when the channel ends)");
    } else if (gif_calls == 0) {
        std::fprintf(stdout, "gap: the GIF end was pending with the channel enabled and interrupts on, and the program's D_STAT acknowledge "
                             "cleared it before the next safe point: its handler never ran (on the console it runs first)\n");
    } else {
        std::fprintf(stdout, "the GIF end's handler ran before the acknowledge (%lu call): set kGapFixed\n", gif_calls);
    }

    // 1. The VIF1 chain, started after the masks went off, ends inside its own CHCR write: masked, as on the console.
    make_vif_chain(kVifReset, false);
    start_vif(kVifReset);
    check(!busy(kD1Chcr), "the VIF1 chain has ended when the CHCR write returns: its end is not late");
    check(status_set(1) && !enabled(1), "its end waits in D_STAT under the mask the program turned off");
    safe_point();
    check(ran(1) == 0, "a masked end does not run the handler");

    // 3. Not lost: enabling the channel again runs it.
    syscall_with(0x16, 1);  // EnableDmac(1)
    safe_point();
    check(ran(1) == 1 && !status_set(1), "EnableDmac(1) later runs the VIF1 handler for the end that waited");
    syscall_with(0x16, 2);  // EnableDmac(2): nothing waits for the GIF any more
    safe_point();
    check(ran(2) == gif_calls, "EnableDmac(2) finds no GIF end left (it was acknowledged)");
}

// 2. A GIF transfer behind a VIF1 chain that masks PATH3: neither end waits for the other or for the mask.
void path3_mask() {
    const unsigned long vif_before = ran(1), gif_before = ran(2);
    make_vif_chain(kVifMask, true);
    start_vif(kVifMask);
    check(!busy(kD1Chcr), "the VIF1 chain that masks PATH3 ends at its start");
    safe_point();
    check(!status_set(1) && ran(1) == vif_before + 1, "and its handler runs at the next safe point");
    check((hw::read32(0x10003020u) & 2u) != 0, "GIF_STAT.M3P is set: PATH3 is masked");
    start_gif();
    check(!busy(kD2Chcr), "a GIF transfer started under the PATH3 mask still ends at its start (the GIF holds the data)");
    safe_point();
    check(ran(2) == gif_before + 1, "and its handler runs at the next safe point");
    make_vif_chain(kVifReset, false);
    start_vif(kVifReset);
    safe_point();
    check((hw::read32(0x10003020u) & 2u) == 0 && ran(1) == vif_before + 2, "the reset chain drops the mask and ends at its start");
}

}  // namespace

int main() {
    ps2::boot(0x00100000u, {});
    ps2::kernel::g_trace_irq = true;
    hw::write32(kDCtrl, 0x103u);  // DMAE, RELE, RCYC: the tester's D_CTRL
    make_gif_packet();
    tester_sequence();
    path3_mask();
    std::printf("airblade dma order: %d failed\n", failed);
    return failed ? 1 : 0;
}
