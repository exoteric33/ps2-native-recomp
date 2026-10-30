#pragma once

// Main processor code the build does not know, translated while the game runs (docs/ee-live.md).
//
// A game build translates the code of the program and of the code windows it has seen (eerecomp). Code a game copies into a window
// later, or a function of a window image that nobody called while the build explored, has no translation, and the runner used to stop
// with "missing code". Now the game waits: the runner takes the window's image from memory (the way scripts/windows.py capture does),
// translates it with the same translator as the build (recomp/eerecomp_gen.cpp), compiles the C++ into a library with a C++ compiler,
// loads it, adds it to the modules it has (add_modules) and goes on. The libraries stay in a cache folder next to the game.

#include <cstdint>
#include <string>

namespace ps2::ee::live {

// Called once, after the generated code is registered and before the game starts. program is the game's executable (its code windows
// are read from it), cache is the folder for the libraries (made when the first one is built), game_dir the folder port.sh made
// (entries.txt and overlays\ in it get what was found), or empty. Loads what earlier runs left in the cache. With no C++ compiler
// found it logs one line and only the loaded libraries are used.
void start(const std::string &program, const std::string &cache, const std::string &game_dir);

bool active();

// The main processor reached pc where nothing is translated. True when code for pc was translated, compiled and loaded (the caller
// looks again); false when that is not possible (nothing is changed; the reason is in the log).
bool translate_missing(std::uint32_t pc);

// Called by the emulation thread at every frame boundary (the vertical blank), after the frame's own work. It watches the code windows
// (a few memory areas the game fills with different code as it goes); when the content of one has stopped changing and nothing
// translated serves it, it starts translating that window on a thread of its own, so that the pause of translate_missing shrinks to
// the rest of the work or disappears. It also takes finished libraries into use, here, on the emulation thread: the game never sees
// a library arrive at any other place. It changes nothing the game can notice (docs/gvz.md).
void frame_tick();

}  // namespace ps2::ee::live

namespace ps2 {

// Is there a translated module in place for the code that sits in the window at `base` over its first `size` bytes? A question only
// (runtime/src/dispatch.cpp): nothing is added or dropped and the spot-check cursor stays where it is, so asking does not change what
// the game does.
bool window_covered(std::uint32_t base, std::uint32_t size);

// A line of text about every fixed base module that covers pc: its name, size, how many of its check words differ from the memory now
// and the first one that does (a debugging aid for PS2_EE_LIVE_VERBOSE: why was a module that was translated for this code not used).
std::string module_report(std::uint32_t pc);

struct ModuleInfo;

// add_modules (generated.hpp) with a rank: of two modules of the same content that are both in place and both have the function, the
// one of the higher rank runs. A quick library (rank 0) is followed by one made with the normal flags (rank 1) for the same window.
//
// `soft` (one entry per module, or null): the groups of check words a live library keeps for the entries that only a guess made
// (recomp::HeadChecks): {entry offset, n, n * {offset, word}}. They do not decide whether the module is in place; the words of an entry
// are compared when that entry is entered, and an entry whose words differ is not served (the game misses it and a new library has it).
struct SoftChecks {
    const std::uint32_t *words = nullptr;
    std::uint32_t count = 0;
};
void add_modules_ranked(const ModuleInfo *modules, std::uint32_t count, int rank, const SoftChecks *soft = nullptr);

}  // namespace ps2
