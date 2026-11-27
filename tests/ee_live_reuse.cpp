// The same code in a window under another image name is served by the translation made before, not translated again (#148).
//
// No game and no disc. A made-up program with a code window at 0x00400000 (an executable segment with no contents in the file). The test
// writes made-up code into the window the way a game loads a file there, and changes it the way a game does:
//   F    addiu v1, zero, 5; j G             (F's code goes on into G without the dispatcher)
//   G    addiu v0, v1, 0x20; jr ra          F returns 0x25
//   D    a word of data right after G's return that decodes as code: the translator takes it for a function (a place after a return)
//   S    jr t0 to X                         a `jr` through a register can go to any entry of its 16 KiB unit, D's too
//   X    addiu v0, zero, 0x33; jr ra
//   40 small functions in the second unit, T one of them
// Steps:
//   1. The first miss at F translates the window (one library).
//   2. The game writes D: the image has a new name and the library is no longer in place. The miss at F is served by the library made
//      in step 1 (a view of it: F, G, X and the 40 functions, not D and not S), no translation. F still returns 0x25.
//   3. The game writes D again and changes T's code: the view is out (T's word is one it checks). The window settles (frame_tick) and the
//      library of step 1 serves it as it is now, nearly all of it: no translation in the background either. T itself is not served.
//   4. A miss at S is translated (its `jr` could reach D), a small one: S then runs and reaches X through the view.
//   5. The game changes G's code: F is not served by anything made before (G is a word F runs), it is translated again, returns 0x35.
//   6. The window holds the code of step 1 again: the first library is in place and F returns 0x25.
// It needs a C++ compiler the live translator can find (clang++ on the PATH).
//
//   ninja -C build/tests ps2eelivereusetest && build/tests/ps2eelivereusetest

