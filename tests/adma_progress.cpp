// No game data: a sound stream sent to the sound chip by auto-DMA (ADMA) moves the IOP DMA channel's address register on as the chip
// takes the data in (48,000 stereo samples a second, 4 bytes each), as on the console, instead of leaving it at the start until the
// whole block is in. Issue 100 (GTA Vice City, the first loading screen never ends): the game's sound driver times the chip at
// start-up by reading that register (libsd's BlockTransStatus) while a looping stream plays, and waits until the position it reads lies
// in a plausible range; with the register standing still it never did. The data must still land where it did (the chip's input buffer
// at SPU2 0x2000 left / 0x2200 right) and the channel must still finish when the last piece is taken.
// The driver's other clock is a quiet noise voice whose output it watches change in the chip's output ring; the chip had no noise
// generator, so that check is here too.
// Build: ninja ps2admaprogresstest, run build/.../ps2admaprogresstest (prints "adma progress: 0 failed").
#include "iop/hw.hpp"
#include "iop/iop.hpp"
#include "iop/spu2.hpp"

#include <cstdio>
#include <cstring>

namespace {
namespace iop = ps2::iop;
namespace hw = ps2::iop::hw;
namespace spu2 = ps2::iop::spu2;

int failed = 0;
void check(bool ok, const char *message, unsigned got) {
    if (!ok) {
        ++failed;
        std::fprintf(stderr, "FAIL: %s (got 0x%X)\n", message, got);
    }
}

constexpr std::uint32_t kSpu2 = 0x1F900000u;
constexpr std::uint32_t kMadr4 = 0x1F8010C0u, kBcr4 = 0x1F8010C4u, kChcr4 = 0x1F8010C8u, kDicr = 0x1F8010F4u;
constexpr std::uint64_t kSampleEeCycles = 294'912'000 / 48'000;  // one stereo sample of the chip in EE cycles
constexpr std::uint32_t kBuffer = 0x6A980;                         // where GTA Vice City keeps its calibration stream

void reg(std::uint32_t offset, std::uint32_t value) { spu2::write(kSpu2 + offset, value, 2); }
std::uint32_t position() { return hw::read(kMadr4, 4) - kBuffer; }
void run_samples(unsigned n) { iop::run(n * kSampleEeCycles); }
// samples played until the channel's address moves (in steps of 8), or `limit` when it does not
unsigned run_until_moves(unsigned limit) {
    const std::uint32_t start = position();
    unsigned n = 0;
    while (n < limit && position() == start) {
        run_samples(8);
        n += 8;
    }
    return n;
}

}

