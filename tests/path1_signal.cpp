// No game data. A VU1 program kicks a GIF packet with SIGNAL, then one with FINISH and LABEL (PATH1). Their CSR bits, SIGLBLID and the
// EE's GS interrupt must arrive, and at the same emulated time with one thread, with the drawing thread only (PS2_NO_VU_THREAD=1) and
// with the vector thread on (the default). Before the fix the vector thread read nobody's XGKICK packets for them: TimeSplitters 2 waited
// for ever at its splash screen (pull request #130).
#include "gs/gif.hpp"
#include "gs/gs.hpp"
#include "hw.hpp"
#include "ps2/memory.hpp"
#include "ps2/system.hpp"

#include <cstdint>
#include <cstdio>
#include <thread>

namespace {
namespace hw = ps2::hw;

constexpr std::uint32_t kChcr = 0x10009000u, kMadr = 0x10009010u, kQwc = 0x10009020u;  // DMA channel 1 (VIF1)
constexpr std::uint32_t kIntcStat = 0x1000F000u, kCsr = 0x12001000u, kImr = 0x12001010u, kSiglblid = 0x12001080u;
constexpr std::uint32_t kBuffer = 0x00110000u;
constexpr std::uint64_t kSignal = 1, kFinish = 2;

int failed = 0;

void check(bool ok, const char *mode, const char *message) {
    if (!ok) {
        ++failed;
        std::fprintf(stderr, "FAIL (%s): %s\n", mode, message);
    }
}

void advance(int cycles) {
    ps2::set_budget(ps2::g_cpu, cycles);
    ps2::g_cpu.budget = 0;
    ps2::preempt(ps2::g_cpu);
}

bool gs_interrupt() { return (hw::read32(kIntcStat) & 1u) != 0; }  // INTC_STAT.GS (not a delivery point)

// A VIF1 list for one normal-mode DMA: the program (XGKICK vi0, then the end), the packet unpacked to VU1 address 0, MSCAL 0.
// `regs` are the A+D register addresses of the packet's qwords and `values` their data.
std::uint32_t build(const std::uint32_t *regs, const std::uint64_t *values, std::uint32_t count) {
    std::uint32_t w[64] = {};
    std::uint32_t n = 3;
    w[n++] = 0x4A000000u | (3u << 16);  // MPG: 3 instruction pairs at 0, data from the next (64-bit aligned) word
    const std::uint32_t program[6] = {
        0x800006FCu, 0x000002FFu,                // XGKICK vi0             | NOP
        0x8000033Cu, 0x400002FFu,                // NOP                    | NOP with the E bit
        0x8000033Cu, 0x000002FFu,                // NOP (the pair after E) | NOP
    };
    for (std::uint32_t x : program) w[n++] = x;
    n = 11;
    w[n++] = 0x6C000000u | ((count + 1) << 16);  // UNPACK V4-32 of the packet to VU1 address 0
    const std::uint64_t tag = count | (1ull << 15) | (1ull << 60);  // NLOOP = count, EOP, PACKED, one register: A+D
    w[n++] = static_cast<std::uint32_t>(tag);
    w[n++] = static_cast<std::uint32_t>(tag >> 32);
    w[n++] = 0xEu;
    w[n++] = 0;
    for (std::uint32_t i = 0; i < count; ++i) {
        w[n++] = static_cast<std::uint32_t>(values[i]);
        w[n++] = static_cast<std::uint32_t>(values[i] >> 32);
        w[n++] = regs[i];
        w[n++] = 0;
    }
    w[n++] = 0x14000000u;  // MSCAL 0
    const std::uint32_t qwc = (n + 3) / 4;
    for (std::uint32_t i = 0; i < qwc * 4; ++i) ps2::st32(kBuffer + i * 4, w[i]);
    return qwc;
}

void kick(std::uint32_t qwc) {
    hw::write32(kMadr, kBuffer);
    hw::write32(kQwc, qwc);
    hw::write32(kChcr, 0x100u | 1u);  // STR, from memory
}

struct Times {
    std::uint64_t signal_frames = 0, finish = 0;
};

// The same steps in every mode; returns when each event was first seen.
Times scenario(const char *mode) {
    Times t;
    ps2::gif::forget_path1();          // each mode starts as a run that has not used PATH1 SIGNAL yet
    hw::write64(kCsr, 0x1Fu);          // clear the events of the mode before
    hw::write32(kIntcStat, 1u);        // and the GS interrupt
    hw::write64(kImr, 0x7F00u & ~0x300u);  // SIGNAL and FINISH may interrupt
    hw::write64(kSiglblid, 0);

    // 1. Not used yet: the SIGNAL comes at the third vblank after the kick (a fence at the next one closes what was kicked before it,
    //    the vblank two after that hands it on, when the vector thread is surely past that fence), whatever reads CSR meanwhile.
    for (const std::uint64_t f = ps2::frame_count(); ps2::frame_count() == f;) advance(1000);
    const std::uint64_t kicked_in = ps2::frame_count();
    const std::uint32_t signal_regs[1] = {0x60};
    const std::uint64_t signal_values[1] = {(0xFFFFull << 32) | 0x1234u};  // ID 0x1234 under mask 0xFFFF
    kick(build(signal_regs, signal_values, 1));
    bool at_vblank = false;
    for (int step = 0; step < 2000; ++step) {
        const std::uint64_t before = ps2::frame_count();
        advance(20000);
        if (hw::read64(kCsr) & kSignal) {
            at_vblank = ps2::frame_count() != before;
            break;
        }
        check(!gs_interrupt(), mode, "no GS interrupt before the SIGNAL is handed on");
    }
    t.signal_frames = ps2::frame_count() - kicked_in;
    check((hw::read64(kCsr) & kSignal) != 0, mode, "CSR.SIGNAL comes");
    check(at_vblank, mode, "the SIGNAL is handed on at a vblank");
    check(t.signal_frames == 3, mode, "the SIGNAL comes at the third vblank after the kick");
    check(gs_interrupt(), mode, "the SIGNAL raises the EE's GS interrupt");
    check((hw::read64(kSiglblid) & 0xFFFFFFFFu) == 0x1234u, mode, "SIGLBLID holds the SIGNAL's ID");

    // 2. Used now: SIGNAL, FINISH and LABEL are handed on at once at the next read of SIGLBLID, FINISH raises its bit after the GS delay.
    hw::write64(kCsr, kSignal);
    hw::write32(kIntcStat, 1u);
    const std::uint32_t regs[3] = {0x60, 0x61, 0x62};
    const std::uint64_t values[3] = {(0xFFull << 32) | 0x99u, 0, (0xFFull << 32) | 0x56u};  // SIGNAL 0x99, FINISH, LABEL 0x56
    const std::uint64_t start = ps2::now();
    kick(build(regs, values, 3));
    advance(2000);
    check(!gs_interrupt(), mode, "nothing before the read");
    const std::uint64_t ids = hw::read64(kSiglblid);  // a delivery point
    check((ids & 0xFFu) == 0x99u && (ids >> 32) == 0x56u, mode, "SIGLBLID holds the SIGNAL's ID and the LABEL when it is read after the kick");
    check((hw::read64(kCsr) & kSignal) != 0 && gs_interrupt(), mode, "the SIGNAL is set and interrupts at that read");
    hw::write64(kCsr, kSignal);
    hw::write32(kIntcStat, 1u);
    for (int step = 0; step < 100000 && !(hw::read64(kCsr) & kFinish); ++step) advance(1);
    t.finish = ps2::now() - start;
    check((hw::read64(kCsr) & kFinish) != 0, mode, "CSR.FINISH comes");
    check(gs_interrupt(), mode, "the FINISH raises the EE's GS interrupt");
    std::printf("%-26s SIGNAL at the %llu. vblank after the kick, FINISH %llu cycles after the next kick\n", mode, static_cast<unsigned long long>(t.signal_frames),
                static_cast<unsigned long long>(t.finish));
    return t;
}

}

int main() {
    ps2::boot(0x00100000u, {});
    hw::write32(0x1000E000u, 1);  // D_CTRL.DMAE
    const Times one = scenario("one thread");
    ps2::start_render_thread();  // nobody draws: the queue only collects what the GS would get
    const Times drawing = scenario("drawing thread, VU1 here");
    ps2::start_vu_thread();
    std::thread(ps2::run_vu_thread).detach();
    const Times vector = scenario("vector thread on");
    check(one.signal_frames == drawing.signal_frames && drawing.signal_frames == vector.signal_frames, "all", "the SIGNAL arrives at the same emulated time in every mode");
    check(one.finish == drawing.finish && drawing.finish == vector.finish, "all", "the FINISH arrives at the same emulated time in every mode");
    std::printf("PATH1 SIGNAL/FINISH/LABEL: %d failed\n", failed);
    return failed ? 1 : 0;
}
