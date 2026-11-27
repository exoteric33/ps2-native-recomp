// A list of start-up routines outside the stretch of the program its build reached comes in with one stop, not one per routine (issue 147).
//
// No game and no disc. A made-up program with one executable segment: a return at its entry is all the build's analysis reaches, then
// data, then a packed run of small functions that only a table of pointers in the data names (the way C++ constructors are listed), then
// more data and one more function far behind it. The test calls the functions in the order of the table the way the game's start-up
// code does: every one that has no code yet is a stop of the live translator. With the code around a miss taken in (code_stretch in
// runtime/src/ee_live.cpp) the whole run comes in with the first stop; the function behind the data is not taken with it (the stretch
// ends at data) and is a stop of its own. It needs a C++ compiler the live translator can find (clang++ on the PATH).
//
//   ninja -C build/tests ps2eelivelisttest && build/tests/ps2eelivelisttest

#include "ps2/ee_live.hpp"
#include "ps2/generated.hpp"
#include "ps2/memory.hpp"
#include "ps2/system.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <vector>

namespace fs = std::filesystem;

namespace {
constexpr std::uint32_t kSegment = 0x00100000, kSegmentSize = 0x10000;
constexpr std::uint32_t kTable = kSegment + 0x2000;   // the list of pointers, in the data
constexpr std::uint32_t kRun = kSegment + 0x8000;     // jr ra; nop, then the functions
constexpr std::uint32_t kFunctions = 48;
constexpr std::uint32_t kFar = kSegment + 0xC008;     // one more function, behind data
constexpr std::uint32_t kData = 0x4C000000;           // opcode 19: no instruction (stands for a float, text, a table)
constexpr std::uint32_t kJrRa = 0x03E00008;

std::uint32_t function_at(std::uint32_t k) { return kRun + 8 + 12 * k; }

void put(std::vector<std::uint8_t> &out, std::size_t at, std::uint32_t v) {
    if (out.size() < at + 4) out.resize(at + 4, 0);
    std::memcpy(out.data() + at, &v, 4);
}

// The segment's contents, as the game would have them in memory.
std::vector<std::uint8_t> segment() {
    std::vector<std::uint8_t> s(kSegmentSize, 0);
    const auto word = [&](std::uint32_t at, std::uint32_t v) { put(s, at - kSegment, v); };
    for (std::uint32_t at = kSegment; at < kSegment + kSegmentSize; at += 4) word(at, kData);
    word(kSegment, kJrRa); word(kSegment + 4, 0);                  // the entry: all the build reaches
    word(kRun, kJrRa); word(kRun + 4, 0);                          // the end of a function before the run
    for (std::uint32_t k = 0; k < kFunctions; ++k) {               // addiu v0, zero, k; jr ra; nop
        word(function_at(k), 0x24020000u | k);
        word(function_at(k) + 4, kJrRa);
        word(function_at(k) + 8, 0);
    }
    word(kFar - 8, kJrRa); word(kFar - 4, 0);
    word(kFar, 0x24020063u); word(kFar + 4, kJrRa); word(kFar + 8, 0);
    for (std::uint32_t k = 0; k < kFunctions; ++k) word(kTable + 4 * k, function_at(k));
    word(kTable + 4 * kFunctions, kFar);
    word(kTable + 4 * kFunctions + 4, 0);
    return s;
}

// An executable with that one segment, entry at its start.
std::vector<std::uint8_t> program(const std::vector<std::uint8_t> &contents) {
    std::vector<std::uint8_t> e(0x100, 0);
    e[0] = 0x7F; e[1] = 'E'; e[2] = 'L'; e[3] = 'F'; e[4] = 1; e[5] = 1; e[6] = 1;
    const auto h16 = [&](std::size_t at, std::uint16_t v) { std::memcpy(e.data() + at, &v, 2); };
    h16(16, 2); h16(18, 8);                 // executable, MIPS
    put(e, 20, 1);                          // version
    put(e, 24, kSegment);                   // entry
    put(e, 28, 52);                         // program header at 52
    h16(40, 52); h16(42, 32); h16(44, 1);   // header size, program header size, one header
    put(e, 52 + 0, 1);                      // PT_LOAD
    put(e, 52 + 4, 0x100);                  // file offset
    put(e, 52 + 8, kSegment);               // virtual address
    put(e, 52 + 12, kSegment);              // physical address
    put(e, 52 + 16, kSegmentSize);          // file size
    put(e, 52 + 20, kSegmentSize);          // memory size
    put(e, 52 + 24, 5);                     // read + execute
    e.insert(e.end(), contents.begin(), contents.end());
    return e;
}
}

int main() {
    const fs::path cache = fs::temp_directory_path() / "ps2-ee-live-list-test";
    std::error_code error;
    fs::remove_all(cache, error);

    const std::vector<std::uint8_t> contents = segment();
    std::memcpy(ps2::g_ram + kSegment, contents.data(), contents.size());
    ps2::ee::live::start(program(contents), cache.string(), "");
    if (!ps2::ee::live::active()) {
        std::printf("ee live list: SKIP, no C++ compiler found for the live translator\n");
        return 77;
    }

    using Clock = std::chrono::steady_clock;
    const auto began = Clock::now();
    int failed = 0;
    // The start-up code: call every function of the list in order. A function without code is a stop.
    int stops = 0;
    for (std::uint32_t k = 0; k < kFunctions; ++k) {
        const std::uint32_t pc = function_at(k);
        if (ps2::has_code(pc)) continue;
        ++stops;
        if (!ps2::ee::live::translate_missing(pc) || !ps2::has_code(pc)) {
            std::printf("  FAIL the stop for function %u (0x%08X) did not bring it in\n", k, pc);
            ++failed;
            break;
        }
    }
    const double list_s = std::chrono::duration<double>(Clock::now() - began).count();
    std::printf("the list of %u functions: %d stops, %.1f s\n", kFunctions, stops, list_s);
    std::uint32_t served = 0;
    for (std::uint32_t k = 0; k < kFunctions; ++k) served += ps2::has_code(function_at(k));
    if (served != kFunctions) { std::printf("  FAIL %u of %u functions have code\n", served, kFunctions); ++failed; }
    if (stops > 2) { std::printf("  FAIL %d stops for one run of functions (one or two expected)\n", stops); ++failed; }
    // The function behind the data is not part of the run: no stop took it, and its own miss brings it in.
    if (ps2::has_code(kFar)) { std::printf("  FAIL the function behind the data came in with the run (the stretch did not end at the data)\n"); ++failed; }
    if (!ps2::ee::live::translate_missing(kFar) || !ps2::has_code(kFar)) { std::printf("  FAIL the function behind the data does not come in by its own miss\n"); ++failed; }
    // Data is never code.
    if (ps2::has_code(kTable) || ps2::has_code(kSegment + 0x4000)) { std::printf("  FAIL data has code\n"); ++failed; }
    std::printf(failed ? "ee live list: %d checks FAILED\n" : "ee live list: PASS\n", failed);
    std::fflush(stdout);
    std::_Exit(failed ? 1 : 0);
}
