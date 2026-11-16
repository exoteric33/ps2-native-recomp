// No game data: an interrupt that is pending when the program turns interrupts back on is taken at once, as on the console, not at
// the next preemption point some instructions later. Issue 100 (GTA Vice City, black screen after the opening films): the
// game's DMA queue kick calls EnableDmac(1) inside DI ... EI while channel 1's status bit is still set from a transfer that ended under
// the mask; on the console the handler runs at the EI, here it ran only later, after the kick had read the state the handler changes,
// and the kick then started an empty queue slot that never finished. The handler here is kHandlerReturn (it returns at once), so
// "taken" shows as the channel's status bit being cleared (the kernel acknowledges the channel before it runs the handlers).
#include "hw.hpp"
#include "kernel.hpp"
#include "ps2/cpu.hpp"
#include "ps2/generated.hpp"
#include "ps2/system.hpp"

#include <cstdio>

namespace {
namespace hw = ps2::hw;

int failed = 0;
void check(bool ok, const char *message) {
    if (!ok) { ++failed; std::fprintf(stderr, "FAIL: %s\n", message); }
}

constexpr std::uint32_t kEie = 0x10000u, kIe = 0x1u;
constexpr std::uint32_t kCaller = 0x00100000u;  // where the "system call" instruction is said to be

void syscall_with(std::uint32_t n, std::uint32_t a0, std::uint32_t a1 = 0, std::uint32_t a2 = 0, std::uint32_t a3 = 0) {
    ps2::Cpu &c = ps2::g_cpu;
    c.r[3].sd[0] = n;
    c.r[4].sd[0] = a0;
    c.r[5].sd[0] = a1;
    c.r[6].sd[0] = a2;
    c.r[7].sd[0] = a3;
    c.pc = kCaller + 4;
    ps2::syscall(c, kCaller);
}

bool channel_pending(unsigned ch) { return (hw::dmac_pending() >> ch) & 1u; }

}  // namespace

int main() {
    ps2::Cpu &c = ps2::g_cpu;
    hw::reset();
    c.cop0[12] = kEie | kIe;
    syscall_with(0x12, 1, ps2::kHandlerReturn, 0, 0);  // AddDmacHandler(1, handler, 0, 0)
    syscall_with(0x12, 2, ps2::kHandlerReturn, 0, 0);  // AddDmacHandler(2, handler, 0, 0)

    // 1. EI with a pending, unmasked channel: the handler runs inside set_interrupts
    c.cop0[12] = kIe;                 // DI
    hw::dmac_raise(1);                // the transfer ended while the channel was masked: CIS1 set
    hw::dmac_set_mask(1, true);       // EnableDmac(1), still under DI
    check(channel_pending(1), "channel 1 is pending under DI");
    ps2::set_interrupts(c, true);     // EI
    check(!channel_pending(1), "EI takes the pending channel 1 interrupt at once");

    // 2. EI with nothing pending changes nothing; DI then EI with the channel masked does not run it
    ps2::set_interrupts(c, false);
    hw::dmac_set_mask(1, false);
    hw::dmac_raise(1);
    ps2::set_interrupts(c, true);
    check(!channel_pending(1), "a masked channel is not pending");
    hw::dmac_set_mask(1, true);       // unmasked outside the kernel's view with interrupts on: taken at the next safe point, not here
    check(channel_pending(1), "unmasking alone does not run the handler");
    ps2::kernel::service(c);
    check(!channel_pending(1), "the next safe point takes it");

    // 3. MTC0 to Status that sets EIE takes it too
    ps2::set_interrupts(c, false);
    hw::dmac_raise(1);
    ps2::mtc0(c, 12, kEie | kIe);
    check(!channel_pending(1), "MTC0 Status with EIE takes the pending interrupt");

    // 4. a system call that leaves an interrupt pending and enabled returns with the unwind bit, so the caller's next
    //    instruction runs after the handler (ERET on the console)
    hw::dmac_set_mask(2, false);
    hw::dmac_raise(2);
    syscall_with(0x16, 2);            // EnableDmac(2) with interrupts on
    check((c.pc & ps2::kUnwind) != 0, "EnableDmac with the channel's status bit set returns through the dispatcher");
    check((c.pc & ~ps2::kUnwind) == kCaller + 4, "and goes on after the system call");
    c.pc &= ~ps2::kUnwind;
    ps2::kernel::service(c);
    check(!channel_pending(2), "where the handler runs");
    syscall_with(0x16, 2);
    check((c.pc & ps2::kUnwind) == 0, "with nothing pending a system call returns as before");

    std::printf("ei pending: %d failed\n", failed);
    return failed ? 1 : 0;
}
