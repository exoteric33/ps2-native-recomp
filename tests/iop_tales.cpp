// No game data: the two IOP calls the Tales of Destiny 2 and Tales of Destiny: Director's Cut log (public issue 59) names as missing,
//   modload #9  LoadModuleBufferAddress(buffer, addr, offset)  and  cdvdman #44  sceCdGetReadPos().
// Rules from ps2sdk (AFL-2.0): iop/system/modload/src/{exports.tab,modload.c} and iop/cdvd/cdvdman/src/{exports.tab,cdvdman.c}.
// #9 links the module image that lies in IOP memory and gives back its id; it does not start it. #44 is the number of bytes of the
// running sceCdRead that have arrived, 0 when no read runs. Proves what the calls do, not that either game starts.
// Build: ninja ps2talestest, run build/.../ps2talestest (prints "tales iop: 0 failed").
#include "iop/iop.hpp"
#include "iop/kernel.hpp"
#include "ps2/disc.hpp"
#include "ps2/memory.hpp"

#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

namespace {
namespace iop = ps2::iop;
namespace kernel = ps2::iop::kernel;
constexpr std::uint32_t kIop = iop::kRamSize;

int failed = 0, checks = 0;
void check(bool ok, const char *message) {
    ++checks;
    if (!ok) { ++failed; std::fprintf(stderr, "FAIL: %s\n", message); }
}

struct Library { const char *name; std::vector<unsigned> functions; };

// A small IOP module. Its start routine stores `marker_value` at IOP address `marker_at` and returns (so a module that was linked but not
// started leaves the marker alone). `libraries` are linked to the HLE libraries, the way a game's own modules import them.
std::vector<std::uint8_t> module(const std::vector<Library> &libraries, std::uint32_t marker_at, std::uint32_t marker_value) {
    constexpr std::uint32_t kImage = 0x700;
    std::vector<std::uint8_t> elf(0x100 + kImage);
    const auto put = [&](std::size_t at, auto value) { std::memcpy(elf.data() + at, &value, sizeof value); };
    std::memcpy(elf.data(), "\177ELF\1\1\1", 7);
    put(16, std::uint16_t{0xFF80}); put(18, std::uint16_t{8}); put(20, 1u);
    put(28, 52u); put(40, std::uint16_t{52}); put(42, std::uint16_t{32}); put(44, std::uint16_t{2});
    put(52, 0x70000080u); put(56, 0x80u);                                  // the iopmod record at file offset 0x80
    put(84, 1u); put(88, 0x100u); put(100, kImage); put(104, kImage);      // the loaded image: file 0x100
    put(0x84, 0x6E0u);                                                     // entry of the module start
    for (std::size_t t = 0; t < libraries.size(); ++t) {
        const std::size_t at = 0x100 + t * 0x100;
        put(at, 0x41E00000u); put(at + 8, 0x101u);
        std::memcpy(elf.data() + at + 12, libraries[t].name, std::strlen(libraries[t].name));
        for (std::size_t i = 0; i < libraries[t].functions.size(); ++i) {
            put(at + 0x14 + i * 8, 0x03E00008u);
            put(at + 0x18 + i * 8, 0x24000000u | libraries[t].functions[i]);
        }
    }
    const std::size_t start = 0x100 + 0x6E0;
    put(start, 0x34080000u | marker_at);                       // ori t0, zero, marker_at
    put(start + 4, 0x3C090000u | (marker_value >> 16));        // lui t1, hi
    put(start + 8, 0x35290000u | (marker_value & 0xFFFFu));    // ori t1, t1, lo
    put(start + 12, 0x03E00008u);                              // jr ra
    put(start + 16, 0xAD090000u);                              // sw t1, 0(t0)   (in the delay slot)
    return elf;
}

std::map<std::string, std::map<unsigned, std::uint32_t>> g_hle;

std::uint32_t call(const char *library, unsigned index, std::vector<std::uint32_t> args) {
    iop::Cpu c{};
    c.r[29] = 0x1F0000;
    for (std::size_t i = 0; i < args.size() && i < 4; ++i) c.r[4 + i] = args[i];
    iop::call_hle(c, g_hle.at(library).at(index));
    return c.r[2];
}

std::int32_t sign(std::uint32_t v) { return static_cast<std::int32_t>(v); }

void put_module(std::uint32_t at, const std::vector<std::uint8_t> &elf) { std::memcpy(iop::g_ram + at, elf.data(), elf.size()); }

}  // namespace

