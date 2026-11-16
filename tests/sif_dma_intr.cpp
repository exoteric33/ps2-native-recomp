// No game data: an IOP module's sceSifSetDma (and sceSifSetDmaIntr) with SIF_DMA_INT_O raises the EE's SIF0 interrupt, DMA channel 5,
// whose handlers the EE kernel runs (AddDmacHandler(5, ...)); without the attribute it does not. On the console INT_O sets the IRQ bit of
// the EE's DMA tag (ps2sdk sifman.c, AFL-2.0): the EE's channel stops after the transfer and raises CIS5, as it does after every command
// packet, and the EE's handler starts it again with isceSifSetDChain. Public report 71 (Over the Hedge) waits for something like this.
// The IOP side is a synthetic module importing sifman's sceSifSetDma (#7) and sceSifSetDmaIntr (#32) and sifcmd's sceSifSendCmd (#12);
// the EE side is played with the calls ps2sdk's sifcmd.c makes (INIT_CMD, then isceSifSetDChain at the end of the interrupt handler).
#include "hw.hpp"
#include "iop/kernel.hpp"
#include "iop/sif.hpp"
#include "ps2/memory.hpp"
#include "ps2/state.hpp"

#include <cstdio>
#include <cstring>
#include <vector>

namespace {
namespace iop = ps2::iop;
namespace kernel = ps2::iop::kernel;
namespace sif = ps2::sif;
namespace hw = ps2::hw;

int failed = 0;
void check(bool ok, const char *message) {
    if (!ok) { ++failed; std::fprintf(stderr, "FAIL: %s\n", message); }
}

constexpr std::uint32_t kCmdInit = 0x20000;                                   // SMFLAG bit (ps2sdk sifdma.h)
constexpr std::uint32_t kSifDmaIntI = 0x2, kSifDmaIntO = 0x4;                 // SifDmaTransfer_t.attr (ps2sdk sifman.h)
constexpr std::uint32_t kRecv = 0x00100000;                                   // the EE's command receive buffer
constexpr std::uint32_t kEePacket = 0x00120000, kEeList = 0x00121000;         // the EE's INIT_CMD packet and its DMA list
constexpr std::uint32_t kEeData = 0x00130000;                                 // where the IOP module's raw transfer lands
constexpr std::uint32_t kIopList = 0x18000, kIopData = 0x18100, kIopPacket = 0x18200, kStack = 0x18400;

// Imports: sifman #7, #32, then sifcmd #12 (two import tables, each `jr ra; addiu zero, zero, index` per function, ending in a zero word).
std::vector<std::uint8_t> module() {
    std::vector<std::uint8_t> elf(0x1A0);
    const auto put = [&](std::size_t at, auto value) { std::memcpy(elf.data() + at, &value, sizeof value); };
    std::memcpy(elf.data(), "\177ELF\1\1\1", 7);
    put(16, std::uint16_t{0xFF80}); put(18, std::uint16_t{8}); put(20, 1u);
    put(28, 52u); put(40, std::uint16_t{52}); put(42, std::uint16_t{32}); put(44, std::uint16_t{2});
    put(52, 0x70000080u); put(56, 0x80u);
    put(84, 1u); put(88, 0x100u); put(100, 0xA0u); put(104, 0xA0u);
    put(0x84, 0x80u); put(0x8C, 0xA0u);                                     // entry +0x80, text size
    put(0x100, 0x41E00000u); put(0x108, 0x101u);                             // import table 1 at module +0x00
    std::memcpy(elf.data() + 0x10C, "sifman", 6);
    put(0x114, 0x03E00008u); put(0x118, 0x24000007u);                       // +0x14 sceSifSetDma
    put(0x11C, 0x03E00008u); put(0x120, 0x24000020u);                       // +0x1C sceSifSetDmaIntr
    put(0x124, 0u); put(0x128, 0u);
    put(0x12C, 0x41E00000u); put(0x134, 0x101u);                             // import table 2 at module +0x2C
    std::memcpy(elf.data() + 0x138, "sifcmd", 6);
    put(0x140, 0x03E00008u); put(0x144, 0x2400000Cu);                       // +0x40 sceSifSendCmd
    put(0x148, 0u); put(0x14C, 0u);
    put(0x180, 0x03E00008u);                                                 // module start (+0x80): jr ra; nop
    return elf;
}

std::uint32_t g_base = 0;
std::uint32_t import(std::uint32_t offset) { return iop::ram32(g_base + offset) & 0x03FFFFFFu; }

// The module sends one raw transfer of 16 bytes (value `word`) to kEeData with attribute `attr`, by sceSifSetDma or sceSifSetDmaIntr.
bool iop_dma(std::uint32_t attr, std::uint32_t word, bool intr = false, std::uint32_t callback = 0) {
    iop::set_ram32(kIopData, word);
    iop::set_ram32(kIopList, kIopData); iop::set_ram32(kIopList + 4, kEeData); iop::set_ram32(kIopList + 8, 16); iop::set_ram32(kIopList + 12, attr);
    iop::Cpu c{};
    c.r[4] = kIopList; c.r[5] = 1; c.r[6] = callback; c.r[7] = 0x5150u; c.r[29] = kStack; c.r[31] = 0x1000;
    iop::call_hle(c, import(intr ? 0x1C : 0x14));
    return c.r[2] != 0;
}

// The module sends a 16-byte command `cid` to the EE (sceSifSendCmd).
bool iop_sends(std::uint32_t cid) {
    iop::Cpu c{};
    c.r[4] = cid; c.r[5] = kIopPacket; c.r[6] = 16; c.r[7] = 0; c.r[29] = kStack; c.r[31] = 0x1000;
    iop::set_ram32(kStack + 16, 0); iop::set_ram32(kStack + 20, 0);
    iop::call_hle(c, import(0x40));
    return c.r[2] != 0;
}

// sceSifInitCmd's last step: command 0x80000002 (opt 0) tells the IOP where the EE receives (sifcmd.c).
void ee_init_cmd() {
    std::uint32_t polls = 0;
    while (!(sif::get_reg(4) & kCmdInit) && polls++ < 1000) iop::run(1000);
    std::memset(ps2::g_ram + kEePacket, 0, 32);
    ps2::st32(kEePacket, 32); ps2::st32(kEePacket + 8, 0x80000002u); ps2::st32(kEePacket + 16, kRecv);
    ps2::st32(kEeList, kEePacket); ps2::st32(kEeList + 4, sif::get_reg(2)); ps2::st32(kEeList + 8, 32); ps2::st32(kEeList + 12, 0);
    check(sif::set_dma(kEeList, 1) != 0, "INIT_CMD is accepted");
}

// Channel 5's interrupt is pending for the EE kernel (CIS5 with CIM5 set): it runs the AddDmacHandler(5) handlers. Taking it clears CIS5,
// as the kernel does before it runs them.
bool ee_takes_interrupt() {
    const bool pending = hw::dmac_pending() & (1u << 5);
    if (pending) hw::dmac_ack(5);
    return pending;
}

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
}

