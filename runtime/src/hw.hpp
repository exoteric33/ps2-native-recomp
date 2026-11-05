#pragma once

#include "ps2/cpu.hpp"

#include <cstdint>
#include <functional>

namespace ps2::state {
class Stream;
}

namespace ps2 {

extern std::uint8_t g_vu0_code[4096];
extern std::uint8_t g_vu0_mem[4096];
extern std::uint8_t g_vu1_code[16384];
extern std::uint8_t g_vu1_mem[16384];

std::uint32_t physical(std::uint32_t a);

namespace hw {

void state(ps2::state::Stream &s);
std::function<void()> event_action(int id, std::uint64_t at);

enum Event {
    kEventVblank,
    kEventVblankEnd,
    kEventTimer0,
    kEventTimer1,
    kEventTimer2,
    kEventTimer3,
    kEventDma0,
    kEventIop = kEventDma0 + 10,
    kEventGsFinish,
    kEventCount
};

void vblank_start();
std::uint32_t intc_pending();
void intc_raise(unsigned cause);
void intc_ack(unsigned cause);
bool intc_set_mask(unsigned cause, bool enabled);
std::uint32_t dmac_pending();
void dmac_ack(unsigned channel);
void dmac_raise(unsigned channel);
bool dmac_set_mask(unsigned channel, bool enabled);
std::uint64_t gs_imr();
std::uint64_t gs_csr();
void gs_set_imr(std::uint64_t value);
void gs_set_crt(std::uint32_t interlace, std::uint32_t mode, std::uint32_t field);
// The video mode of the last SetGsCrt call (0 when it was never called): 2 NTSC, 3 PAL, 0x50 480p, 0x51 1080i, 0x52 720p, VGA modes
// above. Safe to read from the window and render threads.
std::uint32_t crt_mode();

std::uint32_t read32(std::uint32_t a);
// INTC_STAT read straight from the memory slow path (the game's waiting loops hammer it), with the waiting-loop skip of hw.cpp
std::uint32_t read_intc_stat();
// Counts the memory accesses that went the slow way (hardware registers, scratch memory of the vector units, ...) and system calls: a
// waiting loop is only skipped when none happened between two of its reads
extern std::uint64_t g_slow_seq;
// Cycles of waiting loops skipped so far (for the line at the end of a run)
std::uint64_t spin_skipped_cycles();
void write32(std::uint32_t a, std::uint32_t v);
std::uint64_t read64(std::uint32_t a);
void write64(std::uint32_t a, std::uint64_t v);
void read128(std::uint32_t a, Reg128 &out);
void write128(std::uint32_t a, const Reg128 &v);

void reset();

}

}
