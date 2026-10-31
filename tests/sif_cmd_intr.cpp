// No game data: exercise the real SIFCMD imports through a synthetic IOP module.
#include "iop/kernel.hpp"
#include "ps2/memory.hpp"

#include <cstdio>
#include <cstring>
#include <vector>

namespace {
int failed = 0;
void check(bool ok, const char *message) {
    if (!ok) { ++failed; std::fprintf(stderr, "FAIL: %s\n", message); }
}
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
}

int main() {
    namespace iop = ps2::iop;
    namespace kernel = ps2::iop::kernel;
    iop::reset();
    const int id = iop::load_module_buffer(module(), "sifcmd-test.irx", "");
    check(id > 0, "synthetic IOP module loads");
    if (id <= 0) return 1;
    const auto base = kernel::modules().at(id).base;
    iop::run(8192); // finish the synthetic module's start thread
    constexpr std::uint32_t packet = 0x18000, payload = 0x18100, stack = 0x18200, dest = 0x100000;
    int calls = 0;
    const auto callback = kernel::hle_stub([&](iop::Cpu &c) {
        ++calls;
        check(c.r[4] == 0x12345678u, "callback receives its stack-passed user argument");
        check(kernel::in_interrupt(), "completion runs in interrupt context");
        check(ps2::ld32(dest) == 0xAABBCCDDu, "payload is copied before completion");
    });
    for (unsigned which = 0; which < 4; ++which) {
        const auto opcode = iop::ram32(base + 20 + which * 8);
        check(opcode >> 26 == iop::kHleOpcode, "SIFCMD import links to its HLE handler");
        for (bool with_callback : {false, true}) {
            iop::Cpu c{};
            c.r[4] = 0x1234; c.r[5] = packet; c.r[6] = 16; c.r[7] = payload; c.r[29] = stack;
            iop::set_ram32(stack + 16, dest); iop::set_ram32(stack + 20, 4);
            iop::set_ram32(stack + 24, with_callback ? callback : 0);
            iop::set_ram32(stack + 28, 0x12345678u);
            iop::set_ram32(payload, 0xAABBCCDDu); ps2::st32(dest, 0);
            const int before = calls;
            iop::call_hle(c, opcode & 0x03FFFFFFu);
            check(c.r[2] != 0, "send returns a successful DMA identifier");
            check(ps2::ld32(dest) == 0xAABBCCDDu, "send copies the extra payload");
            check(calls == before, "completion is deferred until after send returns");
            iop::run(8192);
            const int expected = before + (which >= 2 && with_callback ? 1 : 0);
            check(calls == expected, "only Intr variants invoke a non-null callback");
            iop::run(8192);
            check(calls == expected, "completion runs exactly once");
        }
    }
    std::printf("SIFCMD completion: %d failed\n", failed);
    return failed ? 1 : 0;
}
