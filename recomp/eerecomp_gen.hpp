#pragma once

// The main processor translator's pieces that two users share: the tool `eerecomp` (a game build: the program and every captured
// image of its code windows in, C++ out) and the runner itself (runtime/src/ee_live.cpp: code a game copies into a window while it
// runs is captured, translated and compiled on the spot, docs/ee-live.md). Both call the same functions, so a window image is
// translated, checked and named the same way however it got compiled.

#include "analysis.hpp"
#include "codegen.hpp"
#include "elf.hpp"
#include "snr2.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace recomp {

// entries.txt: one entry point per token, a plain address of the program or <window image name>:<offset in the window>; '#' starts a
// comment line.
struct Entries {
    std::vector<std::uint32_t> main;
    std::map<std::string, std::vector<std::uint32_t>> modules;
};
Entries read_entries(const std::string &path);

// Jumps and calls of the program that go to an address resolved when the game runs (imports of a relocatable module).
std::set<std::uint32_t> dynamic_sites(const Links &links);

// Calls the static program makes into the window seed every image of it, so each is translated from those entry points whether or
// not it has been seen.
std::vector<std::uint32_t> window_entries(const Image &image, const Program &program, std::uint32_t base, std::uint32_t size);

// A window image is served only while every word of it the game runs as code is in place, so the checks are all the translated code
// words: the game writes the window's data freely, and a change any translation covers is a different image and a call into it must
// pick the other module.
std::vector<std::pair<std::uint32_t, std::uint32_t>> checks_for_window(const Snr2Module &o, const Program &program);

// For an image with data between its code (a program the game reads into memory as one piece): the checks are only the first words
// of every function, up to its return, at most 16. A data word that the search for functions took for code and that the game writes
// later (a counter) would otherwise make the whole image look changed. A different image in the same place still differs in these.
// The entries that only a guess made (Program::weak: a word of data that points at code, a place after a return) are often data after
// all, and the game writes data (a counter, a table); a changed word there must not make the whole image look changed. Their first words
// are not in `hard` but in `soft` (groups of {entry offset, n, n * {offset, word}}, Translation::soft): the runner looks at the words of
// an entry when it is called and lets only that entry go when they differ.
struct HeadChecks {
    std::vector<std::pair<std::uint32_t, std::uint32_t>> hard;
    std::vector<std::uint32_t> soft;
};
HeadChecks entry_head_checks(const Snr2Module &o, const Program &program);

// ---------------------------------------------------------------------------------------------------------------- capturing

// The code windows of an executable (what scripts/windows.py list prints): an executable LOAD segment with no file contents is a
// window the game unpacks different code into as it goes. Each is 16 KiB aligned; a second window in the same unit moves up.
inline constexpr std::uint32_t kWindowUnit = 0x4000;
inline constexpr std::uint32_t kWindowCap = 0x40000;
std::vector<std::uint32_t> code_windows(const std::vector<std::uint8_t> &elf);

// Every segment that has contents in the file, code or data: {address, size in memory}. Memory the game loads code into does not overlap them.
std::vector<std::pair<std::uint32_t, std::uint32_t>> file_segments(const std::vector<std::uint8_t> &elf);

// The executable segments that have contents in the file (what the program itself translates): {address, size in memory}.
std::vector<std::pair<std::uint32_t, std::uint32_t>> code_segments(const std::vector<std::uint8_t> &elf);

// How far the image in the window at base reaches in memory (what scripts/windows.py capture does): the units the game wrote, up to
// the first entirely empty one, at least one unit, at most kWindowCap. ram is the whole memory of size ram_size.
std::uint32_t window_extent(const std::uint8_t *ram, std::uint32_t ram_size, std::uint32_t base);

// "<base as 8 hex digits>_<CRC-32 of the image as 8 hex digits>", the name windows.py gives a captured image (<name>.bin).
std::string window_name(std::uint32_t base, const std::uint8_t *image, std::size_t size);

// ---------------------------------------------------------------------------------------------------------------- live

// The program's own analysis, for the calls it makes into a window. Built once (about half a second for a big game).
class StaticProgram {
public:
    StaticProgram(const std::string &elf_path, const std::vector<std::uint32_t> &entries);
    std::vector<std::uint32_t> calls_into(std::uint32_t base, std::uint32_t size) const;
    // The stretch of the program that holds its code: from the first to the last instruction the analysis reached (and the `.text` section
    // when the file has one). The rest of an executable segment is data; {0, 0} when nothing was reached.
    std::pair<std::uint32_t, std::uint32_t> code_range() const;

private:
    Image image_;
    Program program_;
};

struct LiveRequest {
    std::uint32_t base = 0;
    std::vector<std::uint8_t> image;     // the window as captured
    std::string name;                    // window_name() of it
    std::string prefix;                  // names the functions of the library (unique inside the runner)
    std::vector<std::uint32_t> entries;  // absolute addresses to translate from
    std::uint64_t stamp = 0;             // not 0: the stamp the library carries (CodegenOptions::live_stamp)
    bool focused = false;                // true: translate only what the entries reach (an earlier library covers the rest of the image)
    bool sparse_checks = false;          // true: the image is checked by the first words of its functions (entry_head_checks)
    std::vector<std::uint32_t> candidates;  // words of memory that point into the image: possible function starts (Search::candidates)
    std::uint32_t text_begin = 0, text_end = 0;   // not 0: the image holds code only in [text_begin, text_end), the rest is data and no pointer into it is a function
    std::vector<std::uint32_t> served;      // calls and jumps to these addresses of the image are not followed: something else translated them (Search::served)
};

struct LiveResult {
    bool ok = false;
    std::string error;
    std::size_t units = 0;               // source files written (plus units.hpp and table.cpp)
    std::size_t code = 0;                // translated instructions
    std::set<std::uint32_t> accepted;    // the entries that are code in this image
    std::set<std::uint32_t> rejected;
    double analyse_s = 0, codegen_s = 0;   // where the time went: the analysis of the image, writing the C++
};

// Translates one captured window image into C++ files in out_dir, as eerecomp does for an image it reads from overlays/ (the same
// analysis, the same checks, the same code generator), as a module of its own that the runner adds to the ones it has.
LiveResult generate_live(const LiveRequest &request, const std::filesystem::path &out_dir);

}  // namespace recomp
