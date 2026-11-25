#include "romdir.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace ps2::iop::romdir {

namespace {

constexpr std::size_t kEntry = 16;        // one directory entry: name (10 bytes), extended-info size (u16), file size (u32)
constexpr std::size_t kMaxFiles = 1024;   // a real image has about 20; more means we are reading something else

std::uint32_t u32(const std::uint8_t *p) { return static_cast<std::uint32_t>(p[0] | p[1] << 8 | p[2] << 16 | static_cast<std::uint32_t>(p[3]) << 24); }

bool name_is(const std::uint8_t *entry, const char *name) {
    const std::size_t n = std::strlen(name);
    if (std::memcmp(entry, name, n) != 0) return false;
    for (std::size_t i = n; i < 10; ++i)
        if (entry[i]) return false;
    return true;
}

}

// The layout follows ps2sdk's romimg tool and its rom0 reader (AFL-2.0): the directory is a list of 16-byte entries that starts with
// RESET and ROMDIR and ends at an entry whose name is empty; the files' data lie one after the other from the start of the image, each
// rounded up to 16 bytes, in directory order (so ROMDIR's own data is the directory). In a reboot image RESET is empty and the
// directory sits at offset 0; in a console ROM it follows the boot code.
Image parse(const std::uint8_t *data, std::size_t size) {
    Image out;
    if (!data || size < 2 * kEntry) {
        out.problem = "too short for a ROM directory (" + std::to_string(size) + " bytes)";
        return out;
    }
    std::size_t dir = 0;
    while (dir + 2 * kEntry <= size && !(name_is(data + dir, "RESET") && name_is(data + dir + kEntry, "ROMDIR"))) dir += kEntry;
    if (dir + 2 * kEntry > size) {
        out.problem = "no ROM directory in it (no RESET and ROMDIR entries)";
        return out;
    }
    std::uint64_t at = 0;
    std::size_t pos = dir;
    for (;; pos += kEntry) {
        if (pos + kEntry > size) {
            out.problem = "the directory runs to the end of the file without an end mark";
            return out;
        }
        if (!data[pos]) break;
        if (out.files.size() == kMaxFiles) {
            out.problem = "more than " + std::to_string(kMaxFiles) + " files in the directory";
            return out;
        }
        File f;
        for (std::size_t i = 0; i < 10 && data[pos + i]; ++i) {
            const char ch = static_cast<char>(data[pos + i]);
            if (ch < 0x21 || ch > 0x7E) {
                out.problem = "an unreadable file name in the directory";
                return out;
            }
            f.name += ch;
        }
        f.size = u32(data + pos + 12);
        f.offset = static_cast<std::uint32_t>(std::min<std::uint64_t>(at, 0xFFFFFFFFu));
        if (at + f.size > size) {
            out.problem = f.name + " runs past the end of the file (" + std::to_string(size) + " bytes; a cut-short copy?)";
            return out;
        }
        f.module = f.size >= 4 && std::memcmp(data + f.offset, "\x7f" "ELF", 4) == 0;
        at += (std::uint64_t{f.size} + 15) & ~std::uint64_t{15};
        out.files.push_back(std::move(f));
    }
    const auto romdir = std::find_if(out.files.begin(), out.files.end(), [](const File &f) { return f.name == "ROMDIR"; });
    if (romdir == out.files.end() || romdir->offset != dir) {
        out.problem = "its ROMDIR entry does not point at the directory";
        return out;
    }
    out.ok = true;
    for (const File &f : out.files) {
        if (f.name != "IOPBTCONF") continue;
        out.has_boot_list = true;
        // One name per line; lines that start with '@' (a load address), '!' (a directive) or '#' (a comment) are not module names.
        std::string line;
        const auto take = [&] {
            const auto first = line.find_first_not_of(" \t\r");
            const auto last = line.find_last_not_of(" \t\r");
            if (first != std::string::npos) {
                const std::string name = line.substr(first, last - first + 1);
                if (name[0] != '@' && name[0] != '!' && name[0] != '#') out.boot.push_back(name);
            }
            line.clear();
        };
        for (std::uint32_t i = 0; i < f.size && data[f.offset + i]; ++i) {
            const char ch = static_cast<char>(data[f.offset + i]);
            if (ch == '\n') take();
            else if (ch >= 0x20 && ch < 0x7F) line += ch;
            else if (ch == '\t' || ch == '\r') line += ' ';
        }
        take();
        break;
    }
    return out;
}

