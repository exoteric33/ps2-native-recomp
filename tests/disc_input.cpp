// No game data: damaged or crafted input from a disc image must be refused cleanly, never crash, over-read, allocate gigabytes
// or loop for ever. Covers the ISO 9660 directory walk (disc.cpp), reading a whole file from the disc (iop/cdvd.cpp), the IOP
// module loader (iop/kernel.cpp) and the boot ELF loader (system.cpp), each with tiny hand-made images, plus one valid image of
// each kind that must still load as before. Build: ninja ps2discinputtest, run build/.../ps2discinputtest (prints
// "disc input: 0 failed").
#include "iop/iop.hpp"
#include "iop/kernel.hpp"
#include "ps2/disc.hpp"
#include "ps2/memory.hpp"
#include "ps2/system.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace ps2::iop::kernel {
std::vector<std::uint8_t> read_disc_file(const std::string &path);
}

namespace {
namespace disc = ps2::disc;
namespace iop = ps2::iop;

int failed = 0, checks = 0;
void check(bool ok, const char *message) {
    ++checks;
    if (!ok) { ++failed; std::fprintf(stderr, "FAIL: %s\n", message); }
}

template <typename T> void put(std::vector<std::uint8_t> &v, std::size_t at, T value) { std::memcpy(v.data() + at, &value, sizeof value); }

// ---- ISO 9660 (ECMA-119): a primary volume descriptor at sector 16, directory records of 33 bytes plus the name ----

constexpr std::uint32_t kSectors = 64;

std::vector<std::uint8_t> image(std::uint32_t root_lba, std::uint32_t root_size) {
    std::vector<std::uint8_t> img(kSectors * disc::kSector);
    std::uint8_t *pvd = img.data() + 16 * disc::kSector;
    std::memcpy(pvd, "\x01" "CD001\x01", 7);
    std::memcpy(pvd + 80, &kSectors, 4);          // volume space size: no second layer to look for
    std::memcpy(pvd + 156 + 2, &root_lba, 4);     // the root directory record: extent ...
    std::memcpy(pvd + 156 + 10, &root_size, 4);   // ... and data length
    return img;
}

// Writes one directory record at byte `at` and returns the offset after it.
std::size_t record(std::vector<std::uint8_t> &img, std::size_t at, std::uint32_t lba, std::uint32_t size, bool dir, const std::string &name,
                   int length_override = -1) {
    const std::size_t length = (33 + name.size() + 1) & ~std::size_t{1};
    img[at] = static_cast<std::uint8_t>(length_override >= 0 ? length_override : length);
    put(img, at + 2, lba);
    put(img, at + 10, size);
    img[at + 25] = dir ? 2 : 0;
    img[at + 32] = static_cast<std::uint8_t>(name.size());
    std::memcpy(img.data() + at + 33, name.data(), name.size());
    return at + length;
}

std::size_t dots(std::vector<std::uint8_t> &img, std::size_t at, std::uint32_t self, std::uint32_t parent) {
    at = record(img, at, self, disc::kSector, true, std::string(1, '\0'));
    return record(img, at, parent, disc::kSector, true, std::string(1, '\1'));
}

void open_image(const std::vector<std::uint8_t> &img, const char *name) {
    disc::open(name, img.size(), [&img](std::uint64_t offset, void *out, std::size_t bytes) -> std::size_t {
        if (offset >= img.size()) return 0;
        const std::size_t n = std::min<std::size_t>(bytes, img.size() - offset);
        std::memcpy(out, img.data() + offset, n);
        return n;
    });
}

void disc_tests() {
    const std::size_t S = disc::kSector;
    {   // a valid image: a file in the root and one in a sub-directory
        auto img = image(20, S);
        std::size_t at = dots(img, 20 * S, 20, 20);
        at = record(img, at, 30, 100, false, "SYSTEM.CNF;1");
        record(img, at, 21, S, true, "DATA");
        at = dots(img, 21 * S, 21, 20);
        record(img, at, 31, 5000, false, "A.BIN;1");
        std::memcpy(img.data() + 30 * S, "BOOT2 = cdrom0:\\X;1", 19);
        open_image(img, "valid.iso");
        const auto cnf = disc::find("cdrom0:\\SYSTEM.CNF;1");
        check(cnf && cnf->sector == 30 && cnf->size == 100 && !cnf->directory, "valid image: SYSTEM.CNF is found");
        const auto a = disc::find("cdrom0:\\DATA\\A.BIN;1");
        check(a && a->sector == 31 && a->size == 5000, "valid image: a file in a sub-directory is found");
        const auto data = iop::kernel::read_disc_file("cdrom0:\\SYSTEM.CNF;1");
        check(data.size() == 100 && std::memcmp(data.data(), "BOOT2", 5) == 0, "valid image: a whole file reads back");
    }
    {   // directories that point back at the root: 40 of them, which used to be followed 40^16 times
        auto img = image(20, S);
        std::size_t at = dots(img, 20 * S, 20, 20);
        for (int i = 0; i < 40; ++i) {
            char name[8];
            std::snprintf(name, sizeof name, "D%02d", i);
            at = record(img, at, 20, S, true, name);
        }
        const auto started = std::chrono::steady_clock::now();
        open_image(img, "loop.iso");
        const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        check(s < 5.0, "looping directories: indexing ends at once");
        check(disc::find("D00").has_value() && disc::find("D39").has_value(), "looping directories: the entries themselves are indexed");
    }
    {   // the last record of the directory says it is longer than what is left of the directory's sector
        auto img = image(20, S);
        std::size_t at = dots(img, 20 * S, 20, 20);
        at = record(img, at, 30, 10, false, "GOOD.BIN;1");
        while (at < S * 21 - 120) at = record(img, at, 31, 10, false, "F");  // fill the sector with small records (a zero length skips to the next sector)
        record(img, at, 31, 10, false, "BAD", 200);                          // less than 120 bytes before the end, says 200 bytes
        open_image(img, "long-record.iso");
        check(disc::find("GOOD.BIN").has_value(), "record past the end: the records before it are indexed");
        check(!disc::find("BAD").has_value(), "record past the end: it is refused");
    }
    {   // a record whose name length runs past its own length (and past the buffer)
        auto img = image(20, S);
        std::size_t at = dots(img, 20 * S, 20, 20);
        at = record(img, at, 30, 10, false, "GOOD.BIN;1");
        while (at < S * 21 - 68) at = record(img, at, 31, 10, false, "F");
        const std::size_t bad = at;
        record(img, bad, 31, 10, false, "B");
        img[bad] = 34;
        img[bad + 32] = 200;
        open_image(img, "long-name.iso");
        check(disc::find("GOOD.BIN").has_value(), "name past the record: the records before it are indexed");
        check(disc::find("GOOD.BIN") && disc::find("B") == std::nullopt, "name past the record: it is refused");
    }
    {   // a root directory of almost 4 GiB: the sector count used to wrap to 0, then the walk read past an empty buffer
        auto img = image(20, 0xFFFFF900u);
        dots(img, 20 * S, 20, 20);
        open_image(img, "huge-dir.iso");
        check(true, "huge directory: no crash");
    }
    {   // a sub-directory of 1 GiB: refused instead of allocated
        auto img = image(20, S);
        std::size_t at = dots(img, 20 * S, 20, 20);
        at = record(img, at, 21, 0x40000000u, true, "BIG");
        record(img, at, 30, 10, false, "AFTER.BIN;1");
        open_image(img, "big-dir.iso");
        check(disc::find("AFTER.BIN").has_value(), "big sub-directory: the walk goes on after it");
    }
    {   // a file of 4 GiB - 1: the size used to wrap, then the vector was grown to 4 GiB
        auto img = image(20, S);
        std::size_t at = dots(img, 20 * S, 20, 20);
        record(img, at, 30, 0xFFFFFFFFu, false, "HUGE.IRX;1");
        open_image(img, "huge-file.iso");
        check(disc::find("HUGE.IRX").has_value(), "huge file: it is in the index");
        check(iop::kernel::read_disc_file("cdrom0:\\HUGE.IRX;1").empty(), "huge file: reading it whole is refused");
    }
}

// ---- IOP modules (IRX): an ELF with an iopmod program header (type 0x70000080) and one PT_LOAD ----

struct Irx {
    std::uint32_t phoff = 52, iopmod_off = 0x80, load_off = 0x100, filesz = 0x200, memsz = 0x200;
    std::uint32_t text = 0x200, data = 0, bss = 0;
    std::uint32_t shoff = 0, rel_off = 0, rel_size = 0;
    std::uint16_t shnum = 0;
};

std::vector<std::uint8_t> irx(const Irx &m) {
    std::vector<std::uint8_t> elf(0x400);
    std::memcpy(elf.data(), "\177ELF\1\1\1", 7);
    put(elf, 16, std::uint16_t{0xFF80}); put(elf, 18, std::uint16_t{8}); put(elf, 20, 1u);
    put(elf, 28, m.phoff); put(elf, 32, m.shoff); put(elf, 40, std::uint16_t{52}); put(elf, 42, std::uint16_t{32});
    put(elf, 44, std::uint16_t{2}); put(elf, 46, std::uint16_t{40}); put(elf, 48, m.shnum);
    put(elf, 52, 0x70000080u); put(elf, 56, m.iopmod_off);
    put(elf, 84, 1u); put(elf, 88, m.load_off); put(elf, 100, m.filesz); put(elf, 104, m.memsz);
    put(elf, 0x84, 0x1E0u);  // the iopmod record: entry, gp, text, data, bss sizes
    put(elf, 0x8C, m.text); put(elf, 0x90, m.data); put(elf, 0x94, m.bss);
    put(elf, 0x100 + 0x1E0, 0x03E00008u);  // jr ra
    if (m.shnum) { put(elf, 0x300 + 4, 9u); put(elf, 0x300 + 16, m.rel_off); put(elf, 0x300 + 20, m.rel_size); }
    return elf;
}

void irx_tests() {
    iop::reset();
    check(iop::load_module_buffer(irx({}), "valid.irx", "") >= 0, "valid module: loads");
    {
        Irx m; m.shoff = 0x300; m.shnum = 1; m.rel_off = 0x380; m.rel_size = 8;
        check(iop::load_module_buffer(irx(m), "valid-rel.irx", "") >= 0, "valid module with a relocation table: loads");
    }
    const auto refused = [](const Irx &m, const char *message) { check(iop::load_module_buffer(irx(m), "bad.irx", "") == -1, message); };
    { Irx m; m.phoff = 0xFFFFFF00u; refused(m, "program headers past the end: refused"); }
    { Irx m; m.iopmod_off = 0xFFFFFFF0u; refused(m, "iopmod record past the end: refused"); }
    { Irx m; m.memsz = 0x10; m.text = 0x10; refused(m, "file size larger than the module's memory: refused"); }
    { Irx m; m.filesz = 0x10000; m.memsz = 0x10000; m.text = 0x10000; refused(m, "contents past the end of the file: refused"); }
    { Irx m; m.load_off = 0xFFFFFF00u; refused(m, "contents offset past the end: refused"); }
    { Irx m; m.text = 0xFFFFFFC0u; refused(m, "sizes that wrap round 4 GiB: refused"); }
    { Irx m; m.text = 0x00300000u; refused(m, "a module larger than IOP memory: refused"); }
    { Irx m; m.shoff = 0xFFFFFF00u; m.shnum = 1; refused(m, "section headers past the end: refused"); }
    { Irx m; m.shoff = 0x300; m.shnum = 1; m.rel_off = 0x7FFFFFF0u; m.rel_size = 0x80; refused(m, "relocation table past the end: refused"); }
    { Irx m; m.shoff = 0x300; m.shnum = 1; m.rel_off = 0x380; m.rel_size = 0xFFFFFFF8u; refused(m, "relocation table too long: refused"); }
    check(iop::load_module_buffer(irx({}), "valid-again.irx", "") >= 0, "valid module after the refused ones: loads");
}

// ---- the boot ELF: PT_LOAD segments copied into the 32 MiB of EE memory ----

struct Segment { std::uint32_t offset = 0x100, vaddr = 0x00100000, filesz = 0x10, memsz = 0x40; };

bool boot(const Segment &s, std::uint32_t phoff, std::uint32_t &entry) {
    std::vector<std::uint8_t> elf(0x200);
    std::memcpy(elf.data(), "\177ELF\1\1\1", 7);
    put(elf, 24, 0x00100008u); put(elf, 28, phoff); put(elf, 42, std::uint16_t{32}); put(elf, 44, std::uint16_t{1});
    put(elf, 52, 1u); put(elf, 56, s.offset); put(elf, 60, s.vaddr); put(elf, 68, s.filesz); put(elf, 72, s.memsz);
    for (int i = 0; i < 16; ++i) elf[0x100 + i] = static_cast<std::uint8_t>(0xA0 + i);
    const auto path = std::filesystem::temp_directory_path() / "ps2-disc-input-test.elf";
    { std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char *>(elf.data()), static_cast<std::streamsize>(elf.size())); }
    const bool ok = ps2::load_elf(path.string(), entry);
    std::filesystem::remove(path);
    return ok;
}
bool boot(const Segment &s, std::uint32_t phoff = 52) { std::uint32_t entry = 0; return boot(s, phoff, entry); }