int main() {
    iop::reset();
    hw::reset();
    hw::dmac_set_mask(5, true);                                               // EnableDmac(DMAC_SIF0), as sceSifInitCmd does
    ee_init_cmd();
    const int id = iop::load_module_buffer(module(), "driver.irx", "");
    check(id > 0, "synthetic IOP module loads");
    if (id <= 0) return 1;
    g_base = kernel::modules().at(id).base;
    iop::run(8192);
    check(iop::ram32(g_base + 0x14) >> 26 == iop::kHleOpcode && iop::ram32(g_base + 0x40) >> 26 == iop::kHleOpcode, "the imports link to HLE");
    ee_takes_interrupt();

    // 1. without the attribute: the data arrives, no interrupt
    check(iop_dma(0, 0x11111111u), "sceSifSetDma without INT_O returns an id");
    check(ps2::ld32(kEeData) == 0x11111111u, "its data is in EE memory");
    check(!ee_takes_interrupt(), "no channel 5 interrupt without INT_O");
    check(iop_dma(kSifDmaIntI, 0x22222222u), "sceSifSetDma with INT_I only returns an id");
    check(!ee_takes_interrupt(), "INT_I alone (the IOP's own interrupt) does not interrupt the EE");

    // 2. with INT_O: the data arrives and the EE's channel 5 handler runs
    check(iop_dma(kSifDmaIntO, 0x33333333u), "sceSifSetDma with INT_O returns an id");
    check(ps2::ld32(kEeData) == 0x33333333u, "its data is in EE memory before the interrupt");
    check(ee_takes_interrupt(), "INT_O raises the EE's channel 5 interrupt");
    check(!ee_takes_interrupt(), "only once");

    // 3. the channel stopped after it: a command waits for the handler's isceSifSetDChain, then arrives with its own interrupt
    ps2::st32(kRecv + 8, 0);
    check(iop_sends(0x00000011u), "the module sends a command");
    check(ps2::ld32(kRecv + 8) == 0 && !ee_takes_interrupt(), "it waits while the EE's channel is stopped");
    sif::packet_processed();                                                  // the game's handler: isceSifSetDChain
    check(ps2::ld32(kRecv + 8) == 0x00000011u && ee_takes_interrupt(), "it arrives once the handler starts the channel again");

    // 4. an INT_O transfer while a packet is still being handled: its interrupt comes after that, in order
    check(iop_dma(kSifDmaIntO, 0x44444444u), "a second INT_O transfer");
    check(ps2::ld32(kEeData) == 0x44444444u, "its data is there");
    check(!ee_takes_interrupt(), "its interrupt waits behind the packet the EE has not finished");
    sif::packet_processed();                                                  // sifcmd's handler, done with the packet
    check(ee_takes_interrupt(), "then it comes");

    // 5. a state saved while that interrupt waits keeps it
    check(iop_dma(kSifDmaIntO, 0x55555555u), "a third INT_O transfer, the channel still stopped");
    check(!ee_takes_interrupt(), "it waits");
    MemoryStream out(true);
    sif::state(out);
    sif::packet_processed();
    check(ee_takes_interrupt(), "it comes when the channel runs");
    MemoryStream in(false);
    in.data = out.data;
    sif::state(in);
    sif::packet_processed();
    check(ee_takes_interrupt(), "it is still waiting after the state is loaded");

    // 6. sceSifSetDmaIntr with INT_O: the interrupt, and the IOP callback once the transfer is done
    sif::packet_processed();
    int calls = 0;
    const std::uint32_t callback = kernel::hle_stub([&](iop::Cpu &c) { ++calls; check(c.r[4] == 0x5150u, "the callback gets its argument"); });
    check(iop_dma(kSifDmaIntO, 0x66666666u, true, callback), "sceSifSetDmaIntr with INT_O returns an id");
    check(ee_takes_interrupt(), "sceSifSetDmaIntr with INT_O raises the interrupt too");
    iop::run(8192);
    check(calls == 1, "and its callback runs once");
    sif::packet_processed();
    check(iop_dma(0, 0x77777777u, true, callback), "sceSifSetDmaIntr without INT_O");
    check(!ee_takes_interrupt(), "does not interrupt the EE");
    iop::run(8192);
    check(calls == 2, "its callback runs as before");

    std::printf("SIF DMA interrupt: %d failed\n", failed);
    return failed ? 1 : 0;
}
