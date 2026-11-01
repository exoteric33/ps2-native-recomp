// No game data: pointers and sizes a game (or a damaged module) passes to the IOP's libraries and to the SIF must never write or
// read past the IOP's 2 MiB or the EE's 32 MiB, nor ask the host for gigabytes. Each bad call is cut or refused (with a log line);
// each matching good call must still do exactly what it did before. Covers sysclib (memcpy, memset, bcmp, bzero, strcpy, strcat),
// ioman read, modload ReferModuleStatus, sifman sceSifSetDma, sifcmd sceSifSendCmd / sceSifGetOtherData, cdvdman sceCdRead, the
// EE's sceSifSetDma, the loadfile server (a module handed over from memory whose headers lie past the end), the fileio read and
// the cdvdfsv read to EE memory. The memories' padding bytes are filled with a pattern that must survive.
// Build: ninja ps2iopboundstest, run build/.../ps2iopboundstest (prints "iop bounds: 0 failed").
#include "iop/iop.hpp"
#include "iop/kernel.hpp"
#include "iop/sif.hpp"
#include "ps2/disc.hpp"
#include "ps2/memory.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

namespace {
namespace iop = ps2::iop;
namespace kernel = ps2::iop::kernel;
constexpr std::uint32_t kIop = iop::kRamSize, kEe = ps2::kRamSize;

int failed = 0, checks = 0;
void check(bool ok, const char *message) {
    ++checks;
    if (!ok) { ++failed; std::fprintf(stderr, "FAIL: %s\n", message); }
}

// ---- a synthetic IOP module whose import tables link to the HLE libraries, as a game's own modules do ----

struct Library { const char *name; std::vector<unsigned> functions; };
const std::vector<Library> kLibraries{
    {"sysclib", {12, 14, 15, 17, 20, 23}},
    {"ioman", {6}},
    {"modload", {17}},
    {"sifman", {7}},
    {"sifcmd", {12, 23}},
    {"cdvdman", {6, 28}},
};

std::vector<std::uint8_t> module() {
    constexpr std::uint32_t kImage = 0x700;
    std::vector<std::uint8_t> elf(0x100 + kImage);
    const auto put = [&](std::size_t at, auto value) { std::memcpy(elf.data() + at, &value, sizeof value); };
    std::memcpy(elf.data(), "\177ELF\1\1\1", 7);
    put(16, std::uint16_t{0xFF80}); put(18, std::uint16_t{8}); put(20, 1u);
    put(28, 52u); put(40, std::uint16_t{52}); put(42, std::uint16_t{32}); put(44, std::uint16_t{2});
    put(52, 0x70000080u); put(56, 0x80u);                                  // the iopmod record at file offset 0x80
    put(84, 1u); put(88, 0x100u); put(100, kImage); put(104, kImage);      // the loaded image: file 0x100
    put(0x84, 0x6E0u);                                                     // entry of the module start
    for (std::size_t t = 0; t < kLibraries.size(); ++t) {
        const std::size_t at = 0x100 + t * 0x100;
        put(at, 0x41E00000u); put(at + 8, 0x101u);
        std::memcpy(elf.data() + at + 12, kLibraries[t].name, std::strlen(kLibraries[t].name));
        for (std::size_t i = 0; i < kLibraries[t].functions.size(); ++i) {
            put(at + 0x14 + i * 8, 0x03E00008u);
            put(at + 0x18 + i * 8, 0x24000000u | kLibraries[t].functions[i]);
        }
    }
    put(0x100 + 0x6E0, 0x03E00008u);  // the module start: jr ra; nop
    return elf;
}

std::map<std::string, std::map<unsigned, std::uint32_t>> g_hle;

std::uint32_t call(const char *library, unsigned index, std::vector<std::uint32_t> args) {
    constexpr std::uint32_t kStack = 0x1F0000;
    iop::Cpu c{};
    c.r[29] = kStack;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (i < 4) c.r[4 + i] = args[i];
        else iop::set_ram32(kStack + 16 + 4 * static_cast<std::uint32_t>(i - 4), args[i]);
    }
    iop::call_hle(c, g_hle.at(library).at(index));
    return c.r[2];
}

// The padding after each memory gets a pattern; a write past the end shows as a changed byte there.
void mark_padding() {
    std::memset(iop::g_ram + kIop, 0xA5, 16);
    std::memset(ps2::g_ram + kEe, 0x5A, 64);
}
bool padding_intact() {
    for (std::uint32_t i = 0; i < 16; ++i) if (iop::g_ram[kIop + i] != 0xA5) return false;
    for (std::uint32_t i = 0; i < 64; ++i) if (ps2::g_ram[kEe + i] != 0x5A) return false;
    return true;
}

