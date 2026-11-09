#pragma once

// Counting only. Everything here is off unless PS2_GPU_STAT=1 (2: also one line per frame). With it off every call is
// one test of a flag; nothing here changes what the painter draws or in which order. The calls come from gpu.cpp: one per shape (draw), one per host
// upload, local copy and palette load, one per painter batch, one per frame, and the report at exit.

#include "../gs.hpp"

#include <cstdint>

namespace ps2::gs::gpu::stat {

extern bool g_on;
void init();  // reads PS2_GPU_STAT
void shape(const State &st, std::uint64_t attr, const Vertex *v, int count, std::uint32_t flags, int minx, int miny, int maxx, int maxy, std::uint64_t tex_lo, std::uint64_t tex_hi);
void upload(std::uint32_t dbp, std::uint32_t dbw, std::uint32_t dpsm, int x, int y, std::uint32_t w, std::uint32_t h, std::uint64_t bytes);
void local_copy(std::uint32_t dbp, std::uint32_t dbw, std::uint32_t dpsm, int x, int y, std::uint32_t w, std::uint32_t h);
void clut_load();
void batch(std::uint32_t prims, const char *why);
void state_loaded();
void frame_end();
void report();

inline bool on() { return g_on; }

}
