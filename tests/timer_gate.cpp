// No game data: the EE timers (Tn_COUNT, Tn_MODE, Tn_COMP) against the rules of the public EE User's Manual's timer chapter
// (#141, Ghost Recon 2, whose clock runs too fast while two TIMER0 handlers of the game run).
//
// What this checks:
// 1. A compare interrupt comes only when EQUF goes from 0 to 1: a handler that does not write the flag back gets one interrupt, a
//    handler that does gets one per match (the rate is unchanged for games that acknowledge).
// 2. The overflow interrupt follows the same rule.
// 3. ZRET and the flags work with both interrupt enables off: the counter never passes the compare value, EQUF and OVFF get set.
// 4. The V-blank gate, GATM 0: the counter stands still during the V-blank and goes on from the same value after it.
// 5. The V-blank gate, GATM 1: the counter restarts at 0 when the V-blank starts, so a compare interrupt comes once a frame.
// 6. The V-blank gate, GATM 2: the counter restarts when the V-blank ends.
// Build and run: ninja ps2timergatetest && ./ps2timergatetest
#include "hw.hpp"
#include "ps2/cpu.hpp"
#include "ps2/system.hpp"

#include <cstdint>
#include <cstdio>
#include <functional>

namespace {
namespace hw = ps2::hw;

int failed = 0;
void check(bool ok, const char *message) {
    if (!ok) { ++failed; std::fprintf(stdout, "FAIL: %s\n", message); }
}

constexpr std::uint32_t kT0 = 0x10000000u;
constexpr std::uint32_t kCount = 0x00, kMode = 0x10, kComp = 0x20;
constexpr std::uint32_t kCue = 0x80u, kZret = 0x40u, kCmpe = 0x100u, kOvfe = 0x200u, kEquf = 0x400u, kOvff = 0x800u;
constexpr std::uint32_t kGate = 0x4u, kGats = 0x8u;
constexpr unsigned kTimer0Bit = 1u << 9;
constexpr std::uint64_t kFrame = 294912000u / 60u;  // one NTSC frame in EE cycles (system.cpp frame_cycles)

std::uint32_t reg(std::uint32_t r) { return hw::read32(kT0 + r); }
void set(std::uint32_t r, std::uint32_t v) { hw::write32(kT0 + r, v); }

// Moves the clock to `at` (now() = at).
void clock_to(std::uint64_t at) {
    ps2::Cpu &c = ps2::g_cpu;
    const std::uint64_t t = ps2::now();
    if (at <= t) return;
    ps2::set_budget(c, static_cast<std::int64_t>(at - t));
    c.budget = 0;
}

// Runs the timer 0 and V-blank end events up to `until`, the way the runtime's event loop would; `on_irq` runs whenever the timer 0
// interrupt bit is set after an event (it plays the game's handler), and the bit is acknowledged.
int run_until(std::uint64_t until, const std::function<void()> &on_irq = [] {}) {
    int irqs = 0;
    for (;;) {
        const std::uint64_t timer = ps2::scheduled_at(hw::kEventTimer0), end = ps2::scheduled_at(hw::kEventVblankEnd);
        const bool timer_first = timer <= end;
        const std::uint64_t at = timer_first ? timer : end;
        if (at > until) break;
        clock_to(at);
        if (timer_first) {
            hw::event_action(hw::kEventTimer0, at)();
        } else {
            ps2::schedule(hw::kEventVblankEnd, UINT64_MAX, [] {});  // the event loop removes an event before it runs it
            hw::event_action(hw::kEventVblankEnd, at)();
        }
        if (hw::intc_stat_raw() & kTimer0Bit) {
            ++irqs;
            hw::write32(0x1000F000u, kTimer0Bit);
            on_irq();
        }
    }
    clock_to(until);
    return irqs;
}

void restart() {
    hw::reset();
    ps2::schedule(hw::kEventVblankEnd, UINT64_MAX, [] {});
    set(kMode, kEquf | kOvff);  // stopped, flags clear
    set(kCount, 0);
}

}  // namespace

