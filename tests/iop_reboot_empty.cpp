// No game data: an IOP reset with no image, as the Need for Speed games issue it (ps2sdk's SifIopReset("", 0)), and the waits that follow.
// The EE side is played here with the same calls ps2sdk's iopcontrol.c, sifcmd.c and sifrpc.c make into the runtime; the IOP side is a
// synthetic module that sends SIF commands to the EE (the way a sound, pad or network module of a game does all the time).
#include "iop/kernel.hpp"
#include "iop/sif.hpp"
#include "ps2/memory.hpp"

#include <cstdio>
#include <cstring>
#include <vector>

namespace {
namespace iop = ps2::iop;
namespace kernel = ps2::iop::kernel;
namespace sif = ps2::sif;

int failed = 0;
void check(bool ok, const char *message) {
    if (!ok) { ++failed; std::fprintf(stderr, "FAIL: %s\n", message); }
}

constexpr std::uint32_t kSifInit = 0x10000, kCmdInit = 0x20000, kBootEnd = 0x40000;   // SMFLAG bits (ps2sdk sifdma.h)
constexpr std::uint32_t kRecv1 = 0x00100000, kRecv2 = 0x00110000;                       // the EE's receive buffers (before and after)
constexpr std::uint32_t kPacket = 0x00120000, kList = 0x00121000;                       // an outgoing packet and its DMA list
constexpr std::uint32_t kIopPacket = 0x18000, kStack = 0x18200;                         // IOP memory for the synthetic sender

// The same module as tests/sif_cmd_intr.cpp: it imports four sifcmd functions (indexes 12, 13, 28, 29), nothing else.
std::vector<std::uint8_t> module() {
    std::vector<std::uint8_t> elf(0x180);
    const auto put = [&](std::size_t at, auto value) { std::memcpy(elf.data() + at, &value, sizeof value); };
    std::memcpy(elf.data(), "\177ELF\1\1\1", 7);
    put(16, std::uint16_t{0xFF80}); put(18, std::uint16_t{8}); put(20, 1u);
    put(28, 52u); put(40, std::uint16_t{52}); put(42, std::uint16_t{32}); put(44, std::uint16_t{2});
    put(52, 0x70000080u); put(56, 0x80u);
    put(84, 1u); put(88, 0x100u); put(100, 0x80u); put(104, 0x80u);
    put(0x84, 0x60u); put(0x8C, 0x80u);
    put(0x100, 0x41E00000u); put(0x108, 0x101u);
    std::memcpy(elf.data() + 0x10C, "sifcmd", 6);
    for (unsigned i = 0; i < 4; ++i) {
        put(0x114 + i * 8, 0x03E00008u);
        put(0x118 + i * 8, 0x24000000u | (i < 2 ? 12u + i : 26u + i));
    }
    put(0x160, 0x03E00008u); // module start: jr ra; nop
    return elf;
}

// The IOP module sends a 16-byte command `cid` to the EE (sceSifSendCmd, import 0 of the module at `base`).
bool iop_sends(std::uint32_t base, std::uint32_t cid) {
    iop::Cpu c{};
    c.r[4] = cid; c.r[5] = kIopPacket; c.r[6] = 16; c.r[7] = 0; c.r[29] = kStack;
    iop::set_ram32(kStack + 16, 0); iop::set_ram32(kStack + 20, 0); iop::set_ram32(kStack + 24, 0); iop::set_ram32(kStack + 28, 0);
    iop::call_hle(c, iop::ram32(base + 20) & 0x03FFFFFFu);
    return c.r[2] != 0;
}

// The EE sends one DMA transfer (sceSifSetDma with a single entry) of `size` bytes at kPacket to `dest`.
std::uint32_t ee_dma(std::uint32_t dest, std::uint32_t size) {
    ps2::st32(kList, kPacket); ps2::st32(kList + 4, dest); ps2::st32(kList + 8, size); ps2::st32(kList + 12, 0);
    return sif::set_dma(kList, 1);
}

// sceSifInitCmd's last step: SIF command 0x80000002 (opt 0) tells the IOP where the EE receives (sifcmd.c).
void ee_init_cmd(std::uint32_t receive) {
    std::uint32_t polls = 0;
    while (!(sif::get_reg(4) & kCmdInit) && polls++ < 1000) iop::run(1000);      // while (!(SMFLAG & CMDINIT))
    check(polls < 1000, "SMFLAG shows the IOP's command layer (CMDINIT)");
    std::memset((ps2::g_ram + (kPacket)), 0, 32);
    ps2::st32(kPacket, 32); ps2::st32(kPacket + 8, 0x80000002u); ps2::st32(kPacket + 12, 0); ps2::st32(kPacket + 16, receive);
    check(ee_dma(sif::get_reg(2), 32) != 0, "INIT_CMD is accepted (SIF_REG_SUBADDR)");
}

// ps2sdk SifIopReset(arg, mode) then SifIopSync(): the register writes and the one DMA transfer, in its order.
void ee_iop_reset(const char *arg, std::uint32_t mode, std::uint32_t cid = 0x80000003u) {
    std::memset((ps2::g_ram + (kPacket)), 0, 104);
    const std::uint32_t arglen = static_cast<std::uint32_t>(std::strlen(arg));
    ps2::st32(kPacket, 104); ps2::st32(kPacket + 8, cid); ps2::st32(kPacket + 16, arglen); ps2::st32(kPacket + 20, mode);
    std::memcpy((ps2::g_ram + (kPacket + 24)), arg, arglen);
    sif::set_reg(0x80000000u, 0);                                // (the dest is SifGetReg(SIF_SYSREG_SUBADDR) below)
    const std::uint32_t dest = sif::get_reg(0x80000000u);
    sif::set_reg(4, kBootEnd);                                    // sceSifSetReg(SIF_REG_SMFLAG, BOOTEND)
    check(ee_dma(dest, 104) != 0, "the reset transfer is accepted");
    sif::set_reg(4, kSifInit);
    sif::set_reg(4, kCmdInit);
    sif::set_reg(0x80000002u, 0);                                // RPCINIT
    sif::set_reg(0x80000000u, 0);                                // SUBADDR
}
}

