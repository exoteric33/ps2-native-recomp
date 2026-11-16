// No game data. Three things testers' logs showed on 2026-11-10 (public issues 57 and 91):
// 1. A two-layer disc has a file system on each layer. The plain search (cdvdman #10 sceCdSearchFile) reads layer 0's; only
//    sceCdLayerSearchFile (#84) with layer 1 reads layer 1's (ps2sdk iop/cdvd/cdvdman cdvdman.c, AFL-2.0). Champions of Norrath got
//    layer 1's copy of a directory from the plain search. A synthetic image here has the same directory on both layers.
// 2. cdvdman #64 sceCdRM(name, status) gives the console's model name (ps2sdk libcdvd-common.h), status 0, returns 1.
// 3. The i.LINK link chip (0x1F808400) as a driver starts it with no cable (ps2sdk iLinkman, AFL-2.0): SOK follows LPSEn in ctrl2,
//    the ctrl0 resets clear, a PHY read finishes with PhyRRx in intr0, the interrupt registers clear the bits written as 1.
#include "iop/hw.hpp"
#include "iop/kernel.hpp"
#include "ps2/disc.hpp"
#include "ps2/memory.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {
namespace iop = ps2::iop;
namespace kernel = ps2::iop::kernel;
namespace disc = ps2::disc;

int failed = 0;
void check(bool ok, const char *message) {
    if (!ok) { ++failed; std::fprintf(stderr, "FAIL: %s\n", message); }
}

// ---- a two-layer ISO 9660 image: layer 0 is 40 sectors (its volume size), layer 1 starts there, its descriptor at 40 + 16 ----

constexpr std::uint32_t kLayer1 = 40, kSectors = 80;

void put(std::vector<std::uint8_t> &img, std::size_t at, std::uint32_t v) { std::memcpy(img.data() + at, &v, 4); }

std::size_t record(std::vector<std::uint8_t> &img, std::size_t at, std::uint32_t lba, std::uint32_t size, bool dir, const std::string &name) {
    const std::size_t length = (33 + name.size() + 1) & ~std::size_t{1};
    img[at] = static_cast<std::uint8_t>(length);
    put(img, at + 2, lba);
    put(img, at + 10, size);
    img[at + 25] = dir ? 2 : 0;
    img[at + 32] = static_cast<std::uint8_t>(name.size());
    std::memcpy(img.data() + at + 33, name.data(), name.size());
    return at + length;
}

// One layer's file system at `base`, sectors counted from `base` as on the disc: root at 20 with BG/ (21) and one file of its own,
// BG/DATA/ (22). Layer 0's own file is A.BIN, layer 1's ONLY1.BIN.
void layer(std::vector<std::uint8_t> &img, std::uint32_t base, std::uint32_t volume_size, const std::string &own_file, std::uint32_t own_lba) {
    const std::size_t pvd = (base + 16) * disc::kSector;
    std::memcpy(img.data() + pvd, "\x01" "CD001\x01", 7);
    put(img, pvd + 80, volume_size);
    put(img, pvd + 156 + 2, 20);
    put(img, pvd + 156 + 10, disc::kSector);
    const auto dir = [&](std::uint32_t lba, std::uint32_t parent) {
        std::size_t at = (base + lba) * disc::kSector;
        at = record(img, at, lba, disc::kSector, true, std::string(1, '\0'));
        return record(img, at, parent, disc::kSector, true, std::string(1, '\1'));
    };
    std::size_t at = dir(20, 20);
    at = record(img, at, 21, disc::kSector, true, "BG");
    record(img, at, own_lba, 100, false, own_file + ";1");
    at = dir(21, 20);
    record(img, at, 22, disc::kSector, true, "DATA");
    dir(22, 21);
}

std::vector<std::uint8_t> g_img;

void open_two_layers() {
    g_img.assign(kSectors * disc::kSector, 0);
    layer(g_img, 0, kLayer1, "A.BIN", 30);
    layer(g_img, kLayer1, kSectors - kLayer1, "ONLY1.BIN", 25);
    disc::open("two-layers.iso", g_img.size(), [](std::uint64_t offset, void *out, std::size_t bytes) -> std::size_t {
        if (offset >= g_img.size()) return 0;
        const std::size_t n = std::min<std::size_t>(bytes, g_img.size() - offset);
        std::memcpy(out, g_img.data() + offset, n);
        return n;
    });
}

