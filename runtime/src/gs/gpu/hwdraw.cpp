// ============================================================================================================================================================
// EXPERIMENTAL: the hardware drawing path ("card path"). Off by default. Not faster than the painter yet.
//
// What it is. The normal drawing path (gpu.cpp + gs.wgsl, "the painter") paints every pixel of every triangle and sprite with one compute shader, exactly as the
// PS2's graphics chip does. This file draws the plain shapes with the graphics card's own fixed-function hardware instead (render pipelines: rasterizer, depth test,
// blender), which is what other PS2 emulators do. The painter is still the reference: whatever the card cannot reproduce exactly goes to the painter, and the card's
// pictures are written back to the emulated graphics memory when something needs them there.
//
// How to turn it on. Set the environment variable PS2_GPU_HW=1 before starting ps2run (nothing else; with it unset no code of this path runs, no shader of it is
// compiled and the device does not ask for its extra feature). The start-up log then shows a "[hw-live]" line, and the program ends with a summary of what the card
// drew and what stayed on the painter ("painter keeps: ..."). On Windows the path picks Vulkan (the card path was faster there than on Direct3D 12 in the one test made);
// PS2_GPU_BACKEND still wins. It needs the internal scale 1 (no PS2_SCALE); at another scale the painter draws everything. The browser build compiles this file as empty stubs.
//
// What works. Plain shapes: textured (palettes included) or not, alpha test, depth test and write, the common blend forms, textures that are pictures the game drew itself
// (sampled directly from the card's picture). Not on the card, drawn by the painter: lines and points, a blend factor taken from the destination alpha, colour
// wrap-around with blending, a destination alpha test, some frame formats, a partial frame mask (the end-of-run summary lists them as "painter keeps"). By default the card also
// takes only batches that need no per-pixel proof that the shapes of a run do not overlap (PS2_GPU_HW_INDEP_MAX) and that are big enough to be worth a switch
// (PS2_GPU_HW_SWITCH_MIN, 256 shapes), so with the defaults it draws only a few percent of the shapes of a busy scene (God of War II hall: about 12,000 of 620,000 in one test).
// "Wide routing" is PS2_GPU_HW=1 PS2_GPU_HW_INDEP_MAX=all PS2_GPU_HW_SWITCH_MIN=0: about 95 % of the plain shapes go to the card.
// The checks that exist: tests/hwalpha.cpp (build with "ninja ps2hwalpha"; random frames drawn by both paths, with a table of how often each feature is exact),
// scripts/hwdiff.py (compares two pictures of one frame), and PS2_GPU_HW_EAGER=1 (writes every picture back after every batch: slow, obviously right; the lazy
// bookkeeping must give the same memory). With the switch off nothing changes: the ordinary test suite gives 54 of 55 (the one difference, vu/lower/efu, is not drawing).
//
// Known problems (measured on one Windows laptop with Intel integrated graphics, 2026-11; numbers will differ on other cards).
//  1. It is slower than the painter. With the wide routing the frame rate with the speed limit off was lower than the painter's: God of War I ship fight 12.6 % lower,
//     God of War II cutscene 5 %, Silent Hill 2 17 %. Where the time goes: the window thread costs 2.5 to 3.7 ms more per frame than with the painter (the check that
//     overlapping shapes of a run are independent, setting up the card's work, copies between the card's pictures and the emulated graphics memory), and the
//     card's replay of just the plain shapes of the God of War II hall took 8.6 ms on Vulkan on that laptop. Both have to come down before it can win.
//  2. The God of War II hall fight crashes after 8 to 28 seconds with the wide routing: wgpu reports "Not enough memory left" in wgpuCommandEncoderFinish. Host memory
//     sits about 500 MB above the painter's, and the program writes 3.5 to 7.7 MB per frame into short-lived buffers (vertices, setups, parameters; a new
//     wgpuQueueWriteBuffer staging allocation each time). Nothing the counters can see grows without limit (PS2_GPU_HW_MEMLOG=300 prints one "[memlog]" line every 300 frames
//     with what this file holds and what wgpu holds). The first fix to try: one persistent buffer per kind that is written in place and grown when needed, instead of a
//     new staging allocation per write. PS2_GPU_HW_CAP_MB=<MB> caps the memory held by the card's pictures, which may help to see where the limit is.
//  3. Tested only on an NVIDIA card (Linux, Vulkan) and an Intel integrated card (Windows, Vulkan). Not tested on AMD or Apple graphics, on Direct3D 12 or on Metal.
//
// Debug switches (all only with PS2_GPU_HW=1): PS2_GPU_HW_LOG=1 (why pictures are written back, what the card draws), PS2_GPU_HW_EAGER=1 (see above), PS2_GPU_HW_TIME=1
// (processor time of each step), PS2_GPU_HW_MEMLOG=<n> (memory counters), PS2_GPU_HW_DEBUG_NO=<features> (bisecting: shapes with these features go to the painter),
// PS2_GPU_HW_ONLY=<classes> (only those classes of shape, with PS2_GPU_BENCH), PS2_GPU_HW_ROUND=0 (the card's own blend rounding), PS2_GPU_HW_DUAL=0 (no dual-source
// blending), PS2_GPU_HW_ASYNC=0 (build a pipeline on first use and wait for it, instead of in the background), PS2_GPU_HW_PAL=0 (no palette
// record), PS2_GPU_HW_LIVE=0 (no live drawing; only the replay benchmark), PS2_GPU_STAT=1 (hwstat.cpp: counts what the game draws, no drawing change). The replay benchmark is
// PS2_GPU_BENCH=<vblank>,<reps> together with PS2_GPU_HW=1 (replays one captured frame on the painter and on the card path and prints the times).
//
// Where things are: hwdraw.cpp (this file: classification of shapes, the card's pictures and their bookkeeping, pipelines, the live path, the replay), hwdraw.wgsl and
// hwdraw_sync.wgsl (shaders), hwcache.cpp (texture bookkeeping and the decoded-texture cache), hwstat.cpp (counting only), and the hooks in gpu.cpp (calls to hw:: and
// hwcache:: functions, each behind a test of the switch).
// ============================================================================================================================================================
#include "hwdraw.hpp"
#ifndef __EMSCRIPTEN__

#include "ps2/os.hpp"
#include "../gs.hpp"
#include "hwcache.hpp"

#include <webgpu/wgpu.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#if defined(__AVX2__)
#include <immintrin.h>
#endif
#include <set>
#include <vector>

extern const char kHwShader[];
extern const char kHwSyncShader[];

#include <algorithm>
#include <array>
#include <bitset>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>

#include <unordered_map>
#if defined(__APPLE__)
#include <mach/mach.h>
#elif defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace ps2::gs::gpu::hw {

// ---------------------------------------------------------------- what the card path holds and creates
// PS2_GPU_HW_MEMLOG=<n> prints one `[memlog] ...` line every n frames (gpu.cpp calls memlog_tick at the end of each submit; with the switch off the call is one test of a bool).
// The wrappers below stand where hwdraw.cpp creates and drops its textures, buffers, bind groups, render pipelines, encoders and passes: with the switch off each is the plain call and one test.
std::uint64_t g_memlog_every = std::strtoull(os::env("PS2_GPU_HW_MEMLOG").c_str(), nullptr, 10);
bool g_memlog = g_memlog_every > 0;
namespace memlog {

struct Tally {
    std::uint64_t made = 0, freed = 0, made_bytes = 0, freed_bytes = 0;
};
struct State {
    std::mutex m;  // pipelines are made on a worker thread
    Tally tex, buf, grp, rpipe, enc, rp, cp;
    std::atomic<std::uint64_t> submits{0}, done{0};  // queue submits, and how many of them the card has finished (an OnSubmittedWorkDone callback per submit)
    std::uint64_t upload_bytes = 0;                  // bytes the card path wrote with wgpuQueueWriteBuffer (vertices, setups, parameters) since the last line
    std::uint64_t max_inflight = 0;                  // the most submits waiting for the card at the end of any submit since the last line
    std::uint64_t sub_passes = 0, max_passes = 0;    // the card path's render and compute passes in this submit, and the most in any one submit since the last line
    std::uint64_t sub_upload = 0, max_upload = 0;    // the same for the bytes uploaded
    std::unordered_map<const void *, std::uint64_t> size;  // bytes of every live texture and buffer made through the wrappers
} S;

std::uint64_t texel_bytes(WGPUTextureFormat f) {
    switch (f) {
        case WGPUTextureFormat_R8Unorm: case WGPUTextureFormat_R8Uint: return 1;
        case WGPUTextureFormat_RG8Unorm: case WGPUTextureFormat_R16Uint: case WGPUTextureFormat_R16Float: case WGPUTextureFormat_Depth16Unorm: return 2;
        case WGPUTextureFormat_RGBA16Float: case WGPUTextureFormat_RG32Float: case WGPUTextureFormat_RG32Uint: return 8;
        case WGPUTextureFormat_RGBA32Float: case WGPUTextureFormat_RGBA32Uint: return 16;
        default: return 4;
    }
}

inline void note(Tally &t, const void *obj, std::uint64_t bytes) {
    std::lock_guard<std::mutex> lock(S.m);
    ++t.made, t.made_bytes += bytes;
    if (bytes) S.size[obj] = bytes;
}
inline void drop(Tally &t, const void *obj) {
    std::lock_guard<std::mutex> lock(S.m);
    ++t.freed;
    if (const auto it = S.size.find(obj); it != S.size.end()) t.freed_bytes += it->second, S.size.erase(it);
}

inline WGPUTexture new_texture(WGPUDevice d, const WGPUTextureDescriptor *td) {
    WGPUTexture t = wgpuDeviceCreateTexture(d, td);
    if (g_memlog && t) note(S.tex, t, static_cast<std::uint64_t>(td->size.width) * td->size.height * td->size.depthOrArrayLayers * td->mipLevelCount * texel_bytes(td->format));
    return t;
}
inline void del_texture(WGPUTexture t) {
    if (g_memlog && t) drop(S.tex, t);
    wgpuTextureRelease(t);
}
inline WGPUBuffer new_buffer(WGPUDevice d, const WGPUBufferDescriptor *bd) {
    WGPUBuffer b = wgpuDeviceCreateBuffer(d, bd);
    if (g_memlog && b) note(S.buf, b, bd->size);
    return b;
}
inline void del_buffer(WGPUBuffer b) {
    if (g_memlog && b) drop(S.buf, b);
    wgpuBufferRelease(b);
}
inline WGPUBindGroup new_group(WGPUDevice d, const WGPUBindGroupDescriptor *gd) {
    WGPUBindGroup g = wgpuDeviceCreateBindGroup(d, gd);
    if (g_memlog && g) note(S.grp, g, 0);
    return g;
}
inline void del_group(WGPUBindGroup g) {
    if (g_memlog && g) drop(S.grp, g);
    wgpuBindGroupRelease(g);
}
inline WGPURenderPipeline new_rpipe(WGPUDevice d, const WGPURenderPipelineDescriptor *pd) {
    WGPURenderPipeline p = wgpuDeviceCreateRenderPipeline(d, pd);
    if (g_memlog && p) note(S.rpipe, p, 0);
    return p;
}
inline void del_rpipe(WGPURenderPipeline p) {
    if (g_memlog && p) drop(S.rpipe, p);
    wgpuRenderPipelineRelease(p);
}
inline WGPUCommandEncoder new_encoder(WGPUDevice d, const WGPUCommandEncoderDescriptor *ed) {
    if (g_memlog) note(S.enc, nullptr, 0);
    return wgpuDeviceCreateCommandEncoder(d, ed);
}
inline WGPURenderPassEncoder begin_render(WGPUCommandEncoder e, const WGPURenderPassDescriptor *rd) {
    if (g_memlog) note(S.rp, nullptr, 0), ++S.sub_passes;
    return wgpuCommandEncoderBeginRenderPass(e, rd);
}
inline WGPUComputePassEncoder begin_compute(WGPUCommandEncoder e, const WGPUComputePassDescriptor *cd) {
    if (g_memlog) note(S.cp, nullptr, 0), ++S.sub_passes;
    return wgpuCommandEncoderBeginComputePass(e, cd);
}

// The process's resident memory in MB (the title bar's number, host/window.cpp resident_mb) and the memory it has committed (Windows: the private bytes, Linux: VmData).
#ifdef _WIN32
struct WinCounters {  // PROCESS_MEMORY_COUNTERS of psapi.h, through kernel32's K32GetProcessMemoryInfo so that nothing else has to be linked
    unsigned long cb, faults;
    std::size_t peak, working, qpp, qp, qnpp, qnp, pagefile, pagefile_peak;
};
bool win_counters(WinCounters &c) {
    using Fn = int(__stdcall *)(void *, void *, unsigned long);
    static const Fn fn = reinterpret_cast<Fn>(reinterpret_cast<void *>(GetProcAddress(GetModuleHandleA("kernel32.dll"), "K32GetProcessMemoryInfo")));
    c = WinCounters{};
    c.cb = sizeof c;
    return fn && fn(GetCurrentProcess(), &c, sizeof c);
}
#endif
double resident_mb() {
#if defined(_WIN32)
    WinCounters c;
    return win_counters(c) ? static_cast<double>(c.working) / (1024.0 * 1024.0) : 0.0;
#elif defined(__APPLE__)
    mach_task_basic_info_data_t info{};
    mach_msg_type_number_t n = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &n) != KERN_SUCCESS) return 0.0;
    return static_cast<double>(info.resident_size) / (1024.0 * 1024.0);
#else
    long pages = 0, resident = 0;
    if (std::FILE *f = std::fopen("/proc/self/statm", "r")) {
        if (std::fscanf(f, "%ld %ld", &pages, &resident) != 2) resident = 0;
        std::fclose(f);
    }
    return static_cast<double>(resident) * static_cast<double>(sysconf(_SC_PAGESIZE)) / (1024.0 * 1024.0);
#endif
}

// Linux only (zeros elsewhere): where the resident memory sits. anon = memory the process and the driver allocated, file = mapped files (the program, libraries), shmem = shared mappings.
struct Split {
    double anon = 0, file = 0, shmem = 0, commit = 0;
    long threads = 0;
};
Split split_mb() {
    Split r;
#if defined(__linux__)
    if (std::FILE *f = std::fopen("/proc/self/status", "r")) {
        char line[256];
        while (std::fgets(line, sizeof line, f)) {
            long kb = 0;
            if (std::sscanf(line, "RssAnon: %ld kB", &kb) == 1) r.anon = static_cast<double>(kb) / 1024.0;
            else if (std::sscanf(line, "RssFile: %ld kB", &kb) == 1) r.file = static_cast<double>(kb) / 1024.0;
            else if (std::sscanf(line, "RssShmem: %ld kB", &kb) == 1) r.shmem = static_cast<double>(kb) / 1024.0;
            else if (std::sscanf(line, "VmData: %ld kB", &kb) == 1) r.commit = static_cast<double>(kb) / 1024.0;
            else if (std::sscanf(line, "Threads: %ld", &kb) == 1) r.threads = kb;
        }
        std::fclose(f);
    }
#elif defined(_WIN32)
    if (WinCounters c; win_counters(c)) r.commit = static_cast<double>(c.pagefile) / (1024.0 * 1024.0);
#endif
    return r;
}

}  // namespace memlog

namespace {

constexpr std::uint32_t kPrimWords = 28, kSetupWords = 40;

// ---------------------------------------------------------------- what the capture keeps

struct Batch {
    std::uint32_t submit, command;
    std::vector<std::uint32_t> prims, setups;
    int minx = 1 << 30, miny = 1 << 30, maxx = -1, maxy = -1;
};
struct Pre {
    std::uint32_t submit, command;
    bool clut;
    std::uint64_t lo, hi;
};

bool g_enabled = false, g_capturing = false;
bool g_pal_refine = true;  // PS2_GPU_HW_PAL=0: no palette record, no refined looks
bool g_dual = false;  // the device has dual-source blending: shapes with a stored-alpha rule draw once (fs_dual), else the alpha channel gets a draw of its own
// Dual-source blending is used when the device has it (PS2_GPU_HW_DUAL=0: the fallback). On an Intel laptop (Vulkan, Windows) it is not repeatable: a draw call
// with dual-source blending whose shapes overlap gave pixels one step apart from run to run with identical input (a God of War II arena scene: one or two words of
// the 4 MB, about one run in two); with the fallback, or with one draw call per shape, every run gave the same memory. It stays on because the fallback costs
// the window thread about 9 ms a frame in a God of War ship fight on that laptop (31 against 22 ms); scripts/hwjudge.py turns it off, so its memory judges repeat.
bool want_dual(WGPUDevice device) { return os::env("PS2_GPU_HW_DUAL") != "0" && wgpuDeviceHasFeature(device, WGPUFeatureName_DualSourceBlending); }
bool g_cache = false;  // PS2_GPU_HW_CACHE=1: decoded textures stay between replays while the memory they came from holds the same bytes
std::vector<hwcache::Request> g_dec_req;  // per entry of the plan's params that is a decode: what to ask the cache
bool g_direct = false;  // PS2_GPU_HW_DIRECT=1: the fragment shader reads the textures from graphics memory, no decode step
bool g_linear = true;   // targets stay linear on the card, are sampled directly and written back lazily (PS2_GPU_HW_LINEAR=0: the trial's page-level write-back)
bool g_overshoot = true;  // PS2_GPU_HW_OVERSHOOT (on unless =0): ignore the one-texel overshoot of the bilinear taps past a target's valid area (the tap reads the edge texel, not the memory beyond)
bool g_self = true;     // PS2_GPU_HW_SELF (on unless =0): a draw that reads the target it draws on reads a copy of the box made just before the draw (the trial wrote the box back and read memory instead)
bool g_log = false;     // PS2_GPU_HW_LOG=1: why write-backs happen
bool g_check = false;   // pipelines are made inside an error scope and a failure is reported (nullptr)
bool g_eager = false;   // PS2_GPU_HW_EAGER=1: write back and reload every target after every batch (the slow, obviously right bookkeeping every lazy result is checked against)
float g_round = 0.496f;  // PS2_GPU_HW_ROUND=<levels> (default just under half a level, 0 = off): the offset used to make the card's rounding to nearest come out as the chip's cut
unsigned g_probe = 0;   // PS2_GPU_HW_PROBE=<n>: measuring aid for the direct fetch (see PROBE in hw.wgsl)
std::vector<Batch> g_batches;
std::vector<Pre> g_pres;

float f(std::uint32_t u) {
    float v;
    std::memcpy(&v, &u, 4);
    return v;
}

// ---------------------------------------------------------------- the survey of the frame
// Counts of shapes and of the pixels they cover (a triangle's area, a sprite's rectangle) per feature, to see which ones a fixed-function
// rasterizer can take. The pixel count is the area before the depth test, so it is the work the compute path does too.

struct Tally {
    std::uint64_t shapes = 0;
    double pixels = 0;
};
std::map<std::string, Tally> g_tally;
double g_total_pixels = 0;
std::uint64_t g_total_shapes = 0;

void tally(const std::string &key, double pixels) {
    Tally &t = g_tally[key];
    ++t.shapes;
    t.pixels += pixels;
}

std::string hex(std::uint32_t v) {
    char b[16];
    std::snprintf(b, sizeof b, "0x%02X", v);
    return b;
}

void pages_of(std::uint32_t bp, std::uint32_t bw, std::uint32_t psm, int x0, int y0, int x1, int y1, std::bitset<512> &out) {
    std::uint32_t pw = 64, ph = 32;
    switch (psm) {
    case 0x02: case 0x0A: case 0x32: case 0x3A: pw = 64, ph = 64; break;
    case 0x13: pw = 128, ph = 64; break;
    case 0x14: pw = 128, ph = 128; break;
    default: break;
    }
    const std::uint64_t per_row = std::max<std::uint32_t>(1, bw * 64 / pw);
    for (int py = std::max(y0, 0) / static_cast<int>(ph); py <= std::max(y1, 0) / static_cast<int>(ph); ++py)
        for (int px = std::max(x0, 0) / static_cast<int>(pw); px <= std::max(x1, 0) / static_cast<int>(pw); ++px)
            out.set(((static_cast<std::uint64_t>(bp) * 256 + (static_cast<std::uint64_t>(px) + static_cast<std::uint64_t>(py) * per_row) * 8192) >> 13) & 511u);
}

std::bitset<512> g_drawn;  // pages of memory drawn into since the capture began
std::map<std::string, Tally> g_targets;

void survey_batch(const Batch &b) {
    const std::uint32_t prims = static_cast<std::uint32_t>(b.prims.size() / kPrimWords);
    const std::uint32_t *s0 = b.setups.data();
    {
        char key[160];
        std::snprintf(key, sizeof key, "target fbp %u fbw %u fpsm %s zbp %u zpsm %s", s0[1], s0[2], hex(s0[3]).c_str(), s0[5], hex(s0[6]).c_str());
        double px = 0;
        for (std::uint32_t i = 0; i < prims; ++i) {
            const std::uint32_t *r = b.prims.data() + i * kPrimWords;
            const double w = (r[3] & 0xFFFFu) - (r[2] & 0xFFFFu) + 1.0, h = (r[3] >> 16) - (r[2] >> 16) + 1.0;
            px += (r[1] & 0xFFu) == 0 ? std::fabs(0.5 / f(r[10])) : w * h;
        }
        Tally &t = g_targets[key];
        ++t.shapes;  // batches
        t.pixels += px;
    }
    std::bitset<512> fbpages;
    pages_of(s0[1], s0[2], s0[3], b.minx, b.miny, b.maxx, b.maxy, fbpages);
    std::bitset<512> reads;
    for (std::uint32_t i = 0; i < prims; ++i) {
        const std::uint32_t *r = b.prims.data() + i * kPrimWords;
        const std::uint32_t *s = b.setups.data() + r[0] * kSetupWords;
        const std::uint32_t kind = r[1] & 3u, flags = s[0], modes = s[7];
        const double w = (r[3] & 0xFFFFu) - (r[2] & 0xFFFFu) + 1.0, h = (r[3] >> 16) - (r[2] >> 16) + 1.0;
        const double px = kind == 0 ? std::fabs(0.5 / f(r[10])) : w * h;
        g_total_pixels += px;
        ++g_total_shapes;
        static const char *const kinds[] = {"triangle", "sprite", "line", "point"};
        tally(std::string("shape: ") + kinds[kind], px);
        const bool tme = flags & 2u, abe = flags & 8u, ate = flags & 64u, zte = flags & 512u;
        // colour / texture
        if (!tme) tally("texture: none (colour only)", px);
        else {
            const std::uint32_t tw = s[15] & 0xFFu, th = s[15] >> 8;
            char key[200];
            std::snprintf(key, sizeof key, "texture: psm %s %ux%u%s cpsm %s tfx %u tcc %u %s wms %u wmt %u %s", hex(s[14]).c_str(), 1u << tw, 1u << th, "", (s[14] == 0x13 || s[14] == 0x14 || s[14] == 0x1B || s[14] == 0x24 || s[14] == 0x2C) ? hex(s[16]).c_str() : "-",
                          (modes >> 20) & 3u, (flags >> 13) & 1u, (flags & 16384u) ? "bilinear" : "nearest", (modes >> 24) & 3u, (modes >> 26) & 3u, (flags & 16u) ? "uv" : "stq");
            tally(key, px);
            // is the texture a picture drawn earlier in this frame?
            std::bitset<512> tp;
            pages_of(s[12], s[13], s[14], 0, 0, (1 << tw) - 1, (1 << th) - 1, tp);
            if ((tp & g_drawn).any()) {
                const bool own = (tp & fbpages).any();
                tally(own ? "texture: reads the picture this batch draws into (feedback)" : "texture: reads a picture drawn earlier in the frame (render to texture)", px);
            }
            if ((tp & fbpages).any() && (tp & g_drawn).none()) tally("texture: overlaps this batch's frame buffer but nothing was drawn there yet", px);
        }
        // blending
        if (abe) {
            const std::uint32_t a = (modes >> 12) & 3u, bb = (modes >> 14) & 3u, c = (modes >> 16) & 3u, d = (modes >> 18) & 3u;
            static const char *const cs[] = {"Cs", "Cd", "0", "?"}, *const cc[] = {"As", "Ad", "FIX", "?"};
            char key[160];
            std::snprintf(key, sizeof key, "blend: (%s-%s)*%s+%s%s%s", cs[a], cs[bb], cc[c], cs[d], c == 2 ? (s[9] == 128 ? " fix=128" : " fix!=128") : "", (flags & 1024u) ? " PABE" : "");
            tally(key, px);
        } else tally("blend: off", px);
        // alpha test
        if (ate) {
            static const char *const atst[] = {"NEVER", "ALWAYS", "LESS", "LEQUAL", "EQUAL", "GEQUAL", "GREATER", "NOTEQUAL"};
            static const char *const afail[] = {"KEEP", "FB_ONLY", "ZB_ONLY", "RGB_ONLY"};
            char key[100];
            std::snprintf(key, sizeof key, "alpha test: %s aref %u fail %s", atst[modes & 7u], s[8], afail[(modes >> 4) & 3u]);
            tally(key, px);
        } else tally("alpha test: off", px);
        if (flags & 128u) tally(std::string("dest alpha test (DATE) datm ") + ((flags & 256u) ? "1" : "0"), px);
        // depth
        if (zte) {
            static const char *const ztst[] = {"NEVER", "ALWAYS", "GEQUAL", "GREATER"};
            char key[100];
            std::snprintf(key, sizeof key, "depth: %s zpsm %s write %s", ztst[(modes >> 8) & 3u], hex(s[6]).c_str(), (flags & 32u) ? "no" : "yes");
            tally(key, px);
        } else tally("depth: test off", px);
        // frame
        char key[100];
        std::snprintf(key, sizeof key, "frame: fpsm %s fbmask %s%s%s%s", hex(s[3]).c_str(), s[4] ? "set 0x" : "none", "", "", "");
        std::string k2 = key;
        if (s[4]) {
            char m[16];
            std::snprintf(m, sizeof m, "%08X", s[4]);
            k2 += m;
        }
        tally(k2, px);
        if (flags & 4u) tally("fog: on", px);
        if (flags & 4096u) tally("frame alpha bit forced (FBA)", px);
        if (!(flags & 2048u)) tally("colour clamp: off (wraps)", px);
        if (flags & 1u) tally("shading: gouraud", px);
        else tally("shading: flat", px);
        if (((modes >> 28) & 3u) != 0) tally("scan mask: on", px);
        // scissor smaller than the box? skip
    }
    pages_of(s0[1], s0[2], s0[3], b.minx, b.miny, b.maxx, b.maxy, g_drawn);
}

}  // namespace

// The one switch of the experimental hardware drawing path: PS2_GPU_HW=1 turns it on, anything else (or nothing) leaves it off (see the top of this file).
bool enabled() {
    static const bool on = [] { return os::env("PS2_GPU_HW") == "1"; }();
    return on;
}

void capture_begin() {
    hwcache::replay_snapshot();
    g_capturing = true;
    g_batches.clear();
    g_pres.clear();
    g_tally.clear();
    g_targets.clear();
    g_drawn.reset();
    g_total_pixels = 0;
    g_total_shapes = 0;
}

bool capturing() { return g_capturing; }

void capture_batch(std::uint32_t submit, std::uint32_t command, const std::uint32_t *prims, std::uint32_t prim_count, const std::uint32_t *setups, std::uint32_t setup_count) {
    Batch &b = g_batches.emplace_back();
    b.submit = submit;
    b.command = command;
    b.prims.assign(prims, prims + static_cast<std::size_t>(prim_count) * kPrimWords);
    b.setups.assign(setups, setups + static_cast<std::size_t>(setup_count) * kSetupWords);
    for (std::uint32_t i = 0; i < prim_count; ++i) {
        const std::uint32_t *r = prims + i * kPrimWords;
        b.minx = std::min(b.minx, static_cast<int>(r[2] & 0xFFFFu));
        b.miny = std::min(b.miny, static_cast<int>(r[2] >> 16));
        b.maxx = std::max(b.maxx, static_cast<int>(r[3] & 0xFFFFu));
        b.maxy = std::max(b.maxy, static_cast<int>(r[3] >> 16));
    }
    survey_batch(b);
}

void capture_pre(std::uint32_t submit, std::uint32_t command, bool clut, std::uint64_t lo, std::uint64_t hi) { g_pres.push_back({submit, command, clut, lo, hi}); }

void survey_print() {
    std::fprintf(stderr, "[hw-survey] %zu batches, %llu shapes, %.0f pixels covered (before the depth test)\n", g_batches.size(), static_cast<unsigned long long>(g_total_shapes), g_total_pixels);
    for (const auto &[key, t] : g_targets) std::fprintf(stderr, "[hw-survey] %-70s batches %4llu pixels %10.0f (%4.1f%%)\n", key.c_str(), static_cast<unsigned long long>(t.shapes), t.pixels, 100.0 * t.pixels / std::max(1.0, g_total_pixels));
    for (const auto &[key, t] : g_tally)
        std::fprintf(stderr, "[hw-survey] %-110s shapes %6llu (%5.1f%%) pixels %10.0f (%5.1f%%)\n", key.c_str(), static_cast<unsigned long long>(t.shapes), 100.0 * static_cast<double>(t.shapes) / static_cast<double>(std::max<std::uint64_t>(1, g_total_shapes)), t.pixels,
                     100.0 * t.pixels / std::max(1.0, g_total_pixels));
}

// ---------------------------------------------------------------- the trial: plan, then replay