bool stood_in(const std::string &name) {
    // Sony's IOP kernel and the system services our runtime implements natively (iop/libs.cpp, iop/io.cpp, iop/cdvd.cpp,
    // iop/cdvdfsv.cpp, iop/sif.cpp: the FILEIO, LOADFILE, heap and CDVD servers). Every reboot image on our own discs holds only these.
    static const char *const kOurs[] = {
        "SYSMEM",  "LOADCORE", "EXCEPMAN", "INTRMANP", "INTRMANI", "SSBUSC",   "DMACMAN", "TIMEMANI", "TIMRMAN", "SYSCLIB",
        "HEAPLIB", "EECONF",   "THREADMAN", "THBASE",  "VBLANK",   "IOMAN",    "MODLOAD", "ROMDRV",   "STDIO",   "SIFMAN",
        "IGREETING", "SIFCMD", "SIFINIT",  "REBOOT",   "LOADFILE", "CDVDMAN",  "CDVDFSV", "FILEIO",   "SECRMAN", "EESYNC",
    };
    return std::any_of(std::begin(kOurs), std::end(kOurs), [&](const char *n) { return name == n; });
}

std::string describe(const Image &image, const std::string &path) {
    std::string out;
    if (image.files.empty() || !image.ok) {
        // An odd image: say so in one line; the files read before the problem are not listed, they could be anything.
        out = "[sif] the IOP image " + path + " could not be listed: " + (image.problem.empty() ? "unknown layout" : image.problem) + "\n";
        return out;
    }
    std::string ours, others, data;
    int n_ours = 0, n_others = 0;
    for (const File &f : image.files) {
        if (f.name == "RESET" || f.name == "ROMDIR" || f.name == "EXTINFO" || f.name == "IOPBTCONF") continue;
        const std::string item = f.name + " " + std::to_string(f.size);
        if (!f.module) data += (data.empty() ? "" : ", ") + item;
        else if (stood_in(f.name)) { ours += (ours.empty() ? "" : ", ") + item; ++n_ours; }
        else { others += (others.empty() ? "" : ", ") + item; ++n_others; }
    }
    out += "[sif] the IOP image " + path + " holds " + std::to_string(n_ours + n_others) + " modules (sizes in bytes); " +
           std::to_string(n_ours) + " we stand in for: " + (ours.empty() ? "none" : ours) + "\n";
    out += "[sif]   modules we do not stand in for (not run at the reboot): " + (others.empty() ? "none" : others) + "\n";
    if (!data.empty()) out += "[sif]   other files in it: " + data + "\n";
    if (!image.has_boot_list) {
        out += "[sif]   no boot list (IOPBTCONF) in it: the console's own list starts, with the image's modules in place of the same names\n";
        return out;
    }
    std::string list;
    bool in_image_mark = false, rom_mark = false;
    for (const std::string &name : image.boot) {
        const bool held = std::any_of(image.files.begin(), image.files.end(), [&](const File &f) { return f.name == name && f.module; });
        list += (list.empty() ? "" : " ") + name;
        if (stood_in(name)) continue;
        if (held) { list += '*'; in_image_mark = true; }
        else { list += '?'; rom_mark = true; }
    }
    out += "[sif]   its boot list (IOPBTCONF, " + std::to_string(image.boot.size()) + " names): " + (list.empty() ? "empty" : list);
    // the marks: names we do not stand in for, which the console starts at the reboot and we do not
    if (in_image_mark && rom_mark) out += "  (not started by us; *: in the image, ?: from the console's ROM)";
    else if (in_image_mark) out += "  (not started by us; *: in the image)";
    else if (rom_mark) out += "  (not started by us; ?: from the console's ROM)";
    out += "\n";
    return out;
}

}