std::uint32_t sector_of(const std::string &path, int layer = -1) {
    const auto e = layer < 0 ? disc::find(path) : disc::find(path, layer);
    return e ? e->sector : 0xFFFFFFFFu;
}

// ---- a synthetic IOP module importing cdvdman #10, #64, #84 and #8 (as in tests/cdvd_sc_rc.cpp) ----

constexpr std::uint32_t kStack = 0x18400, kFile = 0x18100, kName = 0x18200, kStatus = 0x18300;

std::vector<std::uint8_t> module() {
    std::vector<std::uint8_t> elf(0x1A0);
    const auto set = [&](std::size_t at, auto value) { std::memcpy(elf.data() + at, &value, sizeof value); };
    std::memcpy(elf.data(), "\177ELF\1\1\1", 7);
    set(16, std::uint16_t{0xFF80}); set(18, std::uint16_t{8}); set(20, 1u);
    set(28, 52u); set(40, std::uint16_t{52}); set(42, std::uint16_t{32}); set(44, std::uint16_t{2});
    set(52, 0x70000080u); set(56, 0x80u);
    set(84, 1u); set(88, 0x100u); set(100, 0xA0u); set(104, 0xA0u);
    set(0x84, 0x80u); set(0x8C, 0xA0u);
    set(0x100, 0x41E00000u); set(0x108, 0x101u);
    std::memcpy(elf.data() + 0x10C, "cdvdman", 7);
    const std::uint32_t indexes[] = {10, 64, 84, 8};
    for (int i = 0; i < 4; ++i) { set(0x114 + 8 * i, 0x03E00008u); set(0x118 + 8 * i, 0x24000000u | indexes[i]); }
    set(0x134, 0u); set(0x138, 0u);
    set(0x180, 0x03E00008u);
    return elf;
}

std::uint32_t g_base = 0;

std::uint32_t call(std::uint32_t offset, std::uint32_t a0, std::uint32_t a1 = 0, std::uint32_t a2 = 0) {
    iop::Cpu c{};
    c.r[4] = a0; c.r[5] = a1; c.r[6] = a2; c.r[29] = kStack; c.r[31] = 0x1000;
    iop::call_hle(c, iop::ram32(g_base + offset) & 0x03FFFFFFu);
    return c.r[2];
}

void text(std::uint32_t at, const char *s) { std::memcpy(iop::ram_ptr(at), s, std::strlen(s) + 1); }

std::uint32_t search(const char *path) { text(kName, path); return call(0x14, kFile, kName) ? iop::ram32(kFile) : 0xFFFFFFFFu; }
std::uint32_t layer_search(const char *path, int layer) {
    text(kName, path);
    return call(0x24, kFile, kName, static_cast<std::uint32_t>(layer)) ? iop::ram32(kFile) : 0xFFFFFFFFu;
}

// ---- i.LINK registers ----
namespace hw = ps2::iop::hw;
constexpr std::uint32_t kIlink = 0x1F808400u;
std::uint32_t reg(std::uint32_t offset) { return hw::read(kIlink + offset, 4); }
void set_reg(std::uint32_t offset, std::uint32_t v) { hw::write(kIlink + offset, v, 4); }
}