namespace {

constexpr std::uint32_t kHwSetup = 48;  // a setup (40 words) plus 8 of the trial's: [40] bit 0 = the output alpha is the blend factor
constexpr std::uint32_t kDecodedWords = 4u << 20;
constexpr std::uint32_t kCacheWords = 16u << 20;  // PS2_GPU_HW_CACHE: 64 MB, a fixed place for every texture of the frame

WGPUStringView sv(const char *s) { return WGPUStringView{s, WGPU_STRLEN}; }

struct Vtx {
    float x, y, z, r, g, b, a, s, t, q, fog;
    std::uint32_t si;
};
static_assert(sizeof(Vtx) == 48);

// How one setup (one state of the chip's registers) is drawn by the card.
enum Factor { kZero, kOne, kSrcA, kInvSrcA, kConst, kInvConst };
struct Look {
    bool ok = false, skip = false, two_pass = false, alpha_out = false, zwrite = false, alpha_two = false;
    const char *why = "";
    std::uint32_t src = kOne, dst = kZero, op = 0;  // blend: factors and operation (0 add, 1 subtract, 2 reverse subtract)
    bool blend = false;
    std::uint32_t premul = 0;  // 1 the shader outputs floor(Cs * C) itself and the card only adds it, 2 the same with the next integer up (the subtract form)
    bool fix = false;          // the chip's factor C is FIX (setup word 9) and not the shape's alpha
    std::uint32_t ztest = 0;  // 0 always, 1 greater or equal, 2 greater
    std::uint32_t mask = 15;  // colour write mask: 1 red, 2 green, 4 blue, 8 alpha
    float constant = 0;
    bool operator==(const Look &o) const {
        return ok == o.ok && skip == o.skip && two_pass == o.two_pass && alpha_out == o.alpha_out && alpha_two == o.alpha_two && zwrite == o.zwrite && src == o.src && dst == o.dst && op == o.op && blend == o.blend && premul == o.premul && fix == o.fix && ztest == o.ztest && mask == o.mask && constant == o.constant;
    }
};

// The chip's blend equation is ((A - B) * C >> 7) + D with A, B, D one of Cs (shape colour), Cd (colour already there), 0 and C one of As, Ad, FIX
// (0x80 counts as 1.0). The card's is src * srcFactor (op) dst * dstFactor. These are the cases that reduce to it directly (worked out on paper
// from the two equations): with C = c, (A,B,D) =
//   (Cs,0,Cd) = Cs*c + Cd              -> src c, dst 1, add
//   (Cs,Cd,Cd) = Cs*c + Cd*(1-c)       -> src c, dst 1-c, add
//   (Cd,Cs,Cs) = Cd*c + Cs*(1-c)       -> src 1-c, dst c, add
//   (0,Cs,Cd) = Cd - Cs*c              -> src c, dst 1, reverse subtract
//   (Cs,Cd,0) = Cs*c - Cd*c            -> src c, dst c, subtract
//   (Cs,0,0) = Cs*c                    -> src c, dst 0, add
// and the ones that do not change anything (A = B). Everything else, and C = Ad (the card's alpha factor would be a/255, the chip's a/128), is left out.
bool blend_of(const std::uint32_t *s, Look &l) {
    const std::uint32_t modes = s[7], a = (modes >> 12) & 3u, b = (modes >> 14) & 3u, c = (modes >> 16) & 3u, d = (modes >> 18) & 3u;
    if (c == 1) {
        l.why = "blend factor is destination alpha";
        return false;
    }
    if (c == 3 || a == 3 || b == 3 || d == 3) {
        l.why = "blend reserved value";
        return false;
    }
    std::uint32_t cf = kSrcA, ci = kInvSrcA;
    if (c == 2) {
        if (s[9] == 128) cf = kOne, ci = kZero;
        else cf = kConst, ci = kInvConst, l.constant = std::min(1.0f, static_cast<float>(s[9]) / 128.0f);
    }
    l.alpha_out = c == 0;
    l.fix = c == 2;
    l.blend = true;
    if (a == b) {  // (A - A) * c + D = D
        if (d == 0) l.blend = false;  // Cs: plain write
        else if (d == 1) l.src = kZero, l.dst = kOne;  // Cd: nothing changes
        else l.src = kZero, l.dst = kZero;
        return true;
    }
    // Rounding: the card's source and factor are 8-bit, its result is rounded to nearest, the chip's is cut down. The three forms in which the
    // source is multiplied alone (nothing of the target is multiplied) are done exactly: the shader computes the whole product as an integer and the card adds it.
    const bool exact = g_round > 0.0f;  // PS2_GPU_HW_ROUND=0 is the card's own rounding, as before this lane
    if (a == 0 && b == 2 && d == 1) exact ? (void)(l.src = kOne, l.dst = kOne, l.premul = 1) : (void)(l.src = cf, l.dst = kOne);
    else if (a == 0 && b == 1 && d == 1) l.src = cf, l.dst = ci;
    else if (a == 1 && b == 0 && d == 0) l.src = ci, l.dst = cf;
    else if (a == 2 && b == 0 && d == 1) exact ? (void)(l.src = kOne, l.dst = kOne, l.op = 2, l.premul = 2) : (void)(l.src = cf, l.dst = kOne, l.op = 2);
    else if (a == 0 && b == 1 && d == 2) l.src = cf, l.dst = cf, l.op = 1;
    else if (a == 0 && b == 2 && d == 2) exact ? (void)(l.src = kOne, l.dst = kZero, l.premul = 1) : (void)(l.src = cf, l.dst = kZero);
    else {
        l.why = "blend equation";
        return false;
    }
    if (l.src == kOne && l.dst == kZero && l.op == 0) l.blend = false;
    return true;
}

Look classify(const std::uint32_t *s, bool decodable) {
    Look l;
    const std::uint32_t flags = s[0], modes = s[7], fpsm = s[3];
    if (flags & 128u) { l.why = "destination alpha test"; return l; }
    if (flags & 1024u) { l.why = "per-pixel alpha blend"; return l; }
    if ((modes >> 28) & 3u) { l.why = "scan mask"; return l; }
    if (fpsm != 0 && fpsm != 1 && fpsm != 2 && fpsm != 0x0A) { l.why = "frame format"; return l; }
    const bool zte = flags & 512u;
    if (zte && s[6] != 0x31) { l.why = "depth format"; return l; }
    if ((flags & 2u) && !decodable) { l.why = "texture window or size"; return l; }
    l.mask = 0;
    if (fpsm <= 1) {
        for (int i = 0; i < 4; ++i) {
            const std::uint32_t m = (s[4] >> (8 * i)) & 0xFFu;
            if (m == 0) l.mask |= 1u << i;
            else if (m != 0xFF) { l.why = "partial frame mask"; return l; }
        }
        if (fpsm == 1) l.mask &= 7u;
    } else {
        if (s[4] & 0xFFFFu) { l.why = "partial frame mask"; return l; }
        l.mask = 15;
    }
    bool zwrite = zte && !(flags & 32u);
    bool keep_alpha = false;  // a failed alpha test leaves the old alpha in place (failure mode 3: colour written, alpha and depth not)
    if (flags & 64u) {  // alpha test
        const std::uint32_t atst = modes & 7u, afail = (modes >> 4) & 3u;
        if (atst == 0) {  // always fails
            if (afail == 0) { l.ok = l.skip = true; return l; }
            if (afail == 1 || afail == 3) zwrite = false;
            if (afail == 3) l.mask &= 7u;
            if (afail == 2) l.mask = 0;
        } else if (atst != 1) {  // decided per pixel
            if (afail == 2) { l.why = "alpha test, depth only on failure"; return l; }
            if ((afail == 1 || afail == 3) && zwrite) l.two_pass = true;  // colour always, depth only where it passed: two draws
            keep_alpha = afail == 3;
        }
    }
    if (zte) {
        const std::uint32_t ztst = (modes >> 8) & 3u;
        if (ztst == 0) { l.ok = l.skip = true; return l; }
        l.ztest = ztst == 1 ? 0 : ztst == 2 ? 1 : 2;
    }
    l.zwrite = zwrite;
    if (flags & 8u) {
        if (!(flags & 2048u) && l.mask != 0) {  // the card's blend saturates; with the clamp off the chip wraps around (the game adds to one channel that way)
            l.why = "colour wrap-around (clamp off) with blending";
            return l;
        }
        if (!blend_of(s, l)) return l;
        if (!l.blend) l.alpha_out = false;  // a plain write (A = B = Cs...) uses no factor: the stored alpha is the raw one
    }
    if (((l.alpha_out && l.blend) || keep_alpha) && (l.mask & 8u)) {  // the blend uses the shape alpha as its factor (a/128 here, the stored alpha is a/255), or a failed test keeps the old alpha: dual-source blending or a draw of its own writes the alpha channel
        l.alpha_two = true;
        l.mask &= 7u;
    }
    l.ok = true;
    return l;
}

// PS2_GPU_HW_ONLY=<class numbers>: only these classes of hardware shapes are drawn (see left_out); empty = all.
bool only_class(int c) {
    static const std::string only = os::env("PS2_GPU_HW_ONLY");
    return only.empty() || ("," + only + ",").find("," + std::to_string(c) + ",") != std::string::npos;
}

struct Rect {
    int x0 = 1 << 30, y0 = 1 << 30, x1 = -1, y1 = -1;
    bool empty() const { return x1 < x0 || y1 < y0; }
    void add(int ax0, int ay0, int ax1, int ay1) {
        x0 = std::min(x0, ax0), y0 = std::min(y0, ay0), x1 = std::max(x1, ax1), y1 = std::max(y1, ay1);
    }
};

std::bitset<512> pages_bytes(std::uint64_t lo, std::uint64_t hi) {
    std::bitset<512> p;
    for (std::uint64_t a = lo & ~8191ull; a < hi; a += 8192) p.set((a >> 13) & 511u);
    return p;
}

struct Target {
    std::uint32_t fbp, fbw, fpsm, zbp, zpsm, setup_word = 0;
    bool has_setup = false;
    int w = 0, h = 0;
    Rect box, dirty;
    std::bitset<512> pages;
    WGPUTexture color = nullptr, depth = nullptr;
    WGPUTextureView color_view = nullptr, depth_view = nullptr;
    WGPUBuffer view_buf = nullptr;
    WGPUBindGroup render_group = nullptr, load_group = nullptr, store_group = nullptr;
    bool resident = false, is_dirty = false, depth_cleared = false, z_dirty = false;
    Rect zdirty;
    std::bitset<512> zpages;
    // The model of the card's pictures: `dirty` is the DRAWN rectangle (the texture is newer than memory there;
    // `is_dirty` = it is not empty), `valid` is the part of the texture that holds real data (the whole capture box once loaded, or once a full-target
    // constant sprite has cleared it), `resident` = the texture is current with memory outside the drawn rectangle (a writer of memory clears it: the dirty
    // list is the whole box, reloaded at the next use). `seq`/`zseq` number the last colour and depth draw, so the end-of-frame write-back can
    // keep the order in which the chip wrote memory where a colour and a depth target share pages.
    Rect valid;
    std::uint32_t seq = 0, zseq = 0;
    bool shadow_needed = false;  // some draw reads a copy of this target (the draw also writes it)
    WGPUTexture shadow = nullptr;
    WGPUTextureView shadow_view = nullptr;
    WGPUBindGroup store_z_group = nullptr;
};

struct CheckJob;
struct Run {  // consecutive shapes drawn with the same state
    std::uint32_t first_vertex = 0, vertices = 0, target = 0, shapes = 0;
    Look look;
    std::uint32_t scissor[4] = {};  // x, y, width, height
    std::uint32_t raw_scissor[2] = {};  // the setup's scissor words 10 and 11 (what makes two shapes one run, the same for batch() and for the checks started in draw())
    std::vector<std::uint8_t> counts;  // vertices of each shape (3 or 6)
    std::vector<std::uint32_t> setups_of;  // the setup (within its batch) of each shape, so a run can be split by the texture source
    // the two-pass alpha test draws every shape's colour first and every shape's depth after, which is the chip's order only when no shape's
    // depth result depends on an earlier shape of the run. serial = id of the run's pixel stamps; flat = all shapes at one depth so far; overlap = two shapes touched one pixel.
    std::uint32_t serial = 0;
    bool flat = true, overlap = false, exact_order = false;
    std::vector<std::uint32_t> cuts;  // shapes (index in the run) that overlap an earlier shape of their segment: a new segment starts there
    float flat_z = 0;
    std::uint32_t src = ~0u;  // the target whose texture the shapes of this run sample directly, or none
    std::shared_ptr<CheckJob> job;  // the open chunk of this run's independence check (the shapes stream into it)
    std::vector<std::shared_ptr<CheckJob>> done_jobs;  // the closed chunks, possibly still running on helper threads; `cuts` is filled when they are collected
    std::vector<std::uint32_t> job_first;  // the run's shape index of the first shape of each chunk (done_jobs, then job)
};

enum OpKind { kCompute, kLoad, kStore, kStoreZ, kDecode, kDraw, kCopy };
struct Op {
    OpKind kind;
    std::uint32_t sub = 0, cmd = 0, target = 0, param = 0, run_first = 0, run_count = 0, decodes = 0;
    bool clear_depth = false;
    std::uint32_t x = 0, y = 0;  // dispatch size for load, store
};

struct Plan {
    std::vector<Vtx> vertices;
    std::vector<std::uint32_t> setups;  // kHwSetup words each
    std::vector<std::array<std::uint32_t, 64>> params;
    std::vector<Target> targets;
    std::vector<Run> runs;
    std::vector<Op> ops;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> decode_ops;  // per kDecode op: param index of the first, count
    std::uint64_t shapes = 0, shapes_ok = 0;
    double pixels = 0, pixels_ok = 0;
    std::map<std::string, Tally> left_out;
    // why render textures were written back to graphics memory (stores, 5 = depth stores) and how many were loaded, with the pixels each moved
    std::array<unsigned, 8> stores_by{};
    std::array<double, 8> store_px_by{};
    unsigned loads = 0, clears = 0, direct_decs = 0, direct_runs = 0, early_stores = 0, overshoot_decs = 0, copies = 0, self_decs = 0;
    std::uint64_t direct_shapes = 0;
    unsigned two_runs = 0, exact_runs = 0, segments = 0;  // runs of the two-pass alpha test, and the ones that are not independent
    std::uint64_t exact_shapes = 0, indep_shapes = 0;
    double load_px = 0;
} g_plan;

thread_local std::vector<std::uint32_t> g_stampw;  // pixel (y * 2048 + x) -> 2 * serial of the last run segment that touched it + 1 when its highest depth came from a sloped triangle (packed, so a vector holds eight pixels)
thread_local std::uint32_t g_run_serial = 0;
bool g_indep = true, g_indep_exact = false;  // PS2_GPU_HW_INDEP=0 turns the check off (the trial's order for every run)

// stamp the pixels of one shape of a run that draws a shape's pixels in several passes (the same pixel rule as the painter's draw(): bounding box, then the edge
// functions with the inclusive bits of rec[27]). Returns false when a pixel already carries the run's stamp AND the later shape would be decided differently from the
// chip's order there. The colour passes of a segment all see the depth from before the segment, the chip's later shape sees what the earlier shapes wrote; the two agree at a
// pixel when the later shape is not behind the earlier ones (GEQUAL: not lower than the highest depth stamped there; GREATER: higher; ALWAYS: no depth test, never a
// conflict), whether or not the earlier shape passed its alpha test (that is not known here, so it is assumed to pass). `exact_z` = PS2_GPU_HW_INDEP=3 stamps overlap without
// looking at depth (the cut of the first version of this lane).
thread_local std::vector<float> g_stampz;           // the highest depth stamped at the pixel in the segment
bool g_indep_depth = true;
bool g_stamp_scalar = false;  // PS2_GPU_HW_STAMP=scalar: the live path's check walks pixel by pixel (the reference of the vector version)
thread_local std::uint64_t g_ov_pixels = 0, g_tie_pixels = 0, g_behind_pixels = 0;  // pixels a shape of a segment covers that an earlier shape of the segment covered, of those with the same depth, and with a lower depth than the earlier one

// what the independence check reads of a shape (the painter's record, 28 words, cut down to 16), so that a helper thread can own it.
struct Stamp {
    int minx, miny, maxx, maxy;
    float x0, y0, x1, y1, x2, y2, inv, z0, dz1, dz2;
    std::uint32_t incl, kind;
};
Stamp stamp_of(const std::uint32_t *r) {
    Stamp t;
    t.minx = static_cast<int>(r[2] & 0xFFFFu), t.miny = static_cast<int>(r[2] >> 16), t.maxx = static_cast<int>(r[3] & 0xFFFFu), t.maxy = static_cast<int>(r[3] >> 16);
    t.kind = r[1] & 3u;
    t.x0 = f(r[4]), t.y0 = f(r[5]), t.x1 = f(r[6]), t.y1 = f(r[7]), t.x2 = f(r[8]), t.y2 = f(r[9]), t.inv = f(r[10]);
    t.incl = r[27];
    t.z0 = static_cast<float>(r[24]);
    t.dz1 = t.kind == 0 ? f(r[25]) : 0.0f, t.dz2 = t.kind == 0 ? f(r[26]) : 0.0f;
    return t;
}

bool stamp_scalar(const Stamp &t, std::uint32_t zt, std::uint32_t serial) {
    if (t.minx > t.maxx || t.miny > t.maxy || t.maxx >= 2048 || t.maxy >= 2048) return true;
    const float x0 = t.x0, y0 = t.y0, x1 = t.x1, y1 = t.y1, x2 = t.x2, y2 = t.y2, inv = t.inv, z0 = t.z0, dz1 = t.dz1, dz2 = t.dz2;
    const std::uint32_t incl = t.incl;
    const int kind = static_cast<int>(t.kind);
    const bool sloped = kind == 0 && (dz1 != 0.0f || dz2 != 0.0f);
    for (int y = t.miny; y <= t.maxy; ++y)
        for (int x = t.minx; x <= t.maxx; ++x) {
            float zp = z0;
            if (kind == 0) {
                const float px = static_cast<float>(x), py = static_cast<float>(y);
                const float w0 = ((x1 - px) * (y2 - py) - (x2 - px) * (y1 - py)) * inv;
                const float w1 = ((x2 - px) * (y0 - py) - (x0 - px) * (y2 - py)) * inv;
                const float w2 = ((x0 - px) * (y1 - py) - (x1 - px) * (y0 - py)) * inv;
                if (!((w0 > 0.0f || (w0 == 0.0f && (incl & 1u))) && (w1 > 0.0f || (w1 == 0.0f && (incl & 2u))) && (w2 > 0.0f || (w2 == 0.0f && (incl & 4u))))) continue;
                zp = z0 + w1 * dz1 + w2 * dz2;
            }
            const std::size_t at = static_cast<std::size_t>(y) * 2048u + static_cast<std::size_t>(x);
            std::uint32_t &st = g_stampw[at];
            if ((st >> 1) == serial) {
                if (!g_indep_depth) return false;
                ++g_ov_pixels;
                if (zp == g_stampz[at]) ++g_tie_pixels;
                else if (zp < g_stampz[at]) ++g_behind_pixels;
                const float margin = (sloped || (st & 1u)) ? 1.0f : 0.0f;
                if (zt != 0 && (zt == 1 ? zp + margin < g_stampz[at] : zp - margin <= g_stampz[at])) return false;
                if (zp >= g_stampz[at]) g_stampz[at] = zp, st = serial * 2u + (sloped ? 1u : 0u);
                else st |= sloped ? 1u : 0u;
            } else {
                st = serial * 2u + (sloped ? 1u : 0u);
                g_stampz[at] = zp;
            }
        }
    return true;
}

bool stamp_shape(const Run &run, const std::uint32_t *r, int) { return stamp_scalar(stamp_of(r), run.look.ztest, run.serial); }

#if defined(__AVX2__)
// stamp_shape with eight pixels of a row at a time (AVX2). The same float operations in the same order per pixel (the build has no fused multiply-add), the same
// decisions; it returns false when any pixel of the shape conflicts (the scalar walk leaves at the first such pixel, and the stamps it already wrote belong to the dead segment
// either way, because the caller starts a new one). The counters of the trial (g_ov_pixels...) are not kept here.
bool stamp_avx2(const Stamp &t, std::uint32_t zt, std::uint32_t serial) {
    const int minx = t.minx, miny = t.miny, maxx = t.maxx, maxy = t.maxy;
    if (minx > maxx || miny > maxy || maxx >= 2048 || maxy >= 2048) return true;
    const float x0 = t.x0, y0 = t.y0, x1 = t.x1, y1 = t.y1, x2 = t.x2, y2 = t.y2, inv = t.inv, z0 = t.z0, dz1 = t.dz1, dz2 = t.dz2;
    const std::uint32_t incl = t.incl;
    const int kind = static_cast<int>(t.kind);
    const bool sloped = kind == 0 && (dz1 != 0.0f || dz2 != 0.0f);
    const __m256 iota = _mm256_setr_ps(0.0f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f);
    const __m256i iota_i = _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7);
    const __m256 vx0 = _mm256_set1_ps(x0), vx1 = _mm256_set1_ps(x1), vx2 = _mm256_set1_ps(x2), vinv = _mm256_set1_ps(inv), vz0 = _mm256_set1_ps(z0), vdz1 = _mm256_set1_ps(dz1), vdz2 = _mm256_set1_ps(dz2);
    const __m256 zero = _mm256_setzero_ps(), one = _mm256_set1_ps(1.0f);
    const __m256 m0 = _mm256_castsi256_ps(_mm256_set1_epi32((incl & 1u) ? -1 : 0)), m1 = _mm256_castsi256_ps(_mm256_set1_epi32((incl & 2u) ? -1 : 0)), m2 = _mm256_castsi256_ps(_mm256_set1_epi32((incl & 4u) ? -1 : 0));
    const __m256i vserial = _mm256_set1_epi32(static_cast<int>(serial)), vserial2 = _mm256_set1_epi32(static_cast<int>(serial * 2u)), ones_i = _mm256_set1_epi32(1);
    const __m256 vsloped = _mm256_castsi256_ps(_mm256_set1_epi32(sloped ? -1 : 0));
    for (int y = miny; y <= maxy; ++y) {
        const float py = static_cast<float>(y);
        const __m256 ay0 = _mm256_set1_ps(y0 - py), ay1 = _mm256_set1_ps(y1 - py), ay2 = _mm256_set1_ps(y2 - py);
        std::uint32_t *const wrow = g_stampw.data() + static_cast<std::size_t>(y) * 2048u;
        float *const zrow = g_stampz.data() + static_cast<std::size_t>(y) * 2048u;
        for (int x = minx; x <= maxx; x += 8) {
            const __m256i inside = _mm256_cmpgt_epi32(_mm256_set1_epi32(maxx - x + 1), iota_i);
            __m256 cover = _mm256_castsi256_ps(inside), zp = vz0;
            if (kind == 0) {
                const __m256 px = _mm256_add_ps(_mm256_set1_ps(static_cast<float>(x)), iota);
                const __m256 dx0 = _mm256_sub_ps(vx0, px), dx1 = _mm256_sub_ps(vx1, px), dx2 = _mm256_sub_ps(vx2, px);
                const __m256 w0 = _mm256_mul_ps(_mm256_sub_ps(_mm256_mul_ps(dx1, ay2), _mm256_mul_ps(dx2, ay1)), vinv);
                const __m256 w1 = _mm256_mul_ps(_mm256_sub_ps(_mm256_mul_ps(dx2, ay0), _mm256_mul_ps(dx0, ay2)), vinv);
                const __m256 w2 = _mm256_mul_ps(_mm256_sub_ps(_mm256_mul_ps(dx0, ay1), _mm256_mul_ps(dx1, ay0)), vinv);
                const __m256 c0 = _mm256_or_ps(_mm256_cmp_ps(w0, zero, _CMP_GT_OQ), _mm256_and_ps(_mm256_cmp_ps(w0, zero, _CMP_EQ_OQ), m0));
                const __m256 c1 = _mm256_or_ps(_mm256_cmp_ps(w1, zero, _CMP_GT_OQ), _mm256_and_ps(_mm256_cmp_ps(w1, zero, _CMP_EQ_OQ), m1));
                const __m256 c2 = _mm256_or_ps(_mm256_cmp_ps(w2, zero, _CMP_GT_OQ), _mm256_and_ps(_mm256_cmp_ps(w2, zero, _CMP_EQ_OQ), m2));
                cover = _mm256_and_ps(cover, _mm256_and_ps(c0, _mm256_and_ps(c1, c2)));
                if (_mm256_movemask_ps(cover) == 0) continue;
                zp = _mm256_add_ps(_mm256_add_ps(vz0, _mm256_mul_ps(w1, vdz1)), _mm256_mul_ps(w2, vdz2));
            }
            const __m256i word = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(wrow + x));
            const __m256 sz = _mm256_loadu_ps(zrow + x);
            const __m256i hit_i = _mm256_and_si256(_mm256_cmpeq_epi32(_mm256_srli_epi32(word, 1), vserial), _mm256_castps_si256(cover));
            const __m256 hit = _mm256_castsi256_ps(hit_i);
            __m256 newz = zp;
            __m256i newg = _mm256_castps_si256(_mm256_and_ps(vsloped, _mm256_castsi256_ps(ones_i)));  // 1 where the new depth is sloped
            if (_mm256_movemask_ps(hit) != 0) {
                const __m256 gold = _mm256_castsi256_ps(_mm256_cmpeq_epi32(_mm256_and_si256(word, ones_i), ones_i));
                if (zt != 0) {
                    const __m256 margin = _mm256_and_ps(_mm256_or_ps(vsloped, gold), one);
                    const __m256 conflict = zt == 1 ? _mm256_cmp_ps(_mm256_add_ps(zp, margin), sz, _CMP_LT_OQ) : _mm256_cmp_ps(_mm256_sub_ps(zp, margin), sz, _CMP_LE_OQ);
                    if (_mm256_movemask_ps(_mm256_and_ps(conflict, hit)) != 0) return false;
                }
                const __m256 ge = _mm256_cmp_ps(zp, sz, _CMP_GE_OQ);
                const __m256 keep_old = _mm256_andnot_ps(ge, hit);  // the earlier depth stays the highest
                newz = _mm256_blendv_ps(zp, sz, keep_old);
                const __m256 g_old = _mm256_and_ps(keep_old, _mm256_and_ps(gold, _mm256_castsi256_ps(ones_i)));
                newg = _mm256_castps_si256(_mm256_or_ps(_mm256_castsi256_ps(newg), g_old));
            }
            const __m256i newword = _mm256_add_epi32(vserial2, newg);
            _mm256_storeu_ps(zrow + x, _mm256_blendv_ps(sz, newz, cover));
            _mm256_storeu_si256(reinterpret_cast<__m256i *>(wrow + x), _mm256_blendv_epi8(word, newword, _mm256_castps_si256(cover)));
        }
    }
    return true;
}
#endif

void note_shape(Run &run, const std::uint32_t *r, int kind) {
    if (g_stampw.empty()) g_stampw.assign(2048u * 2048u + 16u, 0), g_stampz.assign(2048u * 2048u + 16u, 0.0f);
    const float z = static_cast<float>(r[24]);
    if (run.shapes == 1) run.flat_z = z;  // the shape is already counted
    if (z != run.flat_z || (kind == 0 && (f(r[25]) != 0.0f || f(r[26]) != 0.0f))) run.flat = false;
    if (!stamp_shape(run, r, kind)) {  // conflicts with an earlier shape of the segment: cut before this shape, stamp it as the first of the next segment
        run.overlap = true;
        run.cuts.push_back(run.shapes - 1);
        run.serial = ++g_run_serial;
        stamp_shape(run, r, kind);
    }
}

// Where the processor time of the live path goes (PS2_GPU_HW_TIME=1; nanoseconds, the nested ones are inside the batch)
bool g_time = false;
constexpr int kTimes = 14;
std::uint64_t g_tns[kTimes] = {};
std::uint64_t g_cut_why[8] = {};  // runs ended inside a batch: all, scissor, ztest, zwrite, alpha-test split, blend, mask
std::uint64_t g_wgpu_submits[2] = {};  // submits without and with card work (PS2_GPU_HW_TIME=1)
const char *const g_tname[kTimes] = {"batch (all)", "independence check", "reader", "writer", "painter batch", "prepare", "encode", "submitted", "display", "batch write-backs and loads",
                                     "wgpu finish, card submits", "wgpu submit, card submits", "wgpu finish, other submits", "wgpu submit, other submits"};
struct Tm {
    int k;
    std::chrono::steady_clock::time_point t0;
    explicit Tm(int kk) : k(kk) {
        if (g_time) t0 = std::chrono::steady_clock::now();
    }
    void stop() {
        if (g_time && k >= 0) g_tns[k] += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
        k = -1;
    }
    ~Tm() { stop(); }
};

// The exact check above costs about 0.5 microseconds per shape (ship: 27 ms of the processor per frame, the painter's whole frame is 18 ms), and
// a grid of cells instead of pixels cuts 92 % of the shapes (tried): neighbours of one mesh share cells and their depths overlap there. So the live path pays for the exact check only
// where it is cheap, and a run that would need it and is longer than g_indep_max shapes goes, with its whole batch, to the painter, which is exact by itself ("when the test fails, the draw
// goes to the painter"). Runs that need no check (the test "always", flat depth with "greater or equal", no alpha-test split) are not affected. PS2_GPU_HW_INDEP_MAX=<shapes> (default below; "all" = the
// part 1 behaviour, check every run).
std::uint32_t g_indep_max = 0;
std::uint64_t g_indep_pix = 0;  // PS2_GPU_HW_INDEP_PIXELS: a run that needs the check and has at most this many pixels in the boxes of its shapes stays on the card
// Switching from the painter to the card costs a load of the picture (and later a write-back of it): a small batch that would switch is not worth it and goes to the painter. A batch that follows
// another card batch stays on the card. PS2_GPU_HW_SWITCH_MIN=<shapes>: smaller batches never start a card stretch (0: every batch may).
std::uint32_t g_switch_min = 256;
bool g_last_card = false;
std::uint64_t g_small_switch = 0, g_small_dirty = 0;
std::uint64_t g_cut_hash = 1469598103934665603ull;
std::uint64_t g_heavy_batches = 0, g_heavy_shapes = 0, g_checked_runs = 0, g_checked_shapes = 0, g_checked_cuts = 0;

// the independence check of one run is a job: the shapes of the run (cut down to what the check reads), the depth test, and the answer, the shapes that start a new segment.
// It runs when the run closes, on the window thread, or on a helper (PS2_HW_CHECK_THREADS, default 2 with a window or PS2_THREADS=1, 0 with PS2_ONE_THREAD=1); the draw is encoded at the end of
// the frame, which waits for it (collect). The answer is the same wherever it runs.
struct CheckJob {
    std::unique_ptr<Stamp[]> shapes;  // filled by the window thread while the worker reads the first `count` of them (the run streams in as its batch is walked)
    std::uint32_t capacity = 0, ztest = 0;
    std::atomic<std::uint32_t> count{0};
    std::atomic<bool> closed{false};  // no more shapes will come
    std::atomic<bool> done{false};
    std::vector<std::uint32_t> cuts;  // written by the worker, read after `done`
};

void check_run(CheckJob &j) {
    if (g_stampw.empty()) g_stampw.assign(2048u * 2048u + 16u, 0), g_stampz.assign(2048u * 2048u + 16u, 0.0f);
    if (g_run_serial > (1u << 29)) std::fill(g_stampw.begin(), g_stampw.end(), 0u), g_run_serial = 0;  // the packed serial has 31 bits
    std::uint32_t serial = ++g_run_serial;
#if defined(__AVX2__)
    const bool wide = g_indep_depth && !g_stamp_scalar;
#else
    const bool wide = false;
#endif
    std::uint32_t k = 0, idle = 0;
    for (;;) {
        const std::uint32_t avail = j.count.load(std::memory_order_acquire);
        if (k == avail) {
            if (j.closed.load(std::memory_order_acquire)) {
                if (k == j.count.load(std::memory_order_acquire)) break;
                continue;
            }
            if (++idle < 64) std::this_thread::yield();
            else std::this_thread::sleep_for(std::chrono::microseconds(30));
            continue;
        }
        idle = 0;
        for (; k < avail; ++k) {
            const Stamp &t = j.shapes[k];
            const auto stamp = [&](std::uint32_t sr) {
#if defined(__AVX2__)
                if (wide) return stamp_avx2(t, j.ztest, sr);
#endif
                return stamp_scalar(t, j.ztest, sr);
            };
            if (!stamp(serial)) {  // conflicts with an earlier shape of the segment: cut before this shape, stamp it as the first of the next segment
                j.cuts.push_back(k);
                serial = ++g_run_serial;
                stamp(serial);
            }
        }
    }
    j.done.store(true, std::memory_order_release);
}

struct CheckPool {
    std::mutex m;
    std::condition_variable cv;
    std::deque<std::shared_ptr<CheckJob>> q;
    std::vector<std::thread> workers;
    void start(unsigned n) {
        for (unsigned i = 0; i < n; ++i)
            workers.emplace_back([this] {
                for (;;) {
                    std::shared_ptr<CheckJob> j;
                    {
                        std::unique_lock<std::mutex> lk(m);
                        cv.wait(lk, [&] { return !q.empty(); });
                        j = std::move(q.front());
                        q.pop_front();
                    }
                    check_run(*j);
                }
            }), workers.back().detach();
    }
    void push(std::shared_ptr<CheckJob> j) {
        {
            std::lock_guard<std::mutex> lk(m);
            q.push_back(std::move(j));
        }
        cv.notify_one();
    }
};
CheckPool *g_pool = nullptr;  // never destroyed: the process ends with _Exit

// The check of a run is cut into chunks of g_check_chunk shapes (0: the whole run is one chunk, the cuts of the first step). Every chunk starts a new segment, so chunks need nothing from each other and
// run on different helpers at once: an extra cut can only make segments shorter, and a shorter segment is as exact as a longer one (a cut is never wrong, only a missing one is). A chunk starts at the first
// shape of the run that cannot be proven independent with the shapes seen so far, and goes to a helper when it is full or the run ends (the helper never waits for the window thread).
std::uint32_t g_check_chunk = 1024;
bool g_merge_passes = true;  // PS2_GPU_HW_MERGE=0: one render pass per card batch

// The check of the open run starts (at its first shape that cannot be proven independent) with the shapes seen so far; the others are added with add_check as they come.
void start_chunk(Run &run, std::vector<Stamp> &buffered, std::uint32_t room, std::uint32_t first) {
    auto j = std::make_shared<CheckJob>();
    const std::uint32_t chunk_room = g_check_chunk ? std::min(room, g_check_chunk) : room;
    j->capacity = static_cast<std::uint32_t>(buffered.size()) + chunk_room;
    j->shapes.reset(new Stamp[j->capacity]);
    std::memcpy(j->shapes.get(), buffered.data(), buffered.size() * sizeof(Stamp));
    j->count.store(static_cast<std::uint32_t>(buffered.size()), std::memory_order_release);
    j->ztest = run.look.ztest;
    buffered.clear();
    run.job = j;
    run.job_first.push_back(first);
}
void close_chunk(Run &run) {
    if (!run.job) return;
    run.job->closed.store(true, std::memory_order_release);
    if (g_pool) g_pool->push(run.job);
    else check_run(*run.job);  // no helper: the walk happens here
    run.done_jobs.push_back(std::move(run.job));
    run.job.reset();
}
void start_check(Run &run, std::vector<Stamp> &buffered, std::uint32_t room) { start_chunk(run, buffered, room, run.shapes - static_cast<std::uint32_t>(buffered.size())); }
// `room`: the shapes the batch still has after this one (the next chunk is not larger)
void add_check(Run &run, const Stamp &t, std::uint32_t room) {
    if (run.job->count.load(std::memory_order_relaxed) == run.job->capacity) {  // the chunk is full: it goes to a helper and this shape starts the next one
        close_chunk(run);
        std::vector<Stamp> one(1, t);
        start_chunk(run, one, room, run.shapes - 1);
        return;
    }
    CheckJob &j = *run.job;
    const std::uint32_t k = j.count.load(std::memory_order_relaxed);
    j.shapes[k] = t;
    j.count.store(k + 1, std::memory_order_release);
}
void end_check(Run &run) { close_chunk(run); }

// The answer of the run's check (waits for it): run.cuts, and the numbers of the check.
void collect(Run &run) {
    for (std::size_t c = 0; c < run.done_jobs.size(); ++c) {
        CheckJob &j = *run.done_jobs[c];
        while (!j.done.load(std::memory_order_acquire)) std::this_thread::yield();
        const std::uint32_t first = run.job_first[c], n = j.count.load();
        if (first) run.cuts.push_back(first);  // the chunk's first shape starts a segment
        for (const std::uint32_t k : j.cuts) run.cuts.push_back(first + k);
        ++g_checked_runs;
        g_checked_shapes += n;
        g_checked_cuts += j.cuts.size();
        g_cut_hash = (g_cut_hash ^ n) * 1099511628211ull;  // the cut points of every chunk, in order: the scalar and the wide walk must give the same value
        for (const std::uint32_t k : j.cuts) g_cut_hash = (g_cut_hash ^ k) * 1099511628211ull;
    }
    run.done_jobs.clear();
    run.job_first.clear();
}

// ---- the independence checks of a batch start while its shapes are collected. gpu.cpp's draw() tells every shape it adds to a card batch (live_shape), the runs are cut by
// the rule batch() uses (same look, same scissor words; shapes the chip draws nothing for are ignored), and full chunks go to the helpers at once. batch() takes the finished runs over
// (adopts them) when every shape of the batch was told here and the rule gave the same runs; otherwise it checks in the old way, shape by shape.
bool split_draws(const Look &l);
std::vector<Look> g_pre_looks;  // the looks of the routed setups, one entry for each different look (the id travels in gpu.cpp's setup cache)
int g_pre_last = -1;
struct PreRun {
    std::uint32_t first = 0, count = 0, sc[2] = {};  // first = ordinal of its first drawn shape in the batch
    int look = -1;
    Run run;
};
struct PreStream {
    bool on = false;
    std::vector<PreRun> done;
    PreRun cur;
    bool open = false;
    std::uint32_t told = 0, ordinal = 0;  // shapes told for the batch, and of those the ones the chip draws
    std::vector<Stamp> buf;
} PRE;

void pre_close() {
    if (!PRE.open) return;
    end_check(PRE.cur.run);
    PRE.done.push_back(std::move(PRE.cur));
    PRE.cur = PreRun();
    PRE.open = false;
}

int pre_look_id(const Look &l) {
    for (std::size_t i = 0; i < g_pre_looks.size(); ++i)
        if (g_pre_looks[i] == l) return static_cast<int>(i);
    g_pre_looks.push_back(l);
    return static_cast<int>(g_pre_looks.size() - 1);
}

// The check's part of one shape of a run, the same for the shapes of draw() and the shapes batch() walks itself. `room` = the size the next chunk may have.
inline void feed_check(Run &run, std::vector<Stamp> &buffered, const std::uint32_t *r, std::uint32_t room) {
    const Look &l = run.look;
    if (!(split_draws(l) && g_indep)) return;
    const float z = static_cast<float>(r[24]);  // the exact check is made when the run closes, and not at all for a flat run
    if (run.shapes == 1) run.flat_z = z;
    if (z != run.flat_z || ((r[1] & 3u) == 0 && (f(r[25]) != 0.0f || f(r[26]) != 0.0f))) run.flat = false;
    if (l.ztest != 0) {  // the test "always" never depends on an earlier shape
        if (run.job) add_check(run, stamp_of(r), room);
        else {
            buffered.push_back(stamp_of(r));
            if (!(run.flat && l.ztest <= 1)) start_check(run, buffered, room);  // not flat any more: the check starts and runs beside the walk
        }
    }
}

// Draws that split one shape over several passes (the alpha test's depth pass, or, without dual-source blending, the alpha channel's own draw) are in the chip's order
// only when no shape of a segment depends on an earlier one.
bool split_draws(const Look &l) { return l.two_pass || (l.alpha_two && !g_dual); }

std::uint32_t add_param(std::initializer_list<std::uint32_t> values) {
    std::array<std::uint32_t, 64> r{};
    std::size_t i = 0;
    for (std::uint32_t v : values) r[i++] = v;
    g_plan.params.push_back(r);
    return static_cast<std::uint32_t>(g_plan.params.size() - 1);
}

double shape_pixels(const std::uint32_t *r) {
    const double w = (r[3] & 0xFFFFu) - (r[2] & 0xFFFFu) + 1.0, h = (r[3] >> 16) - (r[2] >> 16) + 1.0;
    return (r[1] & 3u) == 0 ? std::fabs(0.5 / f(r[10])) : w * h;
}

void corner(std::vector<Vtx> &out, float x, float y, double z, std::uint32_t colour, float s, float t, float q, float fog, std::uint32_t si) {
    Vtx v;
    v.x = x, v.y = y;
    v.z = static_cast<float>(std::clamp(z, 0.0, 16777215.0) / 16777216.0);
    v.r = static_cast<float>(colour & 0xFFu), v.g = static_cast<float>((colour >> 8) & 0xFFu), v.b = static_cast<float>((colour >> 16) & 0xFFu), v.a = static_cast<float>(colour >> 24);
    v.s = s, v.t = t, v.q = q, v.fog = fog;
    v.si = si;
    out.push_back(v);
}

std::uint32_t target_for(const Batch &b) {
    const std::uint32_t *s = b.setups.data();
    for (std::uint32_t i = 0; i < g_plan.targets.size(); ++i) {
        const Target &t = g_plan.targets[i];
        if (t.fbp == s[1] && t.fbw == s[2] && t.fpsm == s[3] && t.zbp == s[5] && t.zpsm == s[6]) return i;
    }
    Target t;
    t.fbp = s[1], t.fbw = s[2], t.fpsm = s[3], t.zbp = s[5], t.zpsm = s[6];
    g_plan.targets.push_back(t);
    return static_cast<std::uint32_t>(g_plan.targets.size() - 1);
}

// the smallest and largest alpha of a shape's texture, from the cache's reduction pass over the decoded copy
// (hwcache::alpha_ranges), by global setup index. Used by a second plan of the same frame (PS2_GPU_HW_ALPHA_RANGE=1 with PS2_GPU_HW_CACHE=1).
struct AlphaRange {
    bool known = false;
    std::uint8_t lo = 0, hi = 255;
};
std::vector<AlphaRange> g_range;
bool g_range_on = false;
std::uint64_t g_narrow_pass = 0, g_narrow_fail = 0, g_narrow_tried = 0;

// Does the shape's alpha test pass for every pixel (1), fail for every pixel (-1) or depend on the pixel (0)? The pixel alpha follows the texture function from the vertex alpha
// (smallest and largest of the shape's corners, the interpolation stays between) and the texture's alpha range; every step is monotone, so the ends of the range decide.
int alpha_outcome(const std::uint32_t *s, const std::uint32_t *r, int kind, const AlphaRange &tr) {
    const std::uint32_t flags = s[0], modes = s[7], aref = s[8], atst = modes & 7u;
    int vlo = 255, vhi = 0;
    for (int k = 0; k < (kind == 0 ? 3 : 1); ++k) {
        const int a = static_cast<int>((kind == 0 ? r[12 + k] : r[13]) >> 24);
        vlo = std::min(vlo, a), vhi = std::max(vhi, a);
    }
    int lo = vlo, hi = vhi;
    if ((flags & 2u) && (flags & 8192u)) {  // the texture's alpha takes part
        if (!tr.known) return 0;
        const int tlo = tr.lo, thi = tr.hi;
        switch ((modes >> 20) & 3u) {
        case 0: lo = std::clamp((tlo * vlo) >> 7, 0, 255), hi = std::clamp((thi * vhi) >> 7, 0, 255); break;
        case 2: lo = std::clamp(tlo + vlo, 0, 255), hi = std::clamp(thi + vhi, 0, 255); break;
        default: lo = tlo, hi = thi; break;
        }
    }
    const int ref = static_cast<int>(aref);
    const auto test = [&](int a) {
        switch (atst) {
        case 2: return a < ref;
        case 3: return a <= ref;
        case 4: return a == ref;
        case 5: return a >= ref;
        case 6: return a > ref;
        default: return a != ref;
        }
    };
    if (atst == 4 || atst == 7) {
        const bool inside = ref >= lo && ref <= hi;
        const bool single = lo == hi;
        if (atst == 4) return !inside ? -1 : single ? 1 : 0;
        return !inside ? 1 : single ? -1 : 0;
    }
    const bool a = test(lo), b = test(hi);
    return a && b ? 1 : (!a && !b ? -1 : 0);
}

// ---- the processor-side record of palette alpha. The palettes live in graphics memory on the card and the processor holds no copy; what it can see is the stream
// of writers (gpu.cpp's hooks pal_upload, pal_write, pal_clut, pal_state, all in the order the game wrote them). It keeps, for every word of memory, the alpha byte of the last small PSMCT32 upload that
// wrote it, plus a sequence number; a draw or copy over a page, a bigger upload or any other format bumps the page's own sequence, and a word whose number is not newer than its page's is unknown.
// A palette load reads its 256 (or 16) entries from this record and keeps (smallest, largest alpha) for each 16 entries of the palette slot it makes. A setup whose texture alpha is one of
// these ranges can be proved to pass its alpha test (alpha_outcome): the shape then needs no split draw and no independence check. Unknown stays what it was: the split draws and the check.
namespace pal {
constexpr std::uint32_t kWords = 1u << 20, kSlots = 2048;
bool on = true;  // PS2_GPU_HW_PAL=0 turns the record off (every range unknown)
struct Grp {
    std::uint8_t lo = 0, hi = 255, known = 0;
};
struct Rec {
    Grp g[32];
    std::uint32_t uid = 0;  // changes with every load into the slot: a cached range is valid for one uid
};
std::vector<std::uint32_t> wseq;
std::vector<std::uint8_t> walpha;
std::vector<Rec> rec;
std::uint32_t pgseq[512] = {};
std::uint32_t ev = 1, uid_next = 1;
std::uint64_t n_loads = 0, n_loads_known = 0, n_shapes_tried = 0, n_shapes_pass = 0, n_setups = 0, n_setups_known = 0;
void ensure() {
    if (!wseq.empty()) return;
    wseq.assign(kWords, 0);
    walpha.assign(kWords, 0);
    rec.assign(kSlots, Rec());
}
inline bool word_known(std::uint32_t w) { return wseq[w] > pgseq[(w >> 11) & 511u]; }
}  // namespace pal

