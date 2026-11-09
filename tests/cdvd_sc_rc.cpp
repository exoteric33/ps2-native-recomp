// No game data: cdvdman's sceCdSC (#50) and sceCdRC (#51) answer like a drive with a disc in it (public issue 57 Champions of Norrath: its
// cdvdstm module calls #50 when it starts; GTA III: mcman calls #51 when it writes a save). Rules from ps2sdk
// iop/cdvd/cdvdman (exports.tab, cdvdman.c, cdvdman.h; AFL-2.0): sceCdRC reads the clock chip (S command 0x08) into an sceCdCLOCK of
// eight BCD bytes and returns 1; sceCdSC(code, param) is a table of values by negative code, unknown codes give 0.
// The IOP side is a synthetic module importing cdvdman #8 (sceCdGetError), #24 (sceCdReadClock), #50 and #51.
#include "hw.hpp"
#include "iop/kernel.hpp"
#include "ps2/memory.hpp"

#include <cstdio>
#include <cstring>
#include <vector>

namespace {
namespace iop = ps2::iop;
namespace kernel = ps2::iop::kernel;
namespace hw = ps2::hw;

int failed = 0;
void check(bool ok, const char *message) {
    if (!ok) { ++failed; std::fprintf(stderr, "FAIL: %s\n", message); }
}

constexpr std::uint32_t kStack = 0x18400, kParam = 0x18100, kClock = 0x18200;

// One import table "cdvdman" (version 0x101) with #8, #24, #50, #51 at module +0x14, +0x1C, +0x24, +0x2C (`jr ra; addiu zero, zero, index`).
std::vector<std::uint8_t> module() {
    std::vector<std::uint8_t> elf(0x1A0);
    const auto put = [&](std::size_t at, auto value) { std::memcpy(elf.data() + at, &value, sizeof value); };
    std::memcpy(elf.data(), "\177ELF\1\1\1", 7);
    put(16, std::uint16_t{0xFF80}); put(18, std::uint16_t{8}); put(20, 1u);
    put(28, 52u); put(40, std::uint16_t{52}); put(42, std::uint16_t{32}); put(44, std::uint16_t{2});
    put(52, 0x70000080u); put(56, 0x80u);
    put(84, 1u); put(88, 0x100u); put(100, 0xA0u); put(104, 0xA0u);
    put(0x84, 0x80u); put(0x8C, 0xA0u);                                     // entry +0x80, text size
    put(0x100, 0x41E00000u); put(0x108, 0x101u);
    std::memcpy(elf.data() + 0x10C, "cdvdman", 7);
    const std::uint32_t indexes[] = {8, 24, 50, 51};
    for (int i = 0; i < 4; ++i) { put(0x114 + 8 * i, 0x03E00008u); put(0x118 + 8 * i, 0x24000000u | indexes[i]); }
    put(0x134, 0u); put(0x138, 0u);
    put(0x180, 0x03E00008u);                                                 // module start (+0x80): jr ra; nop
    return elf;
}

std::uint32_t g_base = 0;
std::uint32_t import(std::uint32_t offset) { return iop::ram32(g_base + offset) & 0x03FFFFFFu; }

std::uint32_t call(std::uint32_t offset, std::uint32_t a0, std::uint32_t a1 = 0) {
    iop::Cpu c{};
    c.r[4] = a0; c.r[5] = a1; c.r[29] = kStack; c.r[31] = 0x1000;
    iop::call_hle(c, import(offset));
    return c.r[2];
}
std::uint32_t get_error() { return call(0x14, 0); }
std::uint32_t read_clock(std::uint32_t at) { return call(0x1C, at); }
std::uint32_t cd_sc(std::uint32_t code, std::uint32_t param = 0) { return call(0x24, code, param); }
std::uint32_t cd_rc(std::uint32_t at) { return call(0x2C, at); }

std::uint8_t byte(std::uint32_t at) { return *iop::ram_ptr(at); }
bool bcd_in(std::uint8_t v, unsigned lo, unsigned hi) {
    if ((v & 0xF) > 9 || (v >> 4) > 9) return false;
    const unsigned n = (v >> 4) * 10 + (v & 0xF);
    return n >= lo && n <= hi;
}
}

