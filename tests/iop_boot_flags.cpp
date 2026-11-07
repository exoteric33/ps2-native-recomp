// No game data: the SMFLAG bits the IOP raises while it boots after a reset (SIFINIT 0x10000, CMDINIT 0x20000, BOOTEND 0x40000),
// as seen by the EE's waits. The EE side is played with the register writes and the one DMA transfer of ps2sdk's SifIopReset
// (iopcontrol.c, AFL-2.0). Public issue 61: a vendor library's wait for the end of the reset is `while (f()) ;` where f() returns 1
// while SMFLAG has BOOTEND (and runs the library's reset callbacks); on the console the bit is down right after the reset command,
// so the loop ends at its first poll. With the bits always up it never ended.
#include "iop/kernel.hpp"
#include "iop/sif.hpp"
#include "ps2/memory.hpp"
#include "ps2/state.hpp"

#include <cstdio>
#include <cstring>
#include <vector>

namespace {
namespace iop = ps2::iop;
namespace sif = ps2::sif;

int failed = 0;
void check(bool ok, const char *message) {
    if (!ok) { ++failed; std::fprintf(stderr, "FAIL: %s\n", message); }
}

constexpr std::uint32_t kSifInit = 0x10000, kCmdInit = 0x20000, kBootEnd = 0x40000, kAll = kSifInit | kCmdInit | kBootEnd;
constexpr std::uint32_t kPacket = 0x00120000, kList = 0x00121000;

// The EE sends one 104-byte reset command with `arg` as its text, and the register writes around it, in ps2sdk's order.
void ee_iop_reset(const char *arg) {
    std::memset(ps2::g_ram + kPacket, 0, 104);
    const std::uint32_t arglen = static_cast<std::uint32_t>(std::strlen(arg));
    ps2::st32(kPacket, 104); ps2::st32(kPacket + 8, 0x80000003u); ps2::st32(kPacket + 16, arglen);
    std::memcpy(ps2::g_ram + kPacket + 24, arg, arglen);
    sif::set_reg(4, kBootEnd);                                   // sceSifSetReg(SIF_REG_SMFLAG, BOOTEND)
    ps2::st32(kList, kPacket); ps2::st32(kList + 4, 0); ps2::st32(kList + 8, 104); ps2::st32(kList + 12, 0);
    sif::set_dma(kList, 1);                                      // to SifGetReg(SIF_SYSREG_SUBADDR) = 0
    sif::set_reg(4, kSifInit);
    sif::set_reg(4, kCmdInit);
}

// The vendor library's wait: 1 while BOOTEND is up.
int vendor_sync() { return (sif::get_reg(4) & kBootEnd) ? 1 : 0; }

// A state in memory, to save and load the SIF section.
struct MemoryStream : ps2::state::Stream {
    std::vector<std::uint8_t> data;
    std::size_t at = 0;
    explicit MemoryStream(bool saving) : Stream(saving) {}
    void bytes(void *p, std::size_t n) override {
        if (saving()) { data.insert(data.end(), static_cast<std::uint8_t *>(p), static_cast<std::uint8_t *>(p) + n); return; }
        std::memcpy(p, data.data() + at, n);
        at += n;
    }
};

void reset_and_wait(const char *arg, const char *what) {
    char text[160];
    check((sif::get_reg(4) & kAll) == kAll, "the IOP is up before the reset");
    ee_iop_reset(arg);
    std::snprintf(text, sizeof text, "%s: SIFINIT, CMDINIT and BOOTEND are down right after the reset command", what);
    check((sif::get_reg(4) & kAll) == 0, text);
    std::snprintf(text, sizeof text, "%s: the vendor wait `while (f())` ends at its first poll", what);
    check(vendor_sync() == 0, text);
    std::uint32_t polls = 0;
    while (!(sif::get_reg(4) & kBootEnd) && polls++ < 1000) iop::run(1000);   // while (!SifIopSync())
    std::snprintf(text, sizeof text, "%s: BOOTEND comes back after the boot time", what);
    check(polls > 0 && polls < 1000, text);
    std::snprintf(text, sizeof text, "%s: SIFINIT, CMDINIT and BOOTEND are all up again", what);
    check((sif::get_reg(4) & kAll) == kAll, text);
}
}

int main() {
    iop::reset();
    iop::run(8192);
    check((sif::get_reg(4) & (kCmdInit | kBootEnd)) == (kCmdInit | kBootEnd), "the console is up with CMDINIT and BOOTEND");
    sif::set_reg(4, kAll);
    check((sif::get_reg(4) & (kCmdInit | kBootEnd)) == (kCmdInit | kBootEnd), "an EE write outside a reset does not take the bits down (nothing would raise them)");

    // 1. a reset with an image (what the IOPRP games do, Gundam: Ichinen Sensou)
    reset_and_wait("rom0:UDNL cdrom0:\\IOPRP300.IMG;1", "with an image");
    // 2. a reset with no image (Need for Speed)
    reset_and_wait("", "with no image");
    // 3. the same again: the bits go down again
    reset_and_wait("rom0:UDNL cdrom0:\\IOPRP300.IMG;1", "a second time");

    // 4. a state saved while the IOP boots (the boot time is not stored) has the bits when it is loaded
    ee_iop_reset("rom0:UDNL cdrom0:\\IOPRP300.IMG;1");
    check((sif::get_reg(4) & kBootEnd) == 0, "the IOP boots again");
    MemoryStream out(true);
    sif::state(out);
    MemoryStream in(false);
    in.data = out.data;
    sif::state(in);
    check((sif::get_reg(4) & kAll) == kAll, "a state loaded in the middle of a boot shows the IOP up");

    std::printf("IOP boot flags: %d failed\n", failed);
    return failed ? 1 : 0;
}
