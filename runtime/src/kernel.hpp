#pragma once

#include "ps2/cpu.hpp"

#include <cstdint>
#include <functional>
#include <string>

namespace ps2::state {
class Stream;
}

namespace ps2::kernel {

void state(ps2::state::Stream &s);
std::function<void()> alarm_action(int id);

void reset();
void start_main_thread(std::uint32_t entry, std::uint32_t gp, std::uint32_t argc, std::uint32_t argv);
void handle_syscall(Cpu &c, std::uint32_t pc);
void service(Cpu &c);
bool must_unwind();
bool in_handler();
bool interruptible();
// Runs the handlers of every pending, enabled interrupt now, nested like those a system call runs (the caller's registers are kept).
// EI, MTC0 to Status and a program's D_STAT acknowledge (hw.cpp) use it; it does nothing inside a handler or with interrupts off.
void take_pending(Cpu &c);
int current_thread();
void on_vblank_start();
void on_hblank();
void wake_idle();
void dump();

// The F9 report's thread list, the same words the end-of-run list uses. Call them only on the emulation thread.
std::string thread_summary();  // one plain line, e.g. "every EE thread waits: main thread on semaphore 49 (count 0, 1 waiting), ..."
std::string thread_report();   // one line per started thread

// The interrupt trace (PS2_TRACE_INTC=1), off by default: g_trace_irq is the one flag the hot paths test.
extern bool g_trace_irq;
void trace_raise(bool dmac, unsigned cause);  // a cause or channel fired (hw.cpp), and what becomes of it
void trace_register_write(int reg, std::uint32_t value, std::uint32_t before, std::uint32_t after);  // a direct write: 0 INTC_STAT, 1 INTC_MASK, 2 D_STAT
// What the interrupt side looks like now, one line each, every line starting with `prefix`: the Status register's IE/EIE bits, the INTC
// and DMAC mask and status, the installed handlers (and, with the trace on, how often each ran). The F9 report and the end-of-run list use it.
std::string interrupt_report(const char *prefix);
// The game routine (interrupt handler, alarm callback, system call the game set) the EE is inside right now, one line starting with
// `prefix`, or nothing when it is inside none. Safe from any thread: the F9 report written by the window uses it.
std::string routine_report(const char *prefix);

// The picture pace (ps2/pace.hpp). pace_setup sets the refreshes per picture (1 = off) and says where it came from
// in the log; pace_picture is called when the game changes the buffer the TV shows (DISPFB1/2); pace_report prints the end-of-run line.
void pace_setup(int every, const char *source);
int pace_every();
void pace_picture();
void pace_report();

}
