#pragma once

#include "ps2/cpu.hpp"

#include <cstdint>
#include <functional>

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
int current_thread();
void on_vblank_start();
void on_hblank();
void wake_idle();
void dump();

// The picture pace (ps2/pace.hpp). pace_setup sets the refreshes per picture (1 = off) and says where it came from
// in the log; pace_picture is called when the game changes the buffer the TV shows (DISPFB1/2); pace_report prints the end-of-run line.
void pace_setup(int every, const char *source);
int pace_every();
void pace_picture();
void pace_report();

}
