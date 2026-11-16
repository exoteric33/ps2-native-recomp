#pragma once

// The texture cache of the hardware drawing path.
//
// One idea: every writer of graphics memory is our own code (gpu.cpp's uploads, local copies, painter draws; the card path's draws and write-backs),
// so each 8 KB page of memory carries a sequence number that the writers bump. A decoded texture is valid while none of the pages it covers was
// written after it was decoded, and while the palette it used is the same one. No texture bytes are hashed and nothing is read back.
//
// Off unless PS2_GPU_TEXSTAT is set (counting only, no card needed) or the card path asks for the cache (PS2_GPU_HW_CACHE=1, replay only).
// With both off every call below is one test of a flag.

#ifndef __EMSCRIPTEN__
#include <webgpu/webgpu.h>
#endif

#include <bitset>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ps2::gs::gpu::hwcache {

using Pages = std::bitset<512>;

enum Writer : std::uint8_t { kNone = 0, kUpload, kDraw, kCopy, kStore, kState, kWriters };

// Everything a decode depends on besides the memory itself (the same fields the trial's decode key held, with the palette by identity, not by slot).
struct Request {
    std::uint32_t tbp, tbw, tpsm;  // TEX0: base (blocks), buffer width (64 pixels), pixel format
    std::uint32_t tw, th;          // log2 of the size, as drawn (not the clamp region)
    std::uint32_t cpsm, csa;       // palette format and offset (palette formats only)
    std::uint32_t texa;            // the TEXA word of gpu.cpp's setup (w[20]) and the enable bit; only read for formats that expand alpha
    std::uint32_t texa_on;
    std::uint32_t clut_slot;       // the painter's palette slot when the shape was drawn
};

struct Lookup {
    bool hit = false;                // the decoded copy is still right: no decode needed
    bool refused = false;            // no room (or the cache is off): the caller takes the trial's route
    bool picture = false;            // the pages were last written by a draw: a picture the game drew, not an upload (direct sampling serves it)
    std::uint32_t entry = ~0u;       // stable id of the entry
    std::uint32_t offset = 0;        // where the decoded words live in the caller's decoded buffer
    std::uint32_t words = 0;
    bool alpha_known = false;
    std::uint8_t alpha_min = 0, alpha_max = 255;  // smallest and largest alpha of the decoded texture (the draw layer cancels alpha tests with it)
};

// ---- switches
extern bool g_on;      // counting (PS2_GPU_TEXSTAT) or cache (PS2_GPU_HW_CACHE) active: gpu.cpp's hooks test this one flag
inline bool on() { return g_on; }
bool counting();       // PS2_GPU_TEXSTAT: gpu.cpp feeds the cache from real play, no decode happens
bool verify_on();      // PS2_GPU_TEXSTAT=3: memory is downloaded every frame and compared with what the page numbers claim
void init();           // reads the switches; call once
void configure(std::uint32_t capacity_words, std::uint32_t stream_words, bool allow_move);  // the card path: size of its decoded buffer, the part for streamed textures, and whether an entry may change its place

// ---- writers of memory. All take the rectangle as the writer sent it; the pages are worked out exactly.
void write_region(std::uint32_t bp, std::uint32_t bw, std::uint32_t psm, int x0, int y0, int x1, int y1, Writer who);
void write_pages(const Pages &pages, Writer who);
// A host upload (the bytes are the ones the game sent). What it leaves in memory is known from the bytes: the same bytes to the same place give the blocks they had before,
// so a texture the game uploads again every frame keeps its decoded copy. `whole_rows`: the chunk starts and ends on the end of a row of pixels.
void write_upload(std::uint32_t bp, std::uint32_t bw, std::uint32_t psm, int x0, int y0, int x1, int y1, bool whole_rows, const std::uint8_t *bytes, std::size_t size);
void state_loaded();   // a save state replaced all of memory and the palettes

// ---- palettes. from/to are the painter's palette slots: the load copies `from` to `to` and overwrites the entries it loads (gpu.cpp's kLoadClut).
void clut_load(std::uint32_t from, std::uint32_t to, std::uint32_t cbp, std::uint32_t cpsm, std::uint32_t csm, std::uint32_t csa, std::uint32_t is8,
               std::uint32_t cbw, std::uint32_t cou, std::uint32_t cov);

// ---- the replay (card path): the frame is run again and again from the same starting memory
void replay_snapshot();                 // memory and palettes as they are now are what each replay starts from
void replay_restore();                  // a replay starts: back to that memory
void forget();                          // drop every decoded copy (a timing variant ran without the rules)
void capture_pre(std::uint32_t sub, std::uint32_t command, std::size_t first, std::size_t last);  // painter command (sub, command) carries uploads / palette loads first..last of this round
void pending_clear();                   // the round of uploads and palette loads has been handed over
void replay_compute(std::uint32_t sub, std::uint32_t command);  // that command runs again: its writes (or, if unknown, a write of everything)
std::uint32_t reserve(const Request &r);                         // plan time: the texture's fixed place in the decoded buffer (~0u: no room)
void stats_reset();
void stats_print(const char *label);    // lookups, hits and decoded bytes since stats_reset

// ---- readers
Request request_of(const std::uint32_t *setup);               // from gpu.cpp's setup words
Lookup lookup(const Request &r);                              // find or make the entry; on a miss the entry counts as decoded now (the caller decodes)
void set_alpha(std::uint32_t entry, std::uint8_t lo, std::uint8_t hi);

// A painter batch asks for these textures (counting mode): setups are `count` records of `stride` words.
void batch_textures(const std::uint32_t *setups, std::uint32_t count, std::uint32_t stride);

// ---- step 5, the card: smallest and largest alpha of every decoded copy, worked out by one reduction pass over the decoded buffer and read back once (not part of the draw
// path: the live path will run it for the new copies after their decode and read the answer a frame later; until then `alpha_known` is false and the range is 0..255).
// PS2_GPU_HW_CACHE_ALPHA_CHECK=1 also reads the decoded buffer back and compares with a processor count. Returns how many copies were measured.
#ifndef __EMSCRIPTEN__
std::size_t alpha_ranges(WGPUDevice device, WGPUQueue queue, WGPUBuffer decoded, std::uint64_t decoded_bytes);
void alpha_summary(std::size_t measured);
#endif

// ---- counting
void frame_end();
void verify(const std::uint8_t *memory);   // after a download of all 4 MB
void report();

}
