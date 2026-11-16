// No game data: LOADFILE call 1 (sceSifLoadElf / sceSifLoadElfPart) copies an EE program from the disc into the EE's memory (public
// issue 98, Xenosaga Episode I: at New Game the game loads its game program this way and calls 0x00A22628 in it; the call was answered
// with 0 and nothing loaded, so the game stopped at empty memory). Packet and answer as in ps2sdk (AFL-2.0, loadfile-common.h:
// entry point at +0, gp at +4, path at +8, section name at +260; the answer is entry point, gp, 0, error). Here the EE side is played
// with made-up packets (tests/fileio_layouts.cpp) on a made-up disc that holds a small made-up MIPS ELF file.
// Build: ninja ps2loadfileelftest, run build/.../ps2loadfileelftest (prints "LOADFILE ELF: 0 failed").
#include "iop/iop.hpp"
#include "iop/kernel.hpp"
#include "iop/sif.hpp"
#include "ps2/disc.hpp"
#include "ps2/memory.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {
namespace iop = ps2::iop;
namespace sif = ps2::sif;
namespace disc = ps2::disc;

int failed = 0;
void check(bool ok, const char *message) {
    if (!ok) { ++failed; std::printf("FAIL: %s\n", message); }
}

constexpr std::uint32_t kXor = 0xACACACACu, kLoadfile = 0x80000006u;
constexpr std::uint32_t kArgs = 0x1E00400, kList = 0x1E00000, kPacket = 0x1E00100;
constexpr std::uint32_t kSentinel = 0xFEEDFEEDu;
constexpr std::uint32_t kEntry = 0x00A22628u, kSegment = 0x00A20000u, kSegmentSize = 0x3000u, kSection = 0x00B00000u;

std::uint32_t word(std::uint32_t at) { return ps2::ld32(at); }

// sceSifLoadElfPart(path, section, &data) as the EE library sends it: the 512-byte argument block is also the answer buffer
// (16 bytes, t_ExecData). Returns the four answer words.
std::vector<std::uint32_t> load_elf(const std::string &path, const std::string &section) {
    std::vector<std::uint8_t> args(512, 0);
    std::memcpy(args.data() + 8, path.c_str(), path.size());
    std::memcpy(args.data() + 8 + 252, section.c_str(), section.size());
    std::memcpy(ps2::g_ram + kArgs, args.data(), args.size());
    for (int i = 0; i < 2; ++i) ps2::st32(kArgs + 4 * i, kSentinel);   // the entry point and gp; the path follows at +8
    const std::uint32_t size = static_cast<std::uint32_t>(args.size());
    std::uint32_t packet[14] = {56, 0, 0x8000000Au, 0, 0, 0, 0, 0, 1, size, kArgs, 16, 0, kLoadfile ^ kXor};
    std::memcpy(ps2::g_ram + kPacket, packet, sizeof packet);
    const std::uint32_t list[8] = {kArgs, 0xDEADBEF0u, size, 0, kPacket, 0x0001E000u, sizeof packet, 0};
    std::memcpy(ps2::g_ram + kList, list, sizeof list);
    sif::set_dma(kList, 2);
    iop::run(4096);
    return {word(kArgs), word(kArgs + 4), word(kArgs + 8), word(kArgs + 12)};
}

// ---- a made-up MIPS ELF: one loadable segment at 0x00A20000 (12 KiB of a pattern, entry 0x00A22628), one empty bss segment,
// and a section ".ovl" of 16 bytes at 0x00B00000 that no segment covers ----
std::vector<std::uint8_t> g_elf;
void put32(std::vector<std::uint8_t> &v, std::size_t at, std::uint32_t x) { std::memcpy(v.data() + at, &x, 4); }
void put16(std::vector<std::uint8_t> &v, std::size_t at, std::uint16_t x) { std::memcpy(v.data() + at, &x, 2); }
std::uint8_t pattern(std::uint32_t i) { return static_cast<std::uint8_t>(i * 7 + 3); }
void make_elf() {
    constexpr std::uint32_t kPh = 0x34, kData = 0x100, kOvl = kData + kSegmentSize, kNames = kOvl + 16, kSh = 0x3200;
    const char names[] = "\0.ovl\0.shstrtab\0";
    g_elf.assign(kSh + 3 * 40, 0);
    std::memcpy(g_elf.data(), "\x7F" "ELF\x01\x01\x01", 7);
    put16(g_elf, 0x10, 2); put16(g_elf, 0x12, 8); put32(g_elf, 0x14, 1);
    put32(g_elf, 0x18, kEntry); put32(g_elf, 0x1C, kPh); put32(g_elf, 0x20, kSh);
    put16(g_elf, 0x28, 0x34); put16(g_elf, 0x2A, 32); put16(g_elf, 0x2C, 2); put16(g_elf, 0x2E, 40); put16(g_elf, 0x30, 3); put16(g_elf, 0x32, 2);
    // PT_LOAD with contents, then PT_LOAD with no file contents (bss)
    put32(g_elf, kPh + 0, 1); put32(g_elf, kPh + 4, kData); put32(g_elf, kPh + 8, kSegment); put32(g_elf, kPh + 16, kSegmentSize); put32(g_elf, kPh + 20, kSegmentSize);
    put32(g_elf, kPh + 32, 1); put32(g_elf, kPh + 40, 0x00A30000u); put32(g_elf, kPh + 52, 0x1000);
    for (std::uint32_t i = 0; i < kSegmentSize; ++i) g_elf[kData + i] = pattern(i);
    for (std::uint32_t i = 0; i < 16; ++i) g_elf[kOvl + i] = static_cast<std::uint8_t>(0xA0 + i);
    std::memcpy(g_elf.data() + kNames, names, sizeof names);
    // section 1 ".ovl" (PROGBITS), section 2 ".shstrtab" (STRTAB)
    const std::size_t s1 = kSh + 40, s2 = kSh + 80;
    put32(g_elf, s1, 1); put32(g_elf, s1 + 4, 1); put32(g_elf, s1 + 12, kSection); put32(g_elf, s1 + 16, kOvl); put32(g_elf, s1 + 20, 16);
    put32(g_elf, s2, 6); put32(g_elf, s2 + 4, 3); put32(g_elf, s2 + 16, kNames); put32(g_elf, s2 + 20, sizeof names);
}