int main() {
    iop::reset();
    const std::size_t base_threads = kernel::thread_ids().size();

    // --- 1. the console is up: the EE has told the IOP where it receives, a game module is loaded and talks to the EE
    ee_init_cmd(kRecv1);
    int id = iop::load_module_buffer(module(), "game.irx", "");
    check(id > 0, "synthetic game module loads");
    if (id <= 0) return 1;
    std::uint32_t base = kernel::modules().at(id).base;
    iop::run(8192);
    ps2::st32(kRecv1, 0); ps2::st32(kRecv1 + 8, 0);
    check(iop_sends(base, 0xAAAA0001u), "the module sends a command");
    check(ps2::ld32(kRecv1 + 8) == 0xAAAA0001u, "it arrives in the EE's receive buffer");
    // The EE has not yet run its interrupt handler for it (that is where a game is when it starts the reset) and the module sends again.
    check(iop_sends(base, 0xAAAA0002u), "the module sends a second command");
    check(ps2::ld32(kRecv1 + 8) == 0xAAAA0001u, "the second waits for the EE (the first is not acknowledged)");

    // --- 2. a command that is not a reset, sent to address 0, does not touch the IOP
    ee_iop_reset("", 0, 0x80000001u);
    check(kernel::modules().count(id) == 1, "a command other than 0x80000003 sent to address 0 leaves the modules alone");

    // --- 3. a reset with an image (what IOPRP games do) is not changed: the modules of the game stay
    ee_iop_reset("rom0:UDNL cdrom0:\\NFSUNDER\\IOPRP271.IMG;1", 0);
    check(kernel::modules().count(id) == 1, "a reset with an image leaves the modules alone (as before)");

    // --- 4. SifIopReset("", 0): the IOP starts again from its own modules
    ee_iop_reset("", 0);
    std::uint32_t polls = 0;
    while (!(sif::get_reg(4) & kBootEnd) && polls++ < 1000) iop::run(1000);   // while (!SifIopSync())
    check(polls < 1000, "SifIopSync() sees the boot-end bit");
    check((sif::get_reg(4) & (kSifInit | kCmdInit | kBootEnd)) == (kSifInit | kCmdInit | kBootEnd), "SMFLAG has SIFINIT, CMDINIT and BOOTEND after the reset");
    check(kernel::modules().empty(), "the game's modules are gone");
    check(kernel::thread_ids().size() == base_threads, "the game's threads are gone");
    ee_init_cmd(kRecv2);                                                     // sceSifInitRpc -> sceSifInitCmd after the reboot
    ps2::st32(kRecv2, 0); ps2::st32(kRecv2 + 8, 0);

    // --- 5. the game loads its modules again; what the old IOP had queued is gone and the new IOP can talk to the EE
    id = iop::load_module_buffer(module(), "game.irx", "");
    check(id > 0, "the module loads again after the reset");
    if (id <= 0) return 1;
    base = kernel::modules().at(id).base;
    iop::run(8192);
    check(iop_sends(base, 0xBBBB0003u), "the new module sends a command");
    check(ps2::ld32(kRecv2 + 8) == 0xBBBB0003u, "it reaches the EE's new receive buffer at once (nothing of the old IOP is in front of it)");
    sif::packet_processed();                                                  // iSifSetDChain at the end of the EE's interrupt handler
    iop::run(8192);
    check(ps2::ld32(kRecv2 + 8) == 0xBBBB0003u, "the old IOP's second command never arrives");

    std::printf("IOP reset with no image: %d failed\n", failed);
    return failed ? 1 : 0;
}