int main() {
    iop::reset();
    hw::reset();
    const int id = iop::load_module_buffer(module(), "driver.irx", "");
    check(id > 0, "synthetic IOP module loads");
    if (id <= 0) return 1;
    g_base = kernel::modules().at(id).base;
    iop::run(8192);
    check(iop::ram32(g_base + 0x14) >> 26 == iop::kHleOpcode && iop::ram32(g_base + 0x2C) >> 26 == iop::kHleOpcode, "the imports link to HLE");

    // 1. sceCdRC: the clock, eight bytes, status 0, BCD in range, 1 returned (mcman asks up to 64 times until it is not 0)
    std::memset(ps2::iop::ram_ptr(kClock), 0xEE, 8);
    check(cd_rc(kClock) == 1, "sceCdRC returns 1");
    check(byte(kClock + 0) == 0, "the clock's status is 0 (read fine)");
    check(bcd_in(byte(kClock + 1), 0, 59) && bcd_in(byte(kClock + 2), 0, 59), "second and minute are BCD, 0 to 59");
    check(bcd_in(byte(kClock + 3), 0, 23), "hour is BCD, 0 to 23");
    check(byte(kClock + 4) == 0, "the pad byte is 0");
    check(bcd_in(byte(kClock + 5), 1, 31) && bcd_in(byte(kClock + 6), 1, 12), "day and month are BCD, 1 to 31 and 1 to 12");
    check(bcd_in(byte(kClock + 7), 0, 99), "year is BCD");
    check((byte(kClock + 6) & 0x80) == 0, "the month's top bit is clear");
    check(cd_rc(0) == 0, "a null pointer is refused (answer 0)");

    // 2. it says the same as sceCdReadClock (the clock may tick between the two calls: try again then)
    bool same = false;
    for (int attempt = 0; attempt < 3 && !same; ++attempt) {
        cd_rc(kClock);
        read_clock(kClock + 0x20);
        same = std::memcmp(ps2::iop::ram_ptr(kClock), ps2::iop::ram_ptr(kClock + 0x20), 8) == 0;
    }
    check(same, "sceCdRC and sceCdReadClock give the same bytes");

    // 3. sceCdSC: the last error and the stream flag (codes -1, 0, 1, 2, -2)
    iop::set_ram32(kParam, 0x7777);
    check(cd_sc(0xFFFFFFFFu, kParam) == 0 && iop::ram32(kParam) == 0, "code -1: no stream, last error 0 at *param");
    check(cd_sc(1, kParam) == 1 && iop::ram32(kParam) == 0, "code 1 sets the stream flag and gives it");
    check(cd_sc(0xFFFFFFFFu, kParam) == 1, "code -1 reads it back");
    check(cd_sc(0, kParam) == 0 && cd_sc(0xFFFFFFFFu, kParam) == 0, "code 0 clears it");
    iop::set_ram32(kParam, 0x0E);
    check(cd_sc(0xFFFFFFFEu, kParam) == 0x0E && get_error() == 0x0E, "code -2 sets the last error, sceCdGetError sees it");
    cd_sc(0xFFFFFFFFu, kParam);
    check(iop::ram32(kParam) == 0x0E, "code -1 hands that error to *param");
    iop::set_ram32(kParam, 0);
    cd_sc(0xFFFFFFFEu, kParam);
    check(get_error() == 0, "and 0 sets it back");

    // 4. plain values: version, timeouts, a read in flight, modes
    check(cd_sc(0xFFFFFFF7u) == 0x0226, "code -9: cdvdman's version (2.38)");
    check(cd_sc(0xFFFFFFF1u) == 15000 && cd_sc(0xFFFFFFEFu) == 5000, "codes -15 and -17: the default timeouts, 15000 and 5000 ms");
    check(cd_sc(0xFFFFFFFDu) == 0, "code -3: no read is running");
    check(cd_sc(0xFFFFFFF4u) == 1 && cd_sc(0xFFFFFFF3u) == 1 && cd_sc(0xFFFFFFEAu) == 1, "codes -12, -13, -22 give 1");
    iop::set_ram32(kParam, 3);
    check(cd_sc(0xFFFFFFF8u, kParam) == 1, "code -8 (spin control) gives 1");
    iop::set_ram32(kParam, 6);
    check(cd_sc(0xFFFFFFF6u, kParam) == 6, "code -10 gives the EE call it was told");
    iop::set_ram32(kParam, 0);
    check(cd_sc(0xFFFFFFF6u, kParam) == 0, "and 0 when none runs");

    // 5. a code that cdvdman does not know gives 0, and the pointer stays untouched
    iop::set_ram32(kParam, 0x1234);
    check(cd_sc(0xFFFFFF00u, kParam) == 0 && iop::ram32(kParam) == 0x1234, "an unknown code gives 0 and leaves *param");

    if (failed) { std::fprintf(stderr, "%d check(s) failed\n", failed); return 1; }
    std::puts("cdvd sceCdSC and sceCdRC: ok");
    return 0;
}
