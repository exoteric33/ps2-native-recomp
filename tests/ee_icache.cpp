// Synthetic code images: an unchanged entry followed by a rewritten delay slot.
#include "ps2/generated.hpp"
#include "ps2/system.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
void before(ps2::Cpu &c, std::uint32_t, std::uint32_t) { ps2::set32(c.r[2], 1); c.pc = c.r[31].w[0]; }
void after(ps2::Cpu &c, std::uint32_t, std::uint32_t) { ps2::set32(c.r[2], 2); c.pc = c.r[31].w[0]; }
}

int main(int argc, char **argv) {
    const unsigned mode = argc > 1 ? static_cast<unsigned>(std::strtoul(argv[1], nullptr, 0)) : 2u;
    const auto number = argc > 2 ? static_cast<std::int32_t>(std::strtol(argv[2], nullptr, 0)) : 0x64;
    if (mode > 3 || (number != 0x64 && number != 0x68 && number != -0x68)) return 2;
    constexpr std::uint32_t base = 0x01000000;
    const std::uint32_t first[] = {0x03E00008, 0x24020001}; // jr ra; addiu v0, zero, 1
    const std::uint32_t second[] = {0x03E00008, 0x24020002};
    const std::uint32_t checks1[] = {0, first[0], 4, first[1]};
    const std::uint32_t checks2[] = {0, second[0], 4, second[1]};
    const std::uint16_t entries[] = {1, 0};
    const ps2::UnitInfo units[] = {{0, 8, before, entries}, {0, 8, after, entries}};
    const ps2::ModuleInfo modules[] = {
        {"first", base, 8, checks1, 2, &units[0], 1},
        {"second", base, 8, checks2, 2, &units[1], 1},
    };
    const ps2::UnitInfo empty{};
    ps2::register_units(&empty, 0);
    ps2::register_modules(modules, 2);
    std::memcpy(ps2::g_ram + base, first, sizeof first);
    ps2::Cpu c{}; c.pc = base; c.r[31].w[0] = 0x00100000;
    ps2::dispatch(c, 0, 0);
    if (c.r[2].w[0] != 1) return 3;
    std::memcpy(ps2::g_ram + base, second, sizeof second);
    c.r[3].w[0] = number; c.r[4].w[0] = mode;
    ps2::syscall(c, 0x00100000);
    c.pc = base;
    ps2::dispatch(c, 0, 0);
    const unsigned expected = mode == 2 || mode == 3 ? 2u : 1u;
    if (c.r[2].w[0] != expected) {
        std::printf("instruction cache: syscall %02X mode %u expected %u, got %u\n", static_cast<unsigned>(number), mode, expected, c.r[2].w[0]);
        return 1;
    }
    // An image with no matching compiled code must not inherit the old instance.
    ps2::g_ram[base + 4] ^= 4;
    c.r[3].w[0] = number; c.r[4].w[0] = 2;
    ps2::syscall(c, 0x00100000);
    if (ps2::has_code(base)) return 4;
    std::memcpy(ps2::g_ram + base, second, sizeof second);
    c.r[3].w[0] = number; c.r[4].w[0] = 2;
    ps2::syscall(c, 0x00100000);
    c.pc = base;
    ps2::dispatch(c, 0, 0);
    if (c.r[2].w[0] != 2) return 5; // the compiled module was retained, only the match was discarded
    std::printf("instruction cache: syscall %02X mode %u PASS, image reselected and unmatched code rejected\n", static_cast<unsigned>(number), mode);
    return 0;
}
