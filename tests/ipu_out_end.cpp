// No game data. #146 (Harry Potter and the Prisoner of Azkaban, USA): the EA logo film stops at its first B-picture. From the
// tester's PS2_TRACE_IPU / PS2_TRACE_DMA run: at start-up the game decodes a 512 x 512 logo picture with one IDEC (1,024 macroblocks,
// 65,536 quadwords of RGB32, one more than a DMA count holds); it gives the IDEC first and starts fromIPU behind it for the first half
// (32,768 quadwords). The second half is the job of its fromIPU handler. Here that handler never ran for the logo: the transfer ended
// inside the CHCR write that started it (the whole picture was already waiting in the IPU), before the game turned the channel's
// interrupt on. Much later, after the film's first colour conversion (CSC of 1,023 + 1 macroblocks, whose two ends the handler did
// get), the handler started the stale second half (0x01EDEF80, 32,768 quadwords), which no IPU command ever fills; at the first
// B-picture the player waits for it for good.
//
// What this checks, with CSC standing in for IDEC (both leave decoded macroblocks in the IPU's output for fromIPU):
// 1. The usual order is unchanged: fromIPU started first, then the command; the transfer ends at the command (the per-macroblock BDEC
//    flow of the film and its CSC).
// 2. The logo's order: the command first, then fromIPU for half of the output. The data is in memory at once except the last quadword;
//    the channel is still running, so the program's acknowledge (D_STAT CIS3) and EnableDmac(3) right behind the start come before
//    its end. The end comes 2 cycles a quadword after the start, and the handler runs then.
// 3. The handler's second half (started again with the rest waiting) ends the same way; then the IPU is idle and raises its interrupt,
//    and the picture in memory is the same as the usual order gives.
// 4. A state saved while an end is held gives it back when loaded.
#include "hw.hpp"
#include "kernel.hpp"
#include "ps2/cpu.hpp"
#include "ps2/generated.hpp"
#include "ps2/memory.hpp"
#include "ps2/system.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

