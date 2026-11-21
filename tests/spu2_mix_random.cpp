// No game data: random sound-chip set-ups run through the mixer, with one fingerprint of everything it made. Used to prove that a change
// to the mixer (runtime/src/iop/spu2.cpp: voices, envelopes, volume sweeps, noise, reverb, the rings in sound memory) is bit for bit
// the same as before: build this program against the runtime before and after the change and compare the two fingerprints.
// The set-ups run one after another in one process (some of the chip's state, like the reverb's last output, is
// not cleared by an IOP reset), so the fingerprint is that of the whole sequence; running the program twice gives the same one.
// What goes into the fingerprint: every output sample, each voice's envelope (ENVX) and the end flags (ENDX) read after every slice of
// IOP time, the IRQ flag, and the whole sound memory at the end (the reverb ring and the chip's own output rings live there).
// Build (no CMake target yet): compile with the runtime's flags and link like ps2spu2core0test; run it, it prints the fingerprint.
#include "iop/iop.hpp"
#include "iop/kernel.hpp"
#include "iop/spu2.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {
namespace iop = ps2::iop;
namespace spu2 = ps2::iop::spu2;

constexpr std::uint32_t kSpu2 = 0x1F900000u;

struct Rng {
    std::uint64_t s;
    std::uint32_t next() {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return static_cast<std::uint32_t>(s >> 11);
    }
    std::uint32_t below(std::uint32_t n) { return next() % n; }
    bool chance(std::uint32_t percent) { return below(100) < percent; }
};

struct Hash {
    std::uint64_t h = 0xCBF29CE484222325ull;
    void add(std::uint32_t v) {
        for (int i = 0; i < 4; ++i) {
            h ^= (v >> (i * 8)) & 0xFFu;
            h *= 0x100000001B3ull;
        }
    }
};

void reg(std::uint32_t offset, std::uint32_t value) { spu2::write(kSpu2 + offset, value & 0xFFFFu, 2); }
std::uint32_t get(std::uint32_t offset) { return spu2::read(kSpu2 + offset, 2); }

// sound data: ADPCM blocks of 8 words with random shifts, filters and flags (end, repeat, loop start) here and there
void fill_ram(Rng &rng) {
    std::uint16_t *ram = spu2::ram();
    for (std::uint32_t at = 0x5000; at < 0x100000; at += 8) {
        std::uint32_t flags = 0;
        if (rng.chance(4)) flags |= 4;                       // loop start
        if (rng.chance(3)) flags |= 1 | (rng.chance(60) ? 2 : 0);  // end, often with repeat
        ram[at] = static_cast<std::uint16_t>(rng.below(13) | rng.below(5) << 4 | flags << 8);
        for (int w = 1; w < 8; ++w) ram[at + w] = static_cast<std::uint16_t>(rng.next());
    }
}

std::uint32_t sound_address(Rng &rng) { return (0x5000u + rng.below((0x100000u - 0x5000u) / 8) * 8u) & 0xFFFFFu; }

void voice_setup(Rng &rng, std::uint32_t core, unsigned v) {
    const std::uint32_t base = core * 0x400u + v * 0x10u;
    // volumes: mostly fixed, sometimes a sweep (bit 15, with its mode bits and rate)
    for (int side = 0; side < 2; ++side) reg(base + side * 2u, rng.chance(25) ? (0x8000u | (rng.next() & 0x7FFFu)) : (rng.next() & 0x7FFFu));
    reg(base + 4, rng.chance(10) ? rng.next() : rng.below(0x4000));  // pitch, now and then above the chip's limit
    reg(base + 6, rng.next());                                         // ADSR1
    reg(base + 8, rng.next());                                         // ADSR2
    const std::uint32_t addr = core * 0x400u + 0x1C0u + v * 12u;
    const std::uint32_t ssa = sound_address(rng);
    reg(addr + 0, ssa >> 16);
    reg(addr + 2, ssa);
    if (rng.chance(50)) {
        const std::uint32_t lsa = sound_address(rng);
        reg(addr + 4, lsa >> 16);
        reg(addr + 6, lsa);
    }
}

