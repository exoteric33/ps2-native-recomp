#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace ps2::state {
class Stream;
}

namespace ps2::iop::spu2 {

void state(ps2::state::Stream &s);

void reset();
// The IOP queue that held the next sound tick was emptied (an IOP reset): the next register access schedules a new one.
void restart_ticks();
std::uint16_t *ram();
std::uint32_t read(std::uint32_t a, unsigned size);
void write(std::uint32_t a, std::uint32_t v, unsigned size);
void dma(int core, std::uint32_t madr, std::uint32_t words, bool to_spu, int channel);
void dma_stop(int core);
void dump_audio(const char *path);
void dump_voices();
// The sound lines of the F9 problem report: voices started, sound data loaded, the stream, volumes, loudness, and a
// one-line reading. Safe to call from any thread; it only reads counters the emulation thread keeps (not part of a save state).
std::string summary();
void set_sink(std::function<void(const std::int16_t *samples, std::size_t frames)> sink);

}
