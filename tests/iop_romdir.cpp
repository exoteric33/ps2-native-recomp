// No game data: crafted IOP reboot images (the ROM directory layout of ps2sdk's romimg, AFL-2.0) and the log lines that list their modules
// and boot list (runtime/src/iop/romdir.cpp, printed by read_reboot_image at an IOP reboot). The modules are four-byte ELF stubs.
#include "iop/romdir.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace {
namespace romdir = ps2::iop::romdir;

int failed = 0;
void check(bool ok, const char *message) {
    if (!ok) { ++failed; std::fprintf(stderr, "FAIL: %s\n", message); }
}
bool has(const std::string &text, const std::string &part) { return text.find(part) != std::string::npos; }

std::vector<std::uint8_t> module(std::size_t size) {
    std::vector<std::uint8_t> m(size, 0x5A);
    std::memcpy(m.data(), "\x7f" "ELF", 4);
    return m;
}
std::vector<std::uint8_t> text(const char *s) { return std::vector<std::uint8_t>(s, s + std::strlen(s)); }

// An image: RESET (boot code of `reset` bytes, empty in a reboot image), ROMDIR (the directory itself), EXTINFO (empty), then `files`.
std::vector<std::uint8_t> image(const std::vector<std::pair<std::string, std::vector<std::uint8_t>>> &files, std::size_t reset = 0) {
    std::vector<std::pair<std::string, std::vector<std::uint8_t>>> all;
    all.push_back({"RESET", std::vector<std::uint8_t>(reset, 0)});
    const std::size_t dir_size = (files.size() + 4) * 16;   // RESET, ROMDIR, EXTINFO, the files and the empty end entry
    all.push_back({"ROMDIR", {}});
    all.push_back({"EXTINFO", {}});
    for (const auto &f : files) all.push_back(f);
    std::vector<std::uint8_t> out;
    std::size_t dir_at = 0;
    for (std::size_t i = 0; i < all.size(); ++i) {
        if (all[i].first == "ROMDIR") {
            dir_at = out.size();
            out.resize(out.size() + dir_size);
        } else {
            out.insert(out.end(), all[i].second.begin(), all[i].second.end());
        }
        out.resize((out.size() + 15) & ~std::size_t{15});
    }
    for (std::size_t i = 0; i < all.size(); ++i) {
        std::uint8_t *e = out.data() + dir_at + i * 16;
        std::memcpy(e, all[i].first.data(), all[i].first.size());
        const std::uint32_t size = all[i].first == "ROMDIR" ? static_cast<std::uint32_t>(dir_size) : static_cast<std::uint32_t>(all[i].second.size());
        std::memcpy(e + 12, &size, 4);
    }
    return out;
}

}

