// No game data: ioman's AddDrv and DelDrv, the way ps2sdk's iomanX does them (AFL-2.0): AddDrv takes a free place and calls the driver's
// init with the device, and fails (-1) when init returns below 0; DelDrv finds the name, calls deinit and frees the place; an unknown name
// fails. Before, both answered -1 at once and never called the driver, so a game driver that checks AddDrv's result did not stay.
// The module is hand-written R3000 code that calls the two imports from its start function: everything it needs is reached relative
// to its own address (bal), so it needs no relocation. Its driver's init and deinit count their calls and clear the 16 bytes at their
// stack pointer (the argument area a callee may use), which must not disturb the ioman call they run inside.
#include "iop/kernel.hpp"
#include "ps2/memory.hpp"

#include <cstdio>
#include <cstring>
#include <vector>

namespace {
namespace iop = ps2::iop;
namespace kernel = ps2::iop::kernel;

int failed = 0;
void check(bool ok, const char *message) {
    if (!ok) { ++failed; std::fprintf(stderr, "FAIL: %s\n", message); }
}

enum : std::uint32_t { zero = 0, v0 = 2, a0 = 4, t0 = 8, t1 = 9, s0 = 16, sp = 29, ra = 31 };
std::uint32_t addiu(std::uint32_t rt, std::uint32_t rs, std::int32_t imm) { return 0x24000000u | rs << 21 | rt << 16 | (static_cast<std::uint32_t>(imm) & 0xFFFFu); }
std::uint32_t sw(std::uint32_t rt, std::int32_t off, std::uint32_t base) { return 0xAC000000u | base << 21 | rt << 16 | (static_cast<std::uint32_t>(off) & 0xFFFFu); }
std::uint32_t lw(std::uint32_t rt, std::int32_t off, std::uint32_t base) { return 0x8C000000u | base << 21 | rt << 16 | (static_cast<std::uint32_t>(off) & 0xFFFFu); }
std::uint32_t addu(std::uint32_t rd, std::uint32_t rs, std::uint32_t rt) { return rs << 21 | rt << 16 | rd << 11 | 0x21u; }
// bal from module offset `at` to module offset `to` (bgezal zero)
std::uint32_t bal(std::uint32_t at, std::uint32_t to) { return 0x04110000u | (static_cast<std::uint32_t>((static_cast<std::int32_t>(to) - static_cast<std::int32_t>(at) - 4) / 4) & 0xFFFFu); }
constexpr std::uint32_t jr_ra = 0x03E00008u, nop = 0;

// Module layout (offsets from its base): imports at +0x00 (AddDrv +0x14, DelDrv +0x1C), init +0x2C, deinit +0x50, start +0x64,
// the device at +0x120: name pointer +0, ops pointer +16, init calls +0x20, deinit calls +0x24, init result +0x28, deinit result +0x2C,
// results of the start function's five calls +0x30 +0x34 +0x38 +0x3C +0x48, the name at +0x40, the ops table (init, deinit) at +0x60.
constexpr std::uint32_t kDevice = 0x120, kSize = 0x190;

std::vector<std::uint8_t> module() {
    std::vector<std::uint8_t> elf(0x100 + kSize);
    const auto put = [&](std::size_t at, auto value) { std::memcpy(elf.data() + at, &value, sizeof value); };
    const auto code = [&](std::uint32_t at, std::initializer_list<std::uint32_t> words) {
        for (std::uint32_t w : words) { put(0x100 + at, w); at += 4; }
    };
    std::memcpy(elf.data(), "\177ELF\1\1\1", 7);
    put(16, std::uint16_t{0xFF80}); put(18, std::uint16_t{8}); put(20, 1u);
    put(28, 52u); put(40, std::uint16_t{52}); put(42, std::uint16_t{32}); put(44, std::uint16_t{2});
    put(52, 0x70000080u); put(56, 0x80u);
    put(84, 1u); put(88, 0x100u); put(100, kSize); put(104, kSize);
    put(0x84, 0x64u); put(0x8C, kSize);                                      // entry, text size
    code(0x00, {0x41E00000u, 0, 0x104u});
    std::memcpy(elf.data() + 0x10C, "ioman", 5);
    code(0x14, {jr_ra, 0x24000014u, jr_ra, 0x24000015u, 0, 0});              // AddDrv (#20), DelDrv (#21)
    code(0x2C, {sw(zero, 0, sp), sw(zero, 4, sp), sw(zero, 8, sp), sw(zero, 12, sp),   // init(device)
                lw(t0, 0x20, a0), addiu(t0, t0, 1), sw(t0, 0x20, a0), jr_ra, lw(v0, 0x28, a0)});
    code(0x50, {lw(t0, 0x24, a0), addiu(t0, t0, 1), sw(t0, 0x24, a0), jr_ra, lw(v0, 0x2C, a0)});   // deinit(device)
    code(0x64, {addiu(sp, sp, -32), sw(ra, 28, sp), sw(s0, 24, sp),
                bal(0x70, 0x78), nop,                                         // ra = base + 0x78
                addiu(s0, ra, kDevice - 0x78),                                // s0 = device
                addiu(t0, s0, 0x40), sw(t0, 0, s0),                           // device->name
                addiu(t0, s0, 0x60), sw(t0, 16, s0),                          // device->ops
                addiu(t1, ra, 0x2C - 0x78), sw(t1, 0, t0),                    // ops->init
                addiu(t1, ra, 0x50 - 0x78), sw(t1, 4, t0),                    // ops->deinit
                addu(a0, s0, zero), bal(0xA0, 0x14), nop, sw(v0, 0x30, s0),   // AddDrv(device): init returns 0
                addiu(a0, s0, 0x40), bal(0xB0, 0x1C), nop, sw(v0, 0x34, s0),  // DelDrv("drvtest")
                addiu(a0, s0, 0x40), bal(0xC0, 0x1C), nop, sw(v0, 0x38, s0),  // DelDrv("drvtest") again
                addiu(t0, zero, -1), sw(t0, 0x28, s0),                        // init will return -1
                addu(a0, s0, zero), bal(0xD8, 0x14), nop, sw(v0, 0x3C, s0),   // AddDrv(device)
                addiu(a0, s0, 0x40), bal(0xE8, 0x1C), nop, sw(v0, 0x48, s0),  // DelDrv("drvtest")
                lw(ra, 28, sp), lw(s0, 24, sp), addu(v0, zero, zero), jr_ra, addiu(sp, sp, 32)});
    std::memcpy(elf.data() + 0x100 + kDevice + 0x40, "drvtest", 7);
    for (std::uint32_t off : {0x30u, 0x34u, 0x38u, 0x3Cu, 0x48u}) put(0x100 + kDevice + off, 0x12345678u);   // not written yet
    return elf;
}
}

int main() {
    iop::reset();
    const int id = iop::load_module_buffer(module(), "drvtest.irx", "");
    check(id > 0, "synthetic driver module loads");
    if (id <= 0) return 1;
    const std::uint32_t device = kernel::modules().at(id).base + kDevice;
    iop::run(65536);
    const auto result = [&](std::uint32_t off) { return static_cast<std::int32_t>(iop::ram32(device + off)); };
    check(result(0x30) == 0, "AddDrv succeeds when the driver's init returns 0");
    check(result(0x34) == 0, "DelDrv of that driver succeeds");
    check(result(0x38) == -1, "a second DelDrv finds no such driver");
    check(result(0x3C) == -1, "AddDrv fails when init returns -1");
    check(result(0x48) == -1, "and that driver is not in the table afterwards");
    check(result(0x20) == 2, "init ran once for each AddDrv");
    check(result(0x24) == 1, "deinit ran once, for the DelDrv that found the driver");
    std::printf("ioman drivers: %d failed (init %d, deinit %d, results %d %d %d %d %d)\n", failed, result(0x20), result(0x24), result(0x30),
                result(0x34), result(0x38), result(0x3C), result(0x48));
    return failed ? 1 : 0;
}
