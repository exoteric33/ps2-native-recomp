// dispatch() must not use more stack the longer the game runs (issue #114: Medal of Honor: Vanguard ended with a stack
// overflow in ps2::dispatch on its first run, while its code was being translated live). Translated code that jumps to an address its own
// unit does not serve goes through dispatch() as a tail call, and dispatch() must hand over to the code it finds the same way: if it calls
// it instead, every such jump keeps one dispatch() frame on the stack until the game's function returns, and a loop that jumps between
// two pieces of code (a code window and the program, or two windows) runs out of stack after some 300,000 rounds, even on the emulation
// thread's 64 MB. No game needed: ninja ps2dispatchstacktest.
//
// The made-up code below does what translated code does for a jump it cannot serve itself: `c.pc = target; [[clang::musttail]] return
// dispatch(c, 0u, depth);`. Three loops, each a few thousand rounds, measure how far the stack moved: a code window that jumps to itself,
// a window and the program jumping to each other, and the same with what happens on a first visit (the window cache emptied by a
// FlushCache every few rounds, an optimised module of the same window switched in halfway). Then the window-program loop runs a million
// rounds on the emulation thread's own stack (host/big_stack_thread.hpp), which overflows it (exception C00000FD on Windows, SIGSEGV
// elsewhere) when dispatch() keeps its frames.
#include "../host/big_stack_thread.hpp"
#include "../runtime/src/dispatch_cache.hpp"
#include "ps2/ee_live.hpp"
#include "ps2/generated.hpp"
#include "ps2/system.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

constexpr std::uint32_t kProgram = 0x00100000;   // the program's own code (a static unit)
constexpr std::uint32_t kUnitSize = 0x4000;
constexpr std::uint32_t kWindow = 0x0012E000;    // a code window the game wrote (like Medal of Honor's loader window at 0x0012ED40)
constexpr std::uint32_t kWindowSize = 0x1800;    // 6 KiB
constexpr std::uint32_t kInWindow = 0xD40;       // the window's entry, 0x0012ED40
constexpr std::uint32_t kInProgram = 0x200;      // the program's entry
constexpr std::uint32_t kReturn = 0x00100400;    // where the "game function" that runs the loop returns to

enum class Loop { WindowSelf, WindowProgram };

struct State {
    Loop loop = Loop::WindowSelf;
    std::uint64_t rounds = 0, target = 0;
    std::uint64_t flush_every = 0;   // invalidate_window_instances() every n rounds (0: never), as the game's FlushCache does
    std::uint64_t upgrade_at = 0;    // switch in the optimised module of the window at this round (0: never)
    std::uintptr_t first = 0, lowest = 0;
};
State g;

void note_stack() {
    const auto here = reinterpret_cast<std::uintptr_t>(__builtin_frame_address(0));
    if (!g.first) g.first = g.lowest = here;
    if (here < g.lowest) g.lowest = here;
}

void switch_in_upgrade();

// The window's code (one unit, one entry): it jumps on through the dispatcher until the loop has run its rounds, then returns.
void window_code(ps2::Cpu &c, std::uint32_t, std::uint32_t depth) {
    note_stack();
    if (++g.rounds >= g.target) {
        c.pc = c.r[31].w[0];
        return;
    }
    if (g.flush_every && g.rounds % g.flush_every == 0) ps2::invalidate_window_instances();
    if (g.upgrade_at && g.rounds == g.upgrade_at) switch_in_upgrade();
    c.pc = g.loop == Loop::WindowSelf ? kWindow + kInWindow : kProgram + kInProgram;
    [[clang::musttail]] return ps2::dispatch(c, 0u, depth);
}

// The program's code: jumps back into the window, which the program's own translation does not serve.
void program_code(ps2::Cpu &c, std::uint32_t, std::uint32_t depth) {
    note_stack();
    c.pc = kWindow + kInWindow;
    [[clang::musttail]] return ps2::dispatch(c, 0u, depth);
}

std::uint16_t g_program_entries[kUnitSize / 4];
std::uint16_t g_window_entries[kWindowSize / 4];
std::uint16_t g_upgrade_entries[kWindowSize / 4];
const ps2::UnitInfo g_program_unit{kProgram, kUnitSize, program_code, g_program_entries};
const ps2::UnitInfo g_window_unit{0, kWindowSize, window_code, g_window_entries};
const ps2::UnitInfo g_upgrade_unit{0, kWindowSize, window_code, g_upgrade_entries};
// The window's check words: the module serves the window while these words are what the memory holds.
std::uint32_t g_checks[4];
const ps2::ModuleInfo g_window{"window (quick)", kWindow, kWindowSize, g_checks, 2, &g_window_unit, 1};
const ps2::ModuleInfo g_upgrade{"window (optimised)", kWindow, kWindowSize, g_checks, 2, &g_upgrade_unit, 1};