int main() {
    // Bus clock (one tick per 2 EE cycles), compare 100 with ZRET: a match every 100 ticks = 200 cycles.
    restart();
    set(kComp, 100);
    set(kMode, kCue | kZret | kCmpe);
    std::uint64_t start = ps2::now();
    int irqs = run_until(start + 2000);  // 10 matches, the handler never writes EQUF back
    check(irqs == 1, "1. without acknowledging, only the first compare match raises an interrupt");
    check(reg(kMode) & kEquf, "1. EQUF stays set");
    std::fprintf(stdout, "1. compare, not acknowledged: %d interrupt(s) in 10 matches\n", irqs);

    set(kMode, kCue | kZret | kCmpe | kEquf);  // the handler acknowledges at last
    irqs = run_until(ps2::now() + 2000, [] { set(kMode, kCue | kZret | kCmpe | kEquf); });
    check(irqs == 10, "1. acknowledging every time: one interrupt per match");
    std::fprintf(stdout, "1. compare, acknowledged: %d interrupt(s) in 10 matches\n", irqs);

    // 2. Overflow every 65536 ticks.
    restart();
    set(kComp, 0x8000);
    set(kMode, kCue | kOvfe);
    start = ps2::now();
    irqs = run_until(start + 3u * 0x10000u * 2u + 10u);
    check(irqs == 1, "2. without acknowledging, only the first overflow raises an interrupt");
    restart();
    set(kComp, 0x8000);
    set(kMode, kCue | kOvfe);
    irqs = run_until(ps2::now() + 3u * 0x10000u * 2u + 10u, [] { set(kMode, kCue | kOvfe | kOvff); });
    check(irqs == 3, "2. acknowledging every time: one interrupt per overflow");

    // 3. ZRET with no interrupt enabled.
    restart();
    set(kComp, 50);
    set(kMode, kCue | kZret);
    start = ps2::now();
    std::uint32_t highest = 0;
    for (int i = 1; i <= 40; ++i) {
        run_until(start + static_cast<std::uint64_t>(i) * 13u);
        if (reg(kCount) > highest) highest = reg(kCount);
    }
    check(highest < 50, "3. ZRET without interrupts: the counter never passes the compare value");
    check(reg(kMode) & kEquf, "3. EQUF is set without the compare interrupt enabled");
    check(hw::intc_stat_raw() == 0, "3. no interrupt is raised");
    restart();
    set(kComp, 0xFFFF);
    set(kMode, kCue);
    run_until(ps2::now() + 0x10001u * 2u);
    check((reg(kMode) & (kEquf | kOvff)) == (kEquf | kOvff), "3. a free-running counter sets EQUF and OVFF");

    // 4. V-blank gate, GATM 0, bus clock/256 (512 cycles a tick).
    const std::uint64_t vblank_len = ps2::hblank_cycles() * 22u;
    restart();
    set(kMode, kCue | kGate | kGats | 2u);
    run_until(ps2::now() + 512u * 40u);
    const std::uint32_t before = reg(kCount);
    hw::vblank_start();
    run_until(ps2::now() + vblank_len / 2u);
    check(reg(kCount) == before, "4. GATM 0: the counter stands still during the V-blank");
    run_until(ps2::now() + vblank_len);  // past the V-blank end
    const std::uint32_t after = reg(kCount);
    const std::uint32_t expected = before + static_cast<std::uint32_t>((vblank_len - vblank_len / 2u) / 512u);
    check(after + 1u >= expected && after <= expected + 1u, "4. GATM 0: it goes on from the same value after the V-blank");
    std::fprintf(stdout, "4. GATM 0: %u before the V-blank, %u after (about %u expected)\n", before, after, expected);

    // 5. V-blank gate, GATM 1, compare 1000 without ZRET: one match a frame, at the same point of each frame.
    restart();
    set(kComp, 1000);
    set(kMode, kCue | kGate | kGats | 0x10u | kCmpe | 2u);
    irqs = 0;
    for (int frame = 0; frame < 30; ++frame) {
        hw::vblank_start();
        check(reg(kCount) == 0, "5. GATM 1: the counter is 0 when the V-blank starts");
        irqs += run_until(ps2::now() + kFrame, [] { set(kMode, kCue | kGate | kGats | 0x10u | kCmpe | 2u | kEquf); });
    }
    check(irqs == 30, "5. GATM 1: one compare interrupt a frame");
    std::fprintf(stdout, "5. GATM 1: %d compare interrupt(s) in 30 frames (ungated it would be about %llu)\n", irqs,
                 static_cast<unsigned long long>(30u * kFrame / 512u / 65536u + 1u));

    // 6. V-blank gate, GATM 2: reset at the end of the V-blank.
    restart();
    set(kMode, kCue | kGate | kGats | 0x20u | 2u);
    run_until(ps2::now() + 512u * 300u);
    hw::vblank_start();
    check(reg(kCount) >= 299, "6. GATM 2: the V-blank start does not reset the counter");
    run_until(ps2::now() + vblank_len + 512u * 5u);
    check(reg(kCount) <= 6, "6. GATM 2: the counter restarts when the V-blank ends");

    std::fprintf(stdout, failed ? "timer gate: %d check(s) failed\n" : "timer gate: all checks passed\n", failed);
    return failed ? 1 : 0;
}
