// No game data: a sound stream that comes in by auto-DMA (ADMA) on sound chip core 0, and what a second sceSdInit does to it
// (issue 51, Mobile Suit Gundam: Federation vs. Zeon: sound effects play, no music or voices).
// What the tester's log showed: the CRI ADX driver (6.56) starts its ADMA loop on core 0 at start-up, the sound driver of the game
// (TSNDDRV) calls sceSdInit a second time right after, and from then on the loop keeps running at the real-time pace (93.75 blocks of
// 2048 bytes a second) while nothing is heard. ps2sdk's open libsd (iop/sound/libsd/src/freesd.c, AFL-2.0) says what a sceSdInit
// writes: the master volumes, the effect volumes and the volumes of the sound-data input (BVOL, the ADMA input) go to 0 and the cores are
// muted, so whatever a driver set before is gone. This test proves the chip side of it, with no disc:
//   1. ADMA on core 0 comes out of the speakers exactly as loud as on core 1 (the core the RE4 driver uses),
//   2. the chip goes on taking the blocks in at the same pace after the wipe, only silent (the signature in the log),
//   3. a driver that sets its volumes again is heard again.
// Build: ninja ps2spu2core0test, run build/.../ps2spu2core0test (prints "spu2 core0: 0 failed").
#include "iop/iop.hpp"
#include "iop/kernel.hpp"
#include "iop/spu2.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {
namespace iop = ps2::iop;
namespace spu2 = ps2::iop::spu2;

int failed = 0, checks = 0;
void check(bool ok, const char *message) {
    ++checks;
    if (!ok) { ++failed; std::fprintf(stderr, "FAIL: %s\n", message); }
}

constexpr std::uint32_t kSpu2 = 0x1F900000u;
constexpr std::uint32_t kBuffer[2] = {0x8E2D0u, 0x8EAD0u};  // two 2048-byte halves, the places RE4's and Gundam's drivers use
constexpr std::uint32_t kBlockBytes = 2048;
// 2048 bytes of sound are 512 frames at 48000 Hz: 10.667 ms, 3'145'728 EE cycles of 294.912 MHz
constexpr std::uint64_t kBlockEeCycles = 3'145'728;

void reg(std::uint32_t offset, std::uint32_t value) { spu2::write(kSpu2 + offset, value, 2); }

// 2048 bytes in the layout the chip takes: 256 frames of left (512 bytes), 256 of right, twice. A tone, loud on both sides.
void fill_block(std::uint32_t at, double phase) {
    for (int piece = 0; piece < 2; ++piece)
        for (int i = 0; i < 256; ++i) {
            const double t = phase + (piece * 256 + i) * 2.0 * M_PI * 440.0 / 48000.0;
            const std::int16_t s = static_cast<std::int16_t>(std::lrint(12000.0 * std::sin(t)));
            std::memcpy(iop::g_ram + at + piece * 1024 + i * 2, &s, 2);
            std::memcpy(iop::g_ram + at + piece * 1024 + 512 + i * 2, &s, 2);
        }
}

std::vector<std::int16_t> g_out;

// One block's time: queue the next half of the loop, as libsd's DMA interrupt does, and let the chip run.
// Returns the output (frames * 2 samples) made in that time.
std::vector<std::int16_t> block(int core, int half, double &phase) {
    fill_block(kBuffer[half], phase);
    phase += 1024 * 2.0 * M_PI * 440.0 / 48000.0;
    g_out.clear();
    spu2::dma(core, kBuffer[half], kBlockBytes / 4, true, core ? 7 : 4);
    iop::run(kBlockEeCycles);
    return g_out;
}

double rms(const std::vector<std::int16_t> &v, std::size_t from) {
    if (v.size() <= from) return 0;
    double sum = 0;
    for (std::size_t i = from; i < v.size(); ++i) sum += static_cast<double>(v[i]) * v[i];
    return std::sqrt(sum / static_cast<double>(v.size() - from));
}

// What libsd's sceSdInit(0) does to a core's volumes (freesd.c ResetAll/InitCoreVolume): master volume, effect volume and the ADMA input
// volume to 0, the ADMA flag cleared, the core muted. A driver has to set them again after it.
void cold_init(int core) {
    const std::uint32_t base = core * 0x400u, master = 0x760u + core * 0x28u;
    reg(base + 0x1B0, 0);       // ADMAS
    reg(base + 0x19A, 0x4000);  // ATTR: muted
    reg(master + 0x0, 0); reg(master + 0x2, 0);  // MVOLL/R
    reg(master + 0x4, 0); reg(master + 0x6, 0);  // EVOLL/R
    reg(master + 0xC, 0); reg(master + 0xE, 0);  // BVOLL/R: the sound-data (ADMA) input
}

