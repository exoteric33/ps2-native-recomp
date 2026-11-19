// The optimised library that follows a quick one must serve everything the quick one did.
//
// No game and no disc. A made-up program and a made-up block of loaded code with three functions: A is found by the search of the
// image itself (it follows a return), B only by the entry the game missed, C only by a pointer to it that sits in memory. The game
// misses B: the quick library is built, then the same code again with the normal flags
// in the background. Both libraries must then serve A, B and C, the better one first, and the "quick only" marker of the image must
// be gone. It needs a C++ compiler the live translator can find (clang++ on the PATH) and takes a minute or two.
//
//   ninja -C build/tests ps2eeliveupgradetest && build/tests/ps2eeliveupgradetest
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
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {
constexpr std::uint32_t kBase = 0x00300000;        // the loaded code
constexpr std::uint32_t kA = kBase + 0x008, kB = kBase + 0x200, kC = kBase + 0x300;
constexpr std::uint32_t kPointer = 0x00200000;     // where memory holds the pointer to C

void put(std::vector<std::uint8_t> &out, std::size_t at, std::uint32_t v) {
    if (out.size() < at + 4) out.resize(at + 4);
    std::memcpy(out.data() + at, &v, 4);
}

// The smallest MIPS executable: one executable segment at 0x00100000 holding "jr ra; nop".
std::vector<std::uint8_t> tiny_program() {
    std::vector<std::uint8_t> e(0x200, 0);
    const std::uint8_t ident[] = {0x7F, 'E', 'L', 'F', 1, 1, 1};
    std::memcpy(e.data(), ident, sizeof ident);
    const auto h16 = [&](std::size_t at, std::uint16_t v) { std::memcpy(e.data() + at, &v, 2); };
    h16(16, 2); h16(18, 8);                 // executable, MIPS
    put(e, 20, 1);                          // version
    put(e, 24, 0x00100000);                 // entry
    put(e, 28, 52);                         // program header at 52
    h16(40, 52); h16(42, 32); h16(44, 1);   // header size, program header size, one header
    put(e, 52 + 0, 1);                      // PT_LOAD
    put(e, 52 + 4, 0x100);                  // file offset
    put(e, 52 + 8, 0x00100000);             // virtual address
    put(e, 52 + 12, 0x00100000);
    put(e, 52 + 16, 8);                     // file size
    put(e, 52 + 20, 8);                     // memory size
    put(e, 52 + 24, 5);                     // read + execute
    put(e, 0x100, 0x03E00008);              // jr ra
    put(e, 0x104, 0);                       // nop
    return e;
}

bool marker_says_quick(const fs::path &cache) {
    std::error_code error;
    bool quick = false;
    for (const auto &entry : fs::directory_iterator(cache, error))
        if (entry.path().extension() == ".mark")
            if (std::ifstream in(entry.path()); in && in.get() == 'f') quick = true;
    return quick;
}
}

int main() {
    const fs::path cache = fs::temp_directory_path() / "ps2-ee-live-upgrade-test";
    std::error_code error;
    fs::remove_all(cache, error);

    const std::vector<std::uint8_t> program = tiny_program();
    ps2::ee::live::start(program, cache.string(), "");
    if (!ps2::ee::live::active()) {
        std::printf("ee live upgrade: SKIP, no C++ compiler found for the live translator\n");
        return 77;
    }

    // Three functions of three instructions: addiu v0, zero, n; jr ra; nop. Z is a return with its delay slot, so A follows a return.
    // B and C sit after a word that is neither a return nor padding: nothing but the entry (B) and the pointer (C) names them.
    std::uint8_t *code = ps2::g_ram + kBase;
    const auto word = [&](std::uint32_t at, std::uint32_t v) { std::memcpy(code + (at - kBase), &v, 4); };
    word(kBase + 0x000, 0x03E00008); word(kBase + 0x004, 0);                                    // Z: jr ra; nop
    const auto function = [&](std::uint32_t at, std::uint32_t n) { word(at, 0x24020000u | n); word(at + 4, 0x03E00008); word(at + 8, 0); };
    function(kA, 1);
    word(kB - 4, 0x7C0F1234);   // junk
    function(kB, 2);
    word(kC - 4, 0x7C0F1234);
    function(kC, 3);
    std::memcpy(ps2::g_ram + kPointer, &kC, 4);

    if (ps2::has_code(kA) || ps2::has_code(kB) || ps2::has_code(kC)) {
        std::printf("ee live upgrade: FAIL, code is there before anything was translated\n");
        return 1;
    }
    ps2::ee::live::translate_missing(kB);

    using Clock = std::chrono::steady_clock;
    const auto began = Clock::now();
    const auto waited = [&] { return std::chrono::duration<double>(Clock::now() - began).count(); };
    bool quick_seen = false;
    for (; waited() < 300; std::this_thread::sleep_for(std::chrono::milliseconds(50))) {
        ps2::ee::live::frame_tick();
        if (!quick_seen && ps2::has_code(kB)) {
            quick_seen = true;
            std::printf("quick library in use after %.1f s: ranks A %d, B %d, C %d\n", waited(), ps2::serving_rank(kA), ps2::serving_rank(kB), ps2::serving_rank(kC));
        }
        if (ps2::serving_rank(kA) == 1) break;   // the optimised library is in
    }
    const int ra = ps2::serving_rank(kA), rb = ps2::serving_rank(kB), rc = ps2::serving_rank(kC);
    const bool quick = marker_says_quick(cache);
    std::printf("after the upgrade (%.1f s): ranks A %d, B %d, C %d (1 = optimised library, 0 = quick one), quick marker %s\n", waited(), ra, rb, rc, quick ? "still set" : "cleared");
    int failed = 0;
    if (!quick_seen) { std::printf("  FAIL the quick library never came\n"); ++failed; }
    if (ra != 1) { std::printf("  FAIL the optimised library does not serve A (found by the image scan)\n"); ++failed; }
    if (rb != 1) { std::printf("  FAIL the optimised library does not serve B (the original miss)\n"); ++failed; }
    if (rc != 1) { std::printf("  FAIL the optimised library does not serve C (a pointer in memory)\n"); ++failed; }
    if (quick && ra == 1 && rb == 1 && rc == 1) { std::printf("  FAIL the marker still says quick\n"); ++failed; }
    if (!quick && (rb != 1 || rc != 1)) { std::printf("  FAIL the marker was cleared although the upgrade covers less than the quick library\n"); ++failed; }
    std::printf(failed ? "ee live upgrade: %d checks FAILED\n" : "ee live upgrade: PASS\n", failed);
    std::fflush(stdout);
    std::_Exit(failed ? 1 : 0);   // a build that is still under way must not hold the test up
}