double seconds_since(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count();
}

void ee_put32(std::uint32_t a, std::uint32_t v) { std::memcpy(ps2::g_ram + a, &v, 4); }
std::uint32_t ee_get32(std::uint32_t a) { std::uint32_t v; std::memcpy(&v, ps2::g_ram + a, 4); return v; }

// A SIF call from the EE to an HLE server, the way the EE kernel sends it: the arguments' address (sceSifSetDma to the RPC receive
// address), then the call packet to the command buffer. The answer lands at `recv`.
void ee_call(std::uint32_t server, std::uint32_t method, std::uint32_t args, std::uint32_t send_size, std::uint32_t recv, std::uint32_t recv_size) {
    constexpr std::uint32_t kList = 0x1E00000, kPacket = 0x1E00100;
    std::uint32_t packet[14] = {56, 0, 0x8000000Au, 0, 0, 0, 0, 0, method, send_size, recv, recv_size, 0, server ^ 0xACACACACu};
    std::memcpy(ps2::g_ram + kPacket, packet, sizeof packet);
    const std::uint32_t list[8] = {args, 0xDEADBEF0u, send_size, 0, kPacket, 0x0001E000u, sizeof packet, 0};
    std::memcpy(ps2::g_ram + kList, list, sizeof list);
    ps2::sif::set_dma(kList, 2);
}

}  // namespace