// What a driver sets up after its own sceSdInit (a CRI driver: the volumes, the core on and unmuted, ADMA on for its core).
void driver_setup(int core) {
    const std::uint32_t base = core * 0x400u, master = 0x760u + core * 0x28u;
    reg(base + 0x19A, 0x8000);  // ATTR: core on
    reg(master + 0x0, 0x3FFF); reg(master + 0x2, 0x3FFF);
    reg(master + 0xC, 0x7FFF); reg(master + 0xE, 0x7FFF);
    reg(base + 0x1B0, 1u << core);
}

struct Run {
    double healthy = 0, wiped = 0, again = 0;
    std::size_t frames = 0;
    bool input_taken = false;
};

// Plays 20 blocks with the driver's setup, 20 after a second sceSdInit, 20 after the driver set its volumes again.
Run play(int core) {
    iop::reset();
    spu2::set_sink([](const std::int16_t *s, std::size_t frames) { g_out.insert(g_out.end(), s, s + frames * 2); });
    double phase = 0;
    int half = 0;
    Run r;
    cold_init(core);  // the driver's own sceSdInit
    driver_setup(core);
    std::vector<std::int16_t> heard;
    for (int i = 0; i < 20; ++i, half ^= 1) {
        const auto out = block(core, half, phase);
        if (i >= 4) heard.insert(heard.end(), out.begin(), out.end());  // the first blocks fill the chip's input
        r.frames += out.size() / 2;
    }
    r.healthy = rms(heard, 0);
    cold_init(core);  // a second sceSdInit, by another driver, while the loop runs
    // The driver sees its loop has stopped (the ADMA flag is 0) and starts it again with sceSdBlockTrans, which writes the flag and nothing
    // else: the volumes stay 0. (In the tester's log the loop restarts a moment after the second init and then runs for good.)
    reg(core * 0x400u + 0x1B0, 1u << core);
    heard.clear();
    const std::uint16_t *ram = spu2::ram();
    const std::uint32_t input = core ? 0x2400u : 0x2000u;
    std::vector<std::uint16_t> before(ram + input, ram + input + 0x200);
    int changed_blocks = 0;
    for (int i = 0; i < 20; ++i, half ^= 1) {
        const auto out = block(core, half, phase);
        if (i >= 4) heard.insert(heard.end(), out.begin(), out.end());
        r.frames += out.size() / 2;
        // the chip's input area in sound memory takes each new block in: the stream moves on although nothing is heard
        std::vector<std::uint16_t> now(ram + input, ram + input + 0x200);
        if (now != before) ++changed_blocks;
        before = now;
    }
    r.wiped = rms(heard, 0);
    r.input_taken = changed_blocks >= 18;
    driver_setup(core);
    heard.clear();
    for (int i = 0; i < 20; ++i, half ^= 1) {
        const auto out = block(core, half, phase);
        if (i >= 4) heard.insert(heard.end(), out.begin(), out.end());
        r.frames += out.size() / 2;
    }
    r.again = rms(heard, 0);
    spu2::set_sink({});
    return r;
}

}  // namespace

int main() {
    const Run c0 = play(0), c1 = play(1);
    std::printf("core 0: healthy %.0f, after a second sceSdInit %.0f, set up again %.0f (RMS of 32768)\n", c0.healthy, c0.wiped, c0.again);
    std::printf("core 1: healthy %.0f, after a second sceSdInit %.0f, set up again %.0f\n", c1.healthy, c1.wiped, c1.again);
    // 60 blocks of 512 frames: the chip ran in real time (a block per 10.667 ms)
    check(c0.frames > 59 * 512 && c0.frames < 62 * 512, "the chip makes 512 frames per block time on core 0");
    check(c0.healthy > 2000, "ADMA on core 0 is heard");
    check(c1.healthy > 2000, "ADMA on core 1 is heard");
    check(std::fabs(c0.healthy - c1.healthy) < 0.02 * c1.healthy, "core 0 and core 1 are equally loud");
    check(c0.wiped < 50, "after a second sceSdInit (volumes 0) core 0 is silent");
    check(c0.input_taken, "the chip still takes the blocks of the loop in while it is silent (the pace does not change)");
    check(c0.again > 2000, "a driver that sets its volumes again is heard again");
    std::printf("spu2 core0: %d of %d checks failed\n", failed, checks);
    std::printf("spu2 core0: %d failed\n", failed);
    return failed ? 1 : 0;
}
