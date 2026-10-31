#pragma once

#include "elf.hpp"
#include "r5900.hpp"

#include <cstdint>
#include <set>
#include <vector>

namespace recomp {

struct Program {
    std::uint32_t base{};
    std::uint32_t end{};
    std::vector<std::uint8_t> code;
    std::set<std::uint32_t> functions;
    std::set<std::uint32_t> entries;
    std::set<std::uint32_t> rejected;
    std::set<std::uint32_t> dynamic;
    // Entries that only a guess made (a data word that points at code, an address constant, a place after a return, a stack frame): not
    // reached by a call or jump of the code that was walked. If the memory there is rewritten later it is most likely data, not code.
    std::set<std::uint32_t> weak;

    bool is_code(std::uint32_t address) const {
        return address >= base && address < end && (address & 3u) == 0 && code[(address - base) >> 2];
    }
};

// How far the search for entry points goes beyond the code that the given entries reach.
struct Search {
    // Scan the whole image for places that look like the start of a function (a stack frame after a return, every word of the image that
    // is an address of code). Off: only what the entries reach, plus the address constants and jump tables inside that code.
    bool whole_image = true;
    // Also take the place right after a return and its delay slot as a function start (a short function without a stack frame, which
    // nothing in the image calls by name). Used for code that is translated while the game runs (docs/gvz.md).
    bool after_return = false;
    // Addresses that something outside the image points at (words of memory, a jump table that a file brought in) and that may be function
    // starts: taken like the words of the image itself, as an entry point when what follows decodes as code (the check `tentative`).
    std::vector<std::uint32_t> candidates;
    // Addresses of the image that some other translation serves already (the program's own code, a library made earlier): a call or a
    // tail jump (`jal`, `j`) to one of them is not followed, it stays a call through the dispatcher, so that a library holds what is new
    // and not the whole call tree below one new function.
    std::set<std::uint32_t> served;
};

Program analyse(const Image &image, const std::vector<std::uint32_t> &extra_entries, const std::set<std::uint32_t> &dynamic = {}, const Search &search = {});

}
