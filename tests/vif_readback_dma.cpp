// No game data. TimeSplitters 2 starts a reverse VIF1 DMA with the previous upload's IRQ still masked and pending.
// It enables the IRQ and waits for two completions. Finishing both transfers before it unmasks the channel loses one.
#include "gs/gs.hpp"
#include "hw.hpp"
#include "ps2/os.hpp"
#include "ps2/system.hpp"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>

namespace {
namespace hw = ps2::hw;
namespace fs = std::filesystem;

constexpr unsigned kVif1 = 1;
constexpr std::uint32_t kChcr = 0x10009000u, kMadr = 0x10009010u, kQwc = 0x10009020u;
constexpr std::uint32_t kStart = 0x100u, kUpload = 1u;
constexpr std::uint32_t kBuffer = 0x00110000u, kReadQwc = 8;
constexpr int kReadCycles = 2 * kReadQwc;

int failed = 0;

void check(bool ok, const char *message) {
    if (!ok) { ++failed; std::fprintf(stderr, "FAIL: %s\n", message); }
}

void advance(int cycles) {
    ps2::set_budget(ps2::g_cpu, cycles);
    ps2::g_cpu.budget = 0;
    ps2::preempt(ps2::g_cpu);
}

bool busy() { return (hw::read32(kChcr) & kStart) != 0; }
bool pending() { return (hw::dmac_pending() & (1u << kVif1)) != 0; }

bool take_interrupt() {
    const bool was_pending = pending();
    if (was_pending) hw::dmac_ack(kVif1);
    return was_pending;
}

void transfer(std::uint32_t qwc, std::uint32_t direction) {
    hw::write32(kMadr, kBuffer);
    hw::write32(kQwc, qwc);
    hw::write32(kChcr, kStart | direction);
}

void readback() {
    // 8x4 PSMCT32 pixels in GS memory give eight quadwords.
    ps2::gs::write(0x50, 1u << 16);               // BITBLTBUF: source buffer width 64 pixels
    ps2::gs::write(0x51, 0);                      // TRXPOS
    ps2::gs::write(0x52, (4ull << 32) | 8u);      // TRXREG
    ps2::gs::write(0x53, 1);                      // TRXDIR: local to host
    hw::write32(0x10003C00u, 0x800000u);           // VIF1_STAT.FDR
    hw::write64(0x12001040u, 1);                  // GS BUSDIR: local to host
    transfer(kReadQwc, 0);
}

void separate_interrupts() {
    hw::dmac_set_mask(kVif1, false);
    transfer(1, kUpload);  // one quadword of zero (VIF NOPs)
    check(!busy() && !pending(), "the upload finishes with its interrupt masked");

    readback();
    check(busy(), "readback remains busy after start");
    hw::dmac_set_mask(kVif1, true);
    check(take_interrupt(), "unmasking delivers the pending upload interrupt");
    advance(kReadCycles - 1);
    check(busy() && !pending(), "readback interrupt has not fired before its deadline");
    advance(1);
    check(!busy(), "readback clears STR on completion");
    check(take_interrupt(), "the readback delivers a separate interrupt");
    advance(kReadCycles);
    check(!pending(), "completion fires once");
}

void cancel_readback() {
    readback();
    advance(4);
    check(busy() && !pending(), "the reverse transfer is still active before cancellation");
    hw::write32(kChcr, 0);
    advance(kReadCycles);
    check(!busy() && !pending(), "clearing STR cancels the pending completion");
    hw::dmac_ack(kVif1);
}

void restore_readback() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto directory = fs::temp_directory_path() / ("ps2-vif1-readback-" + std::to_string(stamp));
    std::error_code error;
    if (!fs::create_directory(directory, error)) {
        check(false, "create a temporary directory for the save-state test");
        return;
    }
    const auto file = directory / "readback.p2s";
    const auto path = ps2::os::utf8(file);

    readback();
    advance(4);
    const bool saved = ps2::save_state(path);
    check(saved, "save with a readback pending");
    advance(kReadCycles - 4);
    check(take_interrupt(), "readback completes before reloading the state");
    if (saved) {
        const bool loaded = ps2::load_state(path);
        check(loaded, "restore the pending readback state");
        if (loaded) {
            advance(kReadCycles - 5);
            check(busy() && !pending(), "restored readback keeps its remaining time");
            advance(1);
            check(!busy() && take_interrupt(), "restored readback completes at its original deadline");
            advance(kReadCycles);
            check(!pending(), "restored completion fires once");
        }
    }

    fs::remove(file, error);
    fs::remove(directory, error);
}

unsigned fqc() { return (hw::read32(0x10003C00u) >> 24) & 0x1Fu; }

// ATV Offroad Fury 3's map loader reads a rectangle by DMA and then waits, with FDR still set, for VIF1_STAT FQC and VPS to be 0.
// A FIFO that read as full for as long as FDR was set hung it on the loading screen. sceGsExecStoreImage reads the FIFO itself and
// polls FQC before each quadword, so FQC must stay non-zero until the last one.
void fifo_empties() {
    // 8x4 PSMCT32 pixels, eight quadwords (with one thread gs::write keeps the emulation thread's copy of these registers)
    ps2::gs::write(0x50, 1u << 16);
    ps2::gs::write(0x51, 0);
    ps2::gs::write(0x52, (4ull << 32) | 8u);
    ps2::gs::write(0x53, 1);
    hw::write32(0x10003C00u, 0x800000u);
    hw::write64(0x12001040u, 1);
    check(fqc() == 8, "the FIFO holds the rectangle's eight quadwords before the game reads them");
    for (int i = 0; i < 7; ++i) {
        ps2::Reg128 q;
        hw::read128(0x10005000u, q);
    }
    check(fqc() == 1, "one quadword is left after seven FIFO reads");
    ps2::gs::write(0x52, (64ull << 32) | 64u);  // the next transfer's size, set before the game is done reading: the GS latched this one
    check(fqc() == 1, "a new TRXREG does not change the download under way");
    transfer(1, 0);
    check(fqc() == 0, "the FIFO is empty once DMA channel 1 took the last quadword");
    check((hw::read32(0x10003C00u) & 3u) == 0, "VPS is idle after the download");
    advance(kReadCycles);
    hw::dmac_ack(kVif1);
    // 3x1 PSMCT32 pixels are 12 bytes: the part-filled quadword waits in the FIFO until the game reads it
    ps2::gs::write(0x52, (1ull << 32) | 3u);
    ps2::gs::write(0x53, 1);
    check(fqc() == 1, "a rectangle smaller than a quadword leaves one in the FIFO");
    {
        ps2::Reg128 q;
        hw::read128(0x10005000u, q);
    }
    check(fqc() == 0, "the FIFO is empty once that quadword is read");
    // A TRXDIR that starts no download (an upload here, or a rectangle the GIF scan never saw) reads as full, as before the fix
    ps2::gs::write(0x53, 0);
    check(fqc() == 16, "with no download started the FIFO reads as full, as before");
    hw::write32(0x10003C00u, 0);
    hw::write64(0x12001040u, 0);
}
}

int main() {
    ps2::boot(0x00100000u, {});
    hw::write32(0x1000E000u, 1);  // D_CTRL.DMAE
    separate_interrupts();
    cancel_readback();
    restore_readback();
    fifo_empties();
    std::printf("VIF1 readback DMA: %d failed\n", failed);
    return failed ? 1 : 0;
}
