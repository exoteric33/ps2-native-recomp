// No game data: the EE's FILEIO calls in both packet layouts (public issue 78, Quake III Revolution PBPX_952.01). The game's library sent
// the old layout (ps2sdk fileio.c, AFL-2.0: open is the mode at +0 and the name at +4, close the fd, read and write the fd, the address and
// the size, lseek the fd, the offset and the whence, the answer is the call's own reply) to a runtime that had switched to the newer one
// (a header of three words, then fd/address/size at +12, +16, +20, the open's name at +20, the answer in the buffer the EE gave with call
// 255), so "rom0:ROMVER" was read as the mode "VER" with an empty name and failed. Here the EE side is played with made-up packets, the
// same two transfers the console's sceSifCallRpc makes (tests/sif_call_log.cpp), once in each layout, with the runtime in the newer
// protocol (as after a reboot with IOPRP205.IMG), then with a made-up disc whose SYSTEM.CNF names an American game.
// Build: ninja ps2fileiolayoutstest, run build/.../ps2fileiolayoutstest (prints "FILEIO layouts: 0 failed").
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

constexpr std::uint32_t kXor = 0xACACACACu, kFileio = 0x80000001u;
constexpr std::uint32_t kArgs = 0x1E00200, kRecv = 0x1E00300, kList = 0x1E00000, kPacket = 0x1E00100, kResult = 0x1E00800, kDest = 0x1E01000;
constexpr std::uint32_t kSentinel = 0xFEEDFEEDu;

std::uint32_t word(std::uint32_t at) { return ps2::ld32(at); }

// One FILEIO call as the EE kernel sends it: the arguments, then the call packet; the answer is read from kRecv.
std::uint32_t ee_call(std::uint32_t method, const std::vector<std::uint8_t> &args) {
    std::memcpy(ps2::g_ram + kArgs, args.data(), args.size());
    ps2::st32(kRecv, kSentinel);
    const std::uint32_t size = static_cast<std::uint32_t>(args.size());
    std::uint32_t packet[14] = {56, 0, 0x8000000Au, 0, 0, 0, 0, 0, method, size, kRecv, 4, 0, kFileio ^ kXor};
    std::memcpy(ps2::g_ram + kPacket, packet, sizeof packet);
    const std::uint32_t list[8] = {kArgs, 0xDEADBEF0u, size, 0, kPacket, 0x0001E000u, sizeof packet, 0};
    std::memcpy(ps2::g_ram + kList, list, sizeof list);
    sif::set_dma(kList, 2);
    iop::run(4096);
    return word(kRecv);
}

struct Packet {
    std::vector<std::uint8_t> bytes;
    explicit Packet(std::size_t size) : bytes(size, 0) {}
    Packet &u32(std::size_t at, std::uint32_t v) { std::memcpy(bytes.data() + at, &v, 4); return *this; }
    Packet &text(std::size_t at, const std::string &s) { std::memcpy(bytes.data() + at, s.c_str(), s.size()); return *this; }
};

// The newer layout (FILEIO 2.05): word 0 is the result buffer, the three words before the fields.
std::uint32_t new_call(std::uint32_t method, const Packet &p) {
    for (int i = 0; i < 12; ++i) ps2::st32(kResult + 4 * i, kSentinel);
    ee_call(method, p.bytes);
    return word(kResult + 16);   // the answer after the 16-byte header
}
bool result_untouched() { return word(kResult + 16) == kSentinel; }

constexpr std::uint32_t kSector = 2048;

// ---- a one-layer ISO 9660 image with SYSTEM.CNF (an American game's boot file) ----
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
const char kCnf[] = "BOOT2 = cdrom0:\\PBPX_952.01;1\r\nVER = 1.00\r\n";
void open_disc() {
    g_img.assign(40 * kSector, 0);
    const std::size_t pvd = 16 * kSector;
    std::memcpy(g_img.data() + pvd, "\x01" "CD001\x01", 7);
    put(pvd + 80, 40);
    put(pvd + 156 + 2, 20);
    put(pvd + 156 + 10, kSector);
    std::size_t at = 20 * kSector;
    at = record(at, 20, kSector, true, std::string(1, '\0'));
    at = record(at, 20, kSector, true, std::string(1, '\1'));
    record(at, 30, sizeof kCnf - 1, false, "SYSTEM.CNF;1");
    std::memcpy(g_img.data() + 30 * kSector, kCnf, sizeof kCnf - 1);
    disc::open("fileio.iso", g_img.size(), [](std::uint64_t offset, void *out, std::size_t bytes) -> std::size_t {
        if (offset >= g_img.size()) return 0;
        const std::size_t n = std::min<std::size_t>(bytes, g_img.size() - offset);
        std::memcpy(out, g_img.data() + offset, n);
        return n;
    });
}

std::string dest_text(std::size_t n) { return std::string(reinterpret_cast<const char *>(ps2::g_ram + kDest), n); }

// ps2sdk SifIopReset("rom0:UDNL IOPRP205.IMG", 0): the reboot request that switches the runtime to the newer protocol.
void reboot_with_image() {
    constexpr std::uint32_t kReboot = 0x00120000, kRebootList = 0x00121000, kBootEnd = 0x40000;
    const char arg[] = "rom0:UDNL IOPRP205.IMG";
    std::memset(ps2::g_ram + kReboot, 0, 104);
    ps2::st32(kReboot, 104); ps2::st32(kReboot + 8, 0x80000003u); ps2::st32(kReboot + 16, sizeof arg - 1);
    std::memcpy(ps2::g_ram + kReboot + 24, arg, sizeof arg - 1);
    sif::set_reg(0x80000000u, 0);
    const std::uint32_t to = sif::get_reg(0x80000000u);
    sif::set_reg(4, kBootEnd);
    ps2::st32(kRebootList, kReboot); ps2::st32(kRebootList + 4, to); ps2::st32(kRebootList + 8, 104); ps2::st32(kRebootList + 12, 0);
    sif::set_dma(kRebootList, 1);
    iop::run(4096);
}
}  // namespace