// The alpha range of the palette a setup's texture reads (known = false when the setup has no 8-bit or 4-bit palette in 32-bit colours, or any of its entries is not known).
AlphaRange pal_range(const std::uint32_t *s, std::uint32_t &uid) {
    AlphaRange r;
    uid = 0;
    if (!pal::on || pal::wseq.empty() || s[16] != 0u || s[22] >= pal::kSlots) return r;
    const pal::Rec &c = pal::rec[s[22]];
    uid = c.uid;
    const std::uint32_t psm = s[14];
    std::uint32_t first = 0, count = 0;
    if (psm == 0x13 || psm == 0x1B) first = 0, count = 16;
    else if (psm == 0x14 || psm == 0x24 || psm == 0x2C) first = s[17] & 15u, count = 1;
    else return r;
    std::uint8_t lo = 255, hi = 0;
    for (std::uint32_t k = first; k < first + count; ++k) {
        if (!c.g[k].known) return r;
        lo = std::min(lo, c.g[k].lo), hi = std::max(hi, c.g[k].hi);
    }
    static const int margin = std::atoi(os::env("PS2_GPU_HW_PAL_MARGIN").c_str());  // debugging aid: widen the range by this much
    r.known = true, r.lo = static_cast<std::uint8_t>(std::max(0, static_cast<int>(lo) - margin)), r.hi = static_cast<std::uint8_t>(std::min(255, static_cast<int>(hi) + margin));
    return r;
}
// The shape's alpha test passes for every pixel (the palette range and the shape's vertex alphas decide it, see alpha_outcome).
inline bool shape_passes(const std::uint32_t *s, const std::uint32_t *r, const AlphaRange &rng) { return alpha_outcome(s, r, static_cast<int>(r[1] & 3u), rng) == 1; }
// What a draw's look is the id of (live_route answers it to draw()): the look as classify() gave it, and the look of the same setup without its alpha test (-1: none) for the shapes that pass it.
struct PreSet {
    int orig = -1, alt = -1;
    AlphaRange rng;
};
std::vector<PreSet> g_pre_sets;
int pre_set_id(int orig, int alt, const AlphaRange &rng) {
    for (std::size_t i = 0; i < g_pre_sets.size(); ++i) {
        const PreSet &p = g_pre_sets[i];
        if (p.orig == orig && p.alt == alt && p.rng.known == rng.known && p.rng.lo == rng.lo && p.rng.hi == rng.hi) return static_cast<int>(i);
    }
    g_pre_sets.push_back({orig, alt, rng});
    return static_cast<int>(g_pre_sets.size() - 1);
}
inline std::uint32_t pal_uid(const std::uint32_t *s) { return (pal::on && !pal::rec.empty() && s[22] < pal::kSlots) ? pal::rec[s[22]].uid : 0u; }

// Build everything that does not depend on the repeat: vertices, runs, the order of the work, and which target is in the render texture when.
void make_plan(const Context &ctx, std::vector<Sub> &subs) {
    Plan &P = g_plan;
    P = Plan();
    // the sub-groups of the capture by (submit, command)
    std::map<std::pair<std::uint32_t, std::uint32_t>, std::uint32_t> batch_at;
    std::map<std::pair<std::uint32_t, std::uint32_t>, std::uint32_t> pre_at;
    for (std::uint32_t i = 0; i < g_batches.size(); ++i) batch_at[{g_batches[i].submit, g_batches[i].command}] = i;
    for (std::uint32_t i = 0; i < g_pres.size(); ++i) pre_at[{g_pres[i].submit, g_pres[i].command}] = i;
    // targets and their boxes first, so the state machine below knows each one's extent
    std::vector<std::uint32_t> batch_target(g_batches.size());
    for (std::uint32_t i = 0; i < g_batches.size(); ++i) {
        batch_target[i] = target_for(g_batches[i]);
        Target &t = P.targets[batch_target[i]];
        t.box.add(g_batches[i].minx, g_batches[i].miny, g_batches[i].maxx, g_batches[i].maxy);
    }
    for (Target &t : P.targets) {
        t.w = static_cast<int>(t.fbw * 64);
        t.h = std::min(t.box.y1 + 1, 2048);
        t.box.x0 = std::max(t.box.x0, 0), t.box.y0 = std::max(t.box.y0, 0);
        t.box.x1 = std::min(t.box.x1, t.w - 1), t.box.y1 = std::min(t.box.y1, t.h - 1);
        pages_of(t.fbp, t.fbw, t.fpsm, t.box.x0, t.box.y0, t.box.x1, t.box.y1, t.pages);
        pages_of(t.zbp, t.fbw, t.zpsm, t.box.x0, t.box.y0, t.box.x1, t.box.y1, t.zpages);
    }
    // per batch: decode plan, setup copies, looks, vertices, runs
    struct BatchPlan {
        std::uint32_t first_run = 0, run_count = 0, decode_first = 0, decode_count = 0;
        struct DecInfo {
            std::array<std::uint32_t, 9> key;
            std::uint32_t words, setup;
            std::bitset<512> pages;
        };
        std::vector<DecInfo> decs;
        std::vector<int> dec_of;  // per setup of the batch: index into decs, or -1
        bool any = false, writes_z = false;
        std::uint32_t base = 0;
        // the texture coordinates the shapes of each setup reach (texel units, before wrap), to know which part of a target is read
        struct Used {
            double u0 = 1e30, u1 = -1e30, v0 = 1e30, v1 = -1e30;
            bool bad = false;
        };
        std::vector<Used> used;
        bool clear_cover = false;  // the first shape is a constant sprite that overwrites the whole target ("clears first")
        std::uint32_t clear_ztest = 0;
    };
    std::vector<BatchPlan> bp(g_batches.size());
    for (std::uint32_t bi = 0; bi < g_batches.size(); ++bi) {
        const Batch &b = g_batches[bi];
        const std::uint32_t nset = static_cast<std::uint32_t>(b.setups.size() / kSetupWords), base = static_cast<std::uint32_t>(P.setups.size() / kHwSetup);
        P.setups.resize(P.setups.size() + static_cast<std::size_t>(nset) * kHwSetup, 0);
        bp[bi].base = base;
        if (!P.targets[batch_target[bi]].has_setup) P.targets[batch_target[bi]].has_setup = true, P.targets[batch_target[bi]].setup_word = base * kHwSetup;
        // decodes
        auto &decs = bp[bi].decs;
        bp[bi].dec_of.assign(nset, -1);
        std::uint32_t used = 0;
        std::vector<char> decodable(nset, 1);
        for (std::uint32_t i = 0; i < nset; ++i) {
            std::uint32_t *w = P.setups.data() + (base + i) * kHwSetup;
            std::memcpy(w, b.setups.data() + i * kSetupWords, kSetupWords * 4);
            w[24] = 0xFFFFFFFFu;
            if (!(w[0] & 2u)) continue;
            std::array<std::uint32_t, 9> key = {w[12], w[13], w[14], w[15], w[16], w[17], w[20], w[22], w[0] & 32768u};
            bool found = false;
            for (std::size_t k = 0; k < decs.size(); ++k)
                if (decs[k].key == key) { bp[bi].dec_of[i] = static_cast<int>(k); found = true; break; }
            if (found) continue;
            const std::uint32_t tw = w[15] & 0xFFu, th = w[15] >> 8, mu = (w[7] >> 24) & 3u, mv = (w[7] >> 26) & 3u;
            const auto inside = [](std::uint32_t mode, std::uint32_t region, std::uint32_t size) { return mode < 2 || ((region & 0x3FFu) < (1u << size) && (region >> 16) < (1u << size)); };
            const std::uint32_t words = 1u << (tw + th);
            if (!inside(mu, w[18], tw) || !inside(mv, w[19], th) || (!g_direct && used + words > kDecodedWords)) { decodable[i] = 0; continue; }
            used += words;
            std::bitset<512> tp;
            pages_of(w[12], w[13], w[14], 0, 0, static_cast<int>(1u << tw) - 1, static_cast<int>(1u << th) - 1, tp);
            bp[bi].dec_of[i] = static_cast<int>(decs.size());
            decs.push_back({key, words, base + i, tp});
        }
        bp[bi].used.assign(nset, BatchPlan::Used());
        std::vector<Look> looks(nset);
        for (std::uint32_t i = 0; i < nset; ++i) {
            looks[i] = classify(b.setups.data() + i * kSetupWords, decodable[i] != 0);
            if (looks[i].ok && !looks[i].skip && !only_class(looks[i].two_pass ? 1 : looks[i].alpha_two ? 2 : 0)) looks[i].ok = false, looks[i].why = "not in PS2_GPU_HW_ONLY";  // debugging aid: bisecting by class
            P.setups[(base + i) * kHwSetup + 40] = looks[i].alpha_out ? 1u : 0u;
        }
        // shapes
        const Target &tg = P.targets[batch_target[bi]];
        Run cur;
        bool open = false;
        const auto close = [&] {
            if (open) {
                if (split_draws(cur.look) && g_indep) {  // exact only when the run is independent; otherwise it is drawn shape by shape (the painter's job in the finished design)
                    if (cur.flat && cur.look.ztest <= 1) cur.cuts.clear();  // flat depth with GEQUAL or ALWAYS: independent as it is
                    cur.exact_order = g_indep_exact && cur.overlap && !cur.cuts.empty();
                    if (!g_indep_exact) P.segments += cur.cuts.size() + 1;
                    ++P.two_runs;
                    if (cur.overlap && !cur.cuts.empty()) ++P.exact_runs, P.exact_shapes += cur.shapes;
                    else P.indep_shapes += cur.shapes;
                }
                if (cur.cuts.empty() || g_indep_exact) {
                    P.runs.push_back(cur);
                    ++bp[bi].run_count;
                } else {  // every route draws the independent segments one after the other (the cut shape starts the next piece)
                    std::uint32_t at = cur.first_vertex, ci = 0;
                    Run piece;
                    const auto start = [&] {
                        piece = Run();
                        piece.look = cur.look, piece.target = cur.target, piece.first_vertex = at, piece.exact_order = cur.exact_order;
                        std::memcpy(piece.scissor, cur.scissor, sizeof piece.scissor);
                    };
                    start();
                    for (std::uint32_t sh = 0; sh < cur.shapes; ++sh) {
                        if (ci < cur.cuts.size() && cur.cuts[ci] == sh) {
                            ++ci;
                            P.runs.push_back(piece);
                            ++bp[bi].run_count;
                            start();
                        }
                        piece.vertices += cur.counts[sh];
                        ++piece.shapes;
                        piece.counts.push_back(cur.counts[sh]);
                        piece.setups_of.push_back(cur.setups_of[sh]);
                        at += cur.counts[sh];
                    }
                    P.runs.push_back(piece);
                    ++bp[bi].run_count;
                }
            }
            open = false;
            cur = Run();
        };
        bp[bi].first_run = static_cast<std::uint32_t>(P.runs.size());
        const std::uint32_t prims = static_cast<std::uint32_t>(b.prims.size() / kPrimWords);
        for (std::uint32_t i = 0; i < prims; ++i) {
            const std::uint32_t *r = b.prims.data() + i * kPrimWords;
            const std::uint32_t *s = b.setups.data() + r[0] * kSetupWords;
            Look l = looks[r[0]];
            if (g_range_on && l.ok && !l.skip && (s[0] & 64u) && (s[7] & 7u) > 1 && ((s[7] >> 4) & 3u) != 2u) {  // an alpha test decided per pixel that the ranges settle: classify the shape again as if the test were not there (always passes) or never passed
                ++g_narrow_tried;
                const int out = alpha_outcome(s, r, static_cast<int>(r[1] & 3u), g_range[base + r[0]]);
                if (out != 0) {
                    std::uint32_t w[kSetupWords];
                    std::memcpy(w, s, sizeof w);
                    if (out > 0) w[0] &= ~64u;
                    else w[7] &= ~7u;
                    const Look n = classify(w, decodable[r[0]] != 0);
                    if (n.ok) l = n, out > 0 ? ++g_narrow_pass : ++g_narrow_fail;
                }
            }
            const double px = shape_pixels(r);
            ++P.shapes;
            P.pixels += px;
            const std::uint32_t kind = r[1] & 3u;
            if (!l.ok || kind > 1) {
                Tally &t = P.left_out[!l.ok ? l.why : "line or point"];
                ++t.shapes;
                t.pixels += px;
                continue;
            }
            ++P.shapes_ok;
            P.pixels_ok += px;
            if (l.skip) continue;  // the chip draws nothing for it either
            const int sx0 = static_cast<int>(s[10] & 0xFFFFu), sx1 = static_cast<int>(s[10] >> 16), sy0 = static_cast<int>(s[11] & 0xFFFFu), sy1 = static_cast<int>(s[11] >> 16);
            const int cx0 = std::max(sx0, 0), cy0 = std::max(sy0, 0), cx1 = std::min(sx1, tg.w - 1), cy1 = std::min(sy1, tg.h - 1);
            if (cx1 < cx0 || cy1 < cy0) continue;
            const std::uint32_t scissor[4] = {static_cast<std::uint32_t>(cx0), static_cast<std::uint32_t>(cy0), static_cast<std::uint32_t>(cx1 - cx0 + 1), static_cast<std::uint32_t>(cy1 - cy0 + 1)};
            if (!open || !(cur.look == l) || std::memcmp(cur.scissor, scissor, sizeof scissor) != 0) {
                close();
                cur.look = l;
                cur.target = batch_target[bi];
                cur.first_vertex = static_cast<std::uint32_t>(P.vertices.size());
                std::memcpy(cur.scissor, scissor, sizeof scissor);
                open = true;
            }
            const std::uint32_t si = base + r[0];
            const std::size_t before = P.vertices.size();
            if (kind == 0) {
                for (int k = 0; k < 3; ++k) {
                    const double dz = k == 0 ? 0.0 : f(r[24 + k]);
                    corner(P.vertices, f(r[4 + 2 * k]), f(r[5 + 2 * k]), static_cast<double>(r[24]) + dz, r[12 + k], f(r[15 + 3 * k]), f(r[16 + 3 * k]), f(r[17 + 3 * k]), static_cast<float>((r[11] >> (8 * k)) & 0xFFu), si);
                }
            } else {
                const float x0 = f(r[4]), y0 = f(r[5]), x1 = x0 + f(r[6]), y1 = y0 + f(r[7]);
                const float s0 = f(r[15]), t0 = f(r[16]), s1 = f(r[18]), t1 = f(r[19]), q = f(r[20]), fog = static_cast<float>((r[11] >> 8) & 0xFFu);
                const double z = r[24];
                corner(P.vertices, x0, y0, z, r[13], s0, t0, q, fog, si);
                corner(P.vertices, x1, y0, z, r[13], s1, t0, q, fog, si);
                corner(P.vertices, x0, y1, z, r[13], s0, t1, q, fog, si);
                corner(P.vertices, x1, y0, z, r[13], s1, t0, q, fog, si);
                corner(P.vertices, x1, y1, z, r[13], s1, t1, q, fog, si);
                corner(P.vertices, x0, y1, z, r[13], s0, t1, q, fog, si);
            }
            const std::uint32_t n = static_cast<std::uint32_t>(P.vertices.size() - before);
            cur.vertices += n;
            ++cur.shapes;
            cur.counts.push_back(static_cast<std::uint8_t>(n));
            cur.setups_of.push_back(r[0]);
            if (split_draws(l) && g_indep) {
                if (!cur.serial) cur.serial = ++g_run_serial;
                note_shape(cur, r, static_cast<int>(kind));
            }
            if (s[0] & 2u) {  // the texel range of the texture coordinates (the interpolated ratio s/q lies between the vertices' when q > 0)
                BatchPlan::Used &u = bp[bi].used[r[0]];
                const bool fst = (s[0] & 16u) != 0;
                const double sw = static_cast<double>(1u << (s[15] & 0xFFu)), sh = static_cast<double>(1u << (s[15] >> 8));
                const auto acc = [&](float sv, float tv, float qv) {
                    double uu, vv;
                    if (fst) uu = sv / 16.0, vv = tv / 16.0;
                    else if (qv > 0.0f) uu = sv / qv * sw, vv = tv / qv * sh;
                    else { u.bad = true; return; }
                    if (!(uu > -1e9 && uu < 1e9 && vv > -1e9 && vv < 1e9)) { u.bad = true; return; }
                    u.u0 = std::min(u.u0, uu), u.u1 = std::max(u.u1, uu), u.v0 = std::min(u.v0, vv), u.v1 = std::max(u.v1, vv);
                };
                if (kind == 0) {
                    for (int k = 0; k < 3; ++k) acc(f(r[15 + 3 * k]), f(r[16 + 3 * k]), f(r[17 + 3 * k]));
                } else {
                    acc(f(r[15]), f(r[16]), f(r[20]));
                    acc(f(r[18]), f(r[19]), f(r[20]));
                }
            }
            if (i == 0 && kind == 1 && !(s[0] & (2u | 4u | 64u)) && !l.blend && !l.two_pass && !l.alpha_two && l.mask == (tg.fpsm == 1 ? 7u : 15u)) {  // a constant sprite over the whole target is a clear
                const float x0 = f(r[4]), y0 = f(r[5]), x1 = x0 + f(r[6]), y1 = y0 + f(r[7]);
                if (x0 <= static_cast<float>(tg.box.x0) && y0 <= static_cast<float>(tg.box.y0) && x1 >= static_cast<float>(tg.box.x1 + 1) && y1 >= static_cast<float>(tg.box.y1 + 1) && cx0 <= tg.box.x0 && cy0 <= tg.box.y0 && cx1 >= tg.box.x1 && cy1 >= tg.box.y1)
                    bp[bi].clear_cover = true, bp[bi].clear_ztest = l.ztest;
            }
            bp[bi].any = true;
            if (l.zwrite) bp[bi].writes_z = true;
        }
        close();
    }
    // The order of the work and the state of the render textures.
    // `cause` says why the next write-back happens: 0 a draw's texture is a picture on the card that cannot be sampled directly, 1 an upload or palette load
    // writes that memory, 2 another target over the same memory, 3 end of frame (the replay's judge needs memory), 4 depth buffer read as a picture or
    // overwritten, 5 other command, 6 eager mode.
    int cause = 0;
    std::uint32_t seq = 0;
    const auto clipped = [](const Target &t, const Rect &r) {
        Rect d;
        d.add(std::max(r.x0, 0), std::max(r.y0, 0), std::min(r.x1, t.w - 1), std::min(r.y1, t.h - 1));
        return d;
    };
    const auto sync_op = [&](OpKind kind, std::uint32_t ti, const Rect &d) {
        const Target &t = P.targets[ti];
        Op op{kind};
        op.target = ti;
        op.param = add_param({t.setup_word, static_cast<std::uint32_t>(d.x0), static_cast<std::uint32_t>(d.y0), static_cast<std::uint32_t>(d.x1 - d.x0 + 1), static_cast<std::uint32_t>(d.y1 - d.y0 + 1)});
        op.x = static_cast<std::uint32_t>((d.x1 - d.x0 + 8) / 8);
        op.y = static_cast<std::uint32_t>((d.y1 - d.y0 + 8) / 8);
        P.ops.push_back(op);
    };
    bool in_frame = true;  // false for the end-of-frame flush and the eager stores: they are not "write-backs during the frame"
    const auto emit_store = [&](std::uint32_t ti) {
        Target &t = P.targets[ti];
        const Rect d = clipped(t, t.dirty);
        if (!d.empty()) {
            ++P.stores_by[cause];
            P.store_px_by[cause] += static_cast<double>(d.x1 - d.x0 + 1) * (d.y1 - d.y0 + 1);
            if (in_frame) ++P.early_stores;
            sync_op(kStore, ti, d);
        }
        t.is_dirty = false;
        t.dirty = Rect();
    };
    const auto emit_store_z = [&](std::uint32_t ti, int why) {
        Target &t = P.targets[ti];
        const Rect d = clipped(t, t.zdirty);
        if (!d.empty()) {
            ++P.stores_by[why];
            P.store_px_by[why] += static_cast<double>(d.x1 - d.x0 + 1) * (d.y1 - d.y0 + 1);
            if (in_frame) ++P.early_stores;
            sync_op(kStoreZ, ti, d);
        }
        t.z_dirty = false;
        t.zdirty = Rect();
    };
    const auto emit_load = [&](std::uint32_t ti) {
        Target &t = P.targets[ti];
        ++P.loads;
        P.load_px += static_cast<double>(t.box.x1 - t.box.x0 + 1) * (t.box.y1 - t.box.y0 + 1);
        sync_op(kLoad, ti, t.box);
        t.resident = true;
        t.valid = t.box;
    };
    // memory that a command reads (`pages`, read) or overwrites (write): whatever the render textures hold newer is written back first, and a render texture
    // whose memory is overwritten is dropped (reloaded at its next use)
    const auto touch = [&](const std::bitset<512> &pages, bool write) {
        for (std::uint32_t ti = 0; ti < P.targets.size(); ++ti) {
            Target &t = P.targets[ti];
            if (t.z_dirty && (t.zpages & pages).any()) emit_store_z(ti, 4);  // the depth buffer is read as a picture (or overwritten): write it back
            if ((t.pages & pages).none()) continue;
            if (t.is_dirty) emit_store(ti);
            if (write) t.resident = false;
        }
    };
    // Which target's texture can the shapes read directly? Same base, same width, a format whose texels the shader can rebuild from the texture's
    // four channels: 32 bit from 32 bit, 24 bit from 32 or 24 bit, 16 bit from the same 16-bit format. A 32-bit texture over a
    // 24-bit target needs the top byte, which the target's texture does not track (channel flag), so that one is written back.
    const auto format_ok = [](std::uint32_t fpsm, std::uint32_t tpsm) {
        if (fpsm == 0) return tpsm == 0 || tpsm == 1;
        if (fpsm == 1) return tpsm == 1;
        return (fpsm == 2 || fpsm == 0x0A) && tpsm == fpsm;
    };
    struct CacheEntry {
        std::array<std::uint32_t, 9> key;
        std::uint32_t offset;
        std::bitset<512> pages;
    };
    std::vector<CacheEntry> cache;
    std::uint32_t cache_used = 0;
    for (std::uint32_t su = 0; su < subs.size(); ++su) {
        for (std::uint32_t ci = 0; ci < subs[su].commands.size(); ++ci) {
            const Cmd &c = subs[su].commands[ci];
            if (const auto it = batch_at.find({su, ci}); it != batch_at.end()) {
                const std::uint32_t bi = it->second, ti = batch_target[bi];
                const BatchPlan &B = bp[bi];
                if (!B.any) continue;
                Target &T = P.targets[ti];
                const Batch &bd = g_batches[bi];
                for (std::uint32_t uj = 0; uj < P.targets.size(); ++uj) {  // other render textures over the same memory go stale
                    Target &U = P.targets[uj];
                    if (uj == ti) continue;
                    if (U.is_dirty && ((U.pages & T.pages).any() || (B.writes_z && (U.pages & T.zpages).any()))) {
                        cause = 2;
                        emit_store(uj);
                        U.resident = false;
                    } else if ((U.pages & T.pages).any()) {
                        U.resident = false;
                    }
                    if (U.z_dirty && (U.zpages & T.pages).any()) emit_store_z(uj, 4);  // this draw's colour lands over a depth buffer that is newer on the card
                }
                if (!T.resident) {
                    if (g_linear && B.clear_cover && (B.clear_ztest == 0 || (B.clear_ztest == 1 && !T.depth_cleared))) {
                        T.resident = true;  // the first shape overwrites every pixel of the target: nothing to load
                        T.valid = T.box;
                        ++P.clears;
                    } else {
                        if (g_log) {
                            const std::uint32_t *r0 = bd.prims.data();
                            const std::uint32_t *s0 = bd.setups.data() + r0[0] * kSetupWords;
                            std::fprintf(stderr, "[hw-log] load of target %u (fbp %u fbw %u fpsm %02X, box %d,%d-%d,%d): clear_cover %d ztest %u; first shape kind %u flags %x blend %d mask %x rect %.1f,%.1f +%.1f,%.1f scissor %u,%u-%u,%u\n", ti, T.fbp, T.fbw, T.fpsm, T.box.x0, T.box.y0, T.box.x1, T.box.y1, B.clear_cover, B.clear_ztest, r0[1] & 3u, s0[0], (s0[0] >> 3) & 1u, s0[4], f(r0[4]), f(r0[5]), f(r0[6]), f(r0[7]), s0[10] & 0xFFFFu, s0[11] & 0xFFFFu, s0[10] >> 16, s0[11] >> 16);
                        }
                        emit_load(ti);
                    }
                }
                if (!g_direct && g_cache) {
                    // every texture of the batch is asked for at its fixed place in the decoded buffer; whether it has to be decoded again is decided when the plan runs
                    std::vector<std::array<std::uint32_t, 3>> asks;
                    for (const auto &d : B.decs) {
                        cause = 0;
                        touch(d.pages, false);  // a texture drawn earlier in the frame is in a render texture: write it back first
                        const hwcache::Request rq = hwcache::request_of(P.setups.data() + static_cast<std::size_t>(d.setup) * kHwSetup);
                        const std::uint32_t at = hwcache::reserve(rq);
                        if (at == ~0u) { std::fprintf(stderr, "[hw-cache] the decoded buffer is full: the plan cannot hold a place for every texture\n"); std::exit(1); }
                        asks.push_back({d.setup * kHwSetup, at, d.words});
                        if (os::env("PS2_GPU_HW_CACHE_SAY") == "1") std::fprintf(stderr, "[hw-cache] plan slot %u words %u (tbp %u tbw %u psm %02X %ux%u slot %u)\n", at, d.words, rq.tbp, rq.tbw, rq.tpsm, 1u << rq.tw, 1u << rq.th, rq.clut_slot);
                    }
                    for (std::uint32_t i = 0; i < B.dec_of.size(); ++i)
                        if (B.dec_of[i] >= 0) P.setups[(B.base + i) * kHwSetup + 24] = asks[static_cast<std::size_t>(B.dec_of[i])][1];
                    if (!asks.empty()) {
                        Op op{kDecode};
                        op.param = static_cast<std::uint32_t>(P.params.size());
                        op.decodes = static_cast<std::uint32_t>(asks.size());
                        for (const auto &fp : asks) add_param({fp[0], fp[1], fp[2]});
                        g_dec_req.resize(P.params.size());
                        for (std::size_t k = 0; k < asks.size(); ++k) g_dec_req[op.param + k] = hwcache::request_of(P.setups.data() + static_cast<std::size_t>(asks[k][0]));
                        P.ops.push_back(op);
                    }
                    Op op{kDraw};
                    op.target = ti;
                    op.run_first = B.first_run;
                    op.run_count = B.run_count;
                    op.clear_depth = !T.depth_cleared;
                    P.ops.push_back(op);
                    T.depth_cleared = true;
                    T.is_dirty = true;
                    if (B.writes_z) {
                        T.z_dirty = true;
                        T.zdirty.add(std::max(bd.minx, 0), std::max(bd.miny, 0), std::min(bd.maxx, T.w - 1), std::min(bd.maxy, T.h - 1));
                    }
                    T.dirty.add(std::max(bd.minx, 0), std::max(bd.miny, 0), std::min(bd.maxx, T.w - 1), std::min(bd.maxy, T.h - 1));
                    continue;
                }
                if (!g_direct) {
                    // textures are decoded first (a texture cache; the key holds everything the decode depends on, the palette copy included)
                    std::uint32_t batch_words = 0;
                    for (const auto &d : B.decs) batch_words += d.words;
                    if (cache_used + batch_words > kDecodedWords) cache.clear(), cache_used = 0;
                    std::vector<std::uint32_t> offsets(B.decs.size());
                    std::vector<std::array<std::uint32_t, 3>> fresh;
                    for (std::size_t k = 0; k < B.decs.size(); ++k) {
                        const auto &d = B.decs[k];
                        bool hit = false;
                        for (const auto &e : cache)
                            if (e.key == d.key) { offsets[k] = e.offset; hit = true; break; }
                        if (hit) continue;
                        cause = 0;
                        touch(d.pages, false);  // a texture drawn earlier in the frame is in a render texture: write it back first
                        offsets[k] = cache_used;
                        cache.push_back({d.key, cache_used, d.pages});
                        fresh.push_back({d.setup * kHwSetup, cache_used, d.words});
                        cache_used += d.words;
                    }
                    for (std::uint32_t i = 0; i < B.dec_of.size(); ++i)
                        if (B.dec_of[i] >= 0) P.setups[(B.base + i) * kHwSetup + 24] = offsets[static_cast<std::size_t>(B.dec_of[i])];
                    if (!fresh.empty()) {
                        Op op{kDecode};
                        op.param = static_cast<std::uint32_t>(P.params.size());
                        op.decodes = static_cast<std::uint32_t>(fresh.size());
                        for (const auto &fp : fresh) add_param({fp[0], fp[1], fp[2]});
                        P.ops.push_back(op);
                    }
                    Op op{kDraw};
                    op.target = ti;
                    op.run_first = B.first_run;
                    op.run_count = B.run_count;
                    op.clear_depth = !T.depth_cleared;
                    P.ops.push_back(op);
                    T.depth_cleared = true;
                    T.is_dirty = true;
                    if (B.writes_z) {
                        T.z_dirty = true;
                        T.zdirty.add(std::max(bd.minx, 0), std::max(bd.miny, 0), std::min(bd.maxx, T.w - 1), std::min(bd.maxy, T.h - 1));
                    }
                    T.dirty.add(std::max(bd.minx, 0), std::max(bd.miny, 0), std::min(bd.maxx, T.w - 1), std::min(bd.maxy, T.h - 1));
                    cache.erase(std::remove_if(cache.begin(), cache.end(), [&](const CacheEntry &e) { return (e.pages & T.pages).any(); }), cache.end());
                    continue;
                }
                // ---- textures read from graphics memory: where a texture is a picture the card drew, sample that picture
                std::vector<std::uint32_t> dec_src(B.decs.size(), ~0u);
                Rect copy_box;  // the part of this batch's target that the batch's own shapes read: copied before the draw
                for (std::size_t k = 0; k < B.decs.size(); ++k) {
                    const auto &d = B.decs[k];
                    const std::uint32_t *w = P.setups.data() + d.setup * kHwSetup;
                    // the texel rectangle the shapes of this texture read, in texture (and, for a matching target, pixel) coordinates
                    Rect used;
                    bool used_ok = g_linear;
                    for (std::uint32_t i = 0; i < B.dec_of.size() && used_ok; ++i) {
                        if (B.dec_of[i] != static_cast<int>(k)) continue;
                        const BatchPlan::Used &u = B.used[i];
                        if (u.bad || u.u0 > u.u1) { used_ok = false; break; }
                        const std::uint32_t *s2 = P.setups.data() + (B.base + i) * kHwSetup;
                        const auto axis = [&](double lo, double hi, std::uint32_t lg, std::uint32_t mode, std::uint32_t region, int &a, int &b) {
                            if (mode == 3) return false;  // region repeat: not in this lane
                            const int size = 1 << lg;
                            a = static_cast<int>(std::floor(lo)) - 1;  // one texel more each side for the bilinear taps
                            b = static_cast<int>(std::floor(hi)) + 1;
                            if (mode == 0) {
                                if (a < 0 || b >= size) a = 0, b = size - 1;  // wraps: the whole texture
                            } else {
                                const int mn = mode == 2 ? static_cast<int>(region & 0xFFFFu) : 0, mx = mode == 2 ? static_cast<int>(region >> 16) : size - 1;
                                a = std::clamp(a, mn, mx), b = std::clamp(b, mn, mx);  // clamping is monotone
                            }
                            return true;
                        };
                        int ax, bx, ay, by;
                        if (!axis(u.u0, u.u1, s2[15] & 0xFFu, (s2[7] >> 24) & 3u, s2[18], ax, bx) || !axis(u.v0, u.v1, s2[15] >> 8, (s2[7] >> 26) & 3u, s2[19], ay, by)) { used_ok = false; break; }
                        used.add(ax, ay, bx, by);
                    }
                    if (used.empty()) used_ok = false;
                    if (used_ok && g_overshoot) {  // overshoot tolerance: a texture that reaches up to 3 texels past the picture it samples reads the picture's edge there (the chip reads the memory beyond, which holds another buffer)
                        for (const Target &U : P.targets) {
                            if (&U == &T || !U.is_dirty || !U.resident || U.valid.x1 != U.w - 1 || U.valid.y1 != U.h - 1 || U.fbp != w[12] || U.fbw != w[13] || !format_ok(U.fpsm, w[14])) continue;
                            if (used.x0 >= U.valid.x0 && used.y0 >= U.valid.y0 && used.x1 <= U.valid.x1 + 3 && used.y1 <= U.valid.y1 + 3) used.x1 = std::min(used.x1, U.valid.x1), used.y1 = std::min(used.y1, U.valid.y1), ++P.overshoot_decs;
                            break;
                        }
                    }
                    // the memory the texture really reads: the pages of the texel rectangle, not of the whole 2^tw x 2^th texture (a 1024 x 1024 texture whose
                    // shapes read a 34-texel strip would otherwise overlap every target on the card)
                    std::bitset<512> read_pages = d.pages;
                    if (used_ok) {
                        read_pages.reset();
                        pages_of(w[12], w[13], w[14], std::max(used.x0, 0), std::max(used.y0, 0), used.x1, used.y1, read_pages);
                    }
                    std::vector<std::uint32_t> cands;
                    bool self = false;
                    for (std::uint32_t uj = 0; uj < P.targets.size(); ++uj) {
                        Target &U = P.targets[uj];
                        if (U.z_dirty && (U.zpages & read_pages).any()) emit_store_z(uj, 4);  // the depth buffer shares the memory the texture is read from
                        if (!U.is_dirty || (U.pages & read_pages).none()) continue;
                        if (used_ok && g_self && uj == ti && U.resident && U.fbp == w[12] && U.fbw == w[13] && format_ok(U.fpsm, w[14]) && used.x0 >= U.valid.x0 && used.y0 >= U.valid.y0 && used.x1 <= U.valid.x1 && used.y1 <= U.valid.y1) {
                            self = true;  // a draw that reads the picture it draws on: it reads a copy of the box made before the draw
                            copy_box.add(used.x0, used.y0, used.x1, used.y1);
                            continue;
                        }
                        if (used_ok && uj != ti && U.fbp == w[12] && U.fbw == w[13] && format_ok(U.fpsm, w[14])) {
                            const Rect dr = clipped(U, U.dirty);
                            if (dr.x1 < used.x0 || dr.x0 > used.x1 || dr.y1 < used.y0 || dr.y0 > used.y1) continue;  // the pixels read are as memory has them
                            if (U.resident && used.x0 >= U.valid.x0 && used.y0 >= U.valid.y0 && used.x1 <= U.valid.x1 && used.y1 <= U.valid.y1) {
                                cands.push_back(uj);
                                continue;
                            }
                        }
                        cause = 0;
                        if (g_log) {  // PS2_GPU_HW_LOG=1: why this picture was written back instead of being sampled
                            const char *why = !g_linear ? "linear off" : !used_ok ? "texel range unknown (q<=0, region repeat, or no shape)" : uj == ti ? "texture is the target being drawn on" : (U.fbp != w[12] || U.fbw != w[13]) ? "address or width differs" : !format_ok(U.fpsm, w[14]) ? "format differs" : !U.resident ? "target not resident" : "texel range outside the valid area";
                            std::fprintf(stderr, "[hw-log] write-back of target %u (fbp %u fbw %u fpsm %02X) for a texture at %u width %u psm %02X size %ux%u: %s; used %d,%d-%d,%d valid %d,%d-%d,%d\n", uj, U.fbp, U.fbw, U.fpsm, w[12], w[13], w[14], 1u << (w[15] & 0xFFu), 1u << (w[15] >> 8), why, used.x0, used.y0, used.x1, used.y1, U.valid.x0, U.valid.y0, U.valid.x1, U.valid.y1);
                        }
                        emit_store(uj);  // stays on the card, clean
                    }
                    if (self && cands.empty()) {
                        dec_src[k] = static_cast<std::uint32_t>(P.targets.size()) + ti;
                        T.shadow_needed = true;
                        ++P.self_decs;
                    } else if (cands.size() == 1 && !self) {
                        dec_src[k] = cands[0];
                        ++P.direct_decs;
                    } else {
                        if (self) cause = 0, emit_store(ti);
                        cause = 0;
                        for (std::uint32_t uj : cands) emit_store(uj);
                    }
                }
                std::vector<std::uint32_t> setup_src(B.dec_of.size(), ~0u);
                for (std::uint32_t i = 0; i < B.dec_of.size(); ++i) {
                    if (B.dec_of[i] < 0 || dec_src[static_cast<std::size_t>(B.dec_of[i])] == ~0u) continue;
                    setup_src[i] = dec_src[static_cast<std::size_t>(B.dec_of[i])];
                    P.setups[(B.base + i) * kHwSetup + 41] = 1;
                }
                if (!copy_box.empty()) {
                    bool any_shadow = false;
                    for (std::uint32_t d2 : dec_src) any_shadow = any_shadow || d2 >= P.targets.size();
                    if (any_shadow) {
                        Op cp{kCopy};
                        cp.target = ti;
                        cp.param = add_param({static_cast<std::uint32_t>(copy_box.x0), static_cast<std::uint32_t>(copy_box.y0), static_cast<std::uint32_t>(copy_box.x1 - copy_box.x0 + 1), static_cast<std::uint32_t>(copy_box.y1 - copy_box.y0 + 1)});
                        P.ops.push_back(cp);
                        ++P.copies;
                    }
                }
                // the batch's runs, split where the texture source changes
                Op op{kDraw};
                op.target = ti;
                op.run_first = static_cast<std::uint32_t>(P.runs.size());
                for (std::uint32_t ri = B.first_run; ri < B.first_run + B.run_count; ++ri) {
                    const Run old = P.runs[ri];
                    Run cur2;
                    bool open2 = false;
                    std::uint32_t at = old.first_vertex;
                    for (std::uint32_t sh = 0; sh < old.shapes; ++sh) {
                        const std::uint32_t src = setup_src[old.setups_of[sh]], n = old.counts[sh];
                        if (!open2 || cur2.src != src || (!old.cuts.empty() && std::binary_search(old.cuts.begin(), old.cuts.end(), sh))) {
                            if (open2) P.runs.push_back(cur2), ++op.run_count;
                            cur2 = Run();
                            cur2.look = old.look;
                            cur2.target = old.target;
                            std::memcpy(cur2.scissor, old.scissor, sizeof cur2.scissor);
                            cur2.first_vertex = at;
                            cur2.src = src;
                            cur2.exact_order = old.exact_order;  // the run's independence (a split by source only makes the pieces smaller; kept as is, conservative)
                            open2 = true;
                        }
                        cur2.vertices += n;
                        ++cur2.shapes;
                        cur2.counts.push_back(static_cast<std::uint8_t>(n));
                        if (src != ~0u) ++P.direct_shapes;
                        at += n;
                    }
                    if (open2) P.runs.push_back(cur2), ++op.run_count;
                }
                for (std::uint32_t ri = op.run_first; ri < op.run_first + op.run_count; ++ri) P.direct_runs += P.runs[ri].src != ~0u;
                op.clear_depth = !T.depth_cleared;
                P.ops.push_back(op);
                T.depth_cleared = true;
                T.is_dirty = true;
                T.seq = ++seq;
                if (B.writes_z) {
                    T.z_dirty = true;
                    T.zseq = seq;
                    T.zdirty.add(std::max(bd.minx, 0), std::max(bd.miny, 0), std::min(bd.maxx, T.w - 1), std::min(bd.maxy, T.h - 1));
                }
                T.dirty.add(std::max(bd.minx, 0), std::max(bd.miny, 0), std::min(bd.maxx, T.w - 1), std::min(bd.maxy, T.h - 1));
                if (g_eager) {  // write back everything this batch drew and reload it before the next draw (a 16-bit target is not reloaded: the reload would cut it to 5 bits per channel, which the texture does not do)
                    in_frame = false;
                    cause = 6;
                    emit_store(ti);
                    if (B.writes_z) emit_store_z(ti, 6);
                    if (T.fpsm != 2 && T.fpsm != 0x0A) T.resident = false;
                    in_frame = true;
                }
                continue;
            }
            if (c.pipeline == ctx.id_decode || c.pipeline == ctx.id_snapshot || c.pipeline == ctx.id_raster) continue;
            Op op{kCompute};
            op.sub = su;
            op.cmd = ci;
            if (const auto pt = pre_at.find({su, ci}); pt != pre_at.end() && (c.pipeline == ctx.id_upload || c.pipeline == ctx.id_load_clut)) {
                const std::bitset<512> written = pages_bytes(g_pres[pt->second].lo, g_pres[pt->second].hi);
                cause = 1;
                touch(written, !g_pres[pt->second].clut);
                if (!g_pres[pt->second].clut) cache.erase(std::remove_if(cache.begin(), cache.end(), [&](const CacheEntry &e) { return (e.pages & written).any(); }), cache.end());
            } else {
                cache.clear();
                std::bitset<512> all;
                all.set();
                cause = 5;
                touch(all, true);
            }
            P.ops.push_back(op);
        }
    }
    // end of the frame: everything the card holds newer goes back, in the order the chip wrote it (colour and depth targets can share pages)
    in_frame = false;
    cause = 3;
    std::vector<std::array<std::uint32_t, 3>> pending;
    for (std::uint32_t ti = 0; ti < P.targets.size(); ++ti) {
        if (P.targets[ti].is_dirty) pending.push_back({P.targets[ti].seq, 0, ti});
        if (P.targets[ti].z_dirty) pending.push_back({P.targets[ti].zseq, 1, ti});
    }
    std::sort(pending.begin(), pending.end());
    for (const auto &e : pending) {
        if (e[1] == 0) emit_store(e[2]);
        else emit_store_z(e[2], 3);
    }
}

}  // namespace

