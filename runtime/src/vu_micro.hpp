#pragma once

#include "ps2/cpu.hpp"

#include <cstdint>
#include <string>

namespace ps2::state {
class Stream;
}

namespace ps2::vu {

void state(ps2::state::Stream &s);

void reset();
void run(int unit, std::uint32_t address);
std::uint32_t tpc(int unit);
void set_tops(std::uint32_t top, std::uint32_t itop);
void code_changed(int unit);
Vu0Regs &regs(int unit);
std::uint64_t interpreted_calls();
// XGKICK packets that did not end within VU1's data memory (see gif::end_runaway_kick), and the end-of-run line of which program sent them.
std::uint64_t runaway_kicks();
void runaway_summary();
// The GIF tags a kick at `address` walks through, one line each (what the report of the first such packet shows).
std::string describe_packet(std::uint32_t address);

}
