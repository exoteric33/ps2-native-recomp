#pragma once

#include <cstdint>

namespace ps2::state {
class Stream;
}

namespace ps2::gif {

void state(ps2::state::Stream &s);

void reset();
void stats();
void transfer(int path, const std::uint8_t *data, std::uint32_t qwc);
// An XGKICK packet that did not end within VU1's data memory (kicked at `address`): end it, so the next kick starts a new packet
void end_runaway_kick(std::uint32_t address);
void render(int path, const std::uint8_t *data, std::uint32_t qwc);
void render_reset();
void scan_signals(int path, const std::uint8_t *data, std::uint32_t qwc);
bool busy(int path);
std::uint32_t read(std::uint32_t a);
void write(std::uint32_t a, std::uint32_t v);
// MSKPATH3 from VIF1, reported back to the EE in GIF_STAT.M3P
void set_path3_masked(bool masked);
// Forgets PATH3 data held back by the mask, on the thread that holds it
void drop_held();
// VIF1 has run out of work (its DMA ended), so an unmasked PATH3 has the GIF to itself
void vif1_idle();
// SIGNAL, FINISH and LABEL in VU1's XGKICK packets (PATH1). The thread that runs VU1 (the emulation thread, or the
// vector thread when it is on) finds them and keeps them; the emulation thread hands them to the GS registers at points it fixes itself,
// the same with the vector thread on or off, so a run gives the same result either way and replays stay exact:
// - at every vblank, what was found before the fence posted two vblanks earlier (system.cpp; the vector thread is always past it by then,
//   so this never waits): deliver_path1_before;
// - once a run has handed any on (since it started or its last state save or load), also at once at a read or write of CSR, IMR or
//   SIGLBLID and at an idle EE while the GS may interrupt for SIGNAL or FINISH, after the vector thread has read everything it was sent:
//   deliver_path1 (true when it handed any on). A game that never sends them never waits for the vector thread, and pays one inline test
//   of g_path1_live at those points (no call).
extern bool g_path1_live;
bool deliver_path1_live();
inline bool path1_live() { return g_path1_live; }
inline bool deliver_path1() { return g_path1_live && deliver_path1_live(); }
void deliver_path1_before(std::uint32_t fence);
// The thread that runs VU1 has passed one more fence (a kVuFence message, or the emulation thread's own count without the vector thread)
void path1_fence();
// A state has no room for them: a save hands everything on first, a load forgets what the run it leaves had found. Both start the run
// over as one that has not used them.
void save_path1();
void forget_path1();
// The vector thread's own PATH1 reader, at a GIF reset that reaches it through its queue
void reset_path1_scan();

}