void elf_tests() {
    std::memset(ps2::g_ram + 0x100000, 0xEE, 0x80);
    std::uint32_t entry = 0;
    check(boot({}, 52, entry) && entry == 0x00100008u, "valid boot ELF: loads");
    check(ps2::g_ram[0x100000] == 0xA0 && ps2::g_ram[0x10000F] == 0xAF && ps2::g_ram[0x100010] == 0 && ps2::g_ram[0x10003F] == 0 &&
          ps2::g_ram[0x100040] == 0xEE, "valid boot ELF: contents copied, the rest of the segment cleared, nothing more");
    { Segment s; s.memsz = 0xFFF00000u; check(!boot(s), "boot ELF memory size that wraps: refused"); }
    { Segment s; s.offset = 0xFFFFFFF0u; s.filesz = 0x20; s.memsz = 0x20; check(!boot(s), "boot ELF file offset that wraps: refused"); }
    { Segment s; s.vaddr = ps2::kRamSize - 0x10; s.filesz = 0x100; s.memsz = 0x10; check(!boot(s), "boot ELF contents past the end of memory: refused"); }
    check(!boot({}, 0xFFFFFF00u), "boot ELF program headers past the end: refused");
}

}

int main() {
    disc_tests();
    irx_tests();
    elf_tests();
    std::printf("disc input: %d failed of %d checks\n", failed, checks);
    return failed ? 1 : 0;
}
