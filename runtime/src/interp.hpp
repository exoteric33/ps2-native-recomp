#pragma once

// The main processor interpreter (runtime/src/interp.cpp): runs code the build has no translation for, instruction
// by instruction, and goes back to translated code as soon as there is some. Shared by dispatch.cpp and interp.cpp.

#include "ps2/generated.hpp"

#include <cstdint>

namespace ps2 {

// Runs the code at `pc`, leaving c.pc as it is (see call_quiet).
void interpret_from(Cpu &c, std::uint32_t pc, std::uint32_t depth);

// A call into the code at `target` that leaves c.pc alone, the way translated code calls the units of its own image: it does not set
// c.pc, which therefore keeps its older value for as long as the callee does not set it. That value is part of a replay's checkpoint, so
// the interpreter does the same: a replay recorded with translated code must go on matching when the interpreter runs a piece of it.
// The signature is that of a unit, so that a tail call is possible.
void call_quiet(Cpu &c, std::uint32_t target, std::uint32_t depth);

// Would translated code that contains `from` have called or jumped to `target` directly (the same translated image), without setting
// c.pc? The image is not known for code that has no translation; the program's own code is one image, and elsewhere what lies within the
// size of the biggest image (4 MiB) of each other is taken as one.
bool same_image(std::uint32_t from, std::uint32_t target);

// Runs the code at c.pc. Same signature as a translated unit, so that dispatch() can tail-call it.
void interpret(Cpu &c, std::uint32_t entry, std::uint32_t depth);

// PS2_INTERP_RANGE=<lo>,<hi> (hex) sends every entry into [lo, hi) to the interpreter, "all" sends everything: a debugging aid that tells a
// translation bug from a runtime one, and the test that the interpreter does what the translated code does.
void read_dispatch_switches();          // PS2_INTERP_RANGE, from ps2::read_switches() at the start of main()
bool interp_forced_any();               // is the switch on at all
bool interp_forced(std::uint32_t pc);   // is pc in the range

// Instructions interpreted so far (cycles charged).
std::uint64_t interp_instructions();

// Counts every module added while the game runs (dispatch.cpp): the interpreter looks for translated code again only after it changed.
std::uint32_t modules_epoch();

// Would dispatch() find translated code at pc, and not send it to the interpreter anyway.
bool code_translated(std::uint32_t pc);

}  // namespace ps2