#include "../runtime/src/dispatch_cache.hpp"
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
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {
constexpr std::uint32_t kProgram = 0x00100000, kProgramSize = 0x1000;
constexpr std::uint32_t kWindow = 0x00400000, kWindowSize = 0x10000;   // the segment; the game fills two units of it
constexpr std::uint32_t kF = kWindow + 0x100, kG = kWindow + 0x200, kD = kWindow + 0x20C, kS = kWindow + 0x218, kX = kWindow + 0x238;
constexpr std::uint32_t kRun = kWindow + 0x4000;   // the 40 small functions
constexpr std::uint32_t kFunctions = 40;
constexpr std::uint32_t kT = kRun + 12 * 5;
constexpr std::uint32_t kData = 0x4C000000;        // opcode 19: no instruction (stands for a table, text)
constexpr std::uint32_t kJrRa = 0x03E00008;

int g_failed = 0;

void check(bool ok, const char *what) {
    if (ok) return;
    std::printf("  FAIL %s\n", what);
    ++g_failed;
}

void put(std::vector<std::uint8_t> &out, std::size_t at, std::uint32_t v) {
    if (out.size() < at + 4) out.resize(at + 4, 0);
    std::memcpy(out.data() + at, &v, 4);
}

void poke(std::uint32_t at, std::uint32_t v) { std::memcpy(ps2::g_ram + at, &v, 4); }

// What a game does after it wrote code (FlushCache): the dispatcher looks at the modules again.
void flush() { ps2::invalidate_window_instances(); }

// The window as the game loads it.
void load_window() {
    for (std::uint32_t at = kWindow; at < kWindow + 0x8000; at += 4) poke(at, kData);
    poke(kF, 0x24030005u);                                    // addiu v1, zero, 5
    poke(kF + 4, 0x08000000u | ((kG >> 2) & 0x03FFFFFFu));   // j G
    poke(kF + 8, 0);
    poke(kG, 0x24620020u);                                    // addiu v0, v1, 0x20
    poke(kG + 4, kJrRa);
    poke(kG + 8, 0);
    poke(kD, 0x24840001u);                                    // "data": addiu a0, a0, 1; jr ra; nop
    poke(kD + 4, kJrRa);
    poke(kD + 8, 0);
    poke(kS, 0x3C080000u | (kX >> 16));                       // lui t0, hi(X)
    poke(kS + 4, 0x35080000u | (kX & 0xFFFFu));               // ori t0, t0, lo(X)
    poke(kS + 8, 0x01000008u);                                // jr t0
    poke(kS + 12, 0);
    poke(kX - 8, kJrRa);                                      // the end of something before X, so X follows a return
    poke(kX - 4, 0);
    poke(kX, 0x24020033u);                                    // addiu v0, zero, 0x33
    poke(kX + 4, kJrRa);
    poke(kX + 8, 0);
    poke(kRun - 8, kJrRa);
    poke(kRun - 4, 0);
    for (std::uint32_t k = 0; k < kFunctions; ++k) {          // addiu v0, zero, 0x100 + k; jr ra; nop
        poke(kRun + 12 * k, 0x24020100u + k);
        poke(kRun + 12 * k + 4, kJrRa);
        poke(kRun + 12 * k + 8, 0);
    }
}

// An executable with a code segment (a return at its entry) and the window: an executable segment with nothing in the file.
std::vector<std::uint8_t> program() {
    std::vector<std::uint8_t> e(0x100, 0);
    e[0] = 0x7F; e[1] = 'E'; e[2] = 'L'; e[3] = 'F'; e[4] = 1; e[5] = 1; e[6] = 1;
    const auto h16 = [&](std::size_t at, std::uint16_t v) { std::memcpy(e.data() + at, &v, 2); };
    h16(16, 2); h16(18, 8);
    put(e, 20, 1);
    put(e, 24, kProgram);
    put(e, 28, 52);
    h16(40, 52); h16(42, 32); h16(44, 2);
    const auto segment = [&](std::size_t at, std::uint32_t offset, std::uint32_t address, std::uint32_t file, std::uint32_t memory) {
        put(e, at + 0, 1); put(e, at + 4, offset); put(e, at + 8, address); put(e, at + 12, address);
        put(e, at + 16, file); put(e, at + 20, memory); put(e, at + 24, 5);
    };
    segment(52, 0x100, kProgram, kProgramSize, kProgramSize);
    segment(84, 0, kWindow, 0, kWindowSize);
    std::vector<std::uint8_t> code(kProgramSize, 0);
    put(code, 0, kJrRa);
    e.insert(e.end(), code.begin(), code.end());
    return e;
}

std::size_t libraries(const fs::path &cache) {
    std::size_t n = 0;
    std::error_code error;
    for (const auto &entry : fs::directory_iterator(cache, error))
        if (entry.is_regular_file() && entry.path().filename().string().find("whole") == std::string::npos && entry.path().extension() != ".mark") ++n;
    return n;
}

// Runs the game function at pc to its return; v0.
std::uint32_t run(std::uint32_t pc) {
    ps2::Cpu c{};
    c.pc = pc;
    ps2::set32(c.r[31], ps2::kHandlerReturn);
    c.budget = 1000000;
    ps2::dispatch(c, 0, 1);
    if (c.pc != ps2::kHandlerReturn) std::printf("  (the function at 0x%08X ended at 0x%08X)\n", pc, c.pc);
    return c.r[2].w[0];
}

double seconds(std::chrono::steady_clock::time_point since) { return std::chrono::duration<double>(std::chrono::steady_clock::now() - since).count(); }
}