int main() {
    iop::reset();
    reboot_with_image();

    // The EE hands over its result buffer with call 255, as the newer library does first.
    ee_call(255, Packet(16).u32(0, kResult).bytes);

    // --- the newer layout (what games use today) must keep working
    std::uint32_t fd = new_call(0, Packet(276).u32(0, kResult).u32(12, 1).text(20, "rom0:ROMVER"));
    check(fd >= 3 && fd < 64, "newer layout: open rom0:ROMVER gives a descriptor");
    check(word(kResult + 4) == 0, "newer layout: the answer names the open call");
    std::memset(ps2::g_ram + kDest, 0, 32);
    check(new_call(2, Packet(24).u32(0, kResult).u32(12, fd).u32(16, kDest).u32(20, 8)) == 8, "newer layout: read of 8 bytes");
    check(dest_text(8) == "0220EC20", "newer layout: ROMVER reads back");
    check(new_call(4, Packet(24).u32(0, kResult).u32(12, fd).u32(16, 4).u32(20, 0)) == 4, "newer layout: lseek");
    check(new_call(1, Packet(16).u32(0, kResult).u32(12, fd)) == 0, "newer layout: close");
    check(new_call(0, Packet(276).u32(0, kResult).u32(12, 1).text(20, "rom0:NOSUCH")) == 0xFFFFFFFEu, "newer layout: a missing file is -2");
    const char hello[] = "";
    std::memcpy(ps2::g_ram + kDest, hello, 1);
    check(new_call(3, Packet(24).u32(0, kResult).u32(12, 5).u32(16, kDest).u32(20, 7)) == 7, "newer layout: write returns the size");
    check(new_call(12, Packet(272).u32(0, kResult).u32(12, kDest).text(16, "cdrom0:\\NOSUCH")) == 0xFFFFFFFEu, "newer layout: stat of a missing file");

    // --- the old layout, in the same protocol: the answer is the call's own reply, the result buffer stays alone
    for (int i = 0; i < 12; ++i) ps2::st32(kResult + 4 * i, kSentinel);
    fd = ee_call(0, Packet(260).u32(0, 1).text(4, "rom0:ROMVER").bytes);
    check(fd >= 3 && fd < 64, "old layout: open rom0:ROMVER gives a descriptor (this failed with -2 before)");
    check(result_untouched(), "old layout: nothing is written to the newer protocol's buffer");
    std::memset(ps2::g_ram + kDest, 0, 32);
    check(ee_call(2, Packet(16).u32(0, fd).u32(4, kDest).u32(8, 8).bytes) == 8, "old layout: read of 8 bytes");
    check(dest_text(8) == "0220EC20", "old layout: ROMVER reads back");
    check(ee_call(4, Packet(12).u32(0, fd).u32(4, 0).u32(8, 2).bytes) == 15, "old layout: lseek to the end gives the size");
    check(ee_call(1, Packet(4).u32(0, fd).bytes) == 0, "old layout: close");
    check(ee_call(1, Packet(4).u32(0, fd).bytes) == 0xFFFFFFF7u, "old layout: a second close is -9");
    check(ee_call(3, Packet(28).u32(0, 1).u32(4, kDest).u32(8, 3).bytes) == 3, "old layout: write returns the size");
    check(ee_call(0, Packet(260).u32(0, 1).text(4, "cdrom0:\\A\\B\\C\\D\\E\\FILE.BIN;1").bytes) == 0xFFFFFFFEu, "old layout: a long name is read from +4 too");
    check(ee_call(12, Packet(260).u32(0, kDest).text(4, "cdrom0:\\NOSUCH").bytes) == 0xFFFFFFFEu, "old layout: stat of a missing file");
    check(result_untouched(), "old layout: still nothing in the newer protocol's buffer");

    // --- both layouts again after each other, on a disc of an American game
    open_disc();
    fd = ee_call(0, Packet(260).u32(0, 1).text(4, "rom0:ROMVER").bytes);
    std::memset(ps2::g_ram + kDest, 0, 32);
    ee_call(2, Packet(16).u32(0, fd).u32(4, kDest).u32(8, 8).bytes);
    check(dest_text(8) == "0220AC20", "the region letter of ROMVER follows the disc's game ID (PBPX: America)");
    ee_call(1, Packet(4).u32(0, fd).bytes);
    fd = ee_call(0, Packet(260).u32(0, 1).text(4, "cdrom0:\\SYSTEM.CNF;1").bytes);
    check(fd >= 3 && fd < 64, "old layout: a long disc path opens");
    std::memset(ps2::g_ram + kDest, 0, 32);
    check(ee_call(2, Packet(16).u32(0, fd).u32(4, kDest).u32(8, 5).bytes) == 5, "old layout: read from the disc");
    check(dest_text(5) == "BOOT2", "old layout: the disc file's bytes arrive");
    ee_call(1, Packet(4).u32(0, fd).bytes);
    fd = new_call(0, Packet(276).u32(0, kResult).u32(12, 1).text(20, "cdrom0:\\SYSTEM.CNF;1"));
    check(fd >= 3 && fd < 64, "newer layout: the same file opens");
    check(new_call(1, Packet(16).u32(0, kResult).u32(12, fd)) == 0, "newer layout: close");

    std::printf("FILEIO layouts: %d failed\n", failed);
    return failed ? 1 : 0;
}