namespace {

struct Gpu {
    Context *ctx = nullptr;
    WGPUBuffer vertices = nullptr, setups = nullptr, params = nullptr, decoded = nullptr;
    WGPUBindGroup group = nullptr;  // gpu.cpp's group 0 with the trial's own params, setups and decoded buffers
    WGPUBindGroupLayout render_layout = nullptr, load_layout = nullptr, store_layout = nullptr;
    WGPUPipelineLayout render_pl = nullptr;
    WGPUShaderModule render_module = nullptr;
    WGPUComputePipeline load = nullptr, store = nullptr, store_z = nullptr, display = nullptr;  // display: the picture of a target into the display buffer
    WGPUBindGroupLayout store_z_layout = nullptr;
    std::map<std::uint32_t, WGPURenderPipeline> pipelines;
    std::map<std::uint64_t, WGPUBindGroup> groups;  // (target drawn on << 32 | target sampled or ~0) -> render bind group
    WGPUTexture dummy = nullptr;
    WGPUTextureView dummy_view = nullptr;
} g_gpu;

WGPUBuffer make_buffer(Context &c, std::uint64_t size, WGPUBufferUsage usage) {
    WGPUBufferDescriptor d = WGPU_BUFFER_DESCRIPTOR_INIT;
    d.size = std::max<std::uint64_t>(size, 16);
    d.usage = usage;
    return memlog::new_buffer(c.device, &d);
}

WGPUBlendFactor factor_of(std::uint32_t f) {
    switch (f) {
    case kZero: return WGPUBlendFactor_Zero;
    case kOne: return WGPUBlendFactor_One;
    case kSrcA: return WGPUBlendFactor_SrcAlpha;
    case kInvSrcA: return WGPUBlendFactor_OneMinusSrcAlpha;
    case kConst: return WGPUBlendFactor_Constant;
    default: return WGPUBlendFactor_OneMinusConstant;
    }
}

// One render pipeline per combination of blend, depth state and colour mask; `depth_only` is the second draw of a two-draw shape.
// with async set the pipeline is made in the background (wgpuDeviceCreateRenderPipelineAsync) and the answer is nullptr until it is ready;
// the live path routes a shape to the painter meanwhile. The keys that were needed are written to games/<name>/hw/pipelines.txt and made at the next start.
std::uint32_t pipe_key(const Look &l, bool depth_only, bool alpha_only) {
    const bool dual = g_dual && l.alpha_two && !depth_only && !alpha_only;
    return (l.blend ? 1u : 0u) | (l.src << 1) | (l.dst << 4) | (l.op << 7) | (l.ztest << 9) | ((l.zwrite ? 1u : 0u) << 11) | (l.mask << 12) | ((l.two_pass ? 1u : 0u) << 16) | ((depth_only ? 1u : 0u) << 17) | ((alpha_only ? 1u : 0u) << 18) | ((dual ? 1u : 0u) << 19) | (l.premul << 20) | ((l.fix ? 1u : 0u) << 22);
}
std::mutex g_scope_mutex;
std::set<std::uint32_t> g_pending;  // keys whose creation was started
std::uint64_t g_async_ready = 0;    // completions so far: the setup cache asks again when it changed
std::uint64_t g_async_done = 0, g_async_failed = 0;
std::string g_pipes_path;
std::set<std::uint32_t> g_pipes_known;
void record_pipeline_key(std::uint32_t key) {
    if (g_pipes_path.empty() || !g_pipes_known.insert(key).second) return;
    std::ofstream out(g_pipes_path, std::ios::app);
    char line[16];
    std::snprintf(line, sizeof line, "%08x\n", key);
    out << line;
}
WGPURenderPipeline build_pipeline(const Look &l, bool depth_only, bool alpha_only) {
    const bool dual = g_dual && l.alpha_two && !depth_only && !alpha_only;
    Context &c = *g_gpu.ctx;
    WGPUVertexAttribute attrs[6] = {};
    const WGPUVertexFormat formats[6] = {WGPUVertexFormat_Float32x2, WGPUVertexFormat_Float32, WGPUVertexFormat_Float32x4, WGPUVertexFormat_Float32x3, WGPUVertexFormat_Float32, WGPUVertexFormat_Uint32};
    const std::uint64_t offsets[6] = {0, 8, 12, 28, 40, 44};
    for (int i = 0; i < 6; ++i) {
        attrs[i] = WGPU_VERTEX_ATTRIBUTE_INIT;
        attrs[i].format = formats[i];
        attrs[i].offset = offsets[i];
        attrs[i].shaderLocation = static_cast<std::uint32_t>(i);
    }
    WGPUVertexBufferLayout vbl = WGPU_VERTEX_BUFFER_LAYOUT_INIT;
    vbl.stepMode = WGPUVertexStepMode_Vertex;
    vbl.arrayStride = sizeof(Vtx);
    vbl.attributeCount = 6;
    vbl.attributes = attrs;
    WGPUBlendState bs = WGPU_BLEND_STATE_INIT;
    bs.color.operation = l.op == 0 ? WGPUBlendOperation_Add : l.op == 1 ? WGPUBlendOperation_Subtract : WGPUBlendOperation_ReverseSubtract;
    bs.color.srcFactor = factor_of(l.src);
    bs.color.dstFactor = factor_of(l.dst);
    bs.alpha.operation = WGPUBlendOperation_Add;
    bs.alpha.srcFactor = WGPUBlendFactor_One;
    bs.alpha.dstFactor = WGPUBlendFactor_Zero;
    if (dual) {  // output 1: the colour factor (shape alpha, 0x80 = 1.0) in rgb, "write the alpha channel" in alpha (hwdraw.wgsl, fs_dual)
        if (!l.blend) bs.color.operation = WGPUBlendOperation_Add, bs.color.srcFactor = WGPUBlendFactor_One, bs.color.dstFactor = WGPUBlendFactor_Zero;
        if (l.src == kSrcA) bs.color.srcFactor = WGPUBlendFactor_Src1;
        else if (l.src == kInvSrcA) bs.color.srcFactor = WGPUBlendFactor_OneMinusSrc1;
        if (l.dst == kSrcA) bs.color.dstFactor = WGPUBlendFactor_Src1;
        else if (l.dst == kInvSrcA) bs.color.dstFactor = WGPUBlendFactor_OneMinusSrc1;
        bs.alpha.srcFactor = WGPUBlendFactor_Src1Alpha;
        bs.alpha.dstFactor = WGPUBlendFactor_OneMinusSrc1Alpha;
    }
    WGPUColorTargetState ct = WGPU_COLOR_TARGET_STATE_INIT;
    ct.format = WGPUTextureFormat_RGBA8Unorm;
    ct.blend = ((l.blend || dual) && !depth_only && !alpha_only) ? &bs : nullptr;
    ct.writeMask = depth_only ? 0 : alpha_only ? 8 : dual ? (l.mask | 8u) : l.mask;
    WGPUDepthStencilState ds = WGPU_DEPTH_STENCIL_STATE_INIT;
    ds.format = WGPUTextureFormat_Depth32Float;
    ds.depthWriteEnabled = (depth_only ? true : alpha_only ? false : (l.zwrite && !l.two_pass)) ? WGPUOptionalBool_True : WGPUOptionalBool_False;
    ds.depthCompare = l.ztest == 0 ? WGPUCompareFunction_Always : l.ztest == 1 ? WGPUCompareFunction_GreaterEqual : WGPUCompareFunction_Greater;
    WGPUConstantEntry k[8];
    for (auto &e : k) e = WGPU_CONSTANT_ENTRY_INIT;
    k[0].key = sv("DEPTH_ONLY");
    k[0].value = depth_only ? 1.0 : 0.0;
    k[1].key = sv("ALPHA_ONLY");
    k[1].value = alpha_only ? 1.0 : 0.0;
    k[2].key = sv("DIRECT");
    k[2].value = g_direct ? 1.0 : 0.0;
    k[3].key = sv("PROBE");
    k[3].value = static_cast<double>(g_probe);
    // BMODE 2 / 3 = the shader computes the product floor / ceil(Cs * C) itself; BMODE 1 = the colour is moved down by whole levels so that the
    // card's rounding to nearest comes out as the chip's cutting down (the forms that multiply the target as well)
    const bool colour_draw = !depth_only && !alpha_only && g_round > 0.0f;
    const bool shifted = colour_draw && l.blend && !l.premul && l.src >= kSrcA;
    k[4].key = sv("BMODE");
    k[4].value = !colour_draw || !l.blend && !l.premul ? 0.0 : l.premul == 1 ? 2.0 : l.premul == 2 ? 3.0 : shifted ? 1.0 : 0.0;
    k[5].key = sv("BFAC");
    k[5].value = l.fix ? 1.0 : 0.0;
    k[6].key = sv("BINV");
    k[6].value = (l.src == kInvSrcA || l.src == kInvConst) ? 1.0 : 0.0;
    WGPUConstantEntry rk = WGPU_CONSTANT_ENTRY_INIT;
    rk.key = sv("RND");
    rk.value = static_cast<double>(g_round);
    k[7] = rk;
    WGPUFragmentState fs = WGPU_FRAGMENT_STATE_INIT;
    fs.module = g_gpu.render_module;
    fs.entryPoint = sv(dual ? "fs_dual" : "fs");
    fs.constantCount = 8;
    fs.constants = k;
    fs.targetCount = 1;
    fs.targets = &ct;
    WGPURenderPipelineDescriptor d = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
    d.layout = g_gpu.render_pl;
    d.vertex.module = g_gpu.render_module;
    d.vertex.entryPoint = sv("vs");
    d.vertex.bufferCount = 1;
    d.vertex.buffers = &vbl;
    d.primitive.topology = WGPUPrimitiveTopology_TriangleList;
    d.primitive.cullMode = WGPUCullMode_None;
    d.primitive.frontFace = WGPUFrontFace_CCW;
    d.depthStencil = &ds;
    d.multisample.count = 1;
    d.multisample.mask = 0xFFFFFFFFu;
    d.fragment = &fs;
    std::lock_guard<std::mutex> scope_lock(g_scope_mutex);  // error scopes are the device's, not the thread's
    if (g_check) wgpuDevicePushErrorScope(c.device, WGPUErrorFilter_Validation);
    WGPURenderPipeline p = memlog::new_rpipe(c.device, &d);
    if (g_check) {  // a pipeline that does not build sends the draw to the painter
        bool done = false, bad = false;
        struct Scope { bool *done, *bad; } scope{&done, &bad};
        WGPUPopErrorScopeCallbackInfo cb = WGPU_POP_ERROR_SCOPE_CALLBACK_INFO_INIT;
        cb.mode = WGPUCallbackMode_AllowProcessEvents;
        cb.callback = [](WGPUPopErrorScopeStatus, WGPUErrorType type, WGPUStringView message, void *u1, void *) {
            Scope *s2 = static_cast<Scope *>(u1);
            if (type != WGPUErrorType_NoError) {
                *s2->bad = true;
                std::fprintf(stderr, "[hw] a pipeline did not build: %.*s\n", static_cast<int>(message.length == WGPU_STRLEN ? std::strlen(message.data) : message.length), message.data);
            }
            *s2->done = true;
        };
        cb.userdata1 = &scope;
        wgpuDevicePopErrorScope(c.device, cb);
        while (!done) wgpuDevicePoll(c.device, true, nullptr);
        if (bad) {
            if (p) memlog::del_rpipe(p);
            p = nullptr;
        }
    }
    return p;
}

// The pipelines of the live path are made by one worker thread (wgpu-native's own wgpuDeviceCreateRenderPipelineAsync is not implemented in this release: it panics), the finished ones are taken over by the window thread.
struct PipeJob { Look l; bool depth_only, alpha_only; std::uint32_t key; };
struct PipeWorker {
    std::mutex m;
    std::condition_variable cv;
    std::deque<PipeJob> jobs;
    std::vector<std::pair<std::uint32_t, WGPURenderPipeline>> done;
    bool started = false;
    void submit(const PipeJob &j) {
        std::lock_guard<std::mutex> lk(m);
        jobs.push_back(j);
        if (!started) {
            started = true;
            std::thread([this] {
                for (;;) {
                    PipeJob job;
                    {
                        std::unique_lock<std::mutex> lk2(m);
                        cv.wait(lk2, [&] { return !jobs.empty(); });
                        job = jobs.front();
                        jobs.pop_front();
                    }
                    WGPURenderPipeline p = build_pipeline(job.l, job.depth_only, job.alpha_only);
                    std::lock_guard<std::mutex> lk2(m);
                    done.emplace_back(job.key, p);
                }
            }).detach();
        }
        cv.notify_one();
    }
};
PipeWorker *g_pipe_worker = nullptr;  // never destroyed: the process ends with _Exit

// The window thread takes the finished pipelines over.
void drain_pipelines() {
    if (!g_pipe_worker) return;
    std::vector<std::pair<std::uint32_t, WGPURenderPipeline>> got;
    {
        std::lock_guard<std::mutex> lk(g_pipe_worker->m);
        got.swap(g_pipe_worker->done);
    }
    for (const auto &kp : got) {
        g_gpu.pipelines[kp.first] = kp.second;
        g_pending.erase(kp.first);
        if (!kp.second) ++g_async_failed;
        ++g_async_ready, ++g_async_done;
    }
}

WGPURenderPipeline pipeline_for(const Look &l, bool depth_only, bool alpha_only = false, bool async = false) {
    const std::uint32_t key = pipe_key(l, depth_only, alpha_only);
    if (const auto it = g_gpu.pipelines.find(key); it != g_gpu.pipelines.end()) return it->second;
    if (async) {
        if (g_pending.count(key)) return nullptr;
        if (!g_pipe_worker) g_pipe_worker = new PipeWorker();
        g_pending.insert(key);
        record_pipeline_key(key);
        g_pipe_worker->submit(PipeJob{l, depth_only, alpha_only, key});
        return nullptr;
    }
    WGPURenderPipeline p = build_pipeline(l, depth_only, alpha_only);
    g_gpu.pipelines[key] = p;
    return p;
}

WGPUBindGroupLayout make_layout(Context &c, std::initializer_list<WGPUBindGroupLayoutEntry> entries) {
    std::vector<WGPUBindGroupLayoutEntry> e(entries);
    WGPUBindGroupLayoutDescriptor d = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
    d.entryCount = e.size();
    d.entries = e.data();
    return wgpuDeviceCreateBindGroupLayout(c.device, &d);
}

// The layouts, the sync pipelines and the draw shader module (shared by the replay and by the running game).
bool build_layouts(Context &c) {
    Gpu &G = g_gpu;
    // layouts and pipelines
    WGPUBindGroupLayoutEntry rl[7];
    for (int i = 0; i < 7; ++i) {
        rl[i] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
        rl[i].binding = static_cast<std::uint32_t>(i);
    }
    rl[0].visibility = rl[1].visibility = WGPUShaderStage_Fragment;
    rl[0].buffer.type = rl[1].buffer.type = WGPUBufferBindingType_ReadOnlyStorage;
    rl[2].visibility = static_cast<WGPUShaderStage>(WGPUShaderStage_Vertex | WGPUShaderStage_Fragment);  // the fragment stage reads view.z (the depth load)
    rl[2].buffer.type = WGPUBufferBindingType_Uniform;
    for (int i = 3; i < 6; ++i) {  // graphics memory, swizzle tables and palettes, for the shader that reads textures itself
        rl[i].visibility = WGPUShaderStage_Fragment;
        rl[i].buffer.type = WGPUBufferBindingType_ReadOnlyStorage;
    }
    rl[6].visibility = WGPUShaderStage_Fragment;  // the picture of another target, sampled directly
    rl[6].texture.sampleType = WGPUTextureSampleType_UnfilterableFloat;
    rl[6].texture.viewDimension = WGPUTextureViewDimension_2D;
    G.render_layout = make_layout(c, {rl[0], rl[1], rl[2], rl[3], rl[4], rl[5], rl[6]});
    WGPUBindGroupLayoutEntry ll = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
    ll.binding = 0;
    ll.visibility = WGPUShaderStage_Compute;
    ll.storageTexture.access = WGPUStorageTextureAccess_WriteOnly;
    ll.storageTexture.format = WGPUTextureFormat_RGBA8Unorm;
    ll.storageTexture.viewDimension = WGPUTextureViewDimension_2D;
    G.load_layout = make_layout(c, {ll});
    WGPUBindGroupLayoutEntry sl = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
    sl.binding = 0;
    sl.visibility = WGPUShaderStage_Compute;
    sl.texture.sampleType = WGPUTextureSampleType_UnfilterableFloat;
    sl.texture.viewDimension = WGPUTextureViewDimension_2D;
    G.store_layout = make_layout(c, {sl});
    sl.texture.sampleType = WGPUTextureSampleType_Depth;
    G.store_z_layout = make_layout(c, {sl});
    const auto pipeline_layout = [&](std::initializer_list<WGPUBindGroupLayout> ls) {
        std::vector<WGPUBindGroupLayout> v(ls);
        WGPUPipelineLayoutDescriptor d = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
        d.bindGroupLayoutCount = v.size();
        d.bindGroupLayouts = v.data();
        return wgpuDeviceCreatePipelineLayout(c.device, &d);
    };
    G.render_pl = pipeline_layout({G.render_layout});
    const std::string sync_source = std::string(c.gs_shader) + kHwSyncShader;
    WGPUShaderSourceWGSL ws = WGPU_SHADER_SOURCE_WGSL_INIT;
    ws.code = WGPUStringView{sync_source.data(), sync_source.size()};
    WGPUShaderModuleDescriptor md = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
    md.nextInChain = &ws.chain;
    WGPUShaderModule sync_module = wgpuDeviceCreateShaderModule(c.device, &md);
    WGPUConstantEntry scale = WGPU_CONSTANT_ENTRY_INIT;
    scale.key = sv("SCALE");
    scale.value = static_cast<double>(c.scale);
    for (int which = 0; which < 4; ++which) {
        WGPUComputePipelineDescriptor d = WGPU_COMPUTE_PIPELINE_DESCRIPTOR_INIT;
        d.layout = pipeline_layout({c.layout, which == 0 ? G.load_layout : which == 2 ? G.store_z_layout : G.store_layout});
        d.compute.module = sync_module;
        d.compute.entryPoint = sv(which == 0 ? "hw_load" : which == 1 ? "hw_store" : which == 2 ? "hw_store_z" : "hw_display");
        d.compute.constantCount = 1;
        d.compute.constants = &scale;
        WGPUComputePipeline &p = which == 0 ? G.load : which == 1 ? G.store : which == 2 ? G.store_z : G.display;
        p = wgpuDeviceCreateComputePipeline(c.device, &d);
        if (!p) return false;
    }
    WGPUShaderSourceWGSL rs = WGPU_SHADER_SOURCE_WGSL_INIT;
    std::string hw_source = kHwShader;  // the dual-source entry point needs the device feature: without it that block is cut out
    if (g_dual) hw_source = "enable dual_source_blending;\n" + hw_source;
    else if (const auto b = hw_source.find("//DUAL_BEGIN"), e = hw_source.find("//DUAL_END"); b != std::string::npos && e != std::string::npos) hw_source.erase(b, e - b);
    rs.code = WGPUStringView{hw_source.data(), hw_source.size()};
    WGPUShaderModuleDescriptor rmd = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
    rmd.nextInChain = &rs.chain;
    G.render_module = wgpuDeviceCreateShaderModule(c.device, &rmd);
    return true;
}

bool build_gpu(Context &c) {
    Plan &P = g_plan;
    Gpu &G = g_gpu;
    G.ctx = &c;
    // buffers (the test program builds many frames in one run, so what a build made before is released or reused)
    if (G.vertices) memlog::del_buffer(G.vertices);
    if (G.setups) memlog::del_buffer(G.setups);
    if (G.params) memlog::del_buffer(G.params);
    if (G.group) memlog::del_group(G.group);
    for (auto &kv : G.groups) memlog::del_group(kv.second);
    G.groups.clear();
    G.vertices = make_buffer(c, P.vertices.size() * sizeof(Vtx), WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst);
    if (!P.vertices.empty()) wgpuQueueWriteBuffer(c.queue, G.vertices, 0, P.vertices.data(), P.vertices.size() * sizeof(Vtx));
    G.setups = make_buffer(c, P.setups.size() * 4, WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst);
    wgpuQueueWriteBuffer(c.queue, G.setups, 0, P.setups.data(), P.setups.size() * 4);
    G.params = make_buffer(c, P.params.size() * 256, WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst);
    if (!P.params.empty()) wgpuQueueWriteBuffer(c.queue, G.params, 0, P.params.data(), P.params.size() * 256);
    if (!G.decoded) G.decoded = make_buffer(c, static_cast<std::uint64_t>(g_cache ? kCacheWords : kDecodedWords) * 4, static_cast<WGPUBufferUsage>(WGPUBufferUsage_Storage | (g_cache ? WGPUBufferUsage_CopySrc : 0)));  // (the test program builds many frames, so the buffer is made once)
    {
        WGPUBindGroupEntry e[8] = {};
        WGPUBuffer buffers[8] = {c.vram, c.tables, G.params, G.setups, c.clut, c.scratch, c.cache, G.decoded};
        for (int i = 0; i < 8; ++i) {
            e[i] = WGPU_BIND_GROUP_ENTRY_INIT;
            e[i].binding = static_cast<std::uint32_t>(i);
            e[i].buffer = buffers[i];
            e[i].size = i == 2 ? 256 : WGPU_WHOLE_SIZE;
        }
        WGPUBindGroupDescriptor d = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
        d.layout = c.layout;
        d.entryCount = 8;
        d.entries = e;
        G.group = memlog::new_group(c.device, &d);
    }
    if (!build_layouts(c)) return false;
    // the render textures
    for (Target &t : P.targets) {
        WGPUTextureDescriptor td = WGPU_TEXTURE_DESCRIPTOR_INIT;
        td.dimension = WGPUTextureDimension_2D;
        td.size = {static_cast<std::uint32_t>(t.w), static_cast<std::uint32_t>(t.h), 1};
        td.mipLevelCount = 1;
        td.sampleCount = 1;
        td.format = WGPUTextureFormat_RGBA8Unorm;
        td.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_StorageBinding | WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopySrc;
        t.color = memlog::new_texture(c.device, &td);
        td.format = WGPUTextureFormat_Depth32Float;
        td.usage = WGPUTextureUsage_RenderAttachment;
        td.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding;
        t.depth = memlog::new_texture(c.device, &td);
        if (t.shadow_needed) {
            td.format = WGPUTextureFormat_RGBA8Unorm;
            td.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
            t.shadow = memlog::new_texture(c.device, &td);
            t.shadow_view = wgpuTextureCreateView(t.shadow, nullptr);
        }
        t.color_view = wgpuTextureCreateView(t.color, nullptr);
        t.depth_view = wgpuTextureCreateView(t.depth, nullptr);
        t.view_buf = make_buffer(c, 16, WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst);
        const float wh[4] = {static_cast<float>(t.w), static_cast<float>(t.h), 0, 0};
        wgpuQueueWriteBuffer(c.queue, t.view_buf, 0, wh, sizeof wh);
        WGPUBindGroupDescriptor d = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
        WGPUBindGroupEntry te = WGPU_BIND_GROUP_ENTRY_INIT;
        te.binding = 0;
        te.textureView = t.color_view;
        d.layout = G.load_layout;
        d.entryCount = 1;
        d.entries = &te;
        t.load_group = memlog::new_group(c.device, &d);
        d.layout = G.store_layout;
        t.store_group = memlog::new_group(c.device, &d);
        te.textureView = t.depth_view;
        d.layout = G.store_z_layout;
        t.store_z_group = memlog::new_group(c.device, &d);
    }
    // one render bind group per (target drawn on, target sampled): the sampled picture is a binding, the other bindings are the same
    {
        WGPUTextureDescriptor td = WGPU_TEXTURE_DESCRIPTOR_INIT;
        td.dimension = WGPUTextureDimension_2D;
        td.size = {1, 1, 1};
        td.mipLevelCount = 1;
        td.sampleCount = 1;
        td.format = WGPUTextureFormat_RGBA8Unorm;
        td.usage = WGPUTextureUsage_TextureBinding;
        G.dummy = memlog::new_texture(c.device, &td);
        G.dummy_view = wgpuTextureCreateView(G.dummy, nullptr);
    }
    const auto group_for = [&](std::uint32_t ti, std::uint32_t src) {
        const std::uint64_t key = (static_cast<std::uint64_t>(ti) << 32) | src;
        if (const auto it = G.groups.find(key); it != G.groups.end()) return it->second;
        Target &t = P.targets[ti];
        WGPUBindGroupEntry re[7] = {};
        for (int i = 0; i < 7; ++i) re[i] = WGPU_BIND_GROUP_ENTRY_INIT, re[i].binding = static_cast<std::uint32_t>(i), re[i].size = WGPU_WHOLE_SIZE;
        re[0].buffer = G.setups;
        re[1].buffer = G.decoded;
        re[2].buffer = t.view_buf;
        re[3].buffer = c.vram;
        re[4].buffer = c.tables;
        re[5].buffer = c.clut;
        const std::uint32_t n_targets = static_cast<std::uint32_t>(P.targets.size());
        re[6].textureView = src == ~0u ? G.dummy_view : src >= n_targets ? P.targets[src - n_targets].shadow_view : P.targets[src].color_view;
        WGPUBindGroupDescriptor d = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
        d.layout = G.render_layout;
        d.entryCount = 7;
        d.entries = re;
        return G.groups[key] = memlog::new_group(c.device, &d);
    };
    for (const Op &o : P.ops)
        if (o.kind == kDraw)
            for (std::uint32_t ri = o.run_first; ri < o.run_first + o.run_count; ++ri) group_for(o.target, P.runs[ri].src);
    // all the pipelines the runs use, compiled before anything is timed
    for (const Run &r : P.runs) {
        pipeline_for(r.look, false);
        if (r.look.two_pass) pipeline_for(r.look, true);
        if (r.look.alpha_two && !g_dual) pipeline_for(r.look, false, true);
    }
    return true;
}

enum Skip : unsigned { kSkipCompute = 1, kSkipSync = 2, kSkipDecode = 4, kSkipDraw = 8, kSkipDepthDraw = 16 };

WGPUCommandBuffer encode(std::vector<Sub> &subs, bool restore, unsigned skip, bool interleave) {
    Context &c = *g_gpu.ctx;
    Plan &P = g_plan;
    WGPUCommandEncoder enc = memlog::new_encoder(c.device, nullptr);
    if (restore) wgpuCommandEncoderCopyBufferToBuffer(enc, c.vram_copy, 0, c.vram, 0, c.vram_bytes);
    // the cache follows the plan only in a full replay that starts from the captured memory; a timing variant that leaves work out drops it
    const bool use_cache = g_cache && skip == 0 && restore;
    if (use_cache) hwcache::replay_restore();
    else if (g_cache) hwcache::forget();
    WGPUComputePassEncoder cp = nullptr;
    const auto open = [&] {
        if (!cp) cp = memlog::begin_compute(enc, nullptr);
    };
    const auto close = [&] {
        if (cp) {
            wgpuComputePassEncoderEnd(cp);
            wgpuComputePassEncoderRelease(cp);
            cp = nullptr;
        }
    };
    for (const Op &op : P.ops) {
        if (use_cache) {  // what this step does to memory, as far as the cache's identities go
            if (op.kind == kCompute) hwcache::replay_compute(op.sub, op.cmd);
            else if (op.kind == kStore || op.kind == kStoreZ) {
                const Target &t = P.targets[op.target];
                const auto &pr = P.params[op.param];
                if (op.kind == kStore) hwcache::write_region(t.fbp, t.fbw, t.fpsm, static_cast<int>(pr[1]), static_cast<int>(pr[2]), static_cast<int>(pr[1] + pr[3]) - 1, static_cast<int>(pr[2] + pr[4]) - 1, hwcache::kStore);
                else hwcache::write_region(t.zbp, t.fbw, t.zpsm, static_cast<int>(pr[1]), static_cast<int>(pr[2]), static_cast<int>(pr[1] + pr[3]) - 1, static_cast<int>(pr[2] + pr[4]) - 1, hwcache::kStore);
            }
        }
        switch (op.kind) {
        case kCompute: {
            if (skip & kSkipCompute) break;
            open();
            const Cmd &cmd = subs[op.sub].commands[op.cmd];
            const std::uint32_t offset = cmd.param * 256u;
            wgpuComputePassEncoderSetPipeline(cp, c.pipelines[cmd.pipeline]);
            wgpuComputePassEncoderSetBindGroup(cp, 0, subs[op.sub].group, 1, &offset);
            wgpuComputePassEncoderDispatchWorkgroups(cp, cmd.x, cmd.y, 1);
            break;
        }
        case kLoad:
        case kStore:
        case kStoreZ: {
            if (skip & kSkipSync) break;
            open();
            const Target &t = P.targets[op.target];
            const std::uint32_t offset = op.param * 256u;
            wgpuComputePassEncoderSetPipeline(cp, op.kind == kLoad ? g_gpu.load : op.kind == kStore ? g_gpu.store : g_gpu.store_z);
            wgpuComputePassEncoderSetBindGroup(cp, 0, g_gpu.group, 1, &offset);
            wgpuComputePassEncoderSetBindGroup(cp, 1, op.kind == kLoad ? t.load_group : op.kind == kStore ? t.store_group : t.store_z_group, 0, nullptr);
            wgpuComputePassEncoderDispatchWorkgroups(cp, op.x, op.y, 1);
            break;
        }
        case kDecode: {
            if (skip & kSkipDecode) break;
            open();
            wgpuComputePassEncoderSetPipeline(cp, c.pipelines[c.id_decode]);
            for (std::uint32_t i = 0; i < op.decodes; ++i) {
                if (use_cache) {
                    const hwcache::Lookup L = hwcache::lookup(g_dec_req[op.param + i]);
                    static const bool force = os::env("PS2_GPU_HW_CACHE_FORCE") == "1";  // decode anyway (the cache's answers are still counted): tells a wrong hit from another fault
                    static const int only_psm = std::atoi(os::env("PS2_GPU_HW_CACHE_FORCE_PSM").c_str());  // decode anyway for this pixel format (a test of which formats' covers are wrong)
                    static const int below = std::atoi(os::env("PS2_GPU_HW_CACHE_FORCE_BELOW").c_str());  // decode anyway for plan entries below this number (to find which hit is wrong)
                    static const bool say = os::env("PS2_GPU_HW_CACHE_SAY") == "1";
                    if (say && op.param + i == 1) std::fprintf(stderr, "[hw-cache] param 1: hit %d picture %d refused %d offset %u words %u\n", L.hit, L.picture, L.refused, L.offset, L.words);
                    if (say && L.hit && static_cast<int>(op.param + i) + 1 == below) {
                        const auto &q = g_dec_req[op.param + i];
                        std::fprintf(stderr, "[hw-cache] entry %u: tbp %u tbw %u psm %02X size %ux%u cpsm %u csa %u slot %u offset %u\n", op.param + i, q.tbp, q.tbw, q.tpsm, 1u << q.tw, 1u << q.th, q.cpsm, q.csa, q.clut_slot, L.offset);
                    }
                    if (L.hit && !force && static_cast<int>(op.param + i) >= below && !(only_psm && static_cast<int>(g_dec_req[op.param + i].tpsm) + 1 == only_psm)) continue;  // the decoded copy at its place is still what the memory says
                    if (L.offset != P.params[op.param + i][1]) std::fprintf(stderr, "[hw-cache] BUG: a texture moved from word %u to %u\n", P.params[op.param + i][1], L.offset);
                }
                const std::uint32_t offset = (op.param + i) * 256u;
                const std::uint32_t words = P.params[op.param + i][2], groups = (words + 63) / 64;
                wgpuComputePassEncoderSetBindGroup(cp, 0, g_gpu.group, 1, &offset);
                wgpuComputePassEncoderDispatchWorkgroups(cp, std::min(groups, 1024u), (groups + 1023) / 1024, 1);
            }
            break;
        }
        case kCopy: {
            if (skip & kSkipSync) break;
            close();
            const Target &t = P.targets[op.target];
            const auto &pr = P.params[op.param];
            WGPUTexelCopyTextureInfo from = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT, to = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
            from.texture = t.color;
            from.origin = {pr[0], pr[1], 0};
            to.texture = t.shadow;
            to.origin = {pr[0], pr[1], 0};
            const WGPUExtent3D size = {pr[2], pr[3], 1};
            wgpuCommandEncoderCopyTextureToTexture(enc, &from, &to, &size);
            break;
        }
        case kDraw: {
            if (skip & kSkipDraw) break;
            close();
            Target &t = P.targets[op.target];
            WGPURenderPassColorAttachment color = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
            color.view = t.color_view;
            color.loadOp = WGPULoadOp_Load;
            color.storeOp = WGPUStoreOp_Store;
            WGPURenderPassDepthStencilAttachment depth = WGPU_RENDER_PASS_DEPTH_STENCIL_ATTACHMENT_INIT;
            depth.view = t.depth_view;
            depth.depthLoadOp = op.clear_depth ? WGPULoadOp_Clear : WGPULoadOp_Load;  // the first draw of a frame clears depth (the game's own clear sprite also draws)
            depth.depthStoreOp = WGPUStoreOp_Store;
            depth.depthClearValue = 0.0f;
            WGPURenderPassDescriptor rd = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
            rd.colorAttachmentCount = 1;
            rd.colorAttachments = &color;
            rd.depthStencilAttachment = &depth;
            WGPURenderPassEncoder rp = memlog::begin_render(enc, &rd);
            wgpuRenderPassEncoderSetVertexBuffer(rp, 0, g_gpu.vertices, 0, WGPU_WHOLE_SIZE);
            for (std::uint32_t ri = op.run_first; ri < op.run_first + op.run_count; ++ri) {
                const Run &r = P.runs[ri];
                wgpuRenderPassEncoderSetBindGroup(rp, 0, g_gpu.groups.at((static_cast<std::uint64_t>(op.target) << 32) | r.src), 0, nullptr);
                wgpuRenderPassEncoderSetScissorRect(rp, r.scissor[0], r.scissor[1], r.scissor[2], r.scissor[3]);
                if (r.look.blend && (r.look.src == kConst || r.look.src == kInvConst || r.look.dst == kConst || r.look.dst == kInvConst)) {
                    const WGPUColor bc = {r.look.constant, r.look.constant, r.look.constant, r.look.constant};
                    wgpuRenderPassEncoderSetBlendConstant(rp, &bc);
                }
                const auto draw = [&](bool depth_only, bool alpha_only, std::uint32_t first, std::uint32_t count) {
                    wgpuRenderPassEncoderSetPipeline(rp, pipeline_for(r.look, depth_only, alpha_only));
                    wgpuRenderPassEncoderDraw(rp, count, 1, first, 0);
                };
                const auto shape_draws = [&](std::uint32_t first, std::uint32_t count) {
                    if (r.look.alpha_two && !g_dual && !(skip & kSkipDepthDraw)) draw(false, true, first, count);  // alpha channel first (it only tests depth), then colour (which writes it)
                    draw(false, false, first, count);
                    if (r.look.two_pass && !(skip & kSkipDepthDraw)) draw(true, false, first, count);
                };
                if (!interleave && !r.exact_order) shape_draws(r.first_vertex, r.vertices);
                else {
                    std::uint32_t at = r.first_vertex;
                    for (const std::uint8_t n : r.counts) {  // each shape's draws together: the depth test of the next shape sees the depth this one wrote
                        shape_draws(at, n);
                        at += n;
                    }
                }
            }
            wgpuRenderPassEncoderEnd(rp);
            wgpuRenderPassEncoderRelease(rp);
            break;
        }
        }
    }
    close();
    WGPUCommandBuffer cb = wgpuCommandEncoderFinish(enc, nullptr);
    wgpuCommandEncoderRelease(enc);
    return cb;
}

double time_it(std::vector<Sub> &subs, unsigned skip, bool interleave, unsigned reps, void (*wait)()) {
    Context &c = *g_gpu.ctx;
    std::vector<WGPUCommandBuffer> cmds;
    for (unsigned r = 0; r < reps; ++r) cmds.push_back(encode(subs, true, skip, interleave));
    wait();
    const auto t0 = std::chrono::steady_clock::now();
    wgpuQueueSubmit(c.queue, cmds.size(), cmds.data());
    wait();
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    for (WGPUCommandBuffer cb : cmds) wgpuCommandBufferRelease(cb);
    return ms / reps;
}

}  // namespace