int main() {
    const fs::path cache = fs::temp_directory_path() / "ps2-ee-live-reuse-test";
    std::error_code error;
    fs::remove_all(cache, error);
    // One library per translation: no second build of the same code with other flags in the background.
#ifdef _WIN32
    _putenv_s("PS2_EE_LIVE_QUICK", "off");
#else
    setenv("PS2_EE_LIVE_QUICK", "off", 1);
#endif
    for (std::uint32_t at = kProgram; at < kProgram + 8; at += 4) poke(at, at == kProgram ? kJrRa : 0);
    load_window();
    ps2::ee::live::start(program(), cache.string(), "");
    if (!ps2::ee::live::active()) {
        std::printf("ee live reuse: SKIP, no C++ compiler found for the live translator\n");
        return 77;
    }
    using Clock = std::chrono::steady_clock;

    // 1. The first miss translates the window.
    auto began = Clock::now();
    check(!ps2::has_code(kF), "F has code before anything was translated");
    check(ps2::ee::live::translate_missing(kF) && ps2::has_code(kF), "the first miss at F brings it in");
    const std::size_t first = libraries(cache);
    std::printf("1. first miss at F: %zu library, %.1f s\n", first, seconds(began));
    check(first == 1, "one library after the first miss");
    check(run(kF) == 0x25, "F returns 0x25");
    check(ps2::has_code(kD) && ps2::has_code(kS) && ps2::has_code(kT) && ps2::has_code(kX), "the whole window came in (D, S, T, X)");

    // 2. Data that the translator took for code changes: the library is out, and a view of it serves F again.
    poke(kD, 0x24840002u);
    flush();
    check(!ps2::has_code(kF), "the library of step 1 still serves F after a word it checks changed");
    began = Clock::now();
    check(ps2::ee::live::translate_missing(kF) && ps2::has_code(kF), "the miss at F after D changed is served");
    std::printf("2. D changed, miss at F: %zu libraries, %.3f s\n", libraries(cache), seconds(began));
    check(libraries(cache) == first, "the miss at F after D changed translated again (a new library)");
    check(run(kF) == 0x25, "F returns 0x25 after D changed");
    check(ps2::has_code(kT) && ps2::has_code(kX), "T and X are served with F");   // (G is no entry: F reaches it by a jump inside the unit)
    check(!ps2::has_code(kD), "D is served although its word changed");
    check(!ps2::has_code(kS), "S is served although its jr can reach D");

    // 3. D again and T's code change: the view of step 2 is out. When the window settles, the library of step 1 serves what it can
    //    as a whole, and nothing is translated in the background.
    poke(kD, 0x24840003u);
    poke(kT, 0x24020077u);
    flush();
    check(!ps2::has_code(kRun), "the view of step 2 still serves after T changed");
    for (int k = 0; k < 8 * 6; ++k) ps2::ee::live::frame_tick();
    std::this_thread::sleep_for(std::chrono::seconds(3));
    for (int k = 0; k < 8; ++k) ps2::ee::live::frame_tick();
    std::printf("3. D and T changed, the window settled: %zu libraries, F %s, the first small function %s\n", libraries(cache), ps2::has_code(kF) ? "served" : "not served",
                ps2::has_code(kRun) ? "served" : "not served");
    check(libraries(cache) == first, "the settled window was translated in the background");
    check(ps2::has_code(kF) && ps2::has_code(kRun) && run(kF) == 0x25 && run(kRun) == 0x100, "the settled window is served by the library of step 1");
    check(!ps2::has_code(kT), "T is served although its code changed");
    check(ps2::ee::live::translate_missing(kT) && run(kT) == 0x77, "T's new code is translated and runs");
    const std::size_t after_t = libraries(cache);

    // 4. S cannot be served by the old library (its jr might reach D): it is translated, and reaches X through the view.
    began = Clock::now();
    check(ps2::ee::live::translate_missing(kS) && ps2::has_code(kS), "the miss at S brings it in");
    std::printf("4. miss at S: %zu libraries, %.1f s\n", libraries(cache), seconds(began));
    check(libraries(cache) == after_t + 1, "S was not translated (one more library)");
    check(run(kS) == 0x33, "S returns 0x33 through X");

    // 5. Code F runs changes: nothing made before serves F.
    poke(kG, 0x24620030u);
    flush();
    check(!ps2::has_code(kF), "F is served after G's code changed");
    const std::size_t before = libraries(cache);
    check(ps2::ee::live::translate_missing(kF) && ps2::has_code(kF), "the miss at F after G changed brings it in");
    check(libraries(cache) == before + 1, "F was not translated after G changed");
    check(run(kF) == 0x35, "F returns 0x35 with G's new code");
    std::printf("5. G changed, miss at F: %zu libraries, F returns 0x%X\n", libraries(cache), run(kF));

    // 6. The code of step 1 is back: its library serves again.
    load_window();
    flush();
    check(ps2::has_code(kF) && run(kF) == 0x25, "the code of step 1 is back and F returns 0x25");

    std::printf(g_failed ? "ee live reuse: %d checks FAILED\n" : "ee live reuse: PASS\n", g_failed);
    std::fflush(stdout);
    std::_Exit(g_failed ? 1 : 0);
}
