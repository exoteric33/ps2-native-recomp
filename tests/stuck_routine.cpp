// No game data: a game routine the kernel runs to the end (here an INTC handler) that never returns is named when the run ends, and in
// the F9 report while it runs (issue #127). A handler that returns leaves no line. The "game code" is two C++
// functions registered as a code unit: one returns at once, the other loops on its own address and only eats game time, as a handler
// that waits for something that cannot come while it runs. A second thread plays the window: it reads the F9 line, then closes the run.
// No game needed: ninja ps2stuckroutinetest.
#include "hw.hpp"
#include "kernel.hpp"
#include "ps2/cpu.hpp"
#include "ps2/generated.hpp"
#include "ps2/system.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

namespace {
namespace hw = ps2::hw;

int failed = 0;
void check(bool ok, const char *message) {
    if (!ok) ++failed;
    std::printf("  %s %s\n", ok ? "ok  " : "FAIL", message);
}

constexpr std::uint32_t kEie = 0x10000u, kIe = 0x1u;
constexpr std::uint32_t kCaller = 0x00100000u;
constexpr std::uint32_t kBase = 0x01000000u, kSize = 0x100u;
constexpr std::uint32_t kReturns = kBase + 0x10u, kForever = kBase + 0x20u, kLoop = kBase + 0x28u;

std::atomic<long> g_spins{0};
void unit_fn(ps2::Cpu &c, std::uint32_t entry, std::uint32_t) {
    if (entry == 1) {  // returns at once
        c.pc = c.r[31].w[0];
        return;
    }
    // never returns: the "loop" at kLoop, a millisecond of game time per pass
    c.budget -= 294912;
    g_spins.fetch_add(1, std::memory_order_relaxed);
    c.pc = kLoop;
}
std::uint16_t g_entries[kSize / 4];
ps2::UnitInfo g_unit{kBase, kSize, unit_fn, g_entries};

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

void take() {
    ps2::Cpu &c = ps2::g_cpu;
    c.pc = kCaller + 4;
    ps2::kernel::service(c);
    c.pc &= ~ps2::kUnwind;
}

std::size_t g_read = 0;
std::string since() {
    std::fflush(stderr);
    std::string all;
    if (FILE *f = std::fopen("stuck_routine_test.log", "rb")) {
        char buf[4096];
        std::size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) all.append(buf, n);
        std::fclose(f);
    }
    std::string part = all.substr(g_read < all.size() ? g_read : all.size());
    g_read = all.size();
    return part;
}

bool has(const std::string &text, const char *needle) { return text.find(needle) != std::string::npos; }

}  // namespace

int main() {
    if (!std::freopen("stuck_routine_test.log", "wb", stderr)) { std::printf("cannot redirect stderr\n"); return 1; }
    ps2::Cpu &c = ps2::g_cpu;
    hw::reset();
    c.cop0[12] = kEie | kIe;
    g_entries[(kReturns - kBase) / 4] = 1;
    g_entries[(kForever - kBase) / 4] = 2;
    g_entries[(kLoop - kBase) / 4] = 2;
    ps2::register_units(&g_unit, 1);
    check(ps2::has_code(kReturns) && ps2::has_code(kForever), "the two handlers are code the dispatcher finds");

    // 1. a handler that returns: no line, now or at the end
    syscall_with(0x10, 9, kReturns, -1, 0);  // AddIntcHandler(TIMER0)
    syscall_with(0x14, 9);                   // EnableIntc(9)
    hw::intc_raise(9);
    take();
    check(ps2::kernel::routine_report("").empty(), "after a handler returned, the F9 report has no routine line");
    since();
    ps2::kernel::dump();
    check(!has(since(), "stuck in a game routine"), "after a handler returned, the closing lines name no routine");

    // 2. a DMAC handler that never returns; the "window" reads the F9 line while it runs, then closes the run
    syscall_with(0x12, 5, kForever, -1, 0);  // AddDmacHandler(SIF0)
    syscall_with(0x16, 5);                   // EnableDmac(5)
    std::string live;
    std::thread window([&] {
        while (g_spins.load(std::memory_order_relaxed) < 1000) std::this_thread::yield();
        live = ps2::kernel::routine_report("    ");
        ps2::halt("window closed");
    });
    hw::dmac_raise(5);
    take();
    window.join();
    std::printf("  F9 line: %s", live.c_str());
    check(has(live, "the EE is inside a game routine: the DMAC 5 (SIF0) handler at 0x01000020 is running since frame"), "the F9 line names the handler while it runs");
    check(has(live, "pc 0x0100002"), "the F9 line has the pc inside the loop");
    check(ps2::kernel::routine_report("").empty(), "after the halt broke the handler off, the live line is gone");
    since();
    ps2::kernel::dump();
    const std::string out = since();
    const std::size_t at = out.find("[ee] stuck in a game routine");
    std::printf("  closing line: %s", at == std::string::npos ? "(none)\n" : out.substr(at, out.find('\n', at) + 1 - at).c_str());
    check(has(out, "[ee] stuck in a game routine: the DMAC 5 (SIF0) handler at 0x01000020 had not returned when the run ended: running for "),
          "the closing lines name the handler that never returned");
    check(has(out, "s of game time); pc 0x01000028 ra 0x"), "the closing line has the game time, pc and ra");

    std::printf("stuck routine: %d failed\n", failed);
    return failed ? 1 : 0;
}