bool match() {
    static const bool on = os::env("PS2_GPU_HW_MATCH") == "1";
    return on;
}

bool left_out(const std::uint32_t *prim, const std::uint32_t *setup) {
    if ((prim[1] & 3u) > 1) return true;
    bool decodable = true;
    if (setup[0] & 2u) {
        const std::uint32_t tw = setup[15] & 0xFFu, th = setup[15] >> 8, mu = (setup[7] >> 24) & 3u, mv = (setup[7] >> 26) & 3u;
        const auto inside = [](std::uint32_t mode, std::uint32_t region, std::uint32_t size) { return mode < 2 || ((region & 0x3FFu) < (1u << size) && (region >> 16) < (1u << size)); };
        decodable = inside(mu, setup[18], tw) && inside(mv, setup[19], th);
    }
    if (!classify(setup, decodable).ok) return true;
    // PS2_GPU_HW_ONLY=<class numbers of PS2_GPU_HW_CLASS, comma separated> keeps only those classes of shape (with MATCH both pictures lack the rest):
    // this is how a wrong picture is bisected, one class at a time. 0 one draw, 1 two draws for depth, 2 two draws for alpha.
    static const std::string only = os::env("PS2_GPU_HW_ONLY");
    if (only.empty()) return false;
    const int c = shape_class(prim, setup);
    return ("," + only + ",").find("," + std::to_string(c) + ",") == std::string::npos;
}

// The classes of shape PS2_GPU_HW_CLASS=1 sorts a frame into: what the card could draw as `classify` sees it (0 to 2),
// then one class per reason the trial leaves a shape out (3 on). Numbers fixed here so gpu.cpp can count and time them.
namespace {
const char *const kClassNames[] = {"hardware, one draw",
                                   "hardware, 2nd draw for depth (alpha test keeps colour only)",
                                   "hardware, 2nd draw for alpha (blend writes the shape's alpha)",
                                   "line or point",
                                   "destination alpha test",
                                   "per-pixel alpha blend",
                                   "scan mask",
                                   "frame format",
                                   "depth format",
                                   "texture window or size",
                                   "partial frame mask",
                                   "alpha test, depth only on failure",
                                   "colour wrap-around (clamp off) with blending",
                                   "blend factor is destination alpha",
                                   "blend reserved value",
                                   "blend equation"};
constexpr int kClassCount = sizeof kClassNames / sizeof *kClassNames;
}  // namespace

int class_count() { return kClassCount; }
const char *class_name(int c) { return kClassNames[c]; }
bool class_is_hardware(int c) { return c < 3; }

int shape_class(const std::uint32_t *prim, const std::uint32_t *setup) {
    if ((prim[1] & 3u) > 1) return 3;
    bool decodable = true;
    if (setup[0] & 2u) {
        const std::uint32_t tw = setup[15] & 0xFFu, th = setup[15] >> 8, mu = (setup[7] >> 24) & 3u, mv = (setup[7] >> 26) & 3u;
        const auto inside = [](std::uint32_t mode, std::uint32_t region, std::uint32_t size) { return mode < 2 || ((region & 0x3FFu) < (1u << size) && (region >> 16) < (1u << size)); };
        decodable = inside(mu, setup[18], tw) && inside(mv, setup[19], th);
    }
    const Look l = classify(setup, decodable);
    if (l.ok) return l.two_pass ? 1 : l.alpha_two ? 2 : 0;
    for (int c = 4; c < kClassCount; ++c)
        if (std::strcmp(kClassNames[c], l.why) == 0) return c;
    return 15;
}

bool class_enabled() {
    static const bool on = os::env("PS2_GPU_HW_CLASS") == "1";
    return on;
}

bool diff_enabled() {
    static const bool on = os::env("PS2_GPU_HW_DIFF") == "1";
    return on;
}

DiffResult &diff_result() {
    static DiffResult r;
    return r;
}

namespace {

// the card path once, from the frame's starting memory, and keep the three memories for the test program (tests/hwalpha.cpp).
void diff_card(Context &ctx, std::vector<Sub> &subs, void (*wait)()) {
    DiffResult &R = diff_result();
    R = DiffResult{};
    const Plan &P = g_plan;
    R.shapes = P.shapes;
    R.shapes_card = P.shapes_ok;
    R.exact_order_shapes = P.exact_shapes;
    for (const auto &[why, t] : P.left_out) R.left_out.emplace_back(why, t.shapes);
    for (const Op &o : P.ops)
        for (std::uint32_t ri = o.kind == kDraw ? o.run_first : 0; o.kind == kDraw && ri < o.run_first + o.run_count; ++ri) {
            const Run &r = P.runs[ri];
            R.draw_calls += 1 + (r.look.two_pass ? 1 : 0) + (r.look.alpha_two && !g_dual ? 1 : 0);
            R.two_pass_shapes += (r.look.two_pass || r.look.alpha_two) ? r.shapes : 0;
        }
    if (P.runs.empty()) return;  // uploads only: memory stays as the painter's replay left it, which is what the next frame starts from
    if (!build_gpu(ctx)) return;
    wait();
    WGPUCommandBuffer cb = encode(subs, true, 0, false);
    wgpuQueueSubmit(ctx.queue, 1, &cb);
    wgpuCommandBufferRelease(cb);
    wait();
    ctx.read_vram(R.card);
    if (ctx.read_before) ctx.read_before(R.before);
    if (ctx.painter_vram) R.painter = *ctx.painter_vram;
    R.valid = true;
}

}

void run(Context &ctx, std::vector<Sub> &subs, unsigned reps, void (*wait)(), void (*shot)(const char *)) {
    if (g_batches.empty()) {
        std::fprintf(stderr, "[hw] no drawing in that frame\n");
        return;
    }
    g_dual = want_dual(ctx.device);  // PS2_GPU_HW_DUAL=0: the fallback (a draw of its own for the alpha channel)
    g_direct = os::env("PS2_GPU_HW_DIRECT") == "1";
    g_probe = static_cast<unsigned>(std::atoi(os::env("PS2_GPU_HW_PROBE").c_str()));
    if (const std::string r = os::env("PS2_GPU_HW_ROUND"); !r.empty()) g_round = static_cast<float>(std::atof(r.c_str()));
    if (g_direct && ctx.scale != 1) {
        std::fprintf(stderr, "[hw] PS2_GPU_HW_DIRECT needs the internal scale 1: using the decode step\n");
        g_direct = false;
    }
    g_linear = os::env("PS2_GPU_HW_LINEAR") != "0" && g_direct;  // the linear targets need the draw shader to read textures itself
    g_eager = os::env("PS2_GPU_HW_EAGER") == "1";
    g_indep = os::env("PS2_GPU_HW_INDEP") != "0";
    g_indep_depth = os::env("PS2_GPU_HW_INDEP") != "3";  // 3: any pixel overlap cuts the segment (the first version of this lane), whatever the depths say
    g_indep_exact = os::env("PS2_GPU_HW_INDEP") == "2";  // 2: not independent runs are drawn shape by shape (exact, slow, the diagnostic)
    g_cache = os::env("PS2_GPU_HW_CACHE") == "1" && !g_direct;
    if (g_cache) hwcache::configure(kCacheWords, 0, false);
    else if (os::env("PS2_GPU_HW_CACHE") == "1") std::fprintf(stderr, "[hw] PS2_GPU_HW_CACHE is for the decode route: with PS2_GPU_HW_DIRECT the textures are read from memory and there is nothing to keep\n");
    g_log = os::env("PS2_GPU_HW_LOG") == "1";
    g_overshoot = os::env("PS2_GPU_HW_OVERSHOOT") != "0";
    g_self = os::env("PS2_GPU_HW_SELF") != "0";
    std::fprintf(stderr, "[hw] targets: %s%s\n", g_linear ? "linear, sampled directly, written back lazily" : "the trial's page-level write-back", g_eager ? ", EAGER (write back and reload after every batch)" : "");
    std::fprintf(stderr, "[hw] textures: %s\n", g_direct ? "read from graphics memory by the draw shader (no decode)" : "decoded first (decode step)");
    if (const std::string in = os::env("PS2_GPU_HW_RANGE_IN"); !in.empty()) {  // alpha ranges a run with PS2_GPU_HW_RANGE_OUT wrote for this same captured frame (the draw route does not decode textures, so it has no ranges of its own)
        if (std::FILE *fp = std::fopen(in.c_str(), "r")) {
            unsigned idx, lo, hi;
            g_range.clear();
            while (std::fscanf(fp, "%u %u %u", &idx, &lo, &hi) == 3) {
                if (idx >= g_range.size()) g_range.resize(idx + 1);
                g_range[idx] = AlphaRange{true, static_cast<std::uint8_t>(lo), static_cast<std::uint8_t>(hi)};
            }
            std::fclose(fp);
            g_range.resize(std::max<std::size_t>(g_range.size(), 1u << 20));
            g_range_on = true;
            std::fprintf(stderr, "[hw] alpha ranges read from %s\n", in.c_str());
        }
    }
    make_plan(ctx, subs);
    if (diff_enabled()) {
        diff_card(ctx, subs, wait);
        return;
    }
    const Plan &P = g_plan;
    std::size_t draws = 0, loads = 0, stores = 0, decodes = 0, computes = 0, two = 0;
    std::size_t used_runs = 0;
    for (const Op &o : P.ops)
        for (std::uint32_t ri = o.kind == kDraw ? o.run_first : 0; o.kind == kDraw && ri < o.run_first + o.run_count; ++ri) {
            const Run &r = P.runs[ri];
            ++used_runs;
            draws += 1 + (r.look.two_pass ? 1 : 0) + (r.look.alpha_two && !g_dual ? 1 : 0), two += (r.look.two_pass || r.look.alpha_two) ? r.shapes : 0;
        }
    for (const Op &o : P.ops) {
        loads += o.kind == kLoad, stores += o.kind == kStore || o.kind == kStoreZ, computes += o.kind == kCompute, decodes += o.kind == kDecode ? o.decodes : 0;
    }
    std::fprintf(stderr, "[hw] plan: %zu shapes in %zu runs (%zu draw calls, %zu shapes drawn twice), %zu render targets, %zu loads, %zu stores, %zu decodes, %zu upload/palette commands\n", static_cast<std::size_t>(P.shapes_ok), used_runs, draws, two, P.targets.size(), loads, stores, decodes, computes);
    {
        static const char *const why[] = {"a draw reads a picture drawn earlier", "an upload or palette load writes that memory", "another render texture over the same pages", "end of frame (the judge needs memory)", "depth buffer (read as a picture or overwritten)", "other command", "eager mode, after every batch", ""};
        unsigned all = 0;
        for (int k = 0; k < 7; ++k) {
            all += P.stores_by[k];
            std::fprintf(stderr, "[hw] write-back to graphics memory: %-48s %3u times, %10.0f pixels\n", why[k], P.stores_by[k], P.store_px_by[k]);
        }
        std::fprintf(stderr, "[hw] write-backs: %u during the frame (the ones a reader forces), %u in all\n", P.early_stores, all);
        std::fprintf(stderr, "[hw] loads from graphics memory into render textures: %u times, %.0f pixels; %u targets started by a clear (no load)\n", P.loads, P.load_px, P.clears);
        std::fprintf(stderr, "[hw] textures sampled straight from a target: %u of the batches' textures, %llu shapes in %u runs (%u with the overshoot ignored); %u textures that are the target drawn on read a copy (%u copies)\n", P.direct_decs, static_cast<unsigned long long>(P.direct_shapes), P.direct_runs, P.overshoot_decs, P.self_decs, P.copies);
    }
    std::fprintf(stderr, "[hw] alpha test, depth at overlaps (what the stamp walk saw): %llu pixels covered again by a later shape of the same segment, %llu of them at exactly the same depth, %llu with the later shape behind\n", static_cast<unsigned long long>(g_ov_pixels), static_cast<unsigned long long>(g_tie_pixels), static_cast<unsigned long long>(g_behind_pixels));
    std::fprintf(stderr, "[hw] alpha test, two draws: %u runs; independent as they are: %llu shapes; with overlaps: %u runs, %llu shapes (cut into %u independent segments, or drawn shape by shape with INDEP=2)\n", P.two_runs, static_cast<unsigned long long>(P.indep_shapes), P.exact_runs, static_cast<unsigned long long>(P.exact_shapes), P.segments);
    std::uint64_t decode_words = 0;
    for (const Op &o : P.ops)
        for (std::uint32_t i = 0; o.kind == kDecode && i < o.decodes; ++i) decode_words += P.params[o.param + i][2];
    std::fprintf(stderr, "[hw] decoded textures: %zu, %llu texels in all (%.1f MB)\n", decodes, static_cast<unsigned long long>(decode_words), static_cast<double>(decode_words) * 4.0 / 1048576.0);
    std::fprintf(stderr, "[hw] covered: %llu of %llu shapes (%.1f%%), %.0f of %.0f pixels (%.1f%%)\n", static_cast<unsigned long long>(P.shapes_ok), static_cast<unsigned long long>(P.shapes), 100.0 * static_cast<double>(P.shapes_ok) / static_cast<double>(std::max<std::uint64_t>(1, P.shapes)), P.pixels_ok, P.pixels,
                 100.0 * P.pixels_ok / std::max(1.0, P.pixels));
    for (const auto &[why, t] : P.left_out) std::fprintf(stderr, "[hw] left out: %-40s shapes %6llu pixels %10.0f (%.1f%%)\n", why.c_str(), static_cast<unsigned long long>(t.shapes), t.pixels, 100.0 * t.pixels / std::max(1.0, P.pixels));
    for (std::size_t i = 0; i < P.targets.size(); ++i) {
        const Target &t = P.targets[i];
        std::fprintf(stderr, "[hw] target %zu: fbp %u fbw %u fpsm 0x%02X, texture %dx%d, box %d,%d-%d,%d\n", i, t.fbp, t.fbw, t.fpsm, t.w, t.h, t.box.x0, t.box.y0, t.box.x1, t.box.y1);
    }
    if (os::env("PS2_GPU_HW_OPS") == "1") {
        std::string line;
        for (const Op &o : P.ops) {
            static const char *const names[] = {"C", "L", "S", "Z", "X", "D", "Y"};
            line += names[o.kind];
            if (o.kind == kLoad || o.kind == kStore || o.kind == kStoreZ || o.kind == kDraw || o.kind == kCopy) line += std::to_string(o.target);
            if (o.kind == kDecode) line += "(" + std::to_string(o.decodes) + ")";
            line += ' ';
        }
        std::fprintf(stderr, "[hw] ops: %s\n", line.c_str());
    }
    if (!build_gpu(ctx)) {
        std::fprintf(stderr, "[hw] could not build the pipelines\n");
        return;
    }
    wait();
    const bool interleave = os::env("PS2_GPU_HW_INTERLEAVE") == "1";
    const bool want_ranges = g_cache && os::env("PS2_GPU_HW_ALPHA_RANGE") == "1";  // a second plan that uses the textures' alpha ranges
    for (int round = 0; round < (want_ranges ? 2 : 1); ++round) {
        if (round == 1) {
            const Plan &P1 = g_plan;
            const std::size_t nsetup = P1.setups.size() / kHwSetup;
            std::map<std::uint32_t, int> asks;  // how often a place of the decoded buffer is decoded in the frame: a range is trusted only for a place decoded once
            for (const Op &o : P1.ops)
                for (std::uint32_t i = 0; o.kind == kDecode && i < o.decodes; ++i) ++asks[P1.params[o.param + i][1]];
            g_range.assign(nsetup, AlphaRange{});
            std::FILE *out_fp = os::env("PS2_GPU_HW_RANGE_OUT").empty() ? nullptr : std::fopen(os::env("PS2_GPU_HW_RANGE_OUT").c_str(), "w");
            std::size_t known = 0, with_test = 0;
            for (std::size_t i = 0; i < nsetup; ++i) {
                const std::uint32_t *w = P1.setups.data() + i * kHwSetup;
                if (!(w[0] & 2u) || !(w[0] & 64u)) continue;
                ++with_test;
                const std::uint32_t place = w[24];
                if (asks[place] != 1) continue;
                const hwcache::Lookup L = hwcache::lookup(hwcache::request_of(w));
                if (L.refused || L.picture || !L.alpha_known || L.offset != place) continue;
                g_range[i] = AlphaRange{true, L.alpha_min, L.alpha_max};
                ++known;
                if (out_fp) std::fprintf(out_fp, "%zu %u %u\n", i, static_cast<unsigned>(L.alpha_min), static_cast<unsigned>(L.alpha_max));
            }
            if (out_fp) std::fclose(out_fp);
            std::fprintf(stderr, "[hw] alpha ranges: %zu of %zu textured alpha-test setups have a trusted range\n", known, with_test);
            g_range_on = true;
            make_plan(ctx, subs);
            if (!build_gpu(ctx)) {
                std::fprintf(stderr, "[hw] could not build the pipelines\n");
                return;
            }
            wait();
            std::size_t draws2 = 0;
            for (const Op &o : P1.ops)
                for (std::uint32_t ri = o.kind == kDraw ? o.run_first : 0; o.kind == kDraw && ri < o.run_first + o.run_count; ++ri) {
                    const Run &r = P1.runs[ri];
                    draws2 += 1 + (r.look.two_pass ? 1 : 0) + (r.look.alpha_two && !g_dual ? 1 : 0);
                }
            std::fprintf(stderr, "[hw] second plan with alpha ranges: %llu alpha-tested shapes tried, %llu always pass, %llu never pass; %zu runs, %zu draw calls, %u alpha-test segments\n", static_cast<unsigned long long>(g_narrow_tried),
                         static_cast<unsigned long long>(g_narrow_pass), static_cast<unsigned long long>(g_narrow_fail), P1.runs.size(), draws2, P1.segments);
        }
        if (g_cache) {  // the first replay finds nothing decoded; the next ones find what the memory has not changed
            hwcache::forget();
            for (int pass = 0; pass < 3; ++pass) {
                hwcache::stats_reset();
                WGPUCommandBuffer first = encode(subs, true, 0, interleave);
                wgpuQueueSubmit(ctx.queue, 1, &first);
                wgpuCommandBufferRelease(first);
                wait();
                hwcache::stats_print(pass == 0 ? "replay 1 (cold)" : pass == 1 ? "replay 2" : "replay 3");
                if (ctx.read_vram) {
                    std::vector<std::uint32_t> mem;
                    ctx.read_vram(mem);
                    std::uint64_t h = 1469598103934665603ull;
                    for (std::uint32_t w : mem) h = (h ^ w) * 1099511628211ull;
                    std::fprintf(stderr, "[cache]   memory after that replay: hash %016llx\n", static_cast<unsigned long long>(h));
                }
            }
        }
        if (g_cache) {  // step 5: the alpha range of every decoded copy, one reduction pass and one read-back
            const std::size_t measured = hwcache::alpha_ranges(ctx.device, ctx.queue, g_gpu.decoded, static_cast<std::uint64_t>(kCacheWords) * 4);
            hwcache::alpha_summary(measured);
        }
    }
    struct Variant {
        const char *name;
        unsigned skip;
        bool interleave;
    };
    const unsigned all = kSkipCompute | kSkipSync | kSkipDecode | kSkipDraw;
    const Variant variants[] = {{"hw everything", 0, interleave},
                                {"hw everything, depth draws left out", kSkipDepthDraw, interleave},
                                {"hw everything, each shape's two draws together", 0, true},
                                {"hw drawing only", all & ~kSkipDraw, interleave},
                                {"hw decode only", all & ~kSkipDecode, interleave},
                                {"hw load and store only", all & ~kSkipSync, interleave},
                                {"hw upload and palette only", all & ~kSkipCompute, interleave},
                                {"hw without drawing", kSkipDraw, interleave},
                                {"hw without decode (the textures cost nothing)", kSkipDecode, interleave},
                                {"hw everything, again (the first reading runs on a cold card)", 0, interleave}};
    time_it(subs, 0, interleave, 3, wait);  // warm up
    for (const Variant &v : variants) {
        std::vector<double> t;
        for (int trial = 0; trial < 5; ++trial) t.push_back(time_it(subs, v.skip, v.interleave, reps, wait));
        std::sort(t.begin(), t.end());
        std::fprintf(stderr, "[bench] %-48s %8.3f ms per frame (median of 5 trials %.3f)\n", v.name, t.front(), t[t.size() / 2]);
    }
    // one more copy for the picture
    WGPUCommandBuffer cb = encode(subs, true, 0, interleave);
    wgpuQueueSubmit(ctx.queue, 1, &cb);
    wgpuCommandBufferRelease(cb);
    wait();
    if (shot) shot("hw");
    if (ctx.read_vram) {  // the judge: the whole memory after one replay of the card path (the same hash in lazy and in eager mode, or the bookkeeping is wrong)
        std::vector<std::uint32_t> mem;
        ctx.read_vram(mem);
        std::uint64_t h = 1469598103934665603ull;
        for (std::uint32_t w : mem) h = (h ^ w) * 1099511628211ull;
        std::fprintf(stderr, "[hw] memory after the card path: hash %016llx%s\n", static_cast<unsigned long long>(h), g_eager ? " (eager)" : g_linear ? " (lazy)" : " (trial)");
        if (ctx.painter_vram && ctx.painter_vram->size() == mem.size()) {
            std::size_t words = 0;
            std::bitset<512> pages;
            for (std::size_t i = 0; i < mem.size(); ++i)
                if (mem[i] != (*ctx.painter_vram)[i]) ++words, pages.set((i / 2048) & 511u);
            std::fprintf(stderr, "[hw] memory against the painter's: %zu words differ in %zu pages (the painter leaves out nothing here unless MATCH is set; the card rounds differently)\n", words, pages.count());
        }
    }
}


// ======================================================================================================================================================
// the running game draws its plain shapes through the card path.
//
// The model of the card's pictures. A *target* is a picture the card holds for one colour buffer (base, width, format); it carries a depth
// picture of the same size that is paired with one depth buffer at a time. Each picture has three rectangles: `valid` (the texture holds the real contents there),
// `stale` (memory is newer than the texture there: someone else wrote it) and `drawn` (the texture is newer than memory there: a write-back writes only this).
// Everything that reads graphics memory calls live_reader / live_writer / live_painter at the point of the command stream where it happens (gpu.cpp); the
// calls put write-backs (and, at the next draw, loads) into the card's list of work, which submit() runs in order with the compute commands.
// ======================================================================================================================================================
namespace live {

using Pg = std::bitset<512>;

bool inter(const Rect &a, const Rect &b) { return !a.empty() && !b.empty() && a.x0 <= b.x1 && b.x0 <= a.x1 && a.y0 <= b.y1 && b.y0 <= a.y1; }
bool covers(const Rect &a, const Rect &b) { return !a.empty() && !b.empty() && a.x0 <= b.x0 && a.y0 <= b.y0 && a.x1 >= b.x1 && a.y1 >= b.y1; }
Rect unite(Rect a, const Rect &b) {
    if (!b.empty()) a.add(b.x0, b.y0, b.x1, b.y1);
    return a;
}

// The GPU objects of one size of one target. Work that is already in the list keeps pointing at the Gen it was made for when the target grows.
struct Gen {
    int w = 0, h = 0;
    std::uint64_t id = 0;  // never reused, so a cached bind group can name the picture it samples
    WGPUTexture color = nullptr, depth = nullptr;
    WGPUTextureView color_view = nullptr, depth_view = nullptr;
    WGPUTexture shadow = nullptr;  // a copy of a box of the colour picture, for batches that sample the picture they draw on (made on first use)
    WGPUTextureView shadow_view = nullptr;
    WGPUBuffer view_buf = nullptr;
    WGPUBindGroup load_group = nullptr, store_group = nullptr, store_z_group = nullptr, render_group = nullptr;
    std::uint64_t render_gen = 0;
    struct SrcGroup {
        std::uint64_t src = 0, gen = 0;  // the id of the sampled picture (this one's own id: the shadow copy), and LV.render_gen when it was made
        WGPUBindGroup group = nullptr;
    };
    SrcGroup src_groups[4];  // render bind groups whose texture is another picture or the shadow copy, most recent first
    ~Gen() {
        for (SrcGroup &s : src_groups)
            if (s.group) memlog::del_group(s.group);
        if (shadow_view) wgpuTextureViewRelease(shadow_view);
        if (shadow) memlog::del_texture(shadow);
        if (load_group) memlog::del_group(load_group);
        if (store_group) memlog::del_group(store_group);
        if (store_z_group) memlog::del_group(store_z_group);
        if (render_group) memlog::del_group(render_group);
        if (view_buf) memlog::del_buffer(view_buf);
        if (color_view) wgpuTextureViewRelease(color_view);
        if (depth_view) wgpuTextureViewRelease(depth_view);
        if (color) memlog::del_texture(color);
        if (depth) memlog::del_texture(depth);
    }
};

struct Tgt {
    std::uint32_t fbp = 0, fbw = 0, fpsm = 0;
    bool paired = false;                // the depth picture belongs to the depth buffer (zbp, zpsm)
    std::uint32_t zbp = 0, zpsm = 0;
    std::uint32_t fmt[kSetupWords] = {};  // a setup of this target's batches: the format words the load and store shaders read
    std::unique_ptr<Gen> g;
    Rect valid, stale, drawn, zvalid, zstale, zdrawn;
    Pg pages, dpages, zpages, zdpages;  // pages of memory the rectangles above cover (valid, drawn; the depth's valid, drawn)
    std::uint64_t used = 0;
    std::uint32_t seq = 0, zseq = 0;    // the order of the last colour and depth draw, so write-backs keep the chip's order
    std::uint64_t sync_serial = ~0ull;  // the submit for which sync_rec is a copy of `fmt` in the setups buffer
    std::uint32_t sync_rec = 0;
};

struct LOp {
    enum Kind { Load, Store, StoreZ, LoadZ, Draw, Display, Copy } kind = Load;
    std::uint32_t after = 0;  // number of compute commands of the submit that run before it
    Gen *g = nullptr;
    Gen *src = nullptr;       // Draw: the picture the shapes marked in setup word 41 sample (g itself: its shadow copy), or none
    std::uint32_t param = 0, x = 0, y = 0, run_first = 0, run_count = 0;
    std::uint32_t rect[4] = {};  // LoadZ, Copy: x, y, width, height
};

struct Stats {
    std::uint64_t card_batches = 0, card_shapes = 0, painter_batches = 0, painter_shapes = 0, runs = 0, draws = 0;
    std::uint64_t loads = 0, load_px = 0, stores = 0, store_px = 0, store_zs = 0, load_zs = 0, growths = 0, evictions = 0, display_card = 0, display_memory = 0, state_drops = 0;
    std::uint64_t by_reader = 0, by_writer = 0, by_overlap = 0, by_read = 0, by_load = 0, by_grow = 0, by_painter = 0, by_eager = 0, by_other = 0;
    std::uint64_t fallbacks = 0;
    std::uint64_t fed_other = 0, fed_self = 0, copy_px = 0, fed_refused = 0, top_byte_kept = 0;  // batches that sampled another picture, or a copy of their own, instead of a write-back; refused: a picture was there but a rule said no
    std::map<std::string, std::uint64_t> refused;
    std::map<std::string, std::pair<std::uint64_t, std::uint64_t>> kinds;  // PS2_GPU_HW_LOG=1: what the card draws (target, depth use, texture, blend), batches and shapes
};

struct Live {
    bool on = false;
    Context ctx{};
    std::vector<std::unique_ptr<Tgt>> targets;
    std::vector<std::unique_ptr<Gen>> retired;
    std::vector<Vtx> vertices;
    std::vector<std::uint32_t> setups;                   // kHwSetup words per record: the batches' setups, then the copies the sync shaders read
    std::vector<std::array<std::uint32_t, 64>> params;   // 256 bytes each, a dynamic offset
    std::vector<Run> runs;
    std::vector<LOp> ops;
    std::size_t cursor = 0;
    std::uint32_t at = 0;                                // compute commands queued so far in this submit (set by gpu.cpp before every call)
    std::uint64_t serial = 1, use = 0, render_gen = 1;
    std::uint32_t seq = 0;
    WGPUBuffer vbuf = nullptr, sbuf = nullptr, pbuf = nullptr, dummy_buf = nullptr;
    std::uint64_t vcap = 0, scap = 0, pcap = 0;
    WGPUBindGroup group = nullptr;
    WGPUTexture dummy_tex = nullptr;
    WGPUTextureView dummy_view = nullptr;
    WGPURenderPipeline load_z = nullptr;
    std::uint64_t bytes = 0, cap_bytes = 768ull << 20;
    std::map<std::uint64_t, std::pair<int, std::uint64_t>> heights;  // (fbp, fbw, fpsm) -> the tallest picture seen and when ("height memory")
    Stats st;
} LV;

std::uint32_t add_p(std::initializer_list<std::uint32_t> values) {
    std::array<std::uint32_t, 64> r{};
    std::size_t i = 0;
    for (std::uint32_t v : values) r[i++] = v;
    LV.params.push_back(r);
    return static_cast<std::uint32_t>(LV.params.size() - 1);
}

std::uint64_t g_before_draw[11] = {};  // PS2_GPU_HW_LOG=1: the op before each draw (kinds 0-5; 6-9: a draw, same/other picture, same/later point; 10: a copy)
LOp &push_op(LOp::Kind kind, Gen *g) {
    LV.ops.emplace_back();
    LOp &o = LV.ops.back();
    o.kind = kind;
    o.after = LV.at;
    o.g = g;
    return o;
}

// A copy of the target's format words in the setups buffer (the sync shaders read them from there); one per target and submit.
std::uint32_t sync_rec(Tgt &t) {
    if (t.sync_serial == LV.serial) return t.sync_rec;
    t.sync_rec = static_cast<std::uint32_t>(LV.setups.size() / kHwSetup);
    LV.setups.resize(LV.setups.size() + kHwSetup, 0);
    std::memcpy(LV.setups.data() + static_cast<std::size_t>(t.sync_rec) * kHwSetup, t.fmt, sizeof t.fmt);
    t.sync_serial = LV.serial;
    return t.sync_rec;
}

Rect clipped(const Gen &g, const Rect &r) {
    Rect d;
    if (!r.empty()) d.add(std::max(r.x0, 0), std::max(r.y0, 0), std::min(r.x1, g.w - 1), std::min(r.y1, g.h - 1));
    return d;
}

void sync_op(LOp::Kind kind, Tgt &t, const Rect &d) {
    LOp &o = push_op(kind, t.g.get());
    o.param = add_p({sync_rec(t) * kHwSetup, static_cast<std::uint32_t>(d.x0), static_cast<std::uint32_t>(d.y0), static_cast<std::uint32_t>(d.x1 - d.x0 + 1), static_cast<std::uint32_t>(d.y1 - d.y0 + 1)});
    o.x = static_cast<std::uint32_t>((d.x1 - d.x0 + 8) / 8);
    o.y = static_cast<std::uint32_t>((d.y1 - d.y0 + 8) / 8);
    if (kind == LOp::LoadZ) o.rect[0] = static_cast<std::uint32_t>(d.x0), o.rect[1] = static_cast<std::uint32_t>(d.y0), o.rect[2] = static_cast<std::uint32_t>(d.x1 - d.x0 + 1), o.rect[3] = static_cast<std::uint32_t>(d.y1 - d.y0 + 1);
}

// ---- write-backs: the card's picture goes to memory
void store_color(Tgt &t, std::uint64_t &why) {
    const Rect d = t.g ? clipped(*t.g, t.drawn) : Rect();
    if (!d.empty()) {
        sync_op(LOp::Store, t, d);
        ++LV.st.stores, ++why;
        LV.st.store_px += static_cast<std::uint64_t>(d.x1 - d.x0 + 1) * (d.y1 - d.y0 + 1);
        if (g_log) std::fprintf(stderr, "[hw-live] store colour fbp %u fbw %u psm %02X %d,%d-%d,%d\n", t.fbp, t.fbw, t.fpsm, d.x0, d.y0, d.x1, d.y1);
    }
    t.drawn = Rect();
    t.dpages.reset();
}

void store_depth(Tgt &t, std::uint64_t &why) {
    const Rect d = t.g ? clipped(*t.g, t.zdrawn) : Rect();
    if (!d.empty()) {
        sync_op(LOp::StoreZ, t, d);
        ++LV.st.store_zs, ++why;
        if (g_log) std::fprintf(stderr, "[hw-live] store depth zbp %u psm %02X %d,%d-%d,%d\n", t.zbp, t.zpsm, d.x0, d.y0, d.x1, d.y1);
    }
    t.zdrawn = Rect();
    t.zdpages.reset();
}

// Everything the card holds newer in these pages of memory goes back, in the order the chip drew it, except the pictures `skip(target, kind)` (kind 0 colour, 1 depth) leaves on the card.
template <class Skip>
void write_back_if(const Pg &p, std::uint64_t &why, Skip skip) {
    struct E {
        std::uint32_t seq;
        int kind;
        Tgt *t;
    };
    std::vector<E> v;
    for (auto &t : LV.targets) {
        if (!t->drawn.empty() && (t->dpages & p).any() && !skip(*t, 0)) v.push_back({t->seq, 0, t.get()});
        if (!t->zdrawn.empty() && (t->zdpages & p).any() && !skip(*t, 1)) v.push_back({t->zseq, 1, t.get()});
    }
    std::sort(v.begin(), v.end(), [](const E &a, const E &b) { return a.seq != b.seq ? a.seq < b.seq : a.kind < b.kind; });
    for (const E &e : v) e.kind == 0 ? store_color(*e.t, why) : store_depth(*e.t, why);
}

// `keep`: the colour picture of that target stays on the card.
void write_back(const Pg &p, std::uint64_t &why, const Tgt *keep = nullptr) {
    write_back_if(p, why, [keep](const Tgt &t, int kind) { return kind == 0 && &t == keep; });
}

void store_all(Tgt &t, std::uint64_t &why) {
    if (t.seq <= t.zseq) {
        store_color(t, why);
        store_depth(t, why);
    } else {
        store_depth(t, why);
        store_color(t, why);
    }
}

void forget_color(Tgt &t) {
    t.valid = t.stale = t.drawn = Rect();
    t.pages.reset();
    t.dpages.reset();
}
void forget_depth(Tgt &t) {
    t.zvalid = t.zstale = t.zdrawn = Rect();
    t.zpages.reset();
    t.zdpages.reset();
}

// ---- the GPU objects of a target
std::unique_ptr<Gen> make_gen(int w, int h) {
    Context &c = *g_gpu.ctx;
    Gpu &G = g_gpu;
    auto g = std::make_unique<Gen>();
    static std::uint64_t ids = 0;
    g->w = w, g->h = h, g->id = ++ids;
    WGPUTextureDescriptor td = WGPU_TEXTURE_DESCRIPTOR_INIT;
    td.dimension = WGPUTextureDimension_2D;
    td.size = {static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h), 1};
    td.mipLevelCount = 1;
    td.sampleCount = 1;
    td.format = WGPUTextureFormat_RGBA8Unorm;
    td.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_StorageBinding | WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopySrc;
    g->color = memlog::new_texture(c.device, &td);
    td.format = WGPUTextureFormat_Depth32Float;
    td.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding;
    g->depth = memlog::new_texture(c.device, &td);
    g->color_view = wgpuTextureCreateView(g->color, nullptr);
    g->depth_view = wgpuTextureCreateView(g->depth, nullptr);
    g->view_buf = make_buffer(c, 16, WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst);
    const float wh[4] = {static_cast<float>(w), static_cast<float>(h), 0, 0};
    wgpuQueueWriteBuffer(c.queue, g->view_buf, 0, wh, sizeof wh);
    WGPUBindGroupDescriptor d = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    WGPUBindGroupEntry te = WGPU_BIND_GROUP_ENTRY_INIT;
    te.binding = 0;
    te.textureView = g->color_view;
    d.entryCount = 1;
    d.entries = &te;
    d.layout = G.load_layout;
    g->load_group = memlog::new_group(c.device, &d);
    d.layout = G.store_layout;
    g->store_group = memlog::new_group(c.device, &d);
    te.textureView = g->depth_view;
    d.layout = G.store_z_layout;
    g->store_z_group = memlog::new_group(c.device, &d);
    LV.bytes += static_cast<std::uint64_t>(w) * h * 8;
    return g;
}