int main() {
    // 1. Sony's modules only, no boot list: the image every game on our own discs reboots with.
    {
        const auto img = image({{"SYSMEM", module(100)}, {"LOADFILE", module(37)}, {"FILEIO", module(64)}});
        const romdir::Image r = romdir::parse(img.data(), img.size());
        check(r.ok && r.problem.empty(), "a plain image is read");
        check(r.files.size() == 6, "six directory entries");
        check(r.files.size() == 6 && r.files[3].name == "SYSMEM" && r.files[3].offset == 7 * 16 && r.files[3].size == 100, "SYSMEM follows the directory");
        check(r.files.size() == 6 && r.files[4].offset == 7 * 16 + 112 && r.files[5].offset == 7 * 16 + 112 + 48, "each file starts on 16 bytes");
        check(r.files.size() == 6 && r.files[5].module && !r.files[0].module, "modules are told by their ELF header");
        check(!r.has_boot_list && r.boot.empty(), "no boot list");
        const std::string log = romdir::describe(r, "cdrom0:\\IOPRP300.IMG;1");
        check(has(log, "[sif] the IOP image cdrom0:\\IOPRP300.IMG;1 holds 3 modules"), "the module count");
        check(has(log, "3 we stand in for: SYSMEM 100, LOADFILE 37, FILEIO 64\n"), "the stood-in list with sizes");
        check(has(log, "(not run at the reboot): none\n"), "no other module");
        check(has(log, "no boot list (IOPBTCONF)"), "says there is no boot list");
        check(!has(log, "other files"), "no data files");
    }
    // 2. A vendor module in the image, named by the image's own boot list, plus a name the image does not hold.
    {
        const auto conf = text("@800\r\nSYSMEM\r\n# a comment\r\n  LOADFILE \r\nVENDOR\r\n!addr 0\r\nSIO2MAN\r\n");
        const auto img = image({{"IOPBTCONF", conf}, {"SYSMEM", module(100)}, {"LOADFILE", module(37)}, {"VENDOR", module(300)}, {"NOTES", text("hi")}});
        const romdir::Image r = romdir::parse(img.data(), img.size());
        check(r.ok, "the vendor image is read");
        check(r.has_boot_list, "its boot list is found");
        check(r.boot == std::vector<std::string>{"SYSMEM", "LOADFILE", "VENDOR", "SIO2MAN"}, "the boot list's names, without directives and comments");
        const std::string log = romdir::describe(r, "cdrom0:\\IRX\\IOPRP271.IMG");
        std::fputs(log.c_str(), stdout);
        check(has(log, "holds 3 modules (sizes in bytes); 2 we stand in for: SYSMEM 100, LOADFILE 37\n"), "the stood-in part");
        check(has(log, "(not run at the reboot): VENDOR 300\n"), "the vendor module with its size");
        check(has(log, "other files in it: NOTES 2\n"), "a data file is not a module");
        check(has(log, "its boot list (IOPBTCONF, 4 names): SYSMEM LOADFILE VENDOR* SIO2MAN?"), "the boot list with marks");
        check(has(log, "not started by us; *: in the image") && has(log, "?: from the console's ROM"), "both marks explained");
        int lines = 0;
        for (char ch : log) lines += ch == '\n';
        check(lines == 4, "four lines");
    }
    // 3. A console ROM layout: the directory follows RESET's boot code.
    {
        const auto img = image({{"SYSMEM", module(20)}}, 0x40);
        const romdir::Image r = romdir::parse(img.data(), img.size());
        check(r.ok && r.files.size() == 4 && r.files[1].offset == 0x40, "a directory after boot code is found");
    }
    // 4. Odd files: each gives one line and no crash.
    {
        const auto one_line = [](const romdir::Image &r, const char *part) {
            const std::string log = romdir::describe(r, "X.IMG");
            int lines = 0;
            for (char ch : log) lines += ch == '\n';
            return !r.ok && lines == 1 && has(log, "[sif] the IOP image X.IMG could not be listed: ") && has(log, part);
        };
        const std::uint8_t tiny[10] = {};
        check(one_line(romdir::parse(tiny, sizeof tiny), "too short"), "a ten-byte file");
        check(one_line(romdir::parse(nullptr, 0), "too short"), "no data at all");
        std::vector<std::uint8_t> noise(4096);
        for (std::size_t i = 0; i < noise.size(); ++i) noise[i] = static_cast<std::uint8_t>(i * 37 + 11);
        check(one_line(romdir::parse(noise.data(), noise.size()), "no ROM directory"), "a file that is not an image");
        const auto img = image({{"SYSMEM", module(100)}, {"VENDOR", module(3000)}});
        check(one_line(romdir::parse(img.data(), img.size() - 1000), "VENDOR runs past the end"), "a cut-short image");
        std::vector<std::uint8_t> open_dir(img.begin(), img.begin() + 5 * 16);   // the directory without its end entry, nothing after
        for (int i : {1, 3, 4}) std::memset(open_dir.data() + i * 16 + 12, 0, 4);   // files that fit: only the end entry is missing
        open_dir[1 * 16 + 12] = 16;
        check(one_line(romdir::parse(open_dir.data(), open_dir.size()), "without an end mark"), "a directory with no end");
        auto bad_name = img;
        bad_name[3 * 16 + 1] = 0x01;
        check(one_line(romdir::parse(bad_name.data(), bad_name.size()), "unreadable file name"), "a name with a control byte");
        auto moved = img;
        moved.resize(moved.size() + 64);   // room at the end, so the shifted files still fit
        moved[0 * 16 + 12] = 0x10;   // RESET claims 16 bytes, so ROMDIR's data would start at 16, not at the directory
        check(one_line(romdir::parse(moved.data(), moved.size()), "does not point at the directory"), "a ROMDIR entry that misses");
    }
    check(romdir::stood_in("CDVDFSV") && romdir::stood_in("THREADMAN") && !romdir::stood_in("SIO2MAN") && !romdir::stood_in("VENDOR"), "the stand-in list");
    if (failed) return 1;
    std::puts("romdir: all checks passed");
    return 0;
}
