// No game data. Set up a four-block waveform through the SPU2 registers and observe NAX, ENVX and output.
#include "iop/iop.hpp"
#include "iop/spu2.hpp"

#include <algorithm>
#include <cstdio>

namespace {
namespace iop = ps2::iop;
namespace spu2 = ps2::iop::spu2;

constexpr std::uint32_t kSpu = 0x1F900000, kBank = 0x10000;
constexpr std::uint32_t kCyclesPerSample = 294'912'000 / 48'000;
unsigned failed = 0;
unsigned checks = 0;

void check(bool ok, unsigned core, bool loop, const char *description) {
    ++checks;
    if (!ok) {
        ++failed;
        std::fprintf(stderr, "FAIL core %u (%s): %s\n", core, loop ? "key-off" : "one-shot", description);
    }
}

void reg(std::uint32_t offset, std::uint32_t value) { spu2::write(kSpu + offset, value, 2); }
unsigned reg(std::uint32_t offset) { return spu2::read(kSpu + offset, 2); }
void run_samples(unsigned samples) { iop::run(samples * kCyclesPerSample); }

void run(unsigned core, bool loop) {
    iop::reset();
    const unsigned base = core * 0x400;
    const unsigned master = 0x760 + core * 0x28;
    reg(base + 0x19A, 0x8000);
    reg(master, 0x3FFF);
    reg(master + 2, 0x3FFF);
    reg(base + 0x1A8, kBank >> 16);
    reg(base + 0x1AA, kBank & 0xFFFF);
    for (unsigned block = 0; block < 4; ++block) {
        const unsigned flags = block == 0 ? 4 : block == 3 ? (loop ? 3 : 1) : 0;
        reg(base + 0x1AC, (flags << 8) | 2);
        for (unsigned i = 0; i < 7; ++i) reg(base + 0x1AC, 0x7777);
    }
    reg(base, 0x3FFF);
    reg(base + 2, 0x3FFF);
    reg(base + 4, 0x1000);
    reg(base + 6, 0x00FF);
    reg(base + 8, 0);
    reg(base + 0x1C0, kBank >> 16);
    reg(base + 0x1C2, kBank & 0xFFFF);
    reg(base + 0x188, 1);
    reg(base + 0x190, 1);

    std::size_t frames = 0;
    bool audible = false;
    spu2::set_sink([&](const std::int16_t *pcm, std::size_t count) {
        frames += count;
        audible |= std::any_of(pcm, pcm + count * 2, [](std::int16_t sample) { return sample != 0; });
    });
    reg(base + 0x1A0, 1);
    run_samples(64);
    check(audible && reg(base + 0xA) > 0, core, loop, "voice starts and produces sound");
    if (loop) reg(base + 0x1A4, 1);
    run_samples(256);
    check(reg(base + 0xA) == 0, core, loop, "envelope has stopped");
    if (!loop) check(reg(base + 0x340) & 1u, core, loop, "one-shot sets ENDX");

    const unsigned before = reg(base + 0x1CA);
    frames = 0;
    audible = false;
    run_samples(32);
    check(reg(base + 0x1CA) != before, core, loop, "NAX advances after ENVX reaches zero");
    check(frames && !audible, core, loop, "stopped voice contributes only silence");

    if (loop) {
        // Arm the IRQ inside the waveform only after release has completed.
        reg(base + 0x340, 0);
        reg(base + 0x19C, (kBank + 8) >> 16);
        reg(base + 0x19E, (kBank + 8) & 0xFFFF);
        reg(base + 0x19A, 0x8040);
        run_samples(128);
        check(reg(0x7C2) & (4u << core), core, loop, "silent reader reaches IRQA");
        check(reg(base + 0x340) & 1u, core, loop, "silent reader reaches an end block");
    }

    // A write to ENVX alone must not restart a stopped voice.
    reg(base + 0xA, 0x3000);
    audible = false;
    run_samples(32);
    check(!audible, core, loop, "writing ENVX alone does not unmute a stopped voice");
    reg(base + 4, 0);
    const unsigned at_zero_pitch = reg(base + 0x1CA);
    run_samples(128);
    check(reg(base + 0x1CA) == at_zero_pitch, core, loop, "zero pitch holds settled NAX");
    spu2::set_sink({});
}
}

int main() {
    for (unsigned core = 0; core < 2; ++core) {
        run(core, true);
        run(core, false);
    }
    std::printf("SPU2 stopped voice: %u failed of %u checks\n", failed, checks);
    return failed ? 1 : 0;
}
