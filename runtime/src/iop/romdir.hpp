#pragma once
// The contents of an IOP reboot image (IOPRP*.IMG, DNAS*.IMG): a ROM directory of files, most of them IOP modules, and maybe its own
// boot list (IOPBTCONF). Read only to tell the player's log what the image holds; nothing here runs a module.
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ps2::iop::romdir {

struct File {
    std::string name;
    std::uint32_t offset = 0, size = 0;
    bool module = false;   // the data starts with an ELF header (an IRX module)
};

struct Image {
    bool ok = false;                  // a ROM directory was found and every file lies inside the image
    std::string problem;              // why not, in a few words, when !ok (or a remark about an odd but readable image)
    std::vector<File> files;          // in directory order, RESET, ROMDIR and EXTINFO included
    bool has_boot_list = false;       // the image has an IOPBTCONF file
    std::vector<std::string> boot;    // its module names in order (directive and comment lines left out)
};

Image parse(const std::uint8_t *data, std::size_t size);

// A Sony system module our runtime stands in for itself (its kernel, SIF, file and disc services are native code), by ROM name.
bool stood_in(const std::string &name);

// The few log lines (each ending in '\n', each starting with "[sif] ") that say what `image` holds: its modules with their sizes, split
// into the ones we stand in for and the others, and its boot list with the others marked.
std::string describe(const Image &image, const std::string &path);

}