void switch_in_upgrade() { ps2::add_modules_ranked(&g_upgrade, 1, 1); }

void setup() {
    std::memset(g_program_entries, 0, sizeof g_program_entries);
    std::memset(g_window_entries, 0, sizeof g_window_entries);
    std::memset(g_upgrade_entries, 0, sizeof g_upgrade_entries);
    g_program_entries[kInProgram / 4] = 1;
    g_window_entries[kInWindow / 4] = 1;
    g_upgrade_entries[kInWindow / 4] = 1;
    // what the game wrote into the window (two words checked)
    const std::uint32_t words[2] = {0x27BDFFF0u, 0x03E00008u};   // addiu sp, sp, -16 / jr ra
    std::memcpy(ps2::g_ram + kWindow + kInWindow, &words[0], 4);
    std::memcpy(ps2::g_ram + kWindow + kInWindow + 4, &words[1], 4);
    g_checks[0] = kInWindow;     g_checks[1] = words[0];
    g_checks[2] = kInWindow + 4; g_checks[3] = words[1];
    ps2::register_units(&g_program_unit, 1);
    ps2::register_modules(&g_window, 1);
}

// Runs one loop as a game function the program called: dispatch() into the window, the window and its partner jump around until the
// rounds are done, then the window returns. The stack the loop used beyond its first round, in bytes.
std::uintptr_t run(Loop loop, std::uint64_t rounds, std::uint64_t flush_every, std::uint64_t upgrade_at, bool &returned) {
    setup();
    g = State{};
    g.loop = loop;
    g.target = rounds;
    g.flush_every = flush_every;
    g.upgrade_at = upgrade_at;
    ps2::Cpu c{};
    c.pc = kWindow + kInWindow;
    ps2::set32(c.r[31], kReturn);
    ps2::dispatch(c, 0, 1);
    returned = c.pc == kReturn && g.rounds == rounds;
    return g.first - g.lowest;
}

int g_failed = 0;

void measure(const char *what, Loop loop, std::uint64_t flush_every, std::uint64_t upgrade_at) {
    constexpr std::uint64_t kRounds = 20000;
    bool returned = false;
    const std::uintptr_t grew = run(loop, kRounds, flush_every, upgrade_at, returned);
    // A flat loop moves by nothing (or a frame of the window code at most); a dispatch() that keeps its frames grows by about 200 bytes a
    // round, 4 MB in these 20,000 rounds.
    const bool ok = returned && grew < 16 * 1024;
    std::printf("  %s %s: the stack grew %zu bytes in %llu rounds (%.1f a round)%s\n", ok ? "ok  " : "FAIL", what, static_cast<std::size_t>(grew),
                static_cast<unsigned long long>(kRounds), static_cast<double>(grew) / kRounds, returned ? "" : ", and the loop did not end where it should");
    if (!ok) ++g_failed;
}

}  // namespace

int main(int argc, char **argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);   // every line out at once: before the fix the program ends with a stack overflow
    const std::uint64_t long_rounds = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 1000000;
    // Everything runs on a thread made the way the emulation thread is (host/big_stack_thread.hpp, 64 MB), the one the game runs on.
    ps2::host::BigStackThread thread([&] {
        std::printf("dispatch into code windows, stack use per round:\n");
        measure("a window that jumps to itself", Loop::WindowSelf, 0, 0);
        measure("a window and the program jumping to each other", Loop::WindowProgram, 0, 0);
        measure("the same on a first visit (cache emptied every 64 rounds, the optimised module switched in halfway)", Loop::WindowProgram, 64, 10000);

        // The long run: before the fix this ends the program with a stack overflow, as in the tester's log.
        std::printf("%llu rounds of the window-program loop on the emulation thread's %zu MB stack ...\n", static_cast<unsigned long long>(long_rounds),
                    ps2::host::kEmulationStackBytes >> 20);
        bool returned = false;
        const std::uintptr_t grew = run(Loop::WindowProgram, long_rounds, 0, 0, returned);
        const bool ok = returned && grew < 16 * 1024;
        std::printf("  %s the long run ended, the stack grew %zu bytes\n", ok ? "ok  " : "FAIL", static_cast<std::size_t>(grew));
        if (!ok) ++g_failed;
    });
    thread.join();

    if (g_failed) {
        std::printf("dispatch stack: %d checks FAILED\n", g_failed);
        return 1;
    }
    std::printf("dispatch stack: PASS\n");
    return 0;
}