int main() {
    iop::reset();
    const int module_id = iop::load_module_buffer(module(), "bounds-test.irx", "");
    check(module_id > 0, "synthetic IOP module loads");
    if (module_id <= 0) return 1;
    const std::uint32_t base = kernel::modules().at(module_id).base;
    for (std::size_t t = 0; t < kLibraries.size(); ++t)
        for (std::size_t i = 0; i < kLibraries[t].functions.size(); ++i) {
            const std::uint32_t op = iop::ram32(base + static_cast<std::uint32_t>(t * 0x100 + 0x14 + i * 8));
            check(op >> 26 == iop::kHleOpcode, "import links to an HLE function");
            g_hle[kLibraries[t].name][kLibraries[t].functions[i]] = op & 0x03FFFFFFu;
        }
    iop::run(8192);  // finish the module's start thread
    mark_padding();

    // ---- sysclib: good calls do what they did ----
    for (std::uint32_t i = 0; i < 256; ++i) iop::g_ram[0x4000 + i] = static_cast<std::uint8_t>(i);
    call("sysclib", 12, {0x3000, 0x4000, 256});
    check(std::memcmp(iop::g_ram + 0x3000, iop::g_ram + 0x4000, 256) == 0, "memcpy copies");
    check(call("sysclib", 15, {0x3000, 0x4000, 256}) == 0, "bcmp of equal blocks is 0");
    call("sysclib", 14, {0x3000, 0x77, 16});
    check(iop::g_ram[0x3000] == 0x77 && iop::g_ram[0x300F] == 0x77 && iop::g_ram[0x3010] == 0x10, "memset fills exactly");
    std::memcpy(iop::g_ram + 0x5000, "hello", 6);
    call("sysclib", 23, {0x5100, 0x5000});
    check(std::strcmp(reinterpret_cast<char *>(iop::g_ram + 0x5100), "hello") == 0, "strcpy copies");
    call("sysclib", 20, {0x5100, 0x5000});
    check(std::strcmp(reinterpret_cast<char *>(iop::g_ram + 0x5100), "hellohello") == 0, "strcat appends");
    call("sysclib", 17, {0x5100, 4});
    check(iop::g_ram[0x5103] == 0 && iop::g_ram[0x5104] == 'o', "bzero clears exactly");

    // ---- sysclib: bad calls near the end of IOP memory are cut or written round to the start ----
    std::memcpy(iop::g_ram + 0x5000, "hello world!", 13);
    call("sysclib", 23, {kIop - 4, 0x5000});
    check(padding_intact(), "strcpy at the end of memory stays inside it");
    check(std::memcmp(iop::g_ram + kIop - 4, "hell", 4) == 0 && std::strcmp(reinterpret_cast<char *>(iop::g_ram), "o world!") == 0,
          "strcpy at the end of memory goes on at the start (the memory is mirrored)");
    std::memcpy(iop::g_ram + kIop - 6, "ab", 3);
    call("sysclib", 20, {kIop - 6, 0x5000});
    check(padding_intact(), "strcat at the end of memory stays inside it");
    call("sysclib", 17, {kIop - 8, 64});
    check(padding_intact(), "bzero past the end stays inside memory");
    check(iop::g_ram[kIop - 1] == 0 && iop::g_ram[0] == 0 && iop::g_ram[55] == 0, "bzero past the end goes on at the start");
    call("sysclib", 15, {kIop - 4, 0x100, 0x1000});
    check(padding_intact(), "bcmp past the end does not crash");

    // ---- ioman, modload ----
    auto t0 = std::chrono::steady_clock::now();
    check(static_cast<std::int32_t>(call("ioman", 6, {999, 0x6000, 0xFFFFFFFFu})) < 0, "ioman read of a bad fd fails");
    check(seconds_since(t0) < 1.0, "ioman read of 4 GiB does not allocate 4 GiB first");
    check(call("modload", 17, {static_cast<std::uint32_t>(module_id), kIop - 8}) == 0, "ReferModuleStatus answers");
    check(padding_intact(), "ReferModuleStatus at the end of memory stays inside it");

    // ---- sifman sceSifSetDma (IOP to EE) ----
    std::memset(ps2::g_ram + 0x100000, 0x77, 0x2000);
    for (std::uint32_t i = 0; i < 16; ++i) iop::g_ram[kIop - 16 + i] = 0x11;
    iop::set_ram32(0x7000, kIop - 16); iop::set_ram32(0x7004, 0x100000); iop::set_ram32(0x7008, 0x100000);
    call("sifman", 7, {0x7000, 1});
    check(ps2::g_ram[0x10000F] == 0x11 && ps2::g_ram[0x100010] == 0x77, "IOP to EE DMA stops at the end of IOP memory");
    iop::set_ram32(0x7000, 0x8000); iop::set_ram32(0x7004, kEe - 32); iop::set_ram32(0x7008, 0x1000);
    call("sifman", 7, {0x7000, 1});
    check(padding_intact(), "IOP to EE DMA stops at the end of EE memory");
    iop::set_ram32(0x7000, 0x8000); iop::set_ram32(0x7004, 0x100000); iop::set_ram32(0x7008, 64);
    std::memset(iop::g_ram + 0x8000, 0x44, 64);
    call("sifman", 7, {0x7000, 1});
    check(ps2::g_ram[0x100000] == 0x44 && ps2::g_ram[0x10003F] == 0x44 && ps2::g_ram[0x100040] == 0x77, "a good IOP to EE DMA copies exactly");

    // ---- sifcmd ----
    check(call("sifcmd", 12, {0x1234, 0x9000, 8, 0, 0, 0}) == 0, "a packet shorter than its header is refused");
    call("sifcmd", 12, {0x1234, 0x9000, 16, 0x8000, kEe - 8, 0x1000});
    check(padding_intact(), "the extra data of a command stops at the end of EE memory");
    std::memset(iop::g_ram + 0xA000, 0x77, 0x2000);
    call("sifcmd", 23, {0, kEe - 8, 0xA000, 0x1000});
    check(iop::g_ram[0xA007] != 0x77 && iop::g_ram[0xA008] == 0x77 && iop::g_ram[0xB000] == 0x77, "sceSifGetOtherData stops at the end of EE memory");


    // ---- the EE side: sceSifSetDma (EE to IOP) ----
    std::memset(ps2::g_ram + kEe - 16, 0x22, 16);
    std::memset(iop::g_ram + 0xC000, 0x77, 0x100);
    const std::uint32_t list[4] = {kEe - 16, 0xC000, 0x80, 0};
    std::memcpy(ps2::g_ram + 0x1D00000, list, sizeof list);
    ps2::sif::set_dma(0x1D00000, 1);
    check(iop::g_ram[0xC00F] == 0x22 && iop::g_ram[0xC010] == 0x77, "EE to IOP DMA stops at the end of EE memory");

    // ---- the loadfile server: a module handed over from IOP memory ----
    constexpr std::uint32_t kArgs = 0x1C00000, kRecv = 0x1C10000;
    // headers that lie past the end of IOP memory (the program header table at 4 GiB - 16): refused, nothing read past it
    std::uint8_t *bad = iop::g_ram + kIop - 0x40;
    std::memset(bad, 0, 0x40);
    std::memcpy(bad, "\177ELF\1\1\1", 7);
    std::uint32_t far = 0xFFFFFFF0u; std::uint16_t many = 0xFFFF;
    std::memcpy(bad + 0x1C, &far, 4); std::memcpy(bad + 0x2C, &many, 2); std::memcpy(bad + 0x2A, &many, 2);
    std::memcpy(bad + 0x20, &far, 4); std::memcpy(bad + 0x30, &many, 2); std::memcpy(bad + 0x2E, &many, 2);
    ee_put32(kArgs, kIop - 0x40); ee_put32(kArgs + 4, 0);
    ee_put32(kRecv, 0x12345678u);
    ee_call(0x80000006u, 6, kArgs, 0x200, kRecv, 8);
    check(static_cast<std::int32_t>(ee_get32(kRecv)) < 0, "a module whose headers lie past the end of IOP memory is refused");
    check(padding_intact(), "the refused module wrote nothing past memory");
    // a good module still loads the same way
    const std::vector<std::uint8_t> good = module();
    std::memcpy(iop::g_ram + 0x180000, good.data(), good.size());
    ee_put32(kArgs, 0x180000); ee_put32(kRecv, 0x12345678u);
    ee_call(0x80000006u, 6, kArgs, 0x200, kRecv, 8);
    for (int i = 0; i < 50 && ee_get32(kRecv) == 0x12345678u; ++i) iop::run(1'000'000);
    check(static_cast<std::int32_t>(ee_get32(kRecv)) > 0, "a good module handed over from memory still loads");

    // ---- the fileio read and the cdvdfsv read to EE memory, with sizes of 4 GiB / 4 G sectors (8 TB asked of the host) ----
    std::vector<std::uint8_t> disc_image(64 * ps2::disc::kSector, 0x5E);
    ps2::disc::open("bounds-test.iso", disc_image.size(), [&](std::uint64_t offset, void *out, std::size_t bytes) -> std::size_t {
        const std::size_t n = offset < disc_image.size() ? std::min<std::size_t>(bytes, disc_image.size() - offset) : 0;
        std::memcpy(out, disc_image.data() + offset, n);
        return n;
    });
    ee_put32(kArgs, 999); ee_put32(kArgs + 4, 0x1000000); ee_put32(kArgs + 8, 0xFFFFFFFFu);
    t0 = std::chrono::steady_clock::now();
    ee_call(0x80000001u, 2, kArgs, 0x10, kRecv, 4);
    check(seconds_since(t0) < 1.0 && padding_intact(), "a fileio read of 4 GiB is cut to EE memory");
    ee_put32(kArgs, 0); ee_put32(kArgs + 4, 0xFFFFFFFFu); ee_put32(kArgs + 8, 0x1F00000);
    ee_call(0x80000595u, 1, kArgs, 0x10, kRecv, 4);
    for (int i = 0; i < 50; ++i) iop::run(4'000'000);
    check(padding_intact() && ps2::g_ram[0x1F00000] == 0x5E, "a cdvdfsv read of 4 G sectors is cut at the end of EE memory");

    // ---- the last ones write over the whole IOP memory (the module and the stacks), so nothing runs on the IOP after them ----
    check(call("cdvdman", 6, {0, 0xFFFFFFFFu, 0x10000, 0}) == 1, "sceCdRead of 4 G sectors starts");
    for (int i = 0; i < 200 && call("cdvdman", 28, {}) != 0x0Au; ++i) iop::run(4'000'000);
    check(call("cdvdman", 28, {}) == 0x0Au, "sceCdRead of 4 G sectors is cut to 2 MiB and finishes");
    check(padding_intact() && iop::g_ram[0x10000] == 0x5E, "sceCdRead of 4 G sectors stays inside IOP memory");
    t0 = std::chrono::steady_clock::now();
    call("sysclib", 14, {0x1000, 0x33, 0xFFFFFF00u});  // the 32-bit end check wrapped: a 4 GiB memset
    check(padding_intact() && seconds_since(t0) < 2.0, "memset of almost 4 GiB is cut to the memory");
    check(iop::g_ram[0] == 0x33 && iop::g_ram[kIop - 1] == 0x33, "memset of almost 4 GiB fills the whole memory once");
    t0 = std::chrono::steady_clock::now();
    call("sysclib", 12, {0x1000, 0x2000, 0xFFFFFF00u});
    check(padding_intact() && seconds_since(t0) < 2.0, "memcpy of almost 4 GiB is cut to the memory");

    std::printf("iop bounds: %d of %d checks failed\n", failed, checks);
    std::printf("iop bounds: %d failed\n", failed);
    return failed ? 1 : 0;
}
