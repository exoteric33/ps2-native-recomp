#pragma once

#include <cstdint>

namespace ps2::state {
class Stream;
}

namespace ps2::iop::hw {

void state(ps2::state::Stream &s);

void reset();
// What an IOP reset (SIF command 0x80000003) clears of the IOP side: interrupts, DMA channels, USB. The sound chip keeps its memory
// and voices, the pads and the memory card their state (the console does not reset them either); only the tick it had queued is
// scheduled again.
void soft_reset();
void raise_interrupt(unsigned line);
void set_interrupt_mask(unsigned line, bool enabled);
int next_pending_interrupt();
void acknowledge_interrupt(unsigned line);
void dma_done(unsigned channel);
// The sound chip took the next piece of an auto-DMA stream: the running channel's address register moves on to `madr`.
void dma_progress(unsigned channel, std::uint32_t madr);

std::uint32_t read(std::uint32_t a, unsigned size);
void write(std::uint32_t a, std::uint32_t v, unsigned size);

std::uint16_t *spu2_ram();

}
