#pragma once

#include <atomic>
#include <cstdint>

// What the crash report (host/crash.cpp) needs to know about the running game, and the test switch that makes it appear on purpose.
// Nothing here costs anything while the game runs: naming a thread happens once per thread, noting a live library once per library,
// noting a system call is one store, and the test switch is one comparison of a global per frame (per sound block, per loop of a
// worker) that is false unless PS2_CRASH_TEST is set.
namespace ps2::crash {

// The threads of the program, named where each one starts. The names are what the crash lines and a debugger show.
enum class Thread : std::uint8_t { Main, Game, VectorUnit, Gpu, Sound, LiveEe, LiveVu, Log, Watchdog, Tiles, Count };

// Names the calling thread (kept for the crash report, and given to the system for debuggers and profilers) and, when the crash report
// is installed, gives it what a crash needs on its own thread (a second stack for a stack overflow).
void enter_thread(Thread thread);

// The name of the calling thread for the report, null when it was never named (the report then asks the system).
const char *thread_name();

// Where the game was: the number of the frame the emulation thread had reached, the main processor's registers as the last translated
// function left them (pc is only as fresh as that function's last write; ra and sp are the game's own), the game's own thread and the
// last system call the game made (number and the pc it came from; 0 and 0 before the first). Read without any lock: a crash report must
// not wait for a thread that may be the one that crashed.
struct Where {
    std::uint64_t frame;
    std::uint32_t pc, ra, sp;
    int ee_thread;
    std::uint32_t syscall, syscall_pc;
    bool syscall_known;
};
Where where();

// The last system call of the game, noted where the kernel takes it (one relaxed store; the report reads it).
extern std::atomic<std::uint64_t> g_last_syscall;
inline void note_syscall(std::uint32_t number, std::uint32_t pc) {
    g_last_syscall.store((static_cast<std::uint64_t>(number) << 32) | pc, std::memory_order_relaxed);
}

// A library of live-compiled game code was loaded (ee_live, vu_live): `inside` is any address in it (a function it exports). The report
// marks stack frames in such a library as live game code. Not the hot path: once per library.
void note_live_library(Thread kind, const void *inside);
struct LiveLibrary {
    std::uint64_t base;  // the library's load address (Windows: 0 here, the report asks the system for the module of `inside`)
    const void *inside;
    Thread kind;
};
// The libraries noted so far (lock-free: entries are written before the count is raised).
const LiveLibrary *live_libraries(int &count);

// The crash report installs a function here that is called by enter_thread() for each thread.
void set_thread_hook(void (*hook)());

// PS2_CRASH_TEST=<thread>:<kind>[:<frame>] makes <thread> fail on purpose (at <frame> of the game or later, default 0), so the crash
// lines can be looked at. thread: main (the window thread), emu or game (the emulation thread), vu, gpu, sound, live-ee, live-vu, tee
// (the thread that copies the log). kind: access (a write to address 0), read (a read of address 0), stack (endless recursion), divide
// (an integer division by zero), illegal (an illegal instruction), abort, throw (an exception nobody catches), param (Windows: a C
// runtime call with an argument it refuses), live (a fault inside live-compiled game code: once a live library is loaded, its init
// function is called with null pointers). Off unless set; false and one comparison per call when off.
void test_parse(const char *spec);
extern int g_test_thread;
void test_fire(Thread thread);
inline void test_poll(Thread thread) {
    if (__builtin_expect(g_test_thread == static_cast<int>(thread), 0)) test_fire(thread);
}
// The same at fixed places that are not per frame (the start of the window thread, of the emulation thread, of the log copy):
// fires at once when PS2_CRASH_TEST names `where` (main, emu or tee).
void test_here(const char *where);

}
