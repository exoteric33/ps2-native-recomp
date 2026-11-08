// No game data: a SIF command from the EE that no IOP module handles, as DreamWorks Madagascar: Escape 2 Africa (public issue 70) sends
// system command 0x80000004 with data for IOP memory before every module it loads and then 30 times a second.
// The EE side is played with the DMA list ps2sdk's _SifSendCmd builds (ee/kernel/src/sifcmd.c): the extra data first, then the packet
// to the IOP's command buffer. On the console the IOP drops a packet whose handler slot is empty (ps2sdk iop/system/sifcmd), while the
// data is already in IOP memory; a module that registers the id with sceSifAddCmdHandler gets the packet.
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

constexpr std::uint32_t kCid = 0x80000004u;
constexpr std::uint32_t kPacket = 0x00120000, kData = 0x00120100, kList = 0x00121000;  // EE memory: packet, its data, the DMA list
constexpr std::uint32_t kIopDest = 0x00030000;                                          // where the data goes in IOP memory
constexpr std::uint32_t kDataSize = 64;

// A synthetic IOP module like the one of tests/sif_cmd_intr.cpp, importing sifcmd 10 (sceSifAddCmdHandler), 11, 12 and 13.
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
        put(0x118 + i * 8, 0x24000000u | (10u + i));
    }
    put(0x160, 0x03E00008u); // module start: jr ra; nop
    return elf;
}

// The EE sends command `cid` with kDataSize bytes of data for kIopDest, the two transfers ps2sdk's _SifSendCmd hands sceSifSetDma.
void ee_send(std::uint32_t cid, std::uint32_t fill) {
    std::memset(ps2::g_ram + kPacket, 0, 16);
    ps2::st32(kPacket, 16u | (kDataSize << 8)); ps2::st32(kPacket + 4, kIopDest); ps2::st32(kPacket + 8, cid); ps2::st32(kPacket + 12, 0);
    for (std::uint32_t i = 0; i < kDataSize; i += 4) ps2::st32(kData + i, fill + i);
    ps2::st32(kList, kData); ps2::st32(kList + 4, kIopDest); ps2::st32(kList + 8, kDataSize); ps2::st32(kList + 12, 0);
    ps2::st32(kList + 16, kPacket); ps2::st32(kList + 20, sif::get_reg(2)); ps2::st32(kList + 24, 16); ps2::st32(kList + 28, 0x44);
    check(sif::set_dma(kList, 2) != 0, "sceSifSetDma takes the two transfers");
}

bool data_arrived(std::uint32_t fill) {
    for (std::uint32_t i = 0; i < kDataSize; i += 4)
        if (iop::ram32(kIopDest + i) != fill + i) return false;
    return true;
}
}

int main() {
    iop::reset();
    iop::run(8192);

    // 1. No module handles 0x80000004: the data lands, the packet is dropped and counted, nothing else happens.
    const std::uint64_t before = sif::unhandled_commands(kCid);
    ee_send(kCid, 0x11110000u);
    check(data_arrived(0x11110000u), "the data sent with an unhandled command is in IOP memory");
    check(sif::unhandled_commands(kCid) == before + 1, "the unhandled command is counted");
    for (int i = 0; i < 100; ++i) ee_send(kCid, 0x22220000u + static_cast<std::uint32_t>(i) * 0x100u);
    check(data_arrived(0x22220000u + 99u * 0x100u), "every later data transfer lands too");
    check(sif::unhandled_commands(kCid) == before + 101, "every unhandled command is counted (the log says it once)");
    iop::run(8192);

    // 2. A module registers 0x80000004 with sceSifAddCmdHandler: the next one goes to its handler with the packet and its argument.
    const int id = iop::load_module_buffer(module(), "sifcmd-handler-test.irx", "");
    check(id > 0, "synthetic IOP module loads");
    if (id <= 0) return 1;
    const auto base = kernel::modules().at(id).base;
    iop::run(8192);
    const std::uint32_t add_handler = iop::ram32(base + 20);
    check(add_handler >> 26 == iop::kHleOpcode, "sceSifAddCmdHandler links to its HLE handler");
    int calls = 0;
    std::uint32_t seen_cid = 0, seen_dest = 0, seen_arg = 0;
    bool data_first = false;
    const std::uint32_t handler = kernel::hle_stub([&](iop::Cpu &c) {
        ++calls;
        data_first = data_arrived(0x33330000u);
        seen_cid = iop::ram32(c.r[4] + 8);
        seen_dest = iop::ram32(c.r[4] + 4);
        seen_arg = c.r[5];
    });
    iop::Cpu c{};
    c.r[4] = kCid; c.r[5] = handler; c.r[6] = 0xCAFEu; c.r[29] = 0x18200;
    iop::call_hle(c, add_handler & 0x03FFFFFFu);
    ee_send(kCid, 0x33330000u);
    check(calls == 1, "a registered handler receives the command once");
    check(seen_cid == kCid && seen_dest == kIopDest, "the handler sees the packet header");
    check(seen_arg == 0xCAFEu, "the handler gets the argument it was registered with");
    check(data_first, "the data is in IOP memory when the handler runs");
    check(sif::unhandled_commands(kCid) == before + 101, "a handled command is not counted as unhandled");

    std::printf("SIF command with no handler: %d failed\n", failed);
    return failed ? 1 : 0;
}