int main() {
    iop::reset();
    spu2::set_sink({});
    // a recognisable stream: 4096 bytes, each 1 KiB piece 512 bytes left then 512 bytes right; the value says which piece and side
    std::uint16_t *data = reinterpret_cast<std::uint16_t *>(iop::g_ram + kBuffer);
    for (unsigned i = 0; i < 2048; ++i) data[i] = static_cast<std::uint16_t>(0x100 * (i / 256 + 1) + i % 256);
    reg(0x19A, 0x8000);  // core 0 on
    reg(0x1B0, 1);       // ADMA on for core 0
    // libsd's block transfer: 16-word blocks, 64 of them (4096 bytes), from the IOP to the chip, start
    hw::write(kBcr4, (64u << 16) | 16u, 4);
    hw::write(kMadr4, kBuffer, 4);
    hw::write(kChcr4, 0x01000201u, 4);

    // The chip's input buffer is a ring of 512 samples read in step with its output rings; it asks for a piece each time it has played
    // half of it, and a piece only goes into the half that is not playing. So: one piece at the start (into the next half), then one
    // piece exactly every 256 samples, each into the half that just finished.
    check(position() == 0x400, "one piece taken at the start: the address is 1024 bytes on", position());
    const std::uint16_t *ram = spu2::ram();
    const unsigned half = ram[0x2000] == 0x100 ? 0 : ram[0x2100] == 0x100 ? 256 : 999;
    check(half != 999, "piece 1 starts at a half of the input buffer", ram[0x2000]);
    if (half == 999) {
        std::printf("adma progress: %d failed\n", failed);
        return 1;
    }
    // the data lands as it always did: the left side from the first 512 bytes of the piece, the right side from the rest
    check(ram[0x2200 + half] == 0x200 && ram[0x2000 + half + 255] == 0x1FF, "piece 1 fills one half, left and right", ram[0x2200 + half]);
    const unsigned first = run_until_moves(600);
    check(first >= 8 && first <= 264, "piece 2 comes within one half of the start", first);
    check(position() == 0x800, "piece 2 moved the address to 2048 bytes", position());
    check(ram[0x2000 + (half + 256) % 512] == 0x300 && ram[0x2200 + (half + 256) % 512] == 0x400, "piece 2 went into the other half", ram[0x2000 + (half + 256) % 512]);
    const unsigned second = run_until_moves(600);
    check(second >= 248 && second <= 264, "piece 3 comes 256 samples after piece 2", second);
    check(position() == 0xC00, "piece 3 moved the address to 3072 bytes", position());
    check(ram[0x2000 + half] == 0x500 && ram[0x2200 + half] == 0x600, "piece 3 went into the half piece 1 played from", ram[0x2000 + half]);
    check((hw::read(kChcr4, 4) & 0x01000000u) != 0, "the channel still runs with a piece left", hw::read(kChcr4, 4));
    const unsigned third = run_until_moves(600);
    check(third >= 248 && third <= 264, "piece 4 comes 256 samples after piece 3", third);
    check(position() == 0x1000, "the last piece moved the address to the end", position());
    run_samples(8);
    check((hw::read(kChcr4, 4) & 0x01000000u) == 0, "the channel has finished after the last piece", hw::read(kChcr4, 4));
    check((hw::read(kDicr, 4) & (1u << 28)) != 0, "channel 4 raised its done flag", hw::read(kDicr, 4));

    // a plain upload (ADMA off) still goes in at once and finishes
    reg(0x1B0, 0);
    reg(0x1A8, 0);
    reg(0x1AA, 0x8000);  // transfer start address 0x8000 (in halfwords)
    hw::write(kBcr4, (4u << 16) | 16u, 4);
    hw::write(kMadr4, kBuffer, 4);
    hw::write(kChcr4, 0x01000201u, 4);
    run_samples(8);
    check(ram[0x8000] == 0x100 && ram[0x8000 + 127] == 0x17F, "a plain upload copies its data at once", ram[0x8000 + 127]);
    check((hw::read(kChcr4, 4) & 0x01000000u) == 0, "a plain upload finishes", hw::read(kChcr4, 4));

    // The other half of the driver's clock: a quiet noise voice (core 1, voice 23, noise clock 30, as GTA Vice City sets it up) whose
    // sound the driver watches change at the start of the core's dry-left output ring (SPU2 0x1800). The noise generator replaces the
    // voice's sound data; with none, the voice plays the zeros at its start address and the ring never changes.
    reg(0x400 + 0x19A, 0x8000 | (30u << 8));  // core 1 on, noise clock 30
    reg(0x400 + 23 * 0x10 + 0x0, 0x00FF);     // volume left, quiet
    reg(0x400 + 23 * 0x10 + 0x6, 0x000F);     // fastest attack, sustain at the top
    reg(0x400 + 23 * 0x10 + 0x8, 0x0005);
    reg(0x400 + 0x1C0 + 23 * 0xC, 0);         // start address 0x2800 (zeros)
    reg(0x400 + 0x1C2 + 23 * 0xC, 0x2800);
    reg(0x400 + 0x186, 0x80);                 // noise on for voice 23
    reg(0x400 + 0x18A, 0x80);                 // voice 23 to the dry left mix
    reg(0x400 + 0x1A2, 0x80);                 // key on
    run_samples(64);
    unsigned changes = 0;
    std::uint16_t last = ram[0x1800];
    for (int cycle = 0; cycle < 16; ++cycle) {
        run_samples(512);
        if (ram[0x1800] != last) ++changes;
        last = ram[0x1800];
    }
    check(changes >= 12, "a noise voice changes the start of the output ring in most of 16 ring cycles", changes);

    std::printf("adma progress: %d failed\n", failed);
    return failed ? 1 : 0;
}