void make_shadow(Gen &g) {
    if (g.shadow) return;
    WGPUTextureDescriptor td = WGPU_TEXTURE_DESCRIPTOR_INIT;
    td.dimension = WGPUTextureDimension_2D;
    td.size = {static_cast<std::uint32_t>(g.w), static_cast<std::uint32_t>(g.h), 1};
    td.mipLevelCount = 1;
    td.sampleCount = 1;
    td.format = WGPUTextureFormat_RGBA8Unorm;
    td.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
    g.shadow = memlog::new_texture(g_gpu.ctx->device, &td);
    g.shadow_view = wgpuTextureCreateView(g.shadow, nullptr);
    LV.bytes += static_cast<std::uint64_t>(g.w) * g.h * 4;
}

// Cached bind groups that sample picture `id` are dropped (the picture is going away; a cached group would keep its texture alive).
void purge_src(std::uint64_t id) {
    for (auto &t : LV.targets)
        if (t->g)
            for (Gen::SrcGroup &s : t->g->src_groups)
                if (s.group && s.src == id) memlog::del_group(s.group), s = Gen::SrcGroup();
}

void retire(Tgt &t) {
    if (!t.g) return;
    if (t.g->shadow) LV.bytes -= static_cast<std::uint64_t>(t.g->w) * t.g->h * 4;
    LV.bytes -= static_cast<std::uint64_t>(t.g->w) * t.g->h * 8;
    LV.retired.push_back(std::move(t.g));
}

// The rows the picture of a target needs: this batch's, rounded up to a page, or the tallest seen for this address in the last 600 frames.
std::uint32_t page_rows(std::uint32_t psm) { return (psm == 2 || psm == 0x0A) ? 64 : 32; }

void ensure_gen(Tgt &t, int need_h, std::uint64_t frame) {
    const int w = static_cast<int>(t.fbw * 64);
    const std::uint64_t key = (static_cast<std::uint64_t>(t.fbp) << 16) | (static_cast<std::uint64_t>(t.fbw) << 8) | t.fpsm;
    const int rows = static_cast<int>(page_rows(t.fpsm));
    int h = std::min(2048, (need_h + rows - 1) / rows * rows);
    if (const auto it = LV.heights.find(key); it != LV.heights.end() && frame < it->second.second + 600) h = std::max(h, it->second.first);
    if (t.g && t.g->h >= need_h && t.g->w == w) return;
    if (t.g) {  // growth: what the old picture holds newer goes to memory, the new picture is loaded from there
        ++LV.st.growths;
        store_all(t, LV.st.by_grow);
        retire(t);
        forget_color(t);
        forget_depth(t);
    }
    auto &slot = LV.heights[key];
    slot = {std::max(h, slot.first), frame};
    t.g = make_gen(w, std::max(h, t.g ? t.g->h : 0));
}

Tgt *find_target(std::uint32_t fbp, std::uint32_t fbw, std::uint32_t fpsm) {
    for (auto &t : LV.targets)
        if (t->fbp == fbp && t->fbw == fbw && t->fpsm == fpsm) return t.get();
    return nullptr;
}

void evict_for(Tgt *keep) {
    while (LV.bytes > LV.cap_bytes) {
        Tgt *old = nullptr;
        for (auto &t : LV.targets)
            if (t.get() != keep && t->g && (!old || t->used < old->used)) old = t.get();
        if (!old) return;
        ++LV.st.evictions;
        store_all(*old, LV.st.by_other);
        retire(*old);
        LV.targets.erase(std::remove_if(LV.targets.begin(), LV.targets.end(), [&](const std::unique_ptr<Tgt> &p) { return p.get() == old; }), LV.targets.end());
    }
}

Tgt &target_of(std::uint32_t fbp, std::uint32_t fbw, std::uint32_t fpsm, const std::uint32_t *setup) {
    if (Tgt *t = find_target(fbp, fbw, fpsm)) return *t;
    auto t = std::make_unique<Tgt>();
    t->fbp = fbp, t->fbw = fbw, t->fpsm = fpsm;
    std::memcpy(t->fmt, setup, sizeof t->fmt);
    LV.targets.push_back(std::move(t));
    return *LV.targets.back();
}

// Does the card take shapes drawn with this setup? Counted by the reason it does not.
std::uint64_t g_waited_routes = 0;  // setups the painter drew only because their pipelines were still being made
bool g_async_pipes = false;  // PS2_GPU_HW_ASYNC=0 builds a pipeline on first use and waits for it
bool g_route_waits = false;  // the last pipelines_ok answered false only because a pipeline is still being made
// 1 ready, 0 failed, -1 still being made
int pipe_state(const Look &l, bool depth_only, bool alpha_only) {
    if (pipeline_for(l, depth_only, alpha_only, g_async_pipes)) return 1;
    return g_pending.count(pipe_key(l, depth_only, alpha_only)) ? -1 : 0;
}
bool pipelines_ok(const Look &l) {
    g_route_waits = false;
    int worst = 1;
    const auto take = [&](int st) { worst = st == 0 ? 0 : (worst == 0 ? 0 : std::min(worst, st)); };
    take(pipe_state(l, false, false));
    if (l.two_pass) take(pipe_state(l, true, false));
    if (l.alpha_two && !g_dual) take(pipe_state(l, false, true));
    g_route_waits = worst == -1;
    return worst == 1;
}

// The recorded keys (games/<name>/hw/pipelines.txt, beside the recorded vector programs; PS2_HW_PIPELINES=<file> names another): one 8-digit hex key per line, the fields of pipe_key.
void load_pipeline_keys() {
    std::string path = os::env("PS2_HW_PIPELINES");
    if (path.empty()) {
        const std::string rec = os::env("PS2_VU_RECORD");  // the folder of the recorded vector programs (the window sets it): the list lies next to it
        if (rec.empty()) return;
        std::error_code ec;
        const std::filesystem::path dir = std::filesystem::path(rec).parent_path() / "hw";
        std::filesystem::create_directories(dir, ec);
        path = (dir / "pipelines.txt").string();
    }
    g_pipes_path = path;
    std::ifstream in(path);
    std::vector<std::uint32_t> keys;
    for (std::string line; std::getline(in, line);) {
        char *end = nullptr;
        const unsigned long k = std::strtoul(line.c_str(), &end, 16);
        if (end != line.c_str()) keys.push_back(static_cast<std::uint32_t>(k));
    }
    for (const std::uint32_t k : keys) g_pipes_known.insert(k);
    unsigned made = 0;
    for (const std::uint32_t k : keys) {
        Look l;
        l.ok = true;
        l.blend = k & 1u, l.src = (k >> 1) & 7u, l.dst = (k >> 4) & 7u, l.op = (k >> 7) & 3u, l.ztest = (k >> 9) & 3u, l.zwrite = (k >> 11) & 1u, l.mask = (k >> 12) & 15u, l.two_pass = (k >> 16) & 1u;
        const bool depth_only = (k >> 17) & 1u, alpha_only = (k >> 18) & 1u, dual = (k >> 19) & 1u;
        l.alpha_two = dual;  // the key only holds "dual" (alpha_two with dual-source blending); a key made without it has alpha_only set or alpha_two is not part of it
        l.premul = (k >> 20) & 3u, l.fix = (k >> 22) & 1u;
        if (pipe_key(l, depth_only, alpha_only) != k) continue;  // made on a device without dual-source blending: not this one's key
        if (!g_gpu.pipelines.count(k)) pipeline_for(l, depth_only, alpha_only, true), ++made;
    }
    // at the start the list is waited for, so that the first frames are drawn as they will be every time
    while (!g_pending.empty()) {
        drain_pipelines();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (made) std::fprintf(stderr, "[hw-live] %u card pipelines made ahead from %s\n", made, path.c_str());
}

}  // namespace live

namespace live {

bool decodable_of(const std::uint32_t *s) {
    if (!(s[0] & 2u)) return true;
    const std::uint32_t tw = s[15] & 0xFFu, th = s[15] >> 8, mu = (s[7] >> 24) & 3u, mv = (s[7] >> 26) & 3u;
    const auto inside = [](std::uint32_t mode, std::uint32_t region, std::uint32_t size) { return mode < 2 || ((region & 0x3FFu) < (1u << size) && (region >> 16) < (1u << size)); };
    return inside(mu, s[18], tw) && inside(mv, s[19], th);
}

Rect meet(const Rect &a, const Rect &b) {
    Rect r;
    if (inter(a, b)) r.add(std::max(a.x0, b.x0), std::max(a.y0, b.y0), std::min(a.x1, b.x1), std::min(a.y1, b.y1));
    return r;
}

std::uint64_t area(const Rect &r) { return r.empty() ? 0 : static_cast<std::uint64_t>(r.x1 - r.x0 + 1) * static_cast<std::uint64_t>(r.y1 - r.y0 + 1); }

// One box of a colour picture loaded from memory, after whatever the card holds newer in those pages has gone back (`keep`: that picture's colour need not, see below).
void load_box(Tgt &t, const Rect &r, const Tgt *keep) {
    if (r.empty()) return;
    Pg pn;
    pages_of(t.fbp, t.fbw, t.fpsm, r.x0, r.y0, r.x1, r.y1, pn);
    write_back(pn, LV.st.by_load, keep);
    sync_op(LOp::Load, t, r);
    ++LV.st.loads;
    LV.st.load_px += area(r);
    t.pages |= pn;
}

// The colour picture of a target is made current in `need`. A stale part (memory is newer) is loaded again, after whatever the card holds newer in those pages has been
// written back, this picture's own included. A part that was never loaded makes the valid rectangle grow: only the new strips around the old valid rectangle are loaded
// (at most four boxes), and this picture's own drawn part needs no write-back for them, because it lies inside the old valid rectangle (a draw makes its box valid first)
// and a load reads only the pixels of its boxes. (Loading the whole grown rectangle would also write the picture back and, for 16-bit colour, cut what it holds to 5 bits.)
void make_current(Tgt &t, const Rect &need) {
    const bool grow = !covers(t.valid, need);
    if (!grow && !inter(t.stale, need)) return;
    if (!t.stale.empty()) load_box(t, meet(t.stale, t.valid), nullptr);
    t.stale = Rect();
    if (!grow) return;
    const Rect u = unite(t.valid, need), v = t.valid;
    if (v.empty()) {
        load_box(t, u, &t);
    } else {
        Rect top, bottom, left, right;
        if (u.y0 < v.y0) top.add(u.x0, u.y0, u.x1, v.y0 - 1);
        if (u.y1 > v.y1) bottom.add(u.x0, v.y1 + 1, u.x1, u.y1);
        if (u.x0 < v.x0) left.add(u.x0, v.y0, v.x0 - 1, v.y1);
        if (u.x1 > v.x1) right.add(v.x1 + 1, v.y0, u.x1, v.y1);
        load_box(t, top, &t);
        load_box(t, bottom, &t);
        load_box(t, left, &t);
        load_box(t, right, &t);
    }
    t.valid = u;
}

void make_current_z(Tgt &t, const Rect &need) {
    Rect from;
    bool grow = false;
    if (!covers(t.zvalid, need)) from = unite(t.zvalid, need), grow = true;
    else if (inter(t.zstale, need)) from = meet(t.zstale, t.zvalid);
    else return;
    if (!from.empty()) {
        Pg pn;
        pages_of(t.zbp, t.fbw, t.zpsm, from.x0, from.y0, from.x1, from.y1, pn);
        write_back(pn, LV.st.by_load);
        sync_op(LOp::LoadZ, t, from);
        ++LV.st.load_zs;
        t.zpages |= pn;
    }
    if (grow) t.zvalid = from;
    t.zstale = Rect();
}

// Does the batch hold a run (consecutive shapes of one look and one scissor, as batch() builds them) that needs the independence check and has more than g_indep_max shapes?
bool too_heavy(const LiveBatch &b, const std::vector<Look> &looks, std::uint32_t nset) {
    Look cl;
    bool open = false, flat = true;
    std::uint32_t sc0 = 0, sc1 = 0, shapes = 0;
    std::uint64_t pix = 0;  // the pixels of the boxes of the run's shapes, what the check costs (about a nanosecond for each)
    float flat_z = 0;
    const auto heavy = [&] { return open && shapes > g_indep_max && pix > g_indep_pix && split_draws(cl) && cl.ztest != 0 && !(flat && cl.ztest <= 1); };
    for (std::uint32_t i = 0; i < b.prim_count; ++i) {
        const std::uint32_t *r = b.prims + static_cast<std::size_t>(i) * kPrimWords;
        if (r[0] >= nset) return false;  // the main loop refuses the batch
        const Look &l = looks[r[0]];
        if (l.skip) continue;
        const std::uint32_t *s = b.setups + static_cast<std::size_t>(r[0]) * kSetupWords;
        if (!open || !(cl == l) || s[10] != sc0 || s[11] != sc1) {
            if (heavy()) return true;
            cl = l, sc0 = s[10], sc1 = s[11], open = true, flat = true, shapes = 0, pix = 0;
        }
        const float z = static_cast<float>(r[24]);
        if (++shapes == 1) flat_z = z;
        pix += static_cast<std::uint64_t>((r[3] & 0xFFFFu) - (r[2] & 0xFFFFu) + 1u) * ((r[3] >> 16) - (r[2] >> 16) + 1u);
        if (z != flat_z || ((r[1] & 3u) == 0 && (f(r[25]) != 0.0f || f(r[26]) != 0.0f))) flat = false;
        if (shapes > g_indep_max && pix > g_indep_pix && (!flat || cl.ztest > 1) && split_draws(cl) && cl.ztest != 0) return true;  // decided: a run that is not flat stays so
    }
    return heavy();
}

// classify() is a function of the setup words alone: remembered by content (a batch has one to three setups and the same few hundred recur every frame)
// a setup whose alpha test is decided per pixel and whose draw therefore splits (colour, then depth) may have shapes that pass it everywhere; those are drawn as the same setup
// without the test (one pass, no cut). `refinable` says the setup is worth looking at, `alt_look_of` gives the look without the test.
bool refinable(const std::uint32_t *s, const Look &l) {
    if (!l.ok || l.skip || !split_draws(l)) return false;
    const std::uint32_t atst = s[7] & 7u;
    static const std::string only_tbp = os::env("PS2_GPU_HW_PAL_TBP");  // debugging aid: only textures at these bases (comma separated) are refined
    if (!only_tbp.empty() && ("," + only_tbp + ",").find("," + std::to_string(s[12]) + ",") == std::string::npos) return false;
    static const std::string only_tfx = os::env("PS2_GPU_HW_PAL_TFX");  // debugging aid: only this texture function (0 modulate, 1 decal, 2 highlight, 3 highlight2) is refined
    if (!only_tfx.empty() && static_cast<std::uint32_t>(std::atoi(only_tfx.c_str())) != ((s[7] >> 20) & 3u)) return false;
    // Only the texture functions in which the texture's alpha alone is the pixel's alpha (decal, highlight 2) are refined. With modulate and highlight the vertex alpha takes part, and refining
    // those left 15 words of the ship's memory different from the strict reference (the strict-order run of the same build) that the range of the vertex alphas does not explain: not refined.
    static const bool vertex_too = os::env("PS2_GPU_HW_PAL_VERTEX") == "1";
    const std::uint32_t tfx = (s[7] >> 20) & 3u;
    if (!vertex_too && !((s[0] & 2u) && (s[0] & 8192u) && (tfx == 1 || tfx == 3))) return false;
    return (s[0] & 64u) && atst != 0 && atst != 1 && g_pal_refine;
}
bool alt_look_of(const std::uint32_t *s, Look &alt) {
    std::uint32_t c[kSetupWords];
    std::memcpy(c, s, sizeof c);
    c[0] &= ~64u;
    alt = classify(c, decodable_of(c));
    return alt.ok && !alt.skip && !split_draws(alt);
}
struct LookEntry {
    std::uint32_t words[kSetupWords];
    Look look;
    bool used = false;
    bool refine = false, has_alt = false;  // the setup can be refined; the refined look is usable (palette range known, pipelines ready)
    Look alt;
    AlphaRange rng;
    std::uint32_t uid = 0;  // the palette record the range was taken from
};
LookEntry g_look_cache[512];
void fill_alt(LookEntry &e, const std::uint32_t *s) {
    e.has_alt = false;
    e.rng = pal_range(s, e.uid);
    if (!((s[0] & 2u) && (s[0] & 8192u)) || e.rng.known) {  // the texture's alpha takes part: its range must be known
        Look alt;
        if (alt_look_of(s, alt)) {
            const bool w = g_route_waits;
            e.has_alt = pipelines_ok(alt);
            g_route_waits = w;
            if (e.has_alt) e.alt = alt;
        }
    }
}
const LookEntry &entry_of(const std::uint32_t *s) {
    std::uint64_t h = 1469598103934665603ull;
    for (std::uint32_t i = 0; i < kSetupWords; ++i) h = (h ^ s[i]) * 1099511628211ull;
    LookEntry &e = g_look_cache[(h ^ (h >> 29)) & 511u];
    if (!e.used || std::memcmp(e.words, s, sizeof e.words) != 0) {
        std::memcpy(e.words, s, sizeof e.words);
        e.look = classify(s, decodable_of(s));
        e.used = true;
        e.refine = refinable(s, e.look);
        if (e.refine) fill_alt(e, s);
        else e.has_alt = false;
    } else if (e.refine && e.uid != pal_uid(s)) {
        fill_alt(e, s);
    }
    return e;
}
const Look &look_of(const std::uint32_t *s) { return entry_of(s).look; }

// One batch of shapes, all of which the card takes: the work goes into the list. Returns false, having done nothing, when it cannot.
// ---- textures that are pictures the card holds
// A batch whose texture is a picture the card drew used to write that picture back to memory first and read memory. Instead the shapes sample the picture
// itself (another target) or a copy of the box they read, taken just before the draw (the target the batch draws on: a render pass cannot sample its own
// attachment). Both are exact: where the picture is current (inside `valid`, not `stale`) it holds what the write-back would have put in memory (the shader
// rebuilds the texel as memory holds it: 16-bit colour cut to 5/5/5/1), and the card reads its textures as they were before the batch either way (gpu.cpp
// cuts a batch where a shape would read what an earlier shape of it drew). The write-back is only put off: the picture stays newer than memory and goes back
// when something else reads that memory.
int g_feedback = 3;  // PS2_GPU_HW_FEEDBACK: bit 0 sample another target, bit 1 sample a copy of the own target (0 = off, write back as before; "other", "self")

// Same rule as the shader's target_raw: 32-bit textures from a 32-bit picture, 24-bit from 32- or 24-bit, 16-bit from the same 16-bit format, and the indices kept in the
// top byte of 32-bit words (8-bit, 4-bit low and high nibble: the same page layout as 32-bit colour) from a 32-bit picture's alpha. A 32-bit texture over a 24-bit picture
// would need the top byte, which the picture does not hold.
bool feed_format_ok(std::uint32_t fpsm, std::uint32_t tpsm) {
    if (fpsm == 0) return tpsm == 0 || tpsm == 1 || tpsm == 0x1B || tpsm == 0x24 || tpsm == 0x2C;
    if (fpsm == 1) return tpsm == 1;
    return (fpsm == 2 || fpsm == 0x0A) && tpsm == fpsm;
}

// The texels a setup's shapes can read (one more on each side for the bilinear taps), as the shader addresses them; false when it cannot be told (q <= 0, region repeat).
struct UsedTex {
    double u0 = 1e30, u1 = -1e30, v0 = 1e30, v1 = -1e30;
    bool bad = false, any = false;
};

bool used_rect(const std::uint32_t *s, const UsedTex &u, Rect &out) {
    if (u.bad || !u.any || u.u0 > u.u1) return false;
    const auto axis = [](double lo, double hi, std::uint32_t lg, std::uint32_t mode, std::uint32_t region, int &a, int &b) {
        if (mode == 3) return false;
        const int size = 1 << lg;
        a = static_cast<int>(std::floor(lo)) - 1;
        b = static_cast<int>(std::floor(hi)) + 1;
        if (mode == 0) {
            if (a < 0 || b >= size) a = 0, b = size - 1;  // wraps: the whole texture
        } else {
            const int mn = mode == 2 ? static_cast<int>(region & 0xFFFFu) : 0, mx = mode == 2 ? static_cast<int>(region >> 16) : size - 1;
            a = std::clamp(a, mn, mx), b = std::clamp(b, mn, mx);  // clamping is monotone
        }
        return true;
    };
    int ax, bx, ay, by;
    if (!axis(u.u0, u.u1, s[15] & 0xFFu, (s[7] >> 24) & 3u, s[18], ax, bx) || !axis(u.v0, u.v1, s[15] >> 8, (s[7] >> 26) & 3u, s[19], ay, by)) return false;
    out = Rect();
    out.add(ax, ay, bx, by);
    return true;
}

// Which picture (if any) the batch samples instead of a write-back; `fed` marks the setups that sample it, `copy_box` is the box to copy when it is the target T.
Tgt *pick_source(const LiveBatch &b, Tgt &T, std::vector<std::uint8_t> &fed, Rect &copy_box, Rect &need, Pg &out_pages) {
    const std::uint32_t nset = b.setup_count;
    Pg rp;
    for (std::uint32_t i = 0; i < b.read_count; ++i) rp |= pages_bytes(b.reads[2 * i], b.reads[2 * i + 1]);
    Tgt *S = nullptr;
    for (std::uint32_t i = 0; i < nset && !S; ++i) {
        const std::uint32_t *s = b.setups + static_cast<std::size_t>(i) * kSetupWords;
        if (!(s[0] & 2u)) continue;
        for (auto &t : LV.targets)
            if (t->g && !t->drawn.empty() && (t->dpages & rp).any() && t->fbp == s[12] && t->fbw == s[13] && feed_format_ok(t->fpsm, s[14])) {
                S = t.get();
                break;
            }
    }
    if (!S) {
        if (g_log) {  // PS2_GPU_HW_LOG=1: a picture the textures overlap that none of them is (another address, width or format)
            for (auto &t : LV.targets) {
                if (!t->g || t->drawn.empty() || !(t->dpages & rp).any()) continue;
                for (std::uint32_t i = 0; i < nset; ++i) {
                    const std::uint32_t *s = b.setups + static_cast<std::size_t>(i) * kSetupWords;
                    if (!(s[0] & 2u)) continue;
                    char key[200];
                    std::snprintf(key, sizeof key, "feedback not possible: texture %u/%u/%02X %ux%u over picture %u/%u/%02X%s", s[12], s[13], s[14], 1u << (s[15] & 0xFFu), 1u << (s[15] >> 8), t->fbp, t->fbw, t->fpsm, t.get() == &T ? " (self)" : "");
                    ++LV.st.kinds[key].first;
                }
            }
        }
        return nullptr;
    }
    const bool self = S == &T;
    if (!(g_feedback & (self ? 2 : 1))) return nullptr;
    static std::vector<UsedTex> used;
    used.assign(nset, UsedTex());
    for (std::uint32_t i = 0; i < b.prim_count; ++i) {  // the texture coordinates of every shape (the interpolated s/q lies between the corners' when q > 0)
        const std::uint32_t *r = b.prims + static_cast<std::size_t>(i) * kPrimWords;
        const std::uint32_t *s = b.setups + static_cast<std::size_t>(r[0]) * kSetupWords;
        if (!(s[0] & 2u)) continue;
        UsedTex &u = used[r[0]];
        u.any = true;
        const bool fst = (s[0] & 16u) != 0;
        const double sw = static_cast<double>(1u << (s[15] & 0xFFu)), sh = static_cast<double>(1u << (s[15] >> 8));
        const auto acc = [&](float sv, float tv, float qv) {
            double uu, vv;
            if (fst) uu = sv / 16.0, vv = tv / 16.0;
            else if (qv > 0.0f) uu = sv / qv * sw, vv = tv / qv * sh;
            else { u.bad = true; return; }
            if (!(uu > -1e9 && uu < 1e9 && vv > -1e9 && vv < 1e9)) { u.bad = true; return; }
            u.u0 = std::min(u.u0, uu), u.u1 = std::max(u.u1, uu), u.v0 = std::min(u.v0, vv), u.v1 = std::max(u.v1, vv);
        };
        if ((r[1] & 3u) == 0) {
            for (int k = 0; k < 3; ++k) acc(f(r[15 + 3 * k]), f(r[16 + 3 * k]), f(r[17 + 3 * k]));
        } else {
            acc(f(r[15]), f(r[16]), f(r[20]));
            acc(f(r[18]), f(r[19]), f(r[20]));
        }
    }
    const Gen &SG = *S->g;
    Pg used_pages;
    bool any = false;
    const auto refuse = [&](const char *why) -> Tgt * {
        ++LV.st.fed_refused;
        if (g_log) ++LV.st.kinds[std::string("feedback refused: ") + why].first;
        fed.assign(nset, 0);
        copy_box = need = Rect();
        out_pages.reset();
        return nullptr;
    };
    for (std::uint32_t i = 0; i < nset; ++i) {
        const std::uint32_t *s = b.setups + static_cast<std::size_t>(i) * kSetupWords;
        if (!(s[0] & 2u) || !used[i].any) continue;
        if (s[12] == S->fbp && s[13] == S->fbw && feed_format_ok(S->fpsm, s[14])) {
            Rect u;
            if (!used_rect(s, used[i], u)) return refuse("texel range unknown");
            // Texels inside the picture's texture are read from it (made current first: a part never loaded, or stale, is loaded from memory). Texels past its edge (the
            // one-texel margin of a strip at the bottom edge, a texture that reaches into the next buffer) are read from memory by the shader (setup word 41 bit 1); they
            // are none of this picture's pixels as long as their pages hold none of its drawn part (a texel right of the buffer's width lies in the next row of pages).
            Rect in;
            in.add(std::max(u.x0, 0), std::max(u.y0, 0), std::min(u.x1, SG.w - 1), std::min(u.y1, SG.h - 1));
            if (in.empty() || !covers(in, u)) {
                Rect parts[4];
                if (u.y1 >= SG.h) parts[0].add(u.x0, SG.h, u.x1, u.y1);
                if (u.x1 >= SG.w) parts[1].add(SG.w, u.y0, u.x1, std::min(u.y1, SG.h - 1));
                if (u.y0 < 0 || u.x0 < 0) return refuse("texels before the picture's origin");
                for (const Rect &p : parts)
                    if (!p.empty()) pages_of(s[12], s[13], s[14], p.x0, p.y0, p.x1, p.y1, out_pages);
                if ((out_pages & S->dpages).any()) return refuse("texels past the picture's edge lie in its drawn pages");
            }
            if (!in.empty()) {
                need = unite(need, in);
                pages_of(s[12], s[13], s[14], in.x0, in.y0, in.x1, in.y1, used_pages);
                if (self) copy_box = unite(copy_box, in);
            }
            fed[i] = 1;
            any = true;
        } else {  // another texture, read from memory: it must not need the picture in memory
            Pg fp;
            pages_of(s[12], s[13], s[14], 0, 0, static_cast<int>(1u << (s[15] & 0xFFu)) - 1, static_cast<int>(1u << (s[15] >> 8)) - 1, fp);
            if ((fp & S->dpages).any()) return refuse("another texture of the batch reads the picture's memory");
        }
    }
    if (!any) return refuse("no shape reads it");
    for (auto &t : LV.targets) {  // nothing else holds newer data in the pages read (the stale rule already says so; checked because it is cheap)
        if (!t->zdrawn.empty() && (t->zdpages & used_pages).any()) return refuse("a depth picture holds newer data there");
        if (t.get() != S && !t->drawn.empty() && (t->dpages & used_pages).any()) return refuse("another picture holds newer data there");
    }
    return S;
}

bool batch(const LiveBatch &b) {
    const std::uint32_t nset = b.setup_count;
    static std::vector<Look> looks, alts;
    static std::vector<AlphaRange> rngs;
    static std::vector<std::uint8_t> has_alt;
    looks.resize(nset), alts.resize(nset), rngs.resize(nset), has_alt.resize(nset);
    for (std::uint32_t i = 0; i < nset; ++i) {
        const LookEntry &e = entry_of(b.setups + static_cast<std::size_t>(i) * kSetupWords);
        looks[i] = e.look, has_alt[i] = e.has_alt ? 1 : 0;
        if (e.has_alt) alts[i] = e.alt, rngs[i] = e.rng;
    }
    const auto look_for = [&](const std::uint32_t *r) -> const Look & {  // the look of this shape: its setup's, or the setup's without the alpha test when the test is proved to pass
        if (has_alt[r[0]]) {
            ++pal::n_shapes_tried;
            if (shape_passes(b.setups + static_cast<std::size_t>(r[0]) * kSetupWords, r, rngs[r[0]])) {
                ++pal::n_shapes_pass;
                if (g_log) {
                    const std::uint32_t *sw = b.setups + static_cast<std::size_t>(r[0]) * kSetupWords;
                    char key[160];
                    std::snprintf(key, sizeof key, "refined: tbp %u psm %02X tme %u tcc %u atst %u afail %u aref %u tfx %u kind %u range %d..%d", sw[12], sw[14], sw[0] & 2u, (sw[0] >> 13) & 1u, sw[7] & 7u, (sw[7] >> 4) & 3u, sw[8], (sw[7] >> 20) & 3u, r[1] & 3u, rngs[r[0]].known ? rngs[r[0]].lo : -1, rngs[r[0]].known ? rngs[r[0]].hi : -1);
                    ++LV.st.kinds[key].second;
                }
                return alts[r[0]];
            }
        }
        return looks[r[0]];
    };
    if (g_indep && g_indep_max != ~0u && too_heavy(b, looks, nset)) {  // a run that needs the exact check and is too long for it: the painter takes the batch (looked for first: it stops at the first shape that decides)
        ++g_heavy_batches;
        g_heavy_shapes += b.prim_count;
        return false;
    }
    bool need_z = false, zw = false, any_draw = false;
    for (std::uint32_t i = 0; i < b.prim_count; ++i) {
        const std::uint32_t *r = b.prims + static_cast<std::size_t>(i) * kPrimWords;
        if ((r[1] & 3u) > 1 || r[0] >= nset) return false;
        const Look &l = looks[r[0]];
        if (!l.ok) return false;
        if (l.skip) continue;
        any_draw = true;
        if (l.zwrite) zw = true;
        if (l.zwrite || l.ztest != 0) need_z = true;
    }
    if (any_draw && !g_last_card && b.prim_count < g_switch_min) {  // would start a card stretch with a few shapes
        ++g_small_switch;
        return false;
    }
    if (any_draw && b.prim_count < g_switch_min) {  // a small batch whose textures lie over newer card data would make the card write the picture back first: the painter does that and the stretch ends
        bool dirty = false;
        for (std::uint32_t i = 0; i < b.read_count && !dirty; ++i) {
            const Pg rp = pages_bytes(b.reads[2 * i], b.reads[2 * i + 1]);
            for (auto &t : LV.targets) dirty = dirty || (!t->drawn.empty() && (t->dpages & rp).any()) || (!t->zdrawn.empty() && (t->zdpages & rp).any());
        }
        if (dirty) {
            ++g_small_dirty;
            return false;
        }
    }
    g_last_card = true;
    ++LV.st.card_batches;
    LV.st.card_shapes += b.prim_count;
    if (g_log) {
        const std::uint32_t *s0 = b.setups + static_cast<std::size_t>(b.prims[0]) * kSetupWords;
        char sig[420];
        std::snprintf(sig, sizeof sig, "target fbp %u fbw %u psm %02X, z %s%s, %s tex %u/%u/%02X %ux%u, blend %d alphatest %d fog %d, box %dx%d", b.fbp, b.fbw, b.fpsm, need_z ? (zw ? "write" : "test") : "off", b.zte ? "" : " (no zte)",
                      (s0[0] & 2u) ? "textured" : "flat", s0[12], s0[13], s0[14], 1u << (s0[15] & 0xFFu), 1u << (s0[15] >> 8), (s0[0] & 8u) ? 1 : 0, (s0[0] & 64u) ? 1 : 0, (s0[0] & 4u) ? 1 : 0, b.maxx - b.minx + 1, b.maxy - b.miny + 1);
        const std::size_t n = std::strlen(sig);
        const Look &l0 = looks[b.prims[0]];
        std::snprintf(sig + n, sizeof sig - n, " | ztst %u zw %d zmsk %d; atst %u afail %u aref %u; abcd %u%u%u%u fix %u; look: ztest %u zwrite %d two %d alpha_two %d src %u dst %u op %u premul %u mask %x",
                      (s0[7] >> 8) & 3u, (s0[0] & 512u) ? 1 : 0, (s0[0] & 32u) ? 1 : 0, s0[7] & 7u, (s0[7] >> 4) & 3u, s0[8], (s0[7] >> 12) & 3u, (s0[7] >> 14) & 3u, (s0[7] >> 16) & 3u, (s0[7] >> 18) & 3u, s0[9], l0.ztest, l0.zwrite ? 1 : 0,
                      l0.two_pass ? 1 : 0, l0.alpha_two ? 1 : 0, l0.src, l0.dst, l0.op, l0.premul, l0.mask);
        auto &k = LV.st.kinds[sig];
        ++k.first;
        k.second += b.prim_count;
    }
    if (!any_draw) return true;
    const std::uint32_t W = b.fbw * 64;
    if (!W || W > 4096) return false;
    Rect B;
    B.add(std::max(b.minx, 0), std::max(b.miny, 0), std::min(b.maxx, static_cast<int>(W) - 1), std::min(b.maxy, 2047));
    if (B.empty()) return true;
    Tgt &T = target_of(b.fbp, b.fbw, b.fpsm, b.setups);
    T.used = ++LV.use;
    ensure_gen(T, B.y1 + 1, b.frame);
    evict_for(&T);
    Gen &G = *T.g;
    // 1. the textures the batch reads come from memory: whatever the card holds newer there goes back first, except a picture the shapes sample directly (above)
    Tm tm_wb(9);
    static std::vector<std::uint8_t> fed;
    fed.assign(nset, 0);
    Rect copy_box;
    Rect need;
    Pg out_pages;
    Tgt *const S = g_feedback && !g_eager ? pick_source(b, T, fed, copy_box, need, out_pages) : nullptr;
    if (!g_feedback || g_eager) {
        for (std::uint32_t i = 0; i < b.read_count; ++i) write_back(pages_bytes(b.reads[2 * i], b.reads[2 * i + 1]), LV.st.by_read);
    } else {
        // Exactly what the card's shader reads from memory goes back first: the base level of each texture that is not sampled from a picture, the 2^tw x 2^th texels it wraps
        // into (no mipmaps, palettes come from the palette buffer). A texture that holds only the top byte of 32-bit words (8-bit or 4-bit "high" indices) does not need a
        // 24-bit colour or depth picture written back: that write-back keeps the top byte of every word as memory has it (24-bit drawing never touches it).
        Pg plain, upper;
        for (std::uint32_t i = 0; i < nset; ++i) {
            const std::uint32_t *s = b.setups + static_cast<std::size_t>(i) * kSetupWords;
            if (!(s[0] & 2u) || fed[i]) continue;
            const bool top = s[14] == 0x1B || s[14] == 0x24 || s[14] == 0x2C;
            pages_of(s[12], s[13], s[14], 0, 0, static_cast<int>(1u << (s[15] & 0xFFu)) - 1, static_cast<int>(1u << (s[15] >> 8)) - 1, top ? upper : plain);
        }
        write_back_if(plain | upper, LV.st.by_read, [&](const Tgt &U, int kind) {
            if (kind == 0 && &U == S) return true;  // sampled directly
            const Pg &dp = kind == 0 ? U.dpages : U.zdpages;
            if ((dp & plain).any()) return false;
            if (kind == 0 ? U.fpsm != 1 : U.zpsm != 0x31) return false;
            ++LV.st.top_byte_kept;
            return true;
        });
    }
    if (S) {
        write_back(out_pages, LV.st.by_read, S);  // texels past the picture's edge come from memory
        if (!need.empty()) make_current(*S, need);  // the texels read inside the picture: loaded where never loaded or stale
    }
    // 2. the pages the batch writes: other pictures over the same memory are written back, then they are stale
    Pg Pc, Pz;
    pages_of(b.fbp, b.fbw, b.fpsm, B.x0, B.y0, B.x1, B.y1, Pc);
    if (need_z) pages_of(b.zbp, b.fbw, b.zpsm, B.x0, B.y0, B.x1, B.y1, Pz);
    for (auto &up : LV.targets) {
        Tgt &U = *up;
        if (&U == &T) continue;
        if ((U.dpages & Pc).any()) store_color(U, LV.st.by_overlap);
        if ((U.zdpages & Pc).any()) store_depth(U, LV.st.by_overlap);
        if (need_z) {
            if ((U.dpages & Pz).any()) store_color(U, LV.st.by_overlap);
            if ((U.zdpages & Pz).any()) store_depth(U, LV.st.by_overlap);
        }
        if ((U.pages & Pc).any() || (zw && (U.pages & Pz).any())) U.stale = U.valid;
        if ((U.zpages & Pc).any() || (zw && (U.zpages & Pz).any())) U.zstale = U.zvalid;
    }
    // 3. the colour picture is current where the batch draws
    make_current(T, B);
    // 4. so is the depth picture, for the depth buffer this batch uses
    if (need_z) {
        if (!T.paired || T.zbp != b.zbp || T.zpsm != b.zpsm) {
            store_depth(T, LV.st.by_other);
            forget_depth(T);
            T.paired = true, T.zbp = b.zbp, T.zpsm = b.zpsm;
            std::memcpy(T.fmt, b.setups, sizeof T.fmt);
            T.sync_serial = ~0ull;
        }
        make_current_z(T, B);
    }
    // 5. setups, vertices and runs (the rules for the draws that split a shape in two)
    tm_wb.stop();
    const std::uint32_t base = static_cast<std::uint32_t>(LV.setups.size() / kHwSetup);
    LV.setups.resize(LV.setups.size() + static_cast<std::size_t>(nset) * kHwSetup, 0);
    for (std::uint32_t i = 0; i < nset; ++i) {
        std::uint32_t *w = LV.setups.data() + static_cast<std::size_t>(base + i) * kHwSetup;
        std::memcpy(w, b.setups + static_cast<std::size_t>(i) * kSetupWords, kSetupWords * 4);
        w[24] = 0xFFFFFFFFu;
        w[40] = looks[i].alpha_out ? 1u : 0u;
        w[41] = fed[i] ? 3u : 0u;  // the shader reads this setup's texels from the picture bound as src_tex, and those past its edge from memory
    }
    const std::uint32_t run_first = static_cast<std::uint32_t>(LV.runs.size());
    Run cur;
    bool open = false;
    std::vector<Stamp> cur_stamps;  // the shapes of the open run that need the independence check, in order
    PreRun *taken = nullptr;  // the pre-run (started by draw()) the open run took its check from
    std::uint32_t drawn = 0, run_first_i = 0, run_last_i = 0;  // shapes the chip draws so far, and the batch indexes of the first and last shape of the open run
    const auto close = [&] {
        if (open) {
            if (taken && cur.shapes != taken->count) {  // not expected: the runs of draw() and of this walk differ. The check is made again here from the records
                cur.done_jobs.clear(), cur.job_first.clear();
                const std::uint32_t total = cur.shapes;
                cur.shapes = 0, cur.flat = true;
                for (std::uint32_t k = run_first_i; k <= run_last_i; ++k) {
                    const std::uint32_t *rk = b.prims + static_cast<std::size_t>(k) * kPrimWords;
                    if (looks[rk[0]].skip) continue;
                    ++cur.shapes;
                    feed_check(cur, cur_stamps, rk, run_last_i - k + 1);
                }
                cur.shapes = total;
            }
            taken = nullptr;
            {
                Tm tm(1);
                end_check(cur);  // the run has a check when it was not proven independent as it came (flat depth with GEQUAL or ALWAYS, or the test "always", need none)
            }
            cur_stamps.clear();
            LV.runs.push_back(std::move(cur));  // the independent segments are drawn one after the other at the encode (the cut shape starts the next piece)
        }
        open = false;
        cur = Run();
    };
    {  // room for the batch's vertices (six for a sprite) without growing in the loop
        const std::size_t need = LV.vertices.size() + static_cast<std::size_t>(b.prim_count) * 6;
        if (need > LV.vertices.capacity()) LV.vertices.reserve(std::max(need, LV.vertices.capacity() * 2));
    }
    struct Box {  // the scissor of a setup, cut to the picture
        std::uint32_t v[4];
        bool ok;
    };
    static std::vector<Box> boxes;
    boxes.resize(nset);
    for (std::uint32_t i = 0; i < nset; ++i) {
        const std::uint32_t *s = b.setups + static_cast<std::size_t>(i) * kSetupWords;
        const int cx0 = std::max(static_cast<int>(s[10] & 0xFFFFu), 0), cy0 = std::max(static_cast<int>(s[11] & 0xFFFFu), 0), cx1 = std::min(static_cast<int>(s[10] >> 16), G.w - 1), cy1 = std::min(static_cast<int>(s[11] >> 16), G.h - 1);
        boxes[i].ok = cx1 >= cx0 && cy1 >= cy0;
        boxes[i].v[0] = static_cast<std::uint32_t>(cx0), boxes[i].v[1] = static_cast<std::uint32_t>(cy0), boxes[i].v[2] = static_cast<std::uint32_t>(cx1 - cx0 + 1), boxes[i].v[3] = static_cast<std::uint32_t>(cy1 - cy0 + 1);
    }
    // the checks started by draw() are taken over when every shape of the batch was told to it and no setup's scissor is empty on this picture (batch() would skip those shapes)
    bool adopt = PRE.on && PRE.told == b.prim_count;
    for (std::uint32_t i = 0; i < nset && adopt; ++i) adopt = boxes[i].ok;
    if (adopt) pre_close();
    std::size_t pre_at = 0;
    std::uint32_t last_set = ~0u;  // the setup of the shape before, while its run is open: the same setup is the same look and the same scissor
    for (std::uint32_t i = 0; i < b.prim_count; ++i) {
        const std::uint32_t *r = b.prims + static_cast<std::size_t>(i) * kPrimWords;
        const Look &l = look_for(r);
        if (l.skip) continue;  // the chip draws nothing for it either
        const std::uint32_t kind = r[1] & 3u;
        const Box &bx = boxes[r[0]];
        if (!bx.ok) continue;
        if (!open || r[0] != last_set) {
          last_set = r[0];
          const std::uint32_t *rs = b.setups + static_cast<std::size_t>(r[0]) * kSetupWords + 10;
          if (!open || !(cur.look == l) || cur.raw_scissor[0] != rs[0] || cur.raw_scissor[1] != rs[1]) {
            if (g_time && open) {  // why the run ends (PS2_GPU_HW_TIME=1)
                const Look &a = cur.look;
                ++g_cut_why[0];
                if (std::memcmp(cur.scissor, bx.v, sizeof cur.scissor) != 0) ++g_cut_why[1];
                if (a.ztest != l.ztest) ++g_cut_why[2];
                if (a.zwrite != l.zwrite) ++g_cut_why[3];
                if (a.two_pass != l.two_pass || a.alpha_two != l.alpha_two) ++g_cut_why[4];
                if (a.blend != l.blend || a.src != l.src || a.dst != l.dst || a.op != l.op || a.premul != l.premul || a.fix != l.fix || a.constant != l.constant) ++g_cut_why[5];
                if (a.mask != l.mask || a.alpha_out != l.alpha_out) ++g_cut_why[6];
            }
            close();
            cur.look = l;
            cur.first_vertex = static_cast<std::uint32_t>(LV.vertices.size());
            std::memcpy(cur.scissor, bx.v, sizeof cur.scissor);
            cur.raw_scissor[0] = rs[0], cur.raw_scissor[1] = rs[1];
            open = true;
            run_first_i = i;
            taken = nullptr;
            if (adopt && pre_at < PRE.done.size()) {
                PreRun &p = PRE.done[pre_at];
                if (p.first == drawn && p.run.look == l && p.sc[0] == rs[0] && p.sc[1] == rs[1]) {
                    taken = &p;
                    ++pre_at;
                    cur.done_jobs = std::move(p.run.done_jobs);
                    cur.job_first = std::move(p.run.job_first);
                } else {
                    adopt = false;  // the rule did not give the same runs (not expected): the rest is checked here
                }
            } else {
                adopt = false;
            }
          }
        }
        const std::uint32_t si = base + r[0];
        const std::size_t before = LV.vertices.size();
        if (kind == 0) {
            for (int k = 0; k < 3; ++k) {
                const double dz = k == 0 ? 0.0 : f(r[24 + k]);
                corner(LV.vertices, f(r[4 + 2 * k]), f(r[5 + 2 * k]), static_cast<double>(r[24]) + dz, r[12 + k], f(r[15 + 3 * k]), f(r[16 + 3 * k]), f(r[17 + 3 * k]), static_cast<float>((r[11] >> (8 * k)) & 0xFFu), si);
            }
        } else {
            const float x0 = f(r[4]), y0 = f(r[5]), x1 = x0 + f(r[6]), y1 = y0 + f(r[7]);
            const float s0 = f(r[15]), t0 = f(r[16]), s1 = f(r[18]), t1 = f(r[19]), q = f(r[20]), fog = static_cast<float>((r[11] >> 8) & 0xFFu);
            const double z = r[24];
            corner(LV.vertices, x0, y0, z, r[13], s0, t0, q, fog, si);
            corner(LV.vertices, x1, y0, z, r[13], s1, t0, q, fog, si);
            corner(LV.vertices, x0, y1, z, r[13], s0, t1, q, fog, si);
            corner(LV.vertices, x1, y0, z, r[13], s1, t0, q, fog, si);
            corner(LV.vertices, x1, y1, z, r[13], s1, t1, q, fog, si);
            corner(LV.vertices, x0, y1, z, r[13], s0, t1, q, fog, si);
        }
        const std::uint32_t n = static_cast<std::uint32_t>(LV.vertices.size() - before);
        cur.vertices += n;
        ++cur.shapes;
        cur.counts.push_back(static_cast<std::uint8_t>(n));
        ++drawn;
        run_last_i = i;
        if (!taken) feed_check(cur, cur_stamps, r, b.prim_count - i);
    }
    close();
    const std::uint32_t run_count = static_cast<std::uint32_t>(LV.runs.size()) - run_first;
    if (run_count) {
        if (S == &T) {  // the box the shapes read, copied after the loads above and just before the draw
            const Rect c = clipped(G, copy_box);
            make_shadow(G);
            if (!c.empty()) {
                LOp &cp = push_op(LOp::Copy, &G);
                cp.rect[0] = static_cast<std::uint32_t>(c.x0), cp.rect[1] = static_cast<std::uint32_t>(c.y0), cp.rect[2] = static_cast<std::uint32_t>(c.x1 - c.x0 + 1), cp.rect[3] = static_cast<std::uint32_t>(c.y1 - c.y0 + 1);
            }
            ++LV.st.fed_self;
            LV.st.copy_px += area(c);
        } else if (S) {
            ++LV.st.fed_other;
        }
        LOp &o = push_op(LOp::Draw, &G);
        o.src = S ? S->g.get() : nullptr;
        o.run_first = run_first;
        o.run_count = run_count;
        LV.st.runs += run_count;
    }
    // 6. the picture is newer than memory where the batch drew
    T.drawn = unite(T.drawn, B);
    T.dpages |= Pc;
    T.seq = ++LV.seq;
    if (zw) {
        T.zdrawn = unite(T.zdrawn, B);
        T.zdpages |= Pz;
        T.zseq = LV.seq;
    }
    if (g_eager) {  // PS2_GPU_HW_EAGER=1: write back and reload after every batch (a 16-bit picture is not reloaded: the reload would cut it to 5 bits per channel, which the texture does not do)
        store_color(T, LV.st.by_eager);
        if (zw) store_depth(T, LV.st.by_eager);
        if (T.fpsm != 2 && T.fpsm != 0x0A) forget_color(T);
    }
    return true;
}

}  // namespace live