constexpr std::uint32_t kSector = 2048;
std::vector<std::uint8_t> g_img;
void put(std::size_t at, std::uint32_t v) { std::memcpy(g_img.data() + at, &v, 4); }
std::size_t record(std::size_t at, std::uint32_t lba, std::uint32_t size, bool dir, const std::string &name) {
    const std::size_t length = (33 + name.size() + 1) & ~std::size_t{1};
    g_img[at] = static_cast<std::uint8_t>(length);
    put(at + 2, lba);
    put(at + 10, size);
    g_img[at + 25] = dir ? 2 : 0;
    g_img[at + 32] = static_cast<std::uint8_t>(name.size());
    std::memcpy(g_img.data() + at + 33, name.data(), name.size());
    return at + length;
}
void open_disc() {
    make_elf();
    g_img.assign(40 * kSector, 0);
    const std::size_t pvd = 16 * kSector;
    std::memcpy(g_img.data() + pvd, "\x01" "CD001\x01", 7);
    put(pvd + 80, 40);
    put(pvd + 156 + 2, 20);
    put(pvd + 156 + 10, kSector);
    std::size_t at = 20 * kSector;
    at = record(at, 20, kSector, true, std::string(1, '\0'));
    at = record(at, 20, kSector, true, std::string(1, '\1'));
    record(at, 22, static_cast<std::uint32_t>(g_elf.size()), false, "OVL.ELF;1");
    std::memcpy(g_img.data() + 22 * kSector, g_elf.data(), g_elf.size());
    disc::open("loadfile.iso", g_img.size(), [](std::uint64_t offset, void *out, std::size_t bytes) -> std::size_t {
        if (offset >= g_img.size()) return 0;
        const std::size_t n = std::min<std::size_t>(bytes, g_img.size() - offset);
        std::memcpy(out, g_img.data() + offset, n);
        return n;
    });
}

bool segment_in_place() {
    for (std::uint32_t i = 0; i < kSegmentSize; ++i)
        if (ps2::g_ram[kSegment + i] != pattern(i)) return false;
    return true;
}
}  // namespace

int main() {
    iop::reset();
    open_disc();

    // --- the whole program ("all", sceSifLoadElf): the segment's bytes arrive, the answer is the entry point
    check(!segment_in_place(), "the segment's memory starts empty");
    ps2::st32(0x00A30000u, 0x12345678u);
    auto answer = load_elf("cdrom0:\\OVL.ELF;1", "all");
    check(answer[0] == kEntry, "\"all\": the answer's first word is the entry point (was 0: the call was not answered)");
    check(answer[1] == 0, "\"all\": the gp is 0");
    check(segment_in_place(), "\"all\": the segment's 12 KiB are in the EE's memory at 0x00A20000");
    check(word(kEntry) != 0, "\"all\": the word at the address the game calls is no longer empty");
    check(word(0x00A30000u) == 0x12345678u, "\"all\": a segment without file contents leaves memory alone, as the IOP's loader does");
    check(ps2::g_ram[kSection] == 0, "\"all\": a section outside every segment is not copied");

    // --- one section (sceSifLoadElfPart with a name): only that section's bytes, at its address
    std::memset(ps2::g_ram + kSegment, 0, kSegmentSize);
    answer = load_elf("cdrom0:\\OVL.ELF;1", ".ovl");
    check(answer[0] != 0, "section: the answer says loaded");
    bool ovl = true;
    for (std::uint32_t i = 0; i < 16; ++i) ovl = ovl && ps2::g_ram[kSection + i] == 0xA0 + i;
    check(ovl, "section: \".ovl\" is at 0x00B00000");
    check(ps2::g_ram[kSegment] == 0 && word(kEntry) == 0, "section: the segment is not copied");

    // --- failures: entry point 0 (the EE library's "load failed") and the IOP's error in the fourth word
    answer = load_elf("cdrom0:\\NOSUCH.ELF;1", "all");
    check(answer[0] == 0 && answer[3] == static_cast<std::uint32_t>(-203), "a missing file: entry point 0, KE_FILEERR");
    answer = load_elf("cdrom0:\\OVL.ELF;1", ".nosuch");
    check(answer[0] == 0 && answer[3] == static_cast<std::uint32_t>(-201), "a missing section: entry point 0, KE_ILLEGAL_OBJECT");

    std::printf("LOADFILE ELF: %d failed\n", failed);
    return failed ? 1 : 0;
}