int main() {
    iop::reset();
    const std::vector<Library> libs{{"modload", {9, 10, 22}}, {"cdvdman", {6, 28, 44}}};
    const int caller = iop::load_module_buffer(module(libs, 0x7F0, 0xCAFE0001u), "tales-test.irx", "");
    check(caller > 0, "synthetic IOP module loads");
    if (caller <= 0) return 1;
    const std::uint32_t base = kernel::modules().at(caller).base;
    for (std::size_t t = 0; t < libs.size(); ++t)
        for (std::size_t i = 0; i < libs[t].functions.size(); ++i) {
            const std::uint32_t op = iop::ram32(base + static_cast<std::uint32_t>(t * 0x100 + 0x14 + i * 8));
            check(op >> 26 == iop::kHleOpcode, "import links to an HLE function");
            g_hle[libs[t].name][libs[t].functions[i]] = op & 0x03FFFFFFu;
        }
    iop::run(8192);
    check(iop::ram32(0x7F0) == 0xCAFE0001u, "the test module's own start routine ran (the marker works)");

    // ---- modload #9 LoadModuleBufferAddress ----
    constexpr std::uint32_t kBuffer = 0x180000, kMarker = 0x7F4;
    put_module(kBuffer, module({}, kMarker, 0x600D0001u));
    const std::size_t before = kernel::modules().size();
    const std::int32_t id = sign(call("modload", 9, {kBuffer, 0, 0}));
    check(id > 0 && kernel::modules().count(id) == 1, "#9 links the module in the buffer and returns its id");
    check(kernel::modules().size() == before + 1, "#9 adds exactly one module");
    iop::run(2'000'000);
    check(iop::ram32(kMarker) == 0, "#9 does not start the module");
    check(iop::start_module(id, "", {}), "StartModule can run the module #9 linked");
    iop::run(2'000'000);
    check(iop::ram32(kMarker) == 0x600D0001u, "the linked module starts and runs its code");

    // #10 is #9 with no destination: same result kind, a module of its own each time
    const std::int32_t id10 = sign(call("modload", 10, {kBuffer}));
    check(id10 > 0 && id10 != id, "#10 links the same buffer as a new module");
    // a destination block and offset are accepted (and not honoured, see libs.cpp)
    const std::int32_t with_addr = sign(call("modload", 9, {kBuffer, 0x1A0000, 0x20}));
    check(with_addr > 0 && with_addr != id && with_addr != id10, "#9 with a destination still links the module");
    // refused buffers give an error code, not a module id
    std::memset(iop::g_ram + 0x1C0000, 0x11, 0x100);
    check(sign(call("modload", 9, {0x1C0000, 0, 0})) < 0, "#9 refuses a buffer that is not an IOP module");
    check(sign(call("modload", 9, {0x1C0000 + 0x20000000u, 0, 0})) < 0, "#9 refuses a buffer that is not an IOP module (uncached alias)");
    check(sign(call("modload", 9, {kIop + 0x100, 0, 0})) < 0, "#9 refuses an address past the end of memory");
    check(kernel::modules().size() == before + 3, "refused buffers add no module");

    // ---- cdvdman #44 sceCdGetReadPos ----
    constexpr std::uint32_t kSectors = 64;
    std::vector<std::uint8_t> disc_image(kSectors * ps2::disc::kSector, 0x5E);
    ps2::disc::open("tales-test.iso", disc_image.size(), [&](std::uint64_t offset, void *out, std::size_t bytes) -> std::size_t {
        const std::size_t n = offset < disc_image.size() ? std::min<std::size_t>(bytes, disc_image.size() - offset) : 0;
        std::memcpy(out, disc_image.data() + offset, n);
        return n;
    });
    check(call("cdvdman", 44, {}) == 0, "#44 is 0 when no read runs");
    check(call("cdvdman", 6, {0, kSectors, 0x10000, 0}) == 1, "sceCdRead of 64 sectors starts");
    check(call("cdvdman", 44, {}) == 0, "#44 is 0 at the very start of a read");
    std::uint32_t first = 0, second = 0;
    for (int i = 0; i < 400 && !first && call("cdvdman", 28, {}) != 0x0Au; ++i) { iop::run(100'000); first = call("cdvdman", 44, {}); }
    check(first > 0 && first < kSectors * ps2::disc::kSector && first % ps2::disc::kSector == 0, "#44 counts whole sectors while the read runs");
    if (call("cdvdman", 28, {}) != 0x0Au) iop::run(500'000);
    second = call("cdvdman", 44, {});
    check(call("cdvdman", 28, {}) == 0x0Au || second > first, "#44 grows while the read goes on");
    check(second < kSectors * ps2::disc::kSector, "#44 stays below the whole read while it runs");
    for (int i = 0; i < 200 && call("cdvdman", 28, {}) != 0x0Au; ++i) iop::run(1'000'000);
    check(call("cdvdman", 28, {}) == 0x0Au, "the read finishes");
    check(call("cdvdman", 44, {}) == 0, "#44 is 0 again when the read is over");
    check(iop::g_ram[0x10000] == 0x5E, "the data arrived");

    std::printf("tales iop: %d of %d checks failed\n", failed, checks);
    std::printf("tales iop: %d failed\n", failed);
    return failed ? 1 : 0;
}