bool live_on() { return live::LV.on; }
void live_position(std::uint32_t commands) { live::LV.at = commands; }

int live_route(const std::uint32_t *s) {
    using namespace live;
    if (!LV.on) return 0;
    if (!s[2] || s[2] * 64 > 4096) return 0;
    if (!g_pending.empty()) drain_pipelines();  // pipelines the worker finished
    const Look l = classify(s, decodable_of(s));
    g_pre_last = -1;
    g_route_waits = false;
    if (!l.ok) {
        ++LV.st.refused[l.why];
        return 0;
    }
    g_pre_last = PRE.on ? pre_set_id(pre_look_id(l), -1, AlphaRange()) : -1;
    if (l.skip) return 2;
    if (!only_class(l.two_pass ? 1 : l.alpha_two ? 2 : 0)) return 0;
    static const std::string only_fbp = os::env("PS2_GPU_HW_ONLY_FBP");  // bisecting a wrong picture: only the pictures at these frame-buffer bases (blocks, comma separated) are drawn by the card
    if (!only_fbp.empty() && ("," + only_fbp + ",").find("," + std::to_string(s[1]) + ",") == std::string::npos) return 0;
    static const std::string no = os::env("PS2_GPU_HW_DEBUG_NO");  // bisecting: shapes with these features go to the painter (tex, blend, atest, z, self = the texture is the picture drawn on, tex4 = 4-bit texture)
    if (!no.empty()) {
        const auto has = [&](const char *k) { return ("," + no + ",").find(std::string(",") + k + ",") != std::string::npos; };
        if ((has("tex") && (s[0] & 2u)) || (has("blend") && (s[0] & 8u)) || (has("atest") && (s[0] & 64u)) || (has("z") && (s[0] & 512u)) || (has("self") && (s[0] & 2u) && s[12] == s[1]) || (has("tex4") && (s[0] & 2u) && (s[14] == 0x14 || s[14] == 0x13)) || (has("afail1") && (s[0] & 64u) && ((s[7] >> 4) & 3u) == 1) || (has("afail2") && (s[0] & 64u) && ((s[7] >> 4) & 3u) == 2)) return 0;
    }
    if (!pipelines_ok(l)) {
        if (g_route_waits) ++g_waited_routes;
        else ++LV.st.refused["pipeline did not build"];
        return 0;
    }
    if (PRE.on && refinable(s, l)) {  // the shapes of this setup that pass the alpha test everywhere are drawn without it (see entry_of)
        LookEntry e;
        e.refine = true;
        fill_alt(e, s);
        if (e.has_alt) g_pre_last = pre_set_id(g_pre_sets[static_cast<std::size_t>(g_pre_last)].orig, pre_look_id(e.alt), e.rng);
    }
    return 1;
}
bool live_waits() { return live::g_route_waits; }
bool live_ready_changed(std::uint64_t &seen) {
    if (seen == g_async_ready) return false;
    seen = g_async_ready;
    return true;
}

int live_look() { return g_pre_last; }

void live_shape(int look, const std::uint32_t *setup, const std::uint32_t *rec, bool fresh) {
    using namespace live;
    if (!PRE.on) return;
    if (fresh) {
        PRE.done.clear();
        PRE.cur = PreRun();
        PRE.open = false;
        PRE.buf.clear();
        PRE.told = PRE.ordinal = 0;
    }
    ++PRE.told;
    if (look < 0) return;
    const PreSet &ps = g_pre_sets[static_cast<std::size_t>(look)];
    const int lid = (ps.alt >= 0 && shape_passes(setup, rec, ps.rng)) ? ps.alt : ps.orig;  // the same rule as batch()'s look_for
    const Look &l = g_pre_looks[static_cast<std::size_t>(lid)];
    if (l.skip) return;  // the chip draws nothing for it: batch() skips it too
    const std::uint32_t sc0 = setup[10], sc1 = setup[11];
    if (!PRE.open || PRE.cur.look != lid || PRE.cur.sc[0] != sc0 || PRE.cur.sc[1] != sc1) {
        pre_close();
        PRE.cur = PreRun();
        PRE.cur.first = PRE.ordinal, PRE.cur.look = lid, PRE.cur.sc[0] = sc0, PRE.cur.sc[1] = sc1;
        PRE.cur.run.look = l;
        PRE.open = true;
        PRE.buf.clear();
    }
    ++PRE.ordinal;
    ++PRE.cur.count;
    ++PRE.cur.run.shapes;
    feed_check(PRE.cur.run, PRE.buf, rec, g_check_chunk);
}

bool live_batch(const LiveBatch &b) {
    if (!live::LV.on) return false;
    Tm tm(0);
    if (live::batch(b)) return true;
    ++live::LV.st.fallbacks;
    return false;
}

void live_reader(std::uint64_t lo, std::uint64_t hi) {
    using namespace live;
    if (!LV.on || hi <= lo) return;
    Tm tm(2);
    write_back(pages_bytes(lo, hi), LV.st.by_reader);
}

void live_writer(std::uint64_t lo, std::uint64_t hi) {
    using namespace live;
    if (!LV.on || hi <= lo) return;
    Tm tm(3);
    const Pg p = pages_bytes(lo, hi);
    write_back(p, LV.st.by_writer);
    for (auto &t : LV.targets) {
        if ((t->pages & p).any()) t->stale = t->valid;
        if ((t->zpages & p).any()) t->zstale = t->zvalid;
    }
}

// The painter draws a batch: it reads the textures, the colour it blends into and the depth it tests, and it writes colour and depth.
void live_painter(const LiveBatch &b) {
    using namespace live;
    if (!LV.on) return;
    Tm tm(4);
    g_last_card = false;
    ++LV.st.painter_batches;
    LV.st.painter_shapes += b.prim_count;
    for (std::uint32_t i = 0; i < b.read_count; ++i) write_back(pages_bytes(b.reads[2 * i], b.reads[2 * i + 1]), LV.st.by_painter);
    Rect B;
    B.add(std::max(b.minx, 0), std::max(b.miny, 0), std::min(b.maxx, 2047), std::min(b.maxy, 2047));
    if (B.empty()) return;
    Pg Pc, Pz;
    pages_of(b.fbp, b.fbw, b.fpsm, B.x0, B.y0, B.x1, B.y1, Pc);
    pages_of(b.zbp, b.fbw, b.zpsm, B.x0, B.y0, B.x1, B.y1, Pz);
    write_back(Pc, LV.st.by_painter);
    if (b.zte) write_back(Pz, LV.st.by_painter);
    for (auto &up : LV.targets) {
        Tgt &t = *up;
        if ((t.pages & Pc).any()) {
            if (t.fbp == b.fbp && t.fbw == b.fbw && t.fpsm == b.fpsm) t.stale = unite(t.stale, B);
            else t.stale = t.valid;
        }
        if ((t.zpages & Pc).any()) t.zstale = t.zvalid;
        if (b.zwrite) {
            if ((t.pages & Pz).any()) t.stale = t.valid;
            if ((t.zpages & Pz).any()) {
                if (t.paired && t.zbp == b.zbp && t.zpsm == b.zpsm && t.fbw == b.fbw) t.zstale = unite(t.zstale, B);
                else t.zstale = t.zvalid;
            }
        }
    }
}

bool live_display(const LiveDisplay &d, const std::uint32_t *params, std::uint32_t count) {
    using namespace live;
    if (!LV.on || d.overlay || (d.psm != 0 && d.psm != 1) || !d.width || !d.height) return false;
    Tm tm(8);
    const int rows = d.halved ? static_cast<int>((d.height + 1) / 2) : static_cast<int>(d.height);
    Rect R;
    R.add(static_cast<int>(d.dbx), static_cast<int>(d.dby), static_cast<int>(d.dbx + d.width) - 1, static_cast<int>(d.dby) + rows - 1);
    Tgt *best = nullptr;
    for (auto &up : LV.targets) {
        Tgt &t = *up;
        if (!t.g || t.fbp != d.fbp || t.fbw != d.fbw || (t.fpsm != 0 && t.fpsm != 1) || t.valid.empty() || R.x1 >= t.g->w || R.y1 >= t.g->h) continue;  // the part of the picture the card never held is loaded from memory (make_current)
        if (!best || t.seq > best->seq) best = &t;
    }
    if (!best) {
        ++LV.st.display_memory;
        if (g_log) {
            std::fprintf(stderr, "[hw-live] display from memory: fbp %u fbw %u psm %02X, %u x %u at %u,%u%s; targets:", d.fbp, d.fbw, d.psm, d.width, d.height, d.dbx, d.dby, d.halved ? " (field)" : "");
            for (auto &up : LV.targets) std::fprintf(stderr, " [fbp %u fbw %u psm %02X valid %d,%d-%d,%d stale %d,%d-%d,%d]", up->fbp, up->fbw, up->fpsm, up->valid.x0, up->valid.y0, up->valid.x1, up->valid.y1, up->stale.x0, up->stale.y0, up->stale.x1, up->stale.y1);
            std::fprintf(stderr, "\n");
        }
        return false;
    }
    ++LV.st.display_card;
    make_current(*best, R);  // a stale strip (the painter drew there) is loaded again first
    LOp &o = push_op(LOp::Display, best->g.get());
    std::array<std::uint32_t, 64> r{};
    for (std::uint32_t i = 0; i < count && i < 64; ++i) r[i] = params[i];
    LV.params.push_back(r);
    o.param = static_cast<std::uint32_t>(LV.params.size() - 1);
    o.x = (d.width + 7) / 8;
    o.y = (d.height + 7) / 8;
    best->used = ++LV.use;
    return true;
}

void live_state_loaded() {
    using namespace live;
    if (!LV.on) return;
    ++LV.st.state_drops;
    for (auto &t : LV.targets) retire(*t);
    LV.targets.clear();
    LV.heights.clear();
    LV.ops.clear();
    LV.cursor = 0;
    LV.vertices.clear();
    LV.setups.clear();
    LV.params.clear();
    LV.runs.clear();
}

// ---- the hooks of the palette record (see `namespace pal` above). gpu.cpp calls them in the order of the game's writes.
// An upload chunk. A small PSMCT32 one (a palette is 16 x 16 or 8 x 2 pixels) gives its words' alpha; any other upload overwrites its pages for the record.
void pal_upload(std::uint32_t dbp, std::uint32_t dbw, std::uint32_t dpsm, std::uint32_t dsax, std::uint32_t dsay, std::uint32_t rrw, std::uint32_t done, std::uint32_t pixels, std::uint32_t lead_bits,
                std::uint32_t bpp, const void *bytes, std::size_t size, std::uint64_t lo, std::uint64_t hi) {
    using namespace pal;
    if (!on) return;
    ensure();
    if (dpsm == 0 && bpp == 32 && lead_bits == 0 && rrw && pixels <= 4096 && size >= static_cast<std::size_t>(pixels) * 4) {
        const std::uint8_t *b = static_cast<const std::uint8_t *>(bytes);
        const std::uint32_t e = ++ev;
        for (std::uint32_t i = 0; i < pixels; ++i) {
            const std::uint32_t p = done + i;
            const std::uint32_t w = (ps2::gs::address32(dbp, dbw, dsax + p % rrw, dsay + p / rrw) >> 2) & (kWords - 1);
            wseq[w] = e, walpha[w] = b[4 * i + 3];
        }
        return;
    }
    const std::uint32_t e = ++ev;
    if (hi > lo)  // the hull of the upload is not page aligned: every page from the one holding its first byte to the one holding its last
        for (std::uint64_t pg = lo >> 13; pg <= ((hi - 1) >> 13); ++pg) pgseq[pg & 511u] = e;
}
// A draw or a local copy overwrites this rectangle of memory.
void pal_write(std::uint32_t bp, std::uint32_t bw, std::uint32_t psm, int x0, int y0, int x1, int y1) {
    using namespace pal;
    if (!on) return;
    ensure();
    std::bitset<512> pg;
    pages_of(bp, bw, psm, x0, y0, x1, y1, pg);
    const std::uint32_t e = ++ev;
    for (std::uint32_t p = 0; p < 512; ++p)
        if (pg[p]) pgseq[p] = e;
}
// A palette load into slot `to` (the entries come from the record of memory as it is now; the other entries of the slot stay what the slot `from` held).
void pal_clut(std::uint32_t from, std::uint32_t to, std::uint32_t cbp, std::uint32_t cpsm, std::uint32_t csm, std::uint32_t csa, bool eight) {
    using namespace pal;
    if (!on) return;
    ensure();
    if (from >= kSlots || to >= kSlots) return;
    Rec &d = rec[to];
    d = rec[from];
    d.uid = uid_next++;
    ++n_loads;
    const std::uint32_t entries = eight ? 256u : 16u;
    if (csm != 0 || cpsm != 0) {  // the other layout and the 16-bit palettes are not read: the groups they overwrite are unknown
        const std::uint32_t first = csm ? (csa & 31u) : (eight ? 0u : (csa & 15u));
        for (std::uint32_t g = 0; g < entries / 16; ++g) d.g[(first + g) & 31u] = Grp();
        return;
    }
    bool all = true;
    for (std::uint32_t g = 0; g < entries / 16; ++g) {
        std::uint8_t lo = 255, hi = 0;
        bool known = true;
        for (std::uint32_t k = g * 16; k < g * 16 + 16; ++k) {
            std::uint32_t x, y;
            if (eight) {
                const std::uint32_t j = (k & ~0x18u) | ((k & 0x08u) << 1) | ((k & 0x10u) >> 1);
                x = j & 15u, y = j >> 4;
            } else {
                x = k & 7u, y = k >> 3;
            }
            const std::uint32_t w = (ps2::gs::address32(cbp, 1, x, y) >> 2) & (kWords - 1);
            if (!word_known(w)) {
                known = false;
                break;
            }
            lo = std::min(lo, walpha[w]), hi = std::max(hi, walpha[w]);
        }
        Grp &t = d.g[eight ? g : (csa & 15u)];
        t.known = known ? 1 : 0, t.lo = known ? lo : 0, t.hi = known ? hi : 255;
        all = all && known;
    }
    if (all) ++n_loads_known;
}
// A saved state replaced all of memory (words = the 4 MB as 32-bit words, or null when they are not in this layout) and the palettes.
void pal_state(const std::uint32_t *words, std::size_t count) {
    using namespace pal;
    if (!on) return;
    ensure();
    ev = 1;
    std::fill(std::begin(pgseq), std::end(pgseq), 0u);
    rec.assign(kSlots, Rec());
    if (words && count == kWords) {
        for (std::uint32_t w = 0; w < kWords; ++w) walpha[w] = static_cast<std::uint8_t>(words[w] >> 24), wseq[w] = 1;
    } else {
        std::fill(wseq.begin(), wseq.end(), 0u);
    }
}

bool live_pending() { return live::LV.on && live::LV.cursor < live::LV.ops.size(); }
bool live_before(std::uint32_t command) { return live::LV.cursor < live::LV.ops.size() && live::LV.ops[live::LV.cursor].after <= command; }

// ---------------------------------------------------------------- uploads through a ring of persistent staging buffers
// A busy fight sends 3 to 19 MB of vertices, setups and parameters to the card every frame. wgpuQueueWriteBuffer makes a new staging allocation for every
// such write and frees it once the card is done with it; on a Windows laptop with Intel graphics (Vulkan) the God of War II hall then stopped after 13 to 28 s
// with "Not enough memory left". So the frame's data goes through a few staging buffers that live for the whole run: the processor writes into one the card has
// finished with (it is mapped), it is unmapped, copied into the vertex, setup and parameter buffers at the start of the submit's card work (live_encode), and
// asked to be mapped again right after the submit (live_submitted). A buffer is only made anew when a frame needs more than it holds (then twice as much).
// PS2_GPU_HW_STAGE=0 goes back to one queue write per kind.
namespace live {

struct Stage {
    WGPUBuffer buf = nullptr;
    std::uint64_t cap = 0;
    bool mapped = false;  // set by the map callback: the card is done with it and the processor may write it
    bool lost = false;    // the map failed (the device is gone): never wait for it again
};
constexpr int kStages = 4;  // the card works up to three submits behind the processor
Stage g_stage[kStages];
int g_stage_next = 0, g_stage_now = -1;      // the stage to use next; the stage of this submit (copied in live_encode, mapped again in live_submitted)
std::uint64_t g_stage_v = 0, g_stage_s = 0, g_stage_p = 0, g_stage_soff = 0, g_stage_poff = 0;
bool g_stage_copy = false;                   // the copies of this submit are still to be recorded
const bool g_staging = os::env("PS2_GPU_HW_STAGE") != "0";

std::uint64_t stage_bytes() {
    std::uint64_t n = 0;
    for (const Stage &s : g_stage) n += s.buf ? s.cap : 0;
    return n;
}

void stage_map(Stage &s) {
    WGPUBufferMapCallbackInfo cb = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
    cb.mode = WGPUCallbackMode_AllowProcessEvents;
    cb.callback = [](WGPUMapAsyncStatus status, WGPUStringView, void *u1, void *) {
        Stage *st = static_cast<Stage *>(u1);
        st->mapped = status == WGPUMapAsyncStatus_Success;
        st->lost = !st->mapped;
    };
    cb.userdata1 = &s;
    wgpuBufferMapAsync(s.buf, WGPUMapMode_Write, 0, s.cap, cb);
}

}  // namespace live

// Writes the frame's vertices, setups and parameters into a staging buffer; false when the old queue writes have to do it.
bool stage_write(std::uint64_t vb, std::uint64_t sb, std::uint64_t pb) {
    using namespace live;
    g_stage_copy = false;
    g_stage_now = -1;
    if (!g_staging || (!vb && !sb && !pb)) return !g_staging ? false : true;
    Context &c = *g_gpu.ctx;
    const auto up = [](std::uint64_t n) { return (n + 255) & ~std::uint64_t(255); };
    const std::uint64_t soff = up(vb), poff = soff + up(sb), need = poff + up(pb);
    const int i = g_stage_next;
    Stage &s = g_stage[i];
    if (s.buf && s.cap >= need) {
        while (!s.mapped && !s.lost) wgpuDevicePoll(c.device, true, nullptr);  // the card is still reading it (three submits ago)
        if (s.lost) return false;
    } else {
        if (s.buf) {
            while (!s.mapped && !s.lost) wgpuDevicePoll(c.device, true, nullptr);  // a callback must not outlive the buffer it names
            memlog::del_buffer(s.buf);
        }
        WGPUBufferDescriptor d = WGPU_BUFFER_DESCRIPTOR_INIT;
        d.size = std::max<std::uint64_t>(std::max<std::uint64_t>(s.cap * 2, 1u << 20), need);
        d.usage = static_cast<WGPUBufferUsage>(WGPUBufferUsage_MapWrite | WGPUBufferUsage_CopySrc);
        d.mappedAtCreation = true;
        s.buf = memlog::new_buffer(c.device, &d);
        s.cap = d.size;
        s.mapped = s.buf != nullptr;
        s.lost = !s.mapped;
        if (s.lost) return false;
    }
    auto *p = static_cast<std::uint8_t *>(wgpuBufferGetMappedRange(s.buf, 0, need));
    if (!p) return false;
    if (vb) std::memcpy(p, LV.vertices.data(), vb);
    if (sb) std::memcpy(p + soff, LV.setups.data(), sb);
    if (pb) std::memcpy(p + poff, LV.params.data(), pb);
    wgpuBufferUnmap(s.buf);
    s.mapped = false;
    g_stage_next = (i + 1) % kStages;
    g_stage_now = i;
    g_stage_v = vb, g_stage_s = sb, g_stage_p = pb, g_stage_soff = soff, g_stage_poff = poff;
    g_stage_copy = true;
    return true;
}

