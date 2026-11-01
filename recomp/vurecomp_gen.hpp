#pragma once

// The vector unit code generator, shared by two users: the tool `vurecomp` (a folder of recorded programs in, the C++ of a
// game build out) and the runner itself (vu_live.cpp: a program the build does not know is generated and compiled while the
// game runs). Both call the same function, so a program behaves the same however it got compiled.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace ps2::vurecomp {

// One program to compile: its key, entry (recorded as %016llX_%04X.bin) and
// the complete code memory: 4 KiB for VU0, 16 KiB for VU1.
struct Input {
    std::uint64_t key = 0;
    std::uint32_t entry = 0;
    std::vector<std::uint8_t> code;
    int unit = 1;
};

struct Result {
    std::size_t programs = 0;      // programs written, jump targets included
    std::size_t jump_targets = 0;  // of them, programs that no input named but an input jumps to
    std::size_t skipped = 0;       // left to the interpreter (too many pipeline states)
    std::vector<std::uint64_t> keys;
};

// Writes vu1_NNN.cpp files and the table into out_dir (old vu1_* files of the folder are removed). With live_stamp == 0 the
// table is the one of a game build (`register_generated_vu()`). With a stamp it is the table of a loadable library: the
// exported functions ps2_vu_live_stamp() and ps2_vu_live_init() (see runtime/src/vu_live.cpp) and no other symbols to be
// found in the runner.
// with_targets adds the programs the inputs jump to (JALR-style calls), which a game build wants so they need no recording of their own;
// live compiling leaves them out and builds a target when the game really gets there.
Result generate_files(std::vector<Input> inputs, const std::filesystem::path &out_dir, std::uint64_t live_stamp, std::size_t per_file = 16, bool with_targets = true);

// The fingerprint a loadable library carries with its programs, so a library made by another version of the generator, the
// headers or the compiler flags is not loaded again (vu_live.cpp mixes it into the stamp).
extern const char *const kGeneratorVersion;

}  // namespace ps2::vurecomp
