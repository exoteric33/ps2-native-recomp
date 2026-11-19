// No game data: the interrupt trace (PS2_TRACE_INTC) says what the EE interrupt side does. Two INTC handlers on the
// vertical blank where the first one stops the chain, one DMAC handler, a masked cause, a cause with interrupts off, a cause with no
// handler, direct register writes, and the report text (F9 and end of run). The handlers here are kHandlerReturn (they return at once
// with whatever is in register 2 when the interrupt is taken, which the test sets), so what each one "answered" is under the test's control.
#include "hw.hpp"
#include "kernel.hpp"
#include "ps2/cpu.hpp"
#include "ps2/generated.hpp"
#include "ps2/system.hpp"

#include <cstdio>
#include <string>

namespace {
namespace hw = ps2::hw;

int failed = 0;
void check(bool ok, const char *message) {
    if (!ok) { ++failed; std::fprintf(stdout, "FAIL: %s\n", message); }
}

constexpr std::uint32_t kEie = 0x10000u, kIe = 0x1u;
constexpr std::uint32_t kCaller = 0x00100000u;

std::int64_t syscall_with(std::uint32_t n, std::uint32_t a0, std::uint32_t a1 = 0, std::uint32_t a2 = 0, std::uint32_t a3 = 0) {
    ps2::Cpu &c = ps2::g_cpu;
    c.r[3].sd[0] = n;
    c.r[4].sd[0] = a0;
    c.r[5].sd[0] = a1;
    c.r[6].sd[0] = a2;
    c.r[7].sd[0] = a3;
    c.pc = kCaller + 4;
    ps2::syscall(c, kCaller);
    return c.r[2].sd[0];
}

// Takes what is pending, with register 2 holding `answer` (what every handler "returns").
void take(std::int32_t answer) {
    ps2::Cpu &c = ps2::g_cpu;
    c.r[2].sd[0] = answer;
    c.pc = kCaller + 4;
    ps2::kernel::service(c);
    c.pc &= ~ps2::kUnwind;
}

std::string g_log;
std::size_t g_read = 0;
// What the trace wrote since the last call (stderr is a file for the whole test).
std::string since() {
    std::fflush(stderr);
    FILE *f = std::fopen("irq_trace_test.log", "rb");
    std::string all;
    if (f) {
        char buf[4096];
        std::size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) all.append(buf, n);
        std::fclose(f);
    }
    std::string part = all.substr(g_read < all.size() ? g_read : all.size());
    g_read = all.size();
    g_log += part;
    return part;
}

bool has(const std::string &text, const char *needle) { return text.find(needle) != std::string::npos; }

}  // namespace