void core_setup(Rng &rng, std::uint32_t core) {
    const std::uint32_t k = core * 0x400u;
    std::uint32_t attr = 0x8000u | (rng.next() & 0x3F00u);  // on, with a random noise clock
    if (rng.chance(70)) attr |= 0x80u;                      // reverb on
    if (rng.chance(30)) attr |= 0x40u;                      // IRQ on
    reg(k + 0x19A, attr);
    const std::uint32_t irqa = rng.below(0x100000);
    reg(k + 0x19C, irqa >> 16);
    reg(k + 0x19E, irqa);
    for (std::uint32_t off = 0x188; off <= 0x196; off += 2) reg(k + off, rng.next());  // dry and wet sends of the voices
    reg(k + 0x198, rng.next());                                                        // MMIX
    reg(k + 0x184, rng.chance(20) ? (1u << rng.below(16)) : 0u);                       // noise voices
    reg(k + 0x186, 0);
    // the reverb: its ring (ESA, EEA) and its 22 addresses, some of them past the ring's end
    const std::uint32_t esa = 0x10000u + rng.below(0x60000);
    reg(k + 0x2E0, esa >> 16);
    reg(k + 0x2E2, esa);
    reg(k + 0x33C, rng.chance(10) ? (esa >> 16) : ((esa >> 16) + 1 + rng.below(4)));
    for (std::uint32_t i = 0; i < 22; ++i) {
        const std::uint32_t a = rng.chance(10) ? rng.next() : rng.below(0x30000);
        reg(k + 0x2E4 + i * 4, a >> 16);
        reg(k + 0x2E6 + i * 4, a);
    }
    // master block: master, effect, ADMA and input volumes and the 10 reverb volumes
    const std::uint32_t m = 0x760u + core * 0x28u;
    for (std::uint32_t off = 0; off < 0x28; off += 2) reg(m + off, rng.chance(10) ? (0x8000u | rng.next()) : rng.next());
    for (unsigned v = 0; v < 24; ++v) voice_setup(rng, core, v);
}

std::uint64_t g_samples = 0, g_loud = 0;

std::uint64_t run(std::uint64_t seed) {
    Rng rng{seed * 0x9E3779B97F4A7C15ull + 1};
    Hash hash;
    iop::reset();
    std::vector<std::int16_t> out;
    spu2::set_sink([&out](const std::int16_t *s, std::size_t frames) { out.insert(out.end(), s, s + frames * 2); });
    fill_ram(rng);
    for (std::uint32_t core = 0; core < 2; ++core) core_setup(rng, core);
    for (int slice = 0; slice < 120; ++slice) {
        // now and then: key voices on or off, change a voice's set-up, the sends or the reverb
        for (std::uint32_t core = 0; core < 2; ++core) {
            const std::uint32_t k = core * 0x400u;
            if (rng.chance(40)) reg(k + 0x1A0, rng.next() & rng.next());
            if (rng.chance(40)) reg(k + 0x1A2, rng.next() & 0xFFu);
            if (rng.chance(25)) reg(k + 0x1A4, rng.next() & rng.next() & rng.next());
            if (rng.chance(15)) reg(k + 0x1A6, rng.next() & 0xFFu);
            if (rng.chance(20)) voice_setup(rng, core, rng.below(24));
            if (rng.chance(5)) core_setup(rng, core);
            if (rng.chance(10)) reg(k + 0x340, 0);  // clear ENDX
        }
        iop::run(200'000 + rng.below(400'000));  // EE cycles: about 2 to 6 batches of 32 samples
        for (std::uint32_t core = 0; core < 2; ++core) {
            const std::uint32_t k = core * 0x400u;
            for (unsigned v = 0; v < 24; ++v) hash.add(get(k + v * 0x10u + 0xA));  // ENVX
            hash.add(get(k + 0x340));
            hash.add(get(k + 0x342));
            hash.add(get(k + 0x344));  // STATX / IRQ state as the chip reports it
        }
    }
    for (std::int16_t s : out) hash.add(static_cast<std::uint16_t>(s));
    g_samples += out.size();
    for (std::int16_t s : out) g_loud += s > 64 || s < -64;
    const std::uint16_t *ram = spu2::ram();
    for (std::uint32_t i = 0; i < (1u << 20); ++i) hash.add(ram[i]);
    hash.add(static_cast<std::uint32_t>(out.size()));
    spu2::set_sink(nullptr);
    return hash.h;
}

}

int main(int argc, char **argv) {
    const int seeds = argc > 1 ? std::atoi(argv[1]) : 40;
    Hash all;
    for (int s = 0; s < seeds; ++s) {
        const std::uint64_t h = run(static_cast<std::uint64_t>(s));
        all.add(static_cast<std::uint32_t>(h));
        all.add(static_cast<std::uint32_t>(h >> 32));
    }
    std::printf("spu2 mix random: %d set-ups, %llu samples (%llu not quiet), fingerprint %016llX\n", seeds, static_cast<unsigned long long>(g_samples),
                static_cast<unsigned long long>(g_loud), static_cast<unsigned long long>(all.h));
    return g_loud ? 0 : 1;
}