int main() {
    // 1a. the disc index: layer 0's entry for the plain search, layer 1's for a layer 1 search
    open_two_layers();
    check(disc::layer1_start() == kLayer1, "the second layer is found at sector 40");
    check(sector_of("cdrom0:\\BG\\DATA;1") == 22, "plain search: \\BG\\DATA of layer 0 (sector 22), not of layer 1");
    check(sector_of("\\BG\\DATA", 0) == 22, "layer 0 search: sector 22");
    check(sector_of("\\BG\\DATA", 1) == kLayer1 + 22, "layer 1 search: sector 40 + 22 (layer 1 counts from its start)");
    check(sector_of("\\A.BIN") == 30 && sector_of("\\A.BIN", 1) == 30, "a file only layer 0 has is found by both searches (as before)");
    check(sector_of("\\ONLY1.BIN") == kLayer1 + 25, "a file only layer 1 has is still found by the plain search (as before)");
    check(sector_of("\\ONLY1.BIN", 1) == kLayer1 + 25, "and by the layer 1 search");

    // 1b. the same through cdvdman's calls
    iop::reset();
    ps2::iop::hw::reset();
    const int id = iop::load_module_buffer(module(), "driver.irx", "");
    check(id > 0, "synthetic IOP module loads");
    if (id <= 0) return 1;
    g_base = kernel::modules().at(id).base;
    iop::run(8192);
    check(iop::ram32(g_base + 0x14) >> 26 == iop::kHleOpcode && iop::ram32(g_base + 0x2C) >> 26 == iop::kHleOpcode, "the imports link to HLE");
    check(search("\\BG\\DATA;1") == 22, "sceCdSearchFile gives layer 0's \\BG\\DATA");
    check(layer_search("\\BG\\DATA;1", 0) == 22, "sceCdLayerSearchFile layer 0 gives layer 0's");
    check(layer_search("\\BG\\DATA;1", 1) == kLayer1 + 22, "sceCdLayerSearchFile layer 1 gives layer 1's");

    // 2. sceCdRM: a model name in 16 bytes, status 0, returns 1
    std::memset(iop::ram_ptr(kName), 0xEE, 16);
    iop::set_ram32(kStatus, 0x7777);
    check(call(0x1C, kName, kStatus) == 1, "sceCdRM returns 1");
    check(std::memcmp(iop::ram_ptr(kName), "SCPH-", 5) == 0, "the name starts with SCPH-");
    check(iop::ram_ptr(kName)[15] == 0, "the name ends inside its 16 bytes");
    check(iop::ram32(kStatus) == 0, "the status is 0");
    check(call(0x2C, 0) == 0, "sceCdGetError still answers (no error)");

    // 3. i.LINK: the start-up of Unreal Tournament's PSX2ILI, as its log shows it, then what an iLinkman-style driver does next
    set_reg(0x24, 0); set_reg(0x2C, 0); set_reg(0x34, 0);
    check(reg(0x00) == 0, "NodeID reads 0");
    set_reg(0x7C, 1);
    check((reg(0x10) & 0x8) == 0, "ctrl2: no SOK while the link is off");
    set_reg(0x10, 0x2);
    check((reg(0x10) & 0xA) == 0xA, "ctrl2: LPSEn written, SOK (clock stable) reads set");
    set_reg(0x08, 0x03000000u | 0x800u);
    check(reg(0x08) == 0x800u, "ctrl0: the transmitter and receiver resets are done at once, other bits stay");
    set_reg(0x20, 0xFFFFFFFFu);
    check(reg(0x20) == 0, "intr0: writing 1s clears, it does not set");
    set_reg(0x14, (3u << 24) | 0x80000000u);
    check((reg(0x14) & 0xC0000000u) == 0, "PHYAccess: the read is done at once");
    check(((reg(0x14) >> 8) & 0xF) == 3 && (reg(0x14) & 0xFF) == 0, "PHYAccess: register 3 comes back, value 0");
    check((reg(0x20) & 0x40000000u) != 0, "intr0: PhyRRx is set after the PHY read");
    set_reg(0x20, 0x40000000u);
    check((reg(0x20) & 0x40000000u) == 0, "intr0: PhyRRx clears when written as 1");
    set_reg(0x14, (4u << 24) | (0x55u << 16) | 0x40000000u);
    check((reg(0x14) & 0xC0000000u) == 0, "PHYAccess: a write is done at once");
    set_reg(0x7C, 0x40);
    check(reg(0x7C) == 0x40, "other registers keep what was written");

    if (failed) { std::fprintf(stderr, "%d check(s) failed\n", failed); return 1; }
    std::puts("disc layers, sceCdRM and i.LINK: ok");
    return 0;
}