void live_prepare() {
    using namespace live;
    Tm tm(5);
    Context &c = *g_gpu.ctx;
    const auto grow = [&](WGPUBuffer &buf, std::uint64_t &cap, std::uint64_t need, WGPUBufferUsage usage) {
        if (need <= cap && buf) return false;
        if (buf) memlog::del_buffer(buf);
        cap = std::max<std::uint64_t>(std::max<std::uint64_t>(cap * 2, 1u << 16), need);
        buf = make_buffer(c, cap, usage);
        return true;
    };
    bool changed = !LV.group;
    changed |= grow(LV.vbuf, LV.vcap, LV.vertices.size() * sizeof(Vtx), static_cast<WGPUBufferUsage>(WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst));
    changed |= grow(LV.sbuf, LV.scap, LV.setups.size() * 4, static_cast<WGPUBufferUsage>(WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst));
    changed |= grow(LV.pbuf, LV.pcap, LV.params.size() * 256, static_cast<WGPUBufferUsage>(WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst));
    if (changed) {
        if (LV.group) memlog::del_group(LV.group);
        WGPUBindGroupEntry e[8] = {};
        WGPUBuffer buffers[8] = {c.vram, c.tables, LV.pbuf, LV.sbuf, c.clut, c.scratch, c.cache, LV.dummy_buf};
        for (int i = 0; i < 8; ++i) {
            e[i] = WGPU_BIND_GROUP_ENTRY_INIT;
            e[i].binding = static_cast<std::uint32_t>(i);
            e[i].buffer = buffers[i];
            e[i].size = i == 2 ? 256 : WGPU_WHOLE_SIZE;
        }
        WGPUBindGroupDescriptor d = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
        d.layout = c.layout;
        d.entryCount = 8;
        d.entries = e;
        LV.group = memlog::new_group(c.device, &d);
        ++LV.render_gen;
    }
    const std::uint64_t vb = LV.vertices.size() * sizeof(Vtx), sb = LV.setups.size() * 4, pb = LV.params.size() * 256;
    if (!os::env("PS2_GPU_HW_WATCH").empty()) {  // the test aid of watch_word: a hash of the bytes this submit sends, to compare runs
        const auto hash = [](const void *data, std::uint64_t n) {
            std::uint64_t h = 1469598103934665603ull;
            const auto *b = static_cast<const std::uint8_t *>(data);
            for (std::uint64_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull;
            return h;
        };
        std::fprintf(stderr, "[hw-watch] data serial %llu v %llu %016llx s %llu %016llx p %llu %016llx\n", static_cast<unsigned long long>(LV.serial), static_cast<unsigned long long>(vb),
                     static_cast<unsigned long long>(hash(LV.vertices.data(), vb)), static_cast<unsigned long long>(sb), static_cast<unsigned long long>(hash(LV.setups.data(), sb)),
                     static_cast<unsigned long long>(pb), static_cast<unsigned long long>(hash(LV.params.data(), pb)));
    }
    if (g_memlog) memlog::S.upload_bytes += vb + sb + pb, memlog::S.sub_upload += vb + sb + pb;
    if (!stage_write(vb, sb, pb)) {  // the old way (PS2_GPU_HW_STAGE=0, or a staging buffer could not be had): one queue write per kind
        if (vb) wgpuQueueWriteBuffer(c.queue, LV.vbuf, 0, LV.vertices.data(), vb);
        if (sb) wgpuQueueWriteBuffer(c.queue, LV.sbuf, 0, LV.setups.data(), sb);
        if (pb) wgpuQueueWriteBuffer(c.queue, LV.pbuf, 0, LV.params.data(), pb);
    }
}

namespace live {

WGPUBindGroup render_group_of(Gen &g, WGPUBuffer view_buf) {
    Context &c = *g_gpu.ctx;
    if (!view_buf && g.render_group && g.render_gen == LV.render_gen) return g.render_group;
    WGPUBindGroupEntry re[7] = {};
    for (int i = 0; i < 7; ++i) re[i] = WGPU_BIND_GROUP_ENTRY_INIT, re[i].binding = static_cast<std::uint32_t>(i), re[i].size = WGPU_WHOLE_SIZE;
    re[0].buffer = LV.sbuf;
    re[1].buffer = LV.dummy_buf;
    re[2].buffer = view_buf ? view_buf : g.view_buf;
    re[3].buffer = c.vram;
    re[4].buffer = c.tables;
    re[5].buffer = c.clut;
    re[6].textureView = LV.dummy_view;
    WGPUBindGroupDescriptor d = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    d.layout = g_gpu.render_layout;
    d.entryCount = 7;
    d.entries = re;
    WGPUBindGroup grp = memlog::new_group(c.device, &d);
    if (view_buf) return grp;  // a group of its own for one depth load (the caller releases it after the submit)
    if (g.render_group) memlog::del_group(g.render_group);
    g.render_group = grp;
    g.render_gen = LV.render_gen;
    return grp;
}

// The render bind group of picture g whose texture slot holds picture src (src == g: g's shadow copy). Kept per picture, four at most, made again when the
// shared buffers were made anew (render_gen).
WGPUBindGroup src_group_of(Gen &g, Gen &src) {
    Gen::SrcGroup *slots = g.src_groups;
    for (int i = 0; i < 4; ++i) {
        if (slots[i].group && slots[i].src == src.id && slots[i].gen == LV.render_gen) {
            const Gen::SrcGroup hit = slots[i];
            for (int k = i; k > 0; --k) slots[k] = slots[k - 1];
            slots[0] = hit;
            return hit.group;
        }
    }
    Context &c = *g_gpu.ctx;
    WGPUBindGroupEntry re[7] = {};
    for (int i = 0; i < 7; ++i) re[i] = WGPU_BIND_GROUP_ENTRY_INIT, re[i].binding = static_cast<std::uint32_t>(i), re[i].size = WGPU_WHOLE_SIZE;
    re[0].buffer = LV.sbuf;
    re[1].buffer = LV.dummy_buf;
    re[2].buffer = g.view_buf;
    re[3].buffer = c.vram;
    re[4].buffer = c.tables;
    re[5].buffer = c.clut;
    re[6].textureView = &src == &g ? g.shadow_view : src.color_view;
    WGPUBindGroupDescriptor d = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    d.layout = g_gpu.render_layout;
    d.entryCount = 7;
    d.entries = re;
    WGPUBindGroup grp = memlog::new_group(c.device, &d);
    if (slots[3].group) memlog::del_group(slots[3].group);
    for (int k = 3; k > 0; --k) slots[k] = slots[k - 1];
    slots[0] = {src.id, LV.render_gen, grp};
    return grp;
}

std::vector<WGPUBindGroup> g_temp_groups;
std::vector<WGPUBuffer> g_temp_buffers;

}  // namespace live

// Every pass costs the window thread time in wgpu and in the driver (a new command buffer and its barriers), whatever is in it. PS2_GPU_HW_SHARE=1 lets the
// card's loads and write-backs (compute work) share the caller's compute pass with the painter's commands around them, so only a render pass ends it.
// Off by default: on an Intel laptop it saved no measurable time.
const bool g_share_pass = os::env("PS2_GPU_HW_SHARE") == "1";

WGPUComputePassEncoder live_encode(WGPUCommandEncoder enc, std::uint32_t upto, WGPUComputePassEncoder pass) {
    using namespace live;
    Tm tm(6);
    Context &c = *g_gpu.ctx;
    WGPUComputePassEncoder cp = pass;
    const auto open = [&] {
        if (!cp) cp = memlog::begin_compute(enc, nullptr);
    };
    const auto close = [&] {
        if (cp) {
            wgpuComputePassEncoderEnd(cp);
            wgpuComputePassEncoderRelease(cp);
            cp = nullptr;
        }
    };
    if (!g_share_pass) close();
    if (g_stage_copy) {  // the frame's data from the staging buffer, before the first of the card's passes that reads it (a copy cannot be inside a pass)
        close();
        WGPUBuffer from = g_stage[g_stage_now].buf;
        if (g_stage_v) wgpuCommandEncoderCopyBufferToBuffer(enc, from, 0, LV.vbuf, 0, g_stage_v);
        if (g_stage_s) wgpuCommandEncoderCopyBufferToBuffer(enc, from, g_stage_soff, LV.sbuf, 0, g_stage_s);
        if (g_stage_p) wgpuCommandEncoderCopyBufferToBuffer(enc, from, g_stage_poff, LV.pbuf, 0, g_stage_p);
        g_stage_copy = false;
    }
    while (LV.cursor < LV.ops.size() && LV.ops[LV.cursor].after <= upto) {
        const LOp &op = LV.ops[LV.cursor++];
        switch (op.kind) {
        case LOp::Load:
        case LOp::Store:
        case LOp::StoreZ:
        case LOp::Display: {
            open();
            const std::uint32_t offset = op.param * 256u;
            wgpuComputePassEncoderSetPipeline(cp, op.kind == LOp::Load ? g_gpu.load : op.kind == LOp::Store ? g_gpu.store : op.kind == LOp::StoreZ ? g_gpu.store_z : g_gpu.display);
            wgpuComputePassEncoderSetBindGroup(cp, 0, LV.group, 1, &offset);
            wgpuComputePassEncoderSetBindGroup(cp, 1, op.kind == LOp::Load ? op.g->load_group : op.kind == LOp::StoreZ ? op.g->store_z_group : op.g->store_group, 0, nullptr);
            wgpuComputePassEncoderDispatchWorkgroups(cp, op.x, op.y, 1);
            break;
        }
        case LOp::LoadZ: {
            close();
            // the setup record that holds the depth buffer's format words is the first word of this op's parameters
            const float vv[4] = {static_cast<float>(op.g->w), static_cast<float>(op.g->h), static_cast<float>(LV.params[op.param][0] / kHwSetup), 0.0f};
            WGPUBuffer vb = make_buffer(c, 16, static_cast<WGPUBufferUsage>(WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst));
            wgpuQueueWriteBuffer(c.queue, vb, 0, vv, sizeof vv);
            WGPUBindGroup grp = render_group_of(*op.g, vb);
            g_temp_buffers.push_back(vb);
            g_temp_groups.push_back(grp);
            WGPURenderPassDepthStencilAttachment depth = WGPU_RENDER_PASS_DEPTH_STENCIL_ATTACHMENT_INIT;
            depth.view = op.g->depth_view;
            depth.depthLoadOp = WGPULoadOp_Load;
            depth.depthStoreOp = WGPUStoreOp_Store;
            WGPURenderPassDescriptor rd = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
            rd.depthStencilAttachment = &depth;
            WGPURenderPassEncoder rp = memlog::begin_render(enc, &rd);
            wgpuRenderPassEncoderSetPipeline(rp, LV.load_z);
            wgpuRenderPassEncoderSetBindGroup(rp, 0, grp, 0, nullptr);
            wgpuRenderPassEncoderSetScissorRect(rp, op.rect[0], op.rect[1], op.rect[2], op.rect[3]);
            wgpuRenderPassEncoderDraw(rp, 3, 1, 0, 0);
            wgpuRenderPassEncoderEnd(rp);
            wgpuRenderPassEncoderRelease(rp);
            break;
        }
        case LOp::Copy: {  // the box a batch reads of the picture it draws on, into the picture's shadow copy
            close();
            WGPUTexelCopyTextureInfo from = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT, to = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
            from.texture = op.g->color;
            from.origin = {op.rect[0], op.rect[1], 0};
            to.texture = op.g->shadow;
            to.origin = from.origin;
            const WGPUExtent3D size = {op.rect[2], op.rect[3], 1};
            wgpuCommandEncoderCopyTextureToTexture(enc, &from, &to, &size);
            break;
        }
        case LOp::Draw: {
            close();
            Gen &g = *op.g;
            WGPURenderPassColorAttachment color = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
            color.view = g.color_view;
            color.loadOp = WGPULoadOp_Load;
            color.storeOp = WGPUStoreOp_Store;
            WGPURenderPassDepthStencilAttachment depth = WGPU_RENDER_PASS_DEPTH_STENCIL_ATTACHMENT_INIT;
            depth.view = g.depth_view;
            depth.depthLoadOp = WGPULoadOp_Load;
            depth.depthStoreOp = WGPUStoreOp_Store;
            WGPURenderPassDescriptor rd = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
            rd.colorAttachmentCount = 1;
            rd.colorAttachments = &color;
            rd.depthStencilAttachment = &depth;
            WGPURenderPassEncoder rp = memlog::begin_render(enc, &rd);
            wgpuRenderPassEncoderSetVertexBuffer(rp, 0, LV.vbuf, 0, WGPU_WHOLE_SIZE);
            wgpuRenderPassEncoderSetBindGroup(rp, 0, op.src ? src_group_of(g, *op.src) : render_group_of(g, nullptr), 0, nullptr);
            // card batches that follow one another on one picture (nothing between them that writes back or loads) are drawn in one render pass
            const LOp *cur_op = &op;
            // the pass's state: a pipeline, scissor or blend constant is set only when it changes (every command costs wgpu time when the encoder is finished)
            WGPURenderPipeline cur_pipe = nullptr;
            std::uint32_t cur_sc[4] = {~0u, ~0u, ~0u, ~0u};
            double cur_bc = -1.0;
            for (;;) {
            for (std::uint32_t ri = cur_op->run_first; ri < cur_op->run_first + cur_op->run_count; ++ri) {
                Run &r = LV.runs[ri];
                {
                    Tm tm_wait(1);
                    collect(r);
                }
                LV.st.runs += r.cuts.size();
                if (std::memcmp(cur_sc, r.scissor, sizeof cur_sc) != 0) {
                    wgpuRenderPassEncoderSetScissorRect(rp, r.scissor[0], r.scissor[1], r.scissor[2], r.scissor[3]);
                    std::memcpy(cur_sc, r.scissor, sizeof cur_sc);
                }
                if (r.look.blend && (r.look.src == kConst || r.look.src == kInvConst || r.look.dst == kConst || r.look.dst == kInvConst) && cur_bc != r.look.constant) {
                    const WGPUColor bc = {r.look.constant, r.look.constant, r.look.constant, r.look.constant};
                    wgpuRenderPassEncoderSetBlendConstant(rp, &bc);
                    cur_bc = r.look.constant;
                }
                const auto draw = [&](bool depth_only, bool alpha_only, std::uint32_t first, std::uint32_t vertices) {
                    if (WGPURenderPipeline p = pipeline_for(r.look, depth_only, alpha_only)) {
                        if (p != cur_pipe) wgpuRenderPassEncoderSetPipeline(rp, p), cur_pipe = p;
                        wgpuRenderPassEncoderDraw(rp, vertices, 1, first, 0);
                        ++LV.st.draws;
                    }
                };
                const auto piece = [&](std::uint32_t first, std::uint32_t vertices) {
                    if (r.look.alpha_two && !g_dual) draw(false, true, first, vertices);  // the alpha channel first (it only tests depth), then colour (which writes it)
                    draw(false, false, first, vertices);
                    if (r.look.two_pass) draw(true, false, first, vertices);
                };
                static const bool one_by_one = os::env("PS2_GPU_HW_ONE_DRAW") == "1";  // a test aid: one draw call per shape
                if (one_by_one) {
                    std::uint32_t at = r.first_vertex;
                    for (std::uint32_t sh = 0; sh < r.shapes; ++sh) piece(at, r.counts[sh]), at += r.counts[sh];
                } else if (r.cuts.empty()) {
                    piece(r.first_vertex, r.vertices);
                } else {  // the independent segments one after the other: a cut shape starts the next one
                    std::uint32_t at = r.first_vertex, first = at, ci = 0;
                    for (std::uint32_t sh = 0; sh < r.shapes; ++sh) {
                        if (ci < r.cuts.size() && r.cuts[ci] == sh) {
                            ++ci;
                            piece(first, at - first);
                            first = at;
                        }
                        at += r.counts[sh];
                    }
                    piece(first, at - first);
                }
            }
            if (g_merge_passes && LV.cursor < LV.ops.size() && LV.ops[LV.cursor].kind == LOp::Draw && LV.ops[LV.cursor].g == op.g && LV.ops[LV.cursor].src == op.src && LV.ops[LV.cursor].after == cur_op->after) {
                cur_op = &LV.ops[LV.cursor++];
                continue;
            }
            break;
            }
            wgpuRenderPassEncoderEnd(rp);
            wgpuRenderPassEncoderRelease(rp);
            break;
        }
        }
    }
    if (!g_share_pass) close();
    return cp;
}

// PS2_GPU_HW_WATCH=<word>: a test aid. After every submit, wait for the card and read that 32-bit word of graphics memory back; print it with the submit's
// card ops (L load, S store, Z depth store, l depth load, D draw, P display; the number is the painter commands before it) whenever it changed.
void watch_word() {
    static const long long word = os::env("PS2_GPU_HW_WATCH").empty() ? -1 : std::strtoll(os::env("PS2_GPU_HW_WATCH").c_str(), nullptr, 10);
    static std::uint32_t last = 0;
    static std::uint64_t submits = 0;
    if (word < 0) return;
    ++submits;
    Context &c = *g_gpu.ctx;
    const std::uint64_t off = static_cast<std::uint64_t>(word) * 4 & ~std::uint64_t(255);
    WGPUBufferDescriptor d = WGPU_BUFFER_DESCRIPTOR_INIT;
    d.size = 256;
    d.usage = static_cast<WGPUBufferUsage>(WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst);
    WGPUBuffer rb = wgpuDeviceCreateBuffer(c.device, &d);
    WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(c.device, nullptr);
    wgpuCommandEncoderCopyBufferToBuffer(enc, c.vram, off, rb, 0, 256);
    WGPUCommandBuffer cb = wgpuCommandEncoderFinish(enc, nullptr);
    wgpuQueueSubmit(c.queue, 1, &cb);
    wgpuCommandBufferRelease(cb);
    wgpuCommandEncoderRelease(enc);
    bool done = false;
    WGPUBufferMapCallbackInfo mi = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
    mi.mode = WGPUCallbackMode_AllowProcessEvents;
    mi.callback = [](WGPUMapAsyncStatus, WGPUStringView, void *flag, void *) { *static_cast<bool *>(flag) = true; };
    mi.userdata1 = &done;
    wgpuBufferMapAsync(rb, WGPUMapMode_Read, 0, 256, mi);
    while (!done) wgpuDevicePoll(c.device, true, nullptr);
    std::uint32_t v = 0;
    if (const void *p = wgpuBufferGetConstMappedRange(rb, 0, 256)) std::memcpy(&v, static_cast<const std::uint8_t *>(p) + (word * 4 - off), 4);
    wgpuBufferUnmap(rb);
    wgpuBufferRelease(rb);
    if (v == last) return;
    last = v;
    std::string ops;
    for (const live::LOp &o : live::LV.ops) {
        static const char kinds[] = "LSZlDPC";
        char b[32];
        std::snprintf(b, sizeof b, " %c%u%s", kinds[o.kind], o.after, o.g ? "" : "?");
        ops += b;
        if (ops.size() > 400) {
            ops += " ...";
            break;
        }
    }
    std::fprintf(stderr, "[hw-watch] submit %llu serial %llu word %08x ops%s\n", static_cast<unsigned long long>(submits), static_cast<unsigned long long>(live::LV.serial), v, ops.c_str());
}

void live_submitted() {
    using namespace live;
    if (!LV.on) return;
    Tm tm(7);
    watch_word();
    if (g_log) {  // what comes before each draw (PS2_GPU_HW_LOG=1): the previous op's kind, and for a draw whether it is on the same picture and at the same point
        for (std::size_t i = 1; i < LV.ops.size(); ++i) {
            const LOp &a = LV.ops[i - 1], &b = LV.ops[i];
            if (b.kind != LOp::Draw) continue;
            const int k = a.kind == LOp::Copy ? 10 : a.kind != LOp::Draw ? a.kind : 6 + (a.g == b.g ? 0 : 2) + (a.after == b.after ? 0 : 1);
            ++g_before_draw[k];
        }
    }
    if (g_stage_now >= 0) {  // the submit that reads the staging buffer is on the queue: map it again (the callback comes once the card has finished it)
        stage_map(g_stage[g_stage_now]);
        g_stage_now = -1;
        g_stage_copy = false;
    }
    LV.ops.clear();
    LV.cursor = 0;
    LV.vertices.clear();
    LV.setups.clear();
    LV.params.clear();
    LV.runs.clear();
    ++LV.serial;
    for (const auto &g : LV.retired) purge_src(g->id);
    LV.retired.clear();
    for (WGPUBindGroup g : g_temp_groups) memlog::del_group(g);
    for (WGPUBuffer b : g_temp_buffers) memlog::del_buffer(b);
    g_temp_groups.clear();
    g_temp_buffers.clear();
}

void live_stats() {
    using namespace live;
    if (!LV.on) return;
    if (g_log) {
        std::fprintf(stderr, "[hw-live] before a draw:");
        for (std::uint64_t n : g_before_draw) std::fprintf(stderr, " %llu", static_cast<unsigned long long>(n));
        std::fprintf(stderr, " (load, store, depth store, depth load, -, display, draw same picture, same picture later, other picture, other picture later, copy)\n");
    }
    const Stats &s = LV.st;
    std::fprintf(stderr, "[hw-live] batches: card %llu (%llu shapes, %llu runs, %llu draws), painter %llu (%llu shapes), fallbacks %llu\n", static_cast<unsigned long long>(s.card_batches),
                 static_cast<unsigned long long>(s.card_shapes), static_cast<unsigned long long>(s.runs), static_cast<unsigned long long>(s.draws), static_cast<unsigned long long>(s.painter_batches),
                 static_cast<unsigned long long>(s.painter_shapes), static_cast<unsigned long long>(s.fallbacks));
    std::fprintf(stderr, "[hw-live] textures that are pictures on the card (PS2_GPU_HW_FEEDBACK=%d): %llu batches sampled another picture, %llu a copy of their own (%llu px copied), %llu refused (written back); %llu 24-bit pictures left on the card for textures of the top byte\n", g_feedback,
                 static_cast<unsigned long long>(s.fed_other), static_cast<unsigned long long>(s.fed_self), static_cast<unsigned long long>(s.copy_px), static_cast<unsigned long long>(s.fed_refused),
                 static_cast<unsigned long long>(s.top_byte_kept));
    std::fprintf(stderr, "[hw-live] pipelines: %llu made in the background, %llu failed, %llu setups waited for one\n[hw-live] loads %llu colour (%llu px) %llu depth; write-backs %llu colour (%llu px) %llu depth; growths %llu evictions %llu state drops %llu\n", static_cast<unsigned long long>(g_async_done), static_cast<unsigned long long>(g_async_failed), static_cast<unsigned long long>(g_waited_routes), static_cast<unsigned long long>(s.loads),
                 static_cast<unsigned long long>(s.load_px), static_cast<unsigned long long>(s.load_zs), static_cast<unsigned long long>(s.stores), static_cast<unsigned long long>(s.store_px),
                 static_cast<unsigned long long>(s.store_zs), static_cast<unsigned long long>(s.growths), static_cast<unsigned long long>(s.evictions), static_cast<unsigned long long>(s.state_drops));
    std::fprintf(stderr, "[hw-live] write-backs by cause: reader %llu writer %llu overlap %llu texture %llu load %llu growth %llu painter %llu eager %llu other %llu; display from a target %llu, from memory %llu\n",
                 static_cast<unsigned long long>(s.by_reader), static_cast<unsigned long long>(s.by_writer), static_cast<unsigned long long>(s.by_overlap), static_cast<unsigned long long>(s.by_read),
                 static_cast<unsigned long long>(s.by_load), static_cast<unsigned long long>(s.by_grow), static_cast<unsigned long long>(s.by_painter), static_cast<unsigned long long>(s.by_eager),
                 static_cast<unsigned long long>(s.by_other), static_cast<unsigned long long>(s.display_card), static_cast<unsigned long long>(s.display_memory));
    if (g_time) {
        for (int i = 0; i < kTimes; ++i) std::fprintf(stderr, "[hw-live] time %-28s %8.1f ms\n", g_tname[i], static_cast<double>(g_tns[i]) / 1e6);
        std::fprintf(stderr, "[hw-live] submits: %llu with card work, %llu without\n", static_cast<unsigned long long>(g_wgpu_submits[1]), static_cast<unsigned long long>(g_wgpu_submits[0]));
    }
    std::fprintf(stderr, "[hw-live] batches with fewer than %u shapes that went to the painter: %llu would have started a card stretch, %llu would have made the card write a picture back first\n", g_switch_min, static_cast<unsigned long long>(g_small_switch),
                 static_cast<unsigned long long>(g_small_dirty));
    if (g_time) std::fprintf(stderr, "[hw-live] runs ended inside a batch by a change of: any %llu, scissor %llu, ztest %llu, zwrite %llu, alpha-test split %llu, blend %llu, mask/alpha %llu\n", static_cast<unsigned long long>(g_cut_why[0]), static_cast<unsigned long long>(g_cut_why[1]), static_cast<unsigned long long>(g_cut_why[2]), static_cast<unsigned long long>(g_cut_why[3]), static_cast<unsigned long long>(g_cut_why[4]), static_cast<unsigned long long>(g_cut_why[5]), static_cast<unsigned long long>(g_cut_why[6]));
    std::fprintf(stderr, "[hw-live] independence: %llu runs (%llu shapes) checked pixel by pixel, %llu cuts (hash %016llx); %llu batches (%llu shapes) with a run over %u shapes that needed the check went to the painter\n", static_cast<unsigned long long>(g_checked_runs),
                 static_cast<unsigned long long>(g_checked_shapes), static_cast<unsigned long long>(g_checked_cuts), static_cast<unsigned long long>(g_cut_hash), static_cast<unsigned long long>(g_heavy_batches), static_cast<unsigned long long>(g_heavy_shapes), g_indep_max);
    if (const std::string path = os::env("PS2_GPU_HW_PAL_DUMP"); !path.empty() && !pal::wseq.empty()) {  // debugging aid: the record as 4 MB of bytes (alpha where known, 0xFF + 256 words...) for a comparison with PS2_DUMP_VRAM_RAW
        std::vector<std::uint16_t> out(pal::kWords);
        for (std::uint32_t w = 0; w < pal::kWords; ++w) out[w] = pal::word_known(w) ? pal::walpha[w] : 0xFFFFu;
        if (std::FILE *f = std::fopen(path.c_str(), "wb")) {
            std::fwrite(out.data(), 2, out.size(), f);
            std::fclose(f);
        }
    }
    std::fprintf(stderr, "[hw-live] palette record: %llu palette loads, %llu with every entry known; %llu shapes of refinable setups looked at, %llu proved to pass the alpha test (one pass, no check)\n", static_cast<unsigned long long>(pal::n_loads),
                 static_cast<unsigned long long>(pal::n_loads_known), static_cast<unsigned long long>(pal::n_shapes_tried), static_cast<unsigned long long>(pal::n_shapes_pass));
    for (const auto &kv : s.kinds) std::fprintf(stderr, "[hw-live] card draws %llu batches (%llu shapes): %s\n", static_cast<unsigned long long>(kv.second.first), static_cast<unsigned long long>(kv.second.second), kv.first.c_str());
    for (const auto &kv : s.refused) std::fprintf(stderr, "[hw-live] painter keeps: %s (%llu setups)\n", kv.first.c_str(), static_cast<unsigned long long>(kv.second));
}

std::uint64_t live_clock() {
    if (!g_time) return 0;
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

void live_wgpu_time(bool card, std::uint64_t before_finish, std::uint64_t before_submit, std::uint64_t after) {
    if (!g_time || !before_finish) return;
    g_tns[card ? 10 : 12] += before_submit - before_finish;
    g_tns[card ? 11 : 13] += after - before_submit;
    ++g_wgpu_submits[card ? 1 : 0];
}

// one line of what the card path holds (our own count of what we made and dropped), what wgpu says it holds in all (the painter's objects included), the card path's
// bookkeeping and the process's resident memory. `made:` is what was created since the last line. Parsed by scripts/hwmem.py.
void memlog_report(void *instance, void *queue, std::uint64_t frame) {
    using namespace live;
    if (queue) {  // one callback per submit: submits minus callbacks is how far the card is behind
        WGPUQueueWorkDoneCallbackInfo cb = WGPU_QUEUE_WORK_DONE_CALLBACK_INFO_INIT;
        cb.mode = WGPUCallbackMode_AllowProcessEvents;
        cb.callback = [](WGPUQueueWorkDoneStatus, WGPUStringView, void *, void *) { memlog::S.done.fetch_add(1); };
        wgpuQueueOnSubmittedWorkDone(static_cast<WGPUQueue>(queue), cb);
        const std::uint64_t sub = memlog::S.submits.fetch_add(1) + 1, done = memlog::S.done.load();
        if (sub > done) memlog::S.max_inflight = std::max(memlog::S.max_inflight, sub - done);
        memlog::S.max_passes = std::max(memlog::S.max_passes, memlog::S.sub_passes);
        memlog::S.max_upload = std::max(memlog::S.max_upload, memlog::S.sub_upload);
        memlog::S.sub_passes = memlog::S.sub_upload = 0;
    }
    static std::uint64_t next = 0;
    static const auto t0 = std::chrono::steady_clock::now();
    static memlog::Tally p_tex, p_buf, p_grp, p_rpipe, p_enc, p_rp, p_cp;
    if (frame < next) return;
    next = frame + g_memlog_every;
    const auto ull = [](std::uint64_t v) { return static_cast<unsigned long long>(v); };
    const auto mb = [](std::uint64_t v) { return static_cast<double>(v) / 1048576.0; };
    memlog::Tally tex, buf, grp, rpipe, enc, rp, cp;
    std::uint64_t tex_live = 0, buf_live = 0;
    {
        std::lock_guard<std::mutex> lock(memlog::S.m);
        tex = memlog::S.tex, buf = memlog::S.buf, grp = memlog::S.grp, rpipe = memlog::S.rpipe, enc = memlog::S.enc, rp = memlog::S.rp, cp = memlog::S.cp;
    }
    tex_live = tex.made_bytes - tex.freed_bytes, buf_live = buf.made_bytes - buf.freed_bytes;
    const memlog::Split sp = memlog::split_mb();
    WGPUGlobalReport r{};
    if (instance) wgpuGenerateReport(static_cast<WGPUInstance>(instance), &r);
    const WGPUHubReport &h = r.hub;
    std::fprintf(stderr,
                 "[memlog] frame=%llu t=%.1f rss=%.1f card_tex=%llu card_tex_mb=%.1f card_buf=%llu card_buf_mb=%.1f card_bg=%llu card_rpipe=%llu"
                 " made_tex=%llu made_buf=%llu made_bg=%llu made_rpipe=%llu made_enc=%llu made_rp=%llu made_cp=%llu"
                 " rss_anon=%.1f rss_file=%.1f rss_shmem=%.1f commit=%.1f upload_mb=%.1f threads=%ld submits=%llu inflight=%llu max_inflight=%llu w_buf=%llu/%llu w_tex=%llu/%llu w_view=%llu w_bg=%llu/%llu w_rpipe=%llu w_cpipe=%llu w_cmdbuf=%llu w_shader=%llu w_layout=%llu w_playout=%llu w_query=%llu",
                 ull(frame), std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(), memlog::resident_mb(), ull(tex.made - tex.freed), mb(tex_live), ull(buf.made - buf.freed), mb(buf_live),
                 ull(grp.made - grp.freed), ull(rpipe.made - rpipe.freed), ull(tex.made - p_tex.made), ull(buf.made - p_buf.made), ull(grp.made - p_grp.made), ull(rpipe.made - p_rpipe.made),
                 ull(enc.made - p_enc.made), ull(rp.made - p_rp.made), ull(cp.made - p_cp.made), sp.anon, sp.file, sp.shmem, sp.commit, mb(memlog::S.upload_bytes), sp.threads, ull(memlog::S.submits.load()), ull(memlog::S.submits.load() - memlog::S.done.load()), ull(memlog::S.max_inflight), ull(h.buffers.numAllocated), ull(h.buffers.numKeptFromUser), ull(h.textures.numAllocated),
                 ull(h.textures.numKeptFromUser), ull(h.textureViews.numAllocated), ull(h.bindGroups.numAllocated), ull(h.bindGroups.numKeptFromUser), ull(h.renderPipelines.numAllocated),
                 ull(h.computePipelines.numAllocated), ull(h.commandBuffers.numAllocated), ull(h.shaderModules.numAllocated), ull(h.bindGroupLayouts.numAllocated), ull(h.pipelineLayouts.numAllocated),
                 ull(h.querySets.numAllocated));
    if (LV.on)
        std::fprintf(stderr, " live_targets=%zu live_retired=%zu live_heights=%zu live_pictures_mb=%.1f live_pipes=%zu live_vtx_mb=%.1f live_setups_mb=%.1f live_params_mb=%.1f live_ops=%zu live_runs_mb=%.1f",
                     LV.targets.size(), LV.retired.size(), LV.heights.size(), mb(LV.bytes), g_gpu.pipelines.size(), mb(LV.vertices.capacity() * sizeof(Vtx)), mb(LV.setups.capacity() * 4),
                     mb(LV.params.capacity() * 256), LV.ops.capacity(), mb(LV.runs.capacity() * sizeof(Run)));
    std::fprintf(stderr, " max_passes=%llu max_upload_mb=%.1f stage_mb=%.1f\n", ull(memlog::S.max_passes), mb(memlog::S.max_upload), mb(live::stage_bytes()));
    p_tex = tex, p_buf = buf, p_grp = grp, p_rpipe = rpipe, p_enc = enc, p_rp = rp, p_cp = cp;
    memlog::S.max_passes = memlog::S.max_upload = 0;
    memlog::S.max_inflight = 0;
    memlog::S.upload_bytes = 0;
}

bool live_init(Context &ctx) {
    using namespace live;
    if (ctx.scale != 1) {
        std::fprintf(stderr, "[hw-live] needs the internal scale 1: the painter draws everything\n");
        return false;
    }
    LV.ctx = ctx;
    g_gpu.ctx = &LV.ctx;
    g_dual = want_dual(ctx.device);
    pal::on = g_pal_refine = os::env("PS2_GPU_HW_PAL") != "0";
    g_direct = true;
    g_linear = true;
    g_check = true;
    g_eager = os::env("PS2_GPU_HW_EAGER") == "1";
    if (const std::string fb = os::env("PS2_GPU_HW_FEEDBACK"); !fb.empty()) g_feedback = fb == "other" ? 1 : fb == "self" ? 2 : fb == "0" ? 0 : 3;
    g_indep = os::env("PS2_GPU_HW_INDEP") != "0";
    g_indep_depth = os::env("PS2_GPU_HW_INDEP") != "3";
    g_indep_exact = false;
    g_indep_max = 0;
    g_indep_pix = 0;
    g_stamp_scalar = os::env("PS2_GPU_HW_STAMP") == "scalar";
    if (!g_pool) {  // helper threads for the independence check
        unsigned n = os::env("PS2_ONE_THREAD").empty() ? std::clamp(std::thread::hardware_concurrency() / 4u, 2u, 4u) : 0u;  // 2 on a 4-thread laptop, 4 on a 16-thread PC
        if (const std::string t = os::env("PS2_HW_CHECK_THREADS"); !t.empty()) n = static_cast<unsigned>(std::atoi(t.c_str()));
        if (n) g_pool = new CheckPool(), g_pool->start(n);
    }
    g_merge_passes = os::env("PS2_GPU_HW_MERGE") != "0";
    g_check_chunk = g_pool ? 1024u : 0u;  // chunks only pay when helpers can take them at once (PS2_HW_CHECK_CHUNK=<shapes>, 0: one chunk per run)
    if (const std::string t = os::env("PS2_HW_CHECK_CHUNK"); !t.empty()) g_check_chunk = static_cast<std::uint32_t>(std::atoi(t.c_str()));
    g_switch_min = 256;
    if (const std::string m = os::env("PS2_GPU_HW_SWITCH_MIN"); !m.empty()) g_switch_min = static_cast<std::uint32_t>(std::atoll(m.c_str()));
    if (const std::string m = os::env("PS2_GPU_HW_INDEP_PIXELS"); !m.empty()) g_indep_pix = static_cast<std::uint64_t>(std::atoll(m.c_str()));
    if (const std::string m = os::env("PS2_GPU_HW_INDEP_MAX"); !m.empty()) g_indep_max = m == "all" ? ~0u : static_cast<std::uint32_t>(std::atoll(m.c_str()));
    PRE.on = g_pool && g_indep && g_check_chunk > 0 && g_indep_max == ~0u && os::env("PS2_GPU_HW_PRE") != "0";  // the checks start in draw() (only where no run goes to the painter for its length)
    g_time = os::env("PS2_GPU_HW_TIME") == "1";
    g_log = os::env("PS2_GPU_HW_LOG") == "1";
    if (const std::string r = os::env("PS2_GPU_HW_ROUND"); !r.empty()) g_round = static_cast<float>(std::atof(r.c_str()));
    if (const std::string m = os::env("PS2_GPU_HW_CAP_MB"); !m.empty()) LV.cap_bytes = static_cast<std::uint64_t>(std::atoll(m.c_str())) << 20;
    if (!build_layouts(ctx) || !g_gpu.display) {
        std::fprintf(stderr, "[hw-live] the card path could not be set up: the painter draws everything\n");
        return false;
    }
    // the depth load: a full-screen triangle writes the depth the memory holds
    {
        WGPUDepthStencilState ds = WGPU_DEPTH_STENCIL_STATE_INIT;
        ds.format = WGPUTextureFormat_Depth32Float;
        ds.depthWriteEnabled = WGPUOptionalBool_True;
        ds.depthCompare = WGPUCompareFunction_Always;
        WGPUFragmentState fs = WGPU_FRAGMENT_STATE_INIT;
        fs.module = g_gpu.render_module;
        fs.entryPoint = sv("fs_load_z");
        fs.targetCount = 0;
        WGPURenderPipelineDescriptor d = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
        d.layout = g_gpu.render_pl;
        d.vertex.module = g_gpu.render_module;
        d.vertex.entryPoint = sv("vs_full");
        d.primitive.topology = WGPUPrimitiveTopology_TriangleList;
        d.primitive.cullMode = WGPUCullMode_None;
        d.depthStencil = &ds;
        d.multisample.count = 1;
        d.multisample.mask = 0xFFFFFFFFu;
        d.fragment = &fs;
        wgpuDevicePushErrorScope(ctx.device, WGPUErrorFilter_Validation);
        LV.load_z = memlog::new_rpipe(ctx.device, &d);
        bool done = false, bad = false;
        struct Scope {
            bool *done, *bad;
        } scope{&done, &bad};
        WGPUPopErrorScopeCallbackInfo cb = WGPU_POP_ERROR_SCOPE_CALLBACK_INFO_INIT;
        cb.mode = WGPUCallbackMode_AllowProcessEvents;
        cb.callback = [](WGPUPopErrorScopeStatus, WGPUErrorType type, WGPUStringView message, void *u1, void *) {
            Scope *s2 = static_cast<Scope *>(u1);
            if (type != WGPUErrorType_NoError) {
                *s2->bad = true;
                std::fprintf(stderr, "[hw-live] the depth load pipeline did not build: %.*s\n", static_cast<int>(message.length == WGPU_STRLEN ? std::strlen(message.data) : message.length), message.data);
            }
            *s2->done = true;
        };
        cb.userdata1 = &scope;
        wgpuDevicePopErrorScope(ctx.device, cb);
        while (!done) wgpuDevicePoll(ctx.device, true, nullptr);
        if (bad || !LV.load_z) return false;
    }
    LV.dummy_buf = make_buffer(ctx, 256, WGPUBufferUsage_Storage);
    WGPUTextureDescriptor td = WGPU_TEXTURE_DESCRIPTOR_INIT;
    td.dimension = WGPUTextureDimension_2D;
    td.size = {1, 1, 1};
    td.mipLevelCount = 1;
    td.sampleCount = 1;
    td.format = WGPUTextureFormat_RGBA8Unorm;
    td.usage = WGPUTextureUsage_TextureBinding;
    LV.dummy_tex = memlog::new_texture(ctx.device, &td);
    LV.dummy_view = wgpuTextureCreateView(LV.dummy_tex, nullptr);
    g_async_pipes = os::env("PS2_GPU_HW_ASYNC") != "0";
    if (g_async_pipes) load_pipeline_keys();
    LV.on = true;
    std::fprintf(stderr, "[hw-live] the running game draws its plain shapes through the card path (PS2_GPU_HW=1)%s%s; dual-source blending %s\n", g_eager ? ", EAGER" : "", g_log ? ", LOG" : "", g_dual ? "on" : "off");
    return true;
}

}
#else  // the browser build has no replay benchmark: nothing to measure there, so the trial is a set of empty functions

namespace ps2::gs::gpu::hw {

bool g_memlog = false;
bool enabled() { return false; }
bool match() { return false; }
bool left_out(const std::uint32_t *, const std::uint32_t *) { return false; }
bool class_enabled() { return false; }
int class_count() { return 0; }
const char *class_name(int) { return ""; }
bool class_is_hardware(int) { return false; }
int shape_class(const std::uint32_t *, const std::uint32_t *) { return 0; }
void capture_begin() {}
bool capturing() { return false; }
void capture_batch(std::uint32_t, std::uint32_t, const std::uint32_t *, std::uint32_t, const std::uint32_t *, std::uint32_t) {}
void capture_pre(std::uint32_t, std::uint32_t, bool, std::uint64_t, std::uint64_t) {}
void survey_print() {}
bool diff_enabled() { return false; }
DiffResult &diff_result() {
    static DiffResult r;
    return r;
}
void run(Context &, std::vector<Sub> &, unsigned, void (*)(), void (*)(const char *)) {}
bool live_init(Context &) { return false; }
bool live_on() { return false; }
void live_position(std::uint32_t) {}
int live_route(const std::uint32_t *) { return 0; }
int live_look() { return -1; }
bool live_waits() { return false; }
bool live_ready_changed(std::uint64_t &) { return false; }
void live_shape(int, const std::uint32_t *, const std::uint32_t *, bool) {}
bool live_batch(const LiveBatch &) { return false; }
void live_painter(const LiveBatch &) {}
void live_reader(std::uint64_t, std::uint64_t) {}
void live_writer(std::uint64_t, std::uint64_t) {}
bool live_display(const LiveDisplay &, const std::uint32_t *, std::uint32_t) { return false; }
void live_state_loaded() {}
void pal_upload(std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, const void *, std::size_t, std::uint64_t, std::uint64_t) {}
void pal_write(std::uint32_t, std::uint32_t, std::uint32_t, int, int, int, int) {}
void pal_clut(std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, bool) {}
void pal_state(const std::uint32_t *, std::size_t) {}
bool live_pending() { return false; }
bool live_before(std::uint32_t) { return false; }
void live_prepare() {}
WGPUComputePassEncoder live_encode(WGPUCommandEncoder, std::uint32_t, WGPUComputePassEncoder pass) { return pass; }
void live_submitted() {}
void live_stats() {}
std::uint64_t live_clock() { return 0; }
void live_wgpu_time(bool, std::uint64_t, std::uint64_t, std::uint64_t) {}
void memlog_report(void *, void *, std::uint64_t) {}

}
#endif