int main() {
    if (!std::freopen("irq_trace_test.log", "wb", stderr)) { std::printf("cannot redirect stderr\n"); return 1; }
    ps2::Cpu &c = ps2::g_cpu;
    hw::reset();
    ps2::kernel::g_trace_irq = true;
    c.cop0[12] = kEie | kIe;

    // 1. the installs: two INTC handlers on the vertical blank (the second goes in front), one DMAC handler
    const int a = static_cast<int>(syscall_with(0x10, 2, ps2::kHandlerReturn, -1, 0xAAAA));  // at the end
    const int b = static_cast<int>(syscall_with(0x10, 2, ps2::kHandlerReturn, 0, 0xBBBB));   // in front: runs first
    const int d = static_cast<int>(syscall_with(0x12, 5, ps2::kHandlerReturn, -1, 0xCCCC));
    std::string out = since();
    check(has(out, "AddIntcHandler: INTC 2 (VBLANK start) handler id 1") && has(out, "place 1 of 1"), "first AddIntcHandler is said, place 1 of 1");
    check(has(out, "in front of the others (next=0): place 1 of 2"), "the handler added with next=0 goes in front, said so");
    check(has(out, "AddDmacHandler: DMAC 5 (SIF0) handler id 3"), "AddDmacHandler is said");
    check(a == 1 && b == 2 && d == 3, "handler ids are 1, 2, 3");

    // 2. enable the causes
    syscall_with(0x14, 2);  // EnableIntc(2)
    syscall_with(0x16, 5);  // EnableDmac(5)
    out = since();
    check(has(out, "EnableIntc(2 VBLANK start): was off, now on") && has(out, "2 handlers for it"), "EnableIntc says it changed the mask and counts the handlers");
    check(has(out, "EnableDmac(5 SIF0): was off, now on") && has(out, "1 handler for it"), "EnableDmac says the same");
    syscall_with(0x14, 2);
    check(has(since(), "was already on"), "a second EnableIntc says it was already on");

    // 3. the vertical blank fires; the first handler (id 2, in front) answers -1, so the chain stops and id 1 does not run
    hw::intc_raise(2);
    take(-1);
    out = since();
    check(has(out, "INTC 2 (VBLANK start) fires (1): enabled and interrupts are on"), "the fire is said with its fate");
    check(has(out, "handler 1 of 2: id 2 0x") && has(out, "arg 0x0000BBBB") && has(out, "returned 0xFFFFFFFF: chain stops here, the later handlers do not run"),
          "the first handler's call says it stopped the chain");
    check(!has(out, "handler 2 of 2"), "the second handler is not called after the chain stopped");

    // 4. the same with an answer of 0: both run, in order
    hw::intc_raise(2);
    take(0);
    out = since();
    check(has(out, "handler 1 of 2: id 2") && has(out, "chain goes on") && has(out, "handler 2 of 2: id 1") && has(out, "last in the chain"), "with answer 0 both handlers run, in order");
    check(has(out, "the answer changed"), "a changed answer of a handler is said again");

    // 5. the DMAC handler
    hw::dmac_raise(5);
    take(0);
    out = since();
    check(has(out, "DMAC 5 (SIF0) fires (1): enabled and interrupts are on") && has(out, "DMAC 5 (SIF0) handler 1 of 1: id 3 0x") && has(out, "arg 0x0000CCCC"),
          "the DMAC handler's fire and call are said");

    // 6. a cause that is masked
    syscall_with(0x15, 2);  // DisableIntc(2)
    since();
    hw::intc_raise(2);
    out = since();
    check(has(out, "INTC 2 (VBLANK start) fires") && has(out, "masked: its mask bit is off"), "a masked cause says why it does not run");

    // 7. interrupts off
    syscall_with(0x14, 2);
    since();
    c.cop0[12] = kIe;  // EIE off
    hw::intc_raise(2);
    out = since();
    check(has(out, "interrupts are off in the Status register"), "a fire with interrupts off says so");
    std::string report = ps2::kernel::interrupt_report("> ");
    check(has(report, "EE interrupts are OFF") && has(report, "EIE is 0"), "the report says interrupts are off and which bit");
    check(has(report, "INTC_STAT 0x0004") && has(report, "waiting to run: 2 VBLANK start"), "the report lists the status bit that waits");
    c.cop0[12] = kEie | kIe;
    take(0);  // delivered now
    since();

    // 8. a cause with no handler
    syscall_with(0x14, 3);  // EnableIntc(3), nothing installed for vblank end
    since();
    hw::intc_raise(3);
    take(0);
    out = since();
    check(has(out, "INTC 3 (VBLANK end): no handler installed for it, acknowledged and dropped"), "a cause with no handler says so");

    // 9. direct register writes
    hw::write32(0x1000F010u, 1u << 4);  // INTC_MASK toggle: VIF0 on
    out = since();
    check(has(out, "direct write INTC_MASK") && has(out, "turned on 4 VIF0"), "a direct INTC_MASK write is said");
    hw::dmac_raise(1);
    hw::write32(0x1000E010u, 1u << 1);  // D_STAT: acknowledge channel 1
    out = since();
    check(has(out, "direct write D_STAT") && has(out, "acknowledged 1 VIF1"), "a direct D_STAT acknowledge is said");

    // 10. the report names the handlers and their counts; removal
    report = ps2::kernel::interrupt_report("> ");
    check(has(report, "EE interrupts are ON") && has(report, "IE 1, EIE 1"), "the report says interrupts are on");
    check(has(report, "INTC 2 (VBLANK start), in the order they run: [id 2 0x") && has(report, "[id 1 0x"), "the report lists the INTC handlers in order");
    check(has(report, "DMAC 5 (SIF0), in the order they run: [id 3 0x") && has(report, "ran 1 times"), "the report lists the DMAC handler with its count");
    check(has(report, "INTC 2 (VBLANK start): fired 4 times"), "the report counts the fires per cause");
    check(syscall_with(0x11, 0, 2) == 2, "RemoveIntcHandler returns the id");
    out = since();
    check(has(out, "RemoveIntcHandler: id 2 removed") && has(out, "id 1 cause 2"), "the removal is said, with the list that is left");
    syscall_with(0x11, 0, 99);
    check(has(since(), "RemoveIntcHandler: id 99 not found"), "removing a handler that is not there is said");

    // 10b. bounded: a game that switches a channel on and off 600 times makes a few lines, not 1200
    since();
    for (int i = 0; i < 600; ++i) {
        syscall_with(0x16, 8);
        syscall_with(0x17, 8);
    }
    out = since();
    std::size_t lines = 0;
    for (char ch : out) lines += ch == '\n';
    check(lines > 0 && lines < 40, "1200 Enable/Disable calls on one channel make fewer than 40 lines");
    check(has(ps2::kernel::interrupt_report(""), "Enable/DisableDmac 1201"), "the report counts the calls that were not printed");

    // 11. off by default: nothing is written with the switch off
    ps2::kernel::g_trace_irq = false;
    hw::intc_raise(2);
    take(0);
    syscall_with(0x10, 2, ps2::kHandlerReturn, -1, 1);
    check(since().empty(), "with the trace off nothing is written");
    check(has(ps2::kernel::interrupt_report(""), "PS2_TRACE_INTC=1"), "the report says how to get the counts when the trace is off");

    std::fflush(stderr);
    std::printf("irq trace: %d failed\n", failed);
    if (failed) std::printf("--- the log ---\n%s\n", g_log.c_str());
    return failed ? 1 : 0;
}
