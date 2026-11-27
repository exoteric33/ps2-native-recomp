#include "hw.hpp"

#include "dmac.hpp"
#include "gs/gif.hpp"
#include "gs/gs.hpp"
#include "iop/sif.hpp"
#include "ipu/ipu.hpp"
#include "vif.hpp"
#include "vu_micro.hpp"
#include "kernel.hpp"
#include "ps2/memory.hpp"
#include "ps2/system.hpp"

#include "ps2/state.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>

namespace ps2::hw {

namespace {

constexpr std::uint32_t kIntcStat = 0x1000F000u, kIntcMask = 0x1000F010u;
constexpr std::uint32_t kDmacStat = 0x1000E010u;

// g_intc_latched is what the guest reads back: the kernel acknowledging an interrupt before
// running the handlers must not hide it from code that polls INTC_STAT, the way sceGsSyncV
// waits for VBlank start while a VBlank handler is installed. Only a guest write clears it.
std::uint32_t g_intc_stat = 0, g_intc_mask = 0, g_intc_latched = 0;
std::uint32_t g_dmac_stat = 0;
std::map<std::uint32_t, std::uint64_t> g_regs;
std::set<std::uint32_t> g_reported;
std::uint32_t g_crt[3] = {};

void report(const char *what, std::uint32_t a, std::uint64_t v) {
    if (g_options.trace_io || g_reported.insert(a).second)
        std::fprintf(stderr, "[io] %s 0x%08X = 0x%llX (pc 0x%08X)\n", what, a, static_cast<unsigned long long>(v), g_cpu.pc);
}

struct Timer {
    std::uint32_t mode = 0, comp = 0, hold = 0;
    std::uint64_t base = 0;
    std::uint32_t frozen = 0;
};

Timer g_timers[4];

// Tn_MODE bits, from the public EE User's Manual (timer chapter): CLKS 0-1, GATE 2, GATS 3 (0 H-blank, 1 V-blank), GATM 4-5, ZRET 6,
// CUE 7, CMPE 8, OVFE 9, EQUF 10 and OVFF 11 (both cleared by writing 1).
constexpr std::uint32_t kGate = 0x4u, kGats = 0x8u, kGatm = 0x30u, kZret = 0x40u, kCue = 0x80u, kCmpe = 0x100u, kOvfe = 0x200u,
                        kEquf = 0x400u, kOvff = 0x800u;

// PS2_TIMER_SIMPLE=1 brings back the earlier model: no V-blank gate, an interrupt at every match even while its flag
// is still set, and no timer events while both interrupt enables are off. Read on first use, so switches.txt can set it.
bool timer_simple() {
    static const bool simple = std::getenv("PS2_TIMER_SIMPLE") != nullptr;
    return simple;
}

// The V-blank gate (GATE=1, GATS=1). Our V-blank lasts from vblank_start() until the kEventVblankEnd event has run, so "inside the
// V-blank" is "that event is still waiting", which a save state carries too.
bool vblank_gate(const Timer &t) { return !timer_simple() && (t.mode & (kGate | kGats)) == (kGate | kGats); }
bool inside_vblank() { return scheduled_at(kEventVblankEnd) != UINT64_MAX; }

// GATM 0 counts only while the gate signal is low; the V-blank signal is high during the V-blank, so the counter stops there and keeps
// its value in `frozen`, as with CUE off.
bool counting(const Timer &t) {
    if (!(t.mode & kCue)) return false;
    return !(vblank_gate(t) && !(t.mode & kGatm) && inside_vblank());
}

std::uint64_t timer_divider(const Timer &t) {
    switch (t.mode & 3u) {
    case 0: return 2;
    case 1: return 32;
    case 2: return 512;
    default: return hblank_cycles();
    }
}

std::uint32_t timer_count(int n) {
    const Timer &t = g_timers[n];
    if (!counting(t)) return t.frozen;
    return static_cast<std::uint32_t>(((now() - t.base) / timer_divider(t)) & 0xFFFFu);
}

std::uint32_t timer_read(int n) {
    const std::uint64_t due = scheduled_at(kEventTimer0 + n);
    if (due > now() || !counting(g_timers[n])) return timer_count(n);
    const Timer &t = g_timers[n];
    return static_cast<std::uint32_t>(((due - 1 - t.base) / timer_divider(t)) & 0xFFFFu);
}

void timer_set_count(int n, std::uint32_t value) {
    Timer &t = g_timers[n];
    t.frozen = value & 0xFFFFu;
    t.base = now() - static_cast<std::uint64_t>(value & 0xFFFFu) * timer_divider(t);
}

void timer_schedule(int n);

void timer_event(int n, std::uint64_t at) {
    Timer &t = g_timers[n];
    if (!counting(t)) return;
    const std::uint32_t count = static_cast<std::uint32_t>(((at - t.base) / timer_divider(t)) & 0xFFFFu);
    // The timer's interrupt request comes when EQUF or OVFF goes from 0 to 1 (the INTC takes edges); while the game has not
    // written the flag back, a new match sets nothing new and raises nothing (EE User's Manual, timer chapter).
    const bool simple = timer_simple();
    if (count == (t.comp & 0xFFFFu)) {
        const bool was = t.mode & kEquf;
        t.mode |= kEquf;
        if ((t.mode & kCmpe) && (simple || !was)) intc_raise(9u + static_cast<unsigned>(n));
        if (t.mode & kZret) t.base = at;
    }
    if (count == 0 && !(t.mode & kZret && t.comp == 0)) {
        const bool was = t.mode & kOvff;
        t.mode |= kOvff;
        if ((t.mode & kOvfe) && (simple || !was)) intc_raise(9u + static_cast<unsigned>(n));
    }
    timer_schedule(n);
}

void timer_schedule(int n) {
    Timer &t = g_timers[n];
    // A match matters when it raises an interrupt, resets the counter (ZRET) or sets a flag the game may poll; with all of that
    // already done, no event is needed.
    const bool simple = timer_simple();
    const bool want_compare = simple ? (t.mode & kCmpe) != 0 : (t.mode & (kCmpe | kZret)) || !(t.mode & kEquf);
    const bool want_overflow = simple ? (t.mode & kOvfe) != 0 : (t.mode & kOvfe) || !(t.mode & kOvff);
    if (!counting(t) || !(want_compare || want_overflow)) {
        schedule(kEventTimer0 + n, ~0ull, [] {});
        return;
    }
    const std::uint32_t count = timer_count(n);
    std::uint32_t to_compare = ((t.comp & 0xFFFFu) - count) & 0xFFFFu;
    if (!to_compare) to_compare = 0x10000u;
    std::uint32_t to_overflow = 0x10000u - count;
    std::uint32_t ticks = to_compare;
    if (want_overflow && to_overflow < ticks) ticks = to_overflow;
    if (!want_compare) ticks = to_overflow;
    const std::uint64_t div = timer_divider(t);
    const std::uint64_t at = t.base + ((now() - t.base) / div + ticks) * div;
    schedule(kEventTimer0 + n, at, [n, at] { timer_event(n, at); });
}

int timer_index(std::uint32_t a) { return static_cast<int>((a >> 11) & 3u); }

bool is_timer(std::uint32_t a) { return a >= 0x10000000u && a < 0x10002000u && (a & 0x7FFu) < 0x40u; }

// An edge of the V-blank gate signal (rising = the V-blank starts). GATM 1 resets the counter at the rising edge, 2 at the falling
// edge, 3 at both; GATM 0 stops the counter for the V-blank (counting() above) and goes on from the same value after it. The H-blank
// gate (GATS 0) is not modelled: it would need the line timing inside a frame.
void timer_gate_edge(bool rising) {
    for (int n = 0; n < 4; ++n) {
        Timer &t = g_timers[n];
        if (!vblank_gate(t) || !(t.mode & kCue)) continue;
        const std::uint32_t gatm = (t.mode & kGatm) >> 4;
        if (gatm == 0) timer_set_count(n, t.frozen);  // the value it stopped at (rising: set just before the V-blank began)
        else if (gatm == 3 || gatm == (rising ? 1u : 2u)) timer_set_count(n, 0);
        else continue;
        timer_schedule(n);
    }
}

void vblank_end() {
    intc_raise(3);
    timer_gate_edge(false);
}

// One line in the normal log for each new timer setting the game makes (a few per timer), so a tester's log shows how its clocks
// are set up without a trace switch.
void report_timer_mode(int n, std::uint32_t mode, std::uint32_t comp) {
    static std::uint32_t last[4] = {~0u, ~0u, ~0u, ~0u};
    static int shown[4] = {};
    mode &= 0x3FFu;
    if (!(mode & kCue) || mode == last[n]) return;
    last[n] = mode;
    if (++shown[n] > 6) {
        if (shown[n] == 7) std::fprintf(stderr, "[timer] T%d: more settings follow, not shown\n", n);
        return;
    }
    static const char *const clock[] = {"bus clock", "bus clock/16", "bus clock/256", "H-blank"};
    static const char *const gatm[] = {"count outside the blank", "reset at its start", "reset at its end", "reset at start and end"};
    char gate[80] = "no gate";
    if (mode & kGate) std::snprintf(gate, sizeof gate, "%s gate, %s%s", mode & kGats ? "V-blank" : "H-blank", gatm[(mode >> 4) & 3u],
                                    mode & kGats ? "" : " (H-blank gate not modelled)");
    std::fprintf(stderr, "[timer] T%d mode 0x%03X: %s, %s%s%s%s, compare 0x%04X (frame %llu, pc 0x%08X)\n", n, mode, clock[mode & 3u], gate,
                 mode & kZret ? ", zero at compare" : "", mode & kCmpe ? ", compare interrupt" : "", mode & kOvfe ? ", overflow interrupt" : "",
                 comp & 0xFFFFu, static_cast<unsigned long long>(frame_count()), g_cpu.pc);
}

}

void reset() {
    g_intc_stat = g_intc_mask = g_intc_latched = g_dmac_stat = 0;
    gs::reset();
    gif::reset();
    vif::reset();
    vu::reset();
    dmac::reset();
    ipu::reset();
    g_regs.clear();
    for (Timer &t : g_timers) t = Timer{};
    g_timers[3].mode = 0x183u;
}

void vblank_start() {
    intc_raise(2);
    gs::vblank(true);
    std::uint32_t counts[4];
    for (int n = 0; n < 4; ++n) counts[n] = timer_count(n);  // before the V-blank begins: a GATM 0 counter stops at this value
    schedule(kEventVblankEnd, now() + hblank_cycles() * 22u, vblank_end);
    for (int n = 0; n < 4; ++n)
        if (vblank_gate(g_timers[n]) && !(g_timers[n].mode & kGatm)) g_timers[n].frozen = counts[n];
    timer_gate_edge(true);
}

std::uint32_t intc_pending() { return g_intc_stat & g_intc_mask; }
std::uint32_t intc_stat_raw() { return g_intc_stat; }
std::uint32_t intc_mask_raw() { return g_intc_mask; }
std::uint32_t dmac_stat_raw() { return g_dmac_stat; }

void intc_raise(unsigned cause) {
    g_intc_stat |= 1u << cause;
    g_intc_latched |= 1u << cause;
    if (g_intc_mask & (1u << cause)) kernel::wake_idle();
    if (kernel::g_trace_irq) [[unlikely]] kernel::trace_raise(false, cause);
}

void intc_ack(unsigned cause) { g_intc_stat &= ~(1u << cause); }

bool intc_set_mask(unsigned cause, bool enabled) {
    const std::uint32_t bit = 1u << cause;
    const bool was = g_intc_mask & bit;
    if (enabled) g_intc_mask |= bit;
    else g_intc_mask &= ~bit;
    return was != enabled;
}

std::uint32_t dmac_pending() {
    const std::uint32_t cis = g_dmac_stat & 0x3FFu, cim = (g_dmac_stat >> 16) & 0x3FFu;
    return cis & cim;
}

void dmac_ack(unsigned channel) { g_dmac_stat &= ~(1u << channel); }

bool dmac_set_mask(unsigned channel, bool enabled) {
    const std::uint32_t bit = 0x10000u << channel;
    const bool was = g_dmac_stat & bit;
    if (enabled) g_dmac_stat |= bit;
    else g_dmac_stat &= ~bit;
    return was != enabled;
}

std::uint64_t gs_imr() { return gs::read_privileged(0x12001010u); }
std::uint64_t gs_csr() { return gs::read_privileged(0x12001000u); }
void gs_set_imr(std::uint64_t value) { gs::write_privileged(0x12001010u, value); }

std::uint32_t crt_mode() { return __atomic_load_n(&g_crt[1], __ATOMIC_RELAXED); }

void gs_set_crt(std::uint32_t interlace, std::uint32_t mode, std::uint32_t field) {
    g_crt[0] = interlace;
    __atomic_store_n(&g_crt[1], mode, __ATOMIC_RELAXED); // the window or render thread reads it (crt_mode)
    g_crt[2] = field;
    std::fprintf(stderr, "[gs] SetGsCrt interlace %u mode 0x%X field %u\n", interlace, mode, field);
    // The BIOS call programs the video output: SMODE2 gets INT (bit 0, interlace) and FFMD (bit 1, 1 = frame mode: both fields read the
    // same lines, so a 224 line buffer fills a 448 line picture), and the game cannot read it back (the GS registers are write-only).
    // Games made with Sony's libgraph write SMODE2 themselves with the same value after the call (checked in Devil May Cry, Gran Turismo 4,
    // Resident Evil 4 and Silent Hill 2); Zone of the Enders does not, it relies on the call, and showed two buffers stacked without this.
    gs::write_privileged(0x12000020u, (interlace & 1u) | ((field & 1u) << 1));
    if (!g_options.rate_forced) g_options.pal = mode == 3u;
}

void dmac_raise(unsigned channel) {
    g_dmac_stat |= 1u << channel;
    if (g_dmac_stat & (0x10000u << channel)) kernel::wake_idle();
    if (kernel::g_trace_irq) [[unlikely]] kernel::trace_raise(true, channel);
}

std::uint64_t g_slow_seq = 0;

namespace {

// Skipping a waiting loop. A game that waits for the next picture reads INTC_STAT again and again in a loop of a few instructions
// (sceGsSyncV): on the real console it just burns the time, here it burned half of the emulation thread (about 5 ms of every
// 20 ms frame in Robotech, about 4 ms in God of War II). When the loop is a fixed point (the same value read, the same registers
// and the same number of cycles between three reads in a row, and no other hardware access or system call in between) every
// further turn would do exactly the same, so the clock is moved forward by whole turns instead of running them. The budget of
// the slice never goes past the next event, so the clock never passes one; the registers, the memory and the hardware are the
// same as after running the turns, which makes the result the same (cycle for cycle) as without the skip. The skip is a
// multiple of four turns, so a loop with two reads of INTC_STAT in a turn keeps its phase. PS2_NO_SPIN_SKIP=1 switches it off.
struct RegSnapshot {
    Reg128 r[32], hi, lo;
    std::uint32_t sa, f[32], facc, fcr31;
};

void snapshot(RegSnapshot &s) {
    std::memcpy(s.r, g_cpu.r, sizeof s.r);
    s.hi = g_cpu.hi;
    s.lo = g_cpu.lo;
    s.sa = g_cpu.sa;
    std::memcpy(s.f, g_cpu.f, sizeof s.f);
    s.facc = g_cpu.facc;
    s.fcr31 = g_cpu.fcr31;
}

bool same_registers(const RegSnapshot &s) {
    return std::memcmp(s.r, g_cpu.r, sizeof s.r) == 0 && std::memcmp(&s.hi, &g_cpu.hi, sizeof s.hi) == 0 && std::memcmp(&s.lo, &g_cpu.lo, sizeof s.lo) == 0 &&
           s.sa == g_cpu.sa && std::memcmp(s.f, g_cpu.f, sizeof s.f) == 0 && s.facc == g_cpu.facc && s.fcr31 == g_cpu.fcr31;
}

struct SpinWatch {
    std::uint32_t value = 0, streak = 0;
    std::uint64_t seq = 0, at = 0, delta = 0, skipped = 0;
    RegSnapshot regs;
};
SpinWatch g_spin;

constexpr std::uint32_t kSpinAfter = 24;  // reads in a row with the same value before the registers are looked at

void spin_watch(std::uint32_t value) {
    SpinWatch &w = g_spin;
    static const bool off = std::getenv("PS2_NO_SPIN_SKIP") != nullptr;  // read on first use, so switches.txt can set it
    if (off) return;
    const std::uint64_t t = now();
    if (value != w.value || g_slow_seq != w.seq || t < w.at) {  // something else happened since the last read: start counting again
        w.value = value;
        w.seq = g_slow_seq;
        w.streak = 0;
        w.at = t;
        return;
    }
    const std::uint64_t delta = t - w.at;
    w.at = t;
    if (++w.streak < kSpinAfter) return;
    if (w.streak == kSpinAfter) {  // first look: take the registers and the time between two reads
        snapshot(w.regs);
        w.delta = delta;
        return;
    }
    if (delta != w.delta || !same_registers(w.regs)) {  // not a fixed point (a counter runs, or the loop is longer): look again later
        snapshot(w.regs);
        w.delta = delta;
        w.streak = kSpinAfter;
        return;
    }
    if (w.streak < kSpinAfter + 2) return;  // three reads in a row looked the same
    const std::int64_t left = g_cpu.budget;
    const std::uint64_t step = 4u * delta;
    if (delta == 0 || left <= static_cast<std::int64_t>(step) + 1) return;
    const std::int64_t skip = static_cast<std::int64_t>(static_cast<std::uint64_t>(left - 1) / step * step);
    g_cpu.budget = static_cast<std::int32_t>(left - skip);
    w.skipped += static_cast<std::uint64_t>(skip);
    w.at = now();
    w.streak = 0;
}

}  // namespace

std::uint64_t spin_skipped_cycles() { return g_spin.skipped; }

std::uint32_t read_intc_stat() {
    const std::uint32_t value = g_intc_stat | (kernel::interruptible() ? g_intc_latched : 0u);
    spin_watch(value);
    return value;
}

std::uint32_t read32(std::uint32_t a) {
    if (a == kIntcStat) return read_intc_stat();
    if (a == kIntcMask) return g_intc_mask;
    if (a == kDmacStat) return g_dmac_stat;
    if (a >= 0x12000000u && a < 0x12002000u) return static_cast<std::uint32_t>(gs::read_privileged(a & ~7u) >> ((a & 4u) * 8));
    if (dmac::owns(a)) return dmac::read(a);
    if (a >= 0x10003000u && a < 0x10003800u) return gif::read(a);
    if (a >= 0x10003800u && a < 0x10003C00u) return vif::read(0, a & 0x3FFu);
    if (a >= 0x10003C00u && a < 0x10004000u) return vif::read(1, a & 0x3FFu);
    if (a >= 0x10002000u && a < 0x10002040u) return ipu::read32(a);
    if (is_timer(a)) {
        const int n = timer_index(a);
        switch (a & 0x30u) {
        case 0x00: return timer_read(n);
        case 0x10: return g_timers[n].mode;
        case 0x20: return g_timers[n].comp;
        default: return g_timers[n].hold;
        }
    }
    if (a >= 0x1000F200u && a < 0x1000F240u) return sif::get_reg(((a >> 4) & 7u) + 1u);
    if (a == 0x1000F130u) return 0;
    if (a == 0x1000F520u) return static_cast<std::uint32_t>(g_regs[0x1000F590u]);
    auto it = g_regs.find(a);
    const std::uint64_t v = it == g_regs.end() ? 0 : it->second;
    report("read", a, v);
    return static_cast<std::uint32_t>(v);
}

void write32(std::uint32_t a, std::uint32_t v) {
    if (a == kIntcStat) {
        const std::uint32_t before = g_intc_stat;
        g_intc_stat &= ~v;
        g_intc_latched &= ~v;
        if (kernel::g_trace_irq) [[unlikely]] kernel::trace_register_write(0, v, before, g_intc_stat);
        return;
    }
    if (a == kIntcMask) {
        const std::uint32_t before = g_intc_mask;
        g_intc_mask ^= v;
        if (kernel::g_trace_irq) [[unlikely]] kernel::trace_register_write(1, v, before, g_intc_mask);
        return;
    }
    if (a == kDmacStat) {
        // A channel end that is enabled while interrupts are on is taken by the console as soon as the channel ends, so a program can
        // never clear one before its handler ran. Here the handler waits for the next safe point, and a program that acknowledges in
        // between erased it: Airblade (issue #110) starts a GIF transfer and then resets the DMAC (D_STAT 0xFF1F, then the masks off),
        // and its graphics library's handler for that transfer never ran. So what is pending runs first, as at EI.
        // (take_pending does nothing inside a handler or with interrupts off.)
        if (v & dmac_pending()) kernel::take_pending(g_cpu);
        const std::uint32_t before = g_dmac_stat;
        g_dmac_stat &= ~(v & 0xFFFFu);
        g_dmac_stat ^= v & 0xFFFF0000u;
        if (kernel::g_trace_irq) [[unlikely]] kernel::trace_register_write(2, v, before, g_dmac_stat);
        return;
    }
    if (a >= 0x12000000u && a < 0x12002000u) {
        if (!(a & 4u)) gs::write_privileged(a, v);
        return;
    }
    if (dmac::owns(a)) { dmac::write(a, v); return; }
    if (a == 0x1000F590u) dmac::enable_write(v);  // D_ENABLEW: stored below, read back through 0x1000F520
    if (a >= 0x10003000u && a < 0x10003800u) { gif::write(a, v); return; }
    if (a >= 0x10003800u && a < 0x10003C00u) { vif::write(0, a & 0x3FFu, v); return; }
    if (a >= 0x10003C00u && a < 0x10004000u) { vif::write(1, a & 0x3FFu, v); return; }
    if ((a >= 0x10002000u && a < 0x10002040u) || (a >= 0x10007010u && a < 0x10007020u)) { ipu::write32(a, v); return; }
    if (is_timer(a)) {
        const int n = timer_index(a);
        Timer &t = g_timers[n];
        switch (a & 0x30u) {
        case 0x00: timer_set_count(n, v); break;
        case 0x10: {
            const std::uint32_t count = timer_count(n);
            t.mode = (t.mode & 0xC00u & ~(v & 0xC00u)) | (v & 0x3FFu);
            timer_set_count(n, count);
            report_timer_mode(n, t.mode, t.comp);
            break;
        }
        case 0x20: t.comp = v & 0xFFFFu; break;
        default: t.hold = v & 0xFFFFu; break;
        }
        timer_schedule(n);
        return;
    }
    if (a >= 0x1000F200u && a < 0x1000F240u) { sif::set_reg(((a >> 4) & 7u) + 1u, v); return; }
    report("write", a, v);
    g_regs[a] = v;
}

std::uint64_t read64(std::uint32_t a) {
    if (a >= 0x12000000u && a < 0x12002000u) return gs::read_privileged(a);
    return read32(a) | (static_cast<std::uint64_t>(read32(a + 4)) << 32);
}

void write64(std::uint32_t a, std::uint64_t v) {
    if (a >= 0x12000000u && a < 0x12002000u) {
        gs::write_privileged(a, v);
        return;
    }
    write32(a, static_cast<std::uint32_t>(v));
    write32(a + 4, static_cast<std::uint32_t>(v >> 32));
}

void read128(std::uint32_t a, Reg128 &out) {
    std::memset(&out, 0, sizeof out);
    if (a >= 0x10005000u && a < 0x10006000u) {
        gs::read_image(reinterpret_cast<std::uint8_t *>(&out), 1);
        return;
    }
    if (a >= 0x10007000u && a < 0x10007010u) {
        ipu::read_fifo(reinterpret_cast<std::uint8_t *>(&out));
        return;
    }
    report("read128", a, 0);
}

void write128(std::uint32_t a, const Reg128 &v) {
    const auto *bytes = reinterpret_cast<const std::uint8_t *>(&v);
    if (a >= 0x10004000u && a < 0x10005000u) { vif::transfer(0, bytes, 1); return; }
    if (a >= 0x10005000u && a < 0x10006000u) { vif::transfer(1, bytes, 1); return; }
    if (a >= 0x10006000u && a < 0x10007000u) { gif::transfer(3, bytes, 1); return; }
    if (a >= 0x10007010u && a < 0x10007020u) { ipu::write_fifo(bytes); return; }
    report("write128", a, v.d[0]);
}

}

namespace ps2::hw {

std::function<void()> event_action(int id, std::uint64_t at) {
    if (id == kEventVblankEnd) return vblank_end;
    if (id == kEventVif1Read) return dmac::finish_vif1_read;
    if (id == kEventGsFinish) return gs::raise_finish;
    if (id >= kEventTimer0 && id <= kEventTimer3) return [n = id - kEventTimer0, at] { timer_event(n, at); };
    return [] {};
}

void state(ps2::state::Stream &s) {
    s.tag("eehw");
    s.pod(g_intc_stat);
    s.pod(g_intc_mask);
    s.pod(g_dmac_stat);
    s.map(g_regs);
    s.pod(g_crt);
    s.pod(g_timers);
}

}