namespace {
namespace hw = ps2::hw;
namespace fs = std::filesystem;

int failed = 0;
void check(bool ok, const char *message) {
    if (!ok) { ++failed; std::fprintf(stdout, "FAIL: %s\n", message); }
}

constexpr std::uint32_t kEie = 0x10000u, kIe = 0x1u;
constexpr std::uint32_t kCaller = 0x00100000u;
constexpr std::uint32_t kDStat = 0x1000E010u, kDCtrl = 0x1000E000u, kIntcStat = 0x1000F000u;
constexpr std::uint32_t kD3Chcr = 0x1000B000u, kD3Madr = 0x1000B010u, kD3Qwc = 0x1000B020u;
constexpr std::uint32_t kD4Chcr = 0x1000B400u, kD4Madr = 0x1000B410u, kD4Qwc = 0x1000B420u;
constexpr std::uint32_t kIpuCmd = 0x10002000u, kIpuCtrl = 0x10002010u;
constexpr std::uint32_t kStart = 0x100u;
constexpr unsigned kFromIpu = 3;

constexpr std::uint32_t kMacroblocks = 2, kInQwc = kMacroblocks * 24, kOutQwc = kMacroblocks * 64, kHalf = kOutQwc / 2;
constexpr std::uint32_t kIn = 0x00130000u, kRef = 0x00140000u, kOut = 0x00150000u, kOut2 = 0x00160000u;

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

// Lets `cycles` of the processor's time pass and runs the events that came due.
void advance(int cycles) {
    ps2::set_budget(ps2::g_cpu, cycles);
    ps2::g_cpu.budget = 0;
    ps2::preempt(ps2::g_cpu);
}

bool running(std::uint32_t chcr) { return (hw::read32(chcr) & kStart) != 0; }
bool status_set(unsigned ch) { return (hw::read32(kDStat) >> ch) & 1u; }
bool ipu_busy() { return (hw::read32(kIpuCtrl) & 0x80000000u) != 0; }

// How often the handler of a channel ran, from the interrupt report (the trace is on).
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

// Two macroblocks of 4:2:0 samples (256 Y, 64 Cb, 64 Cr each), a pattern with every value in it.
void make_macroblocks() {
    for (std::uint32_t i = 0; i < kInQwc * 16; ++i) ps2::g_ram[kIn + i] = static_cast<std::uint8_t>((i * 37u + (i >> 7)) & 0xFFu);
}

void reset_ipu() {
    hw::write32(kIpuCtrl, 0x40000000u);  // RST
    hw::write32(kIpuCmd, 0x00000000u);   // BCLR 0
}

void feed_input() {
    hw::write32(kD4Madr, kIn);
    hw::write32(kD4Qwc, kInQwc);
    hw::write32(kD4Chcr, kStart);  // normal mode, from memory
}

void start_from_ipu(std::uint32_t madr, std::uint32_t qwc) {
    hw::write32(kD3Madr, madr);
    hw::write32(kD3Qwc, qwc);
    hw::write32(kD3Chcr, kStart);
}

void csc(std::uint32_t count) { hw::write32(kIpuCmd, 0x70000000u | count); }  // CSC to RGB32, no dither

bool same(std::uint32_t a, std::uint32_t b, std::uint32_t bytes) { return std::memcmp(ps2::g_ram + a, ps2::g_ram + b, bytes) == 0; }

bool zero(std::uint32_t a, std::uint32_t bytes) {
    for (std::uint32_t i = 0; i < bytes; ++i)
        if (ps2::g_ram[a + i]) return false;
    return true;
}

// 1. fromIPU first, then the command: it ends at the command.
void usual_order() {
    reset_ipu();
    feed_input();
    start_from_ipu(kRef, kOutQwc);
    check(running(kD3Chcr), "usual order: fromIPU waits for output before the command");
    csc(kMacroblocks);
    check(!running(kD3Chcr) && hw::read32(kD3Qwc) == 0, "usual order: fromIPU has ended when the command write returns");
    check(status_set(kFromIpu), "usual order: its end is raised at once");
    check(!ipu_busy(), "usual order: the IPU is idle");
    check(!zero(kRef, kOutQwc * 16), "usual order: the picture is in memory");
    hw::write32(kDStat, 1u << kFromIpu);  // acknowledge
}

// 2. and 3. The logo's order.
void logo_order() {
    reset_ipu();
    feed_input();
    csc(kMacroblocks);
    check(ipu_busy(), "logo order: the IPU holds the converted picture, nothing takes it yet");
    hw::write32(kIntcStat, 1u << 8);  // acknowledge an IPU interrupt from before

    start_from_ipu(kOut, kHalf);
    check(running(kD3Chcr) && hw::read32(kD3Qwc) == 1, "logo order: the first half is still running after its start, one quadword left");
    check(same(kOut, kRef, (kHalf - 1) * 16), "logo order: all but its last quadword are in memory at once");
    check(!status_set(kFromIpu), "logo order: no end raised at the start");

    // The program right behind the start: acknowledge what may be pending, turn the channel's interrupt on.
    hw::write32(kDStat, 1u << kFromIpu);
    syscall_with(0x16, kFromIpu);  // EnableDmac(3)
    safe_point();
    check(ran(kFromIpu) == 0, "logo order: nothing to run yet");

    advance(2 * static_cast<int>(kHalf) - 1);
    check(running(kD3Chcr), "logo order: one cycle before its bus time the first half still runs");
    advance(1);
    check(!running(kD3Chcr) && hw::read32(kD3Qwc) == 0, "logo order: the first half ends 2 cycles a quadword after its start");
    check(same(kOut, kRef, kHalf * 16), "logo order: its last quadword is in memory");
    safe_point();
    check(ran(kFromIpu) == 1, "logo order: the handler ran for the first half (it was lost before)");
    check(ipu_busy(), "logo order: the IPU still holds the second half");

    // What the game's handler does: the second half, behind the first.
    start_from_ipu(kOut + kHalf * 16, kHalf);
    check(running(kD3Chcr), "logo order: the second half runs after its start");
    advance(2 * static_cast<int>(kHalf));
    check(!running(kD3Chcr), "logo order: the second half ends");
    safe_point();
    check(ran(kFromIpu) == 2, "logo order: the handler ran for the second half");
    check(!ipu_busy(), "logo order: the IPU is idle after the whole picture went out");
    check((hw::read32(kIntcStat) >> 8) & 1u, "logo order: the IPU raised its interrupt");
    check(same(kOut, kRef, kOutQwc * 16), "logo order: the picture in memory is the one the usual order gives");
    syscall_with(0x17, kFromIpu);  // DisableDmac(3)
}

// 4. A state saved with an end held.
void saved_while_held() {
    reset_ipu();
    feed_input();
    csc(kMacroblocks);
    start_from_ipu(kOut2, kOutQwc);
    check(running(kD3Chcr), "state: the transfer is held when the state is saved");
    const fs::path file = fs::temp_directory_path() / ("ps2_ipu_out_end_" + std::to_string(std::rand()) + ".state");
    check(ps2::save_state(file.string()), "state: saved");
    advance(2 * static_cast<int>(kOutQwc));
    check(!running(kD3Chcr), "state: the held end comes in the run that saved it");
    check(ps2::load_state(file.string()), "state: loaded");
    std::error_code ignored;
    fs::remove(file, ignored);
    check(running(kD3Chcr) && hw::read32(kD3Qwc) == 1, "state: after loading, the transfer is held again with one quadword left");
    advance(2 * static_cast<int>(kOutQwc));
    check(!running(kD3Chcr) && !ipu_busy(), "state: after loading, the held end comes (its event got its action back)");
    check(same(kOut2, kRef, kOutQwc * 16), "state: the picture is complete");
}

}  // namespace

int main() {
    ps2::boot(0x00100000u, {});
    ps2::kernel::g_trace_irq = true;
    hw::write32(kDCtrl, 0xB3u);  // the tester's D_CTRL: DMAE, RELE, stall source fromIPU, stall drain GIF
    syscall_with(0x12, kFromIpu, ps2::kHandlerReturn, 0, 0xBE);  // AddDmacHandler(3, handler, 0, arg): the game's arg
    ps2::g_cpu.cop0[12] = kEie | kIe;
    make_macroblocks();
    usual_order();
    logo_order();
    saved_while_held();
    std::printf("ipu fromIPU end: %d failed\n", failed);
    return failed ? 1 : 0;
}
