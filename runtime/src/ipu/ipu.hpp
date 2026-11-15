#pragma once

#include <cstdint>

namespace ps2::state {
class Stream;
}

namespace ps2::ipu {

void state(ps2::state::Stream &s);

void reset();
std::uint32_t read32(std::uint32_t a);
std::uint64_t read64(std::uint32_t a);  // read32(a) and read32(a + 4) in one call
void write32(std::uint32_t a, std::uint32_t v);
void write_fifo(const std::uint8_t *qword);
void read_fifo(std::uint8_t *qword);
void dma_started(int channel);
void stats();

// Commands that decode or convert a picture (IDEC, BDEC, VDEC, FDEC, CSC) given since the start: the picture pace reads it once a
// refresh to see that a film is playing. Not part of a save state.
std::uint64_t decode_commands();

}
