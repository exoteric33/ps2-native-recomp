#include "gpu.hpp"
#include "hwdraw.hpp"
#include "overlay.hpp"
#include "setup.hpp"
#include "tiles.hpp"
#include "ps2/hitch.hpp"
#include "ps2/os.hpp"
#include "ps2/system.hpp"
#include "ps2/video.hpp"

#include <webgpu/webgpu.h>
#ifndef __EMSCRIPTEN__
#include <webgpu/wgpu.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#ifdef __linux__
#include <pthread.h>
#endif

#include "trusted.hpp"
#include "hwcache.hpp"
#include "hwstat.hpp"

extern const char kGsShader[];
extern const char kBlitShader[];

namespace ps2::gs {
void swizzle_tables(std::vector<std::uint32_t> &out);
}

namespace ps2::gs::gpu {

namespace {

enum Pipeline { kUpload, kCopyRead, kCopyWrite, kLoadClut, kDisplay, kRaster, kResolve, kSnapshot, kDecode, kPipelines };
const char *const kEntryPoints[kPipelines] = {"upload", "copy_read", "copy_write", "load_clut", "display", "raster", "resolve", "snapshot", "decode"};

constexpr std::uint32_t kParamStride = 256;
constexpr std::uint32_t kClutSlots = 2048;
constexpr std::uint32_t kClutEntries = 512;
constexpr std::uint32_t kPrimWords = 28;
static_assert(kPrimWords == tiles::kPrimWords, "the tile lists (tiles.cpp) read the shape records");
// word 1 of a shape: its kind (0 triangle, 1 sprite, 2 line, 3 point) in the low byte, and kUpscaledSource when its texture is an
// upscaled picture the GS drew (gs.wgsl reads both)
constexpr std::uint32_t kKindMask = 0xFFu, kUpscaledSource = 0x100u;
static_assert(kKindMask == tiles::kKindMask, "the tile lists (tiles.cpp) read the shape records");
constexpr std::uint32_t kSetupWords = 40;  // 25 fields, then the texture, frame and depth formats as the shader needs them (fmt_words)
constexpr std::uint32_t kMipWords = 8;     // the mipmap words of a setup, in a table beside the setups (Batch::mips, make_mip)
constexpr std::uint32_t kDecodedWords = 4u << 20;
constexpr std::uint32_t kDecodePrims = 64;


Mode g_mode = Mode::off;
bool g_live = false;  // PS2_GPU_HW=1 (experimental, off by default; see the top of hwdraw.cpp): the plain shapes go through the card path
std::uint64_t g_dispatches[kPipelines] = {}, g_submits = 0;
std::uint64_t g_mip_setups = 0, g_mip_shapes = 0, g_filter_setups = 0, g_filter_shapes = 0;  // the mipmaps line of stats(): setups and shapes that read levels above 0 / only pick the filter per pixel

WGPUInstance g_instance = nullptr;
WGPUAdapter g_adapter = nullptr;
WGPUSurface g_surface = nullptr;
WGPUTextureFormat g_surface_format = WGPUTextureFormat_Undefined;
WGPUPresentMode g_present_mode = WGPUPresentMode_Fifo;
std::uint32_t g_frame_latency = 0;  // pictures the swap chain may keep in flight; 0 = the library's default (2)
std::uint32_t g_surface_width = 0, g_surface_height = 0;
WGPURenderPipeline g_blit = nullptr;
WGPUBindGroup g_blit_group = nullptr;
WGPUBuffer g_blit_params = nullptr;
// The overlay (overlay.hpp): the quick menu's panel, drawn over the game's picture in the present pass with alpha blending. Its pixels
// sit in a storage buffer like everything else here (no texture: the same code on every backend and in the browser build).
struct Overlay {
    WGPURenderPipeline pipeline = nullptr;
    WGPUBindGroupLayout layout = nullptr;
    WGPUBindGroup group = nullptr;
    WGPUBuffer pixels = nullptr, rect = nullptr;
    std::uint64_t capacity = 0;  // bytes of `pixels`
    std::uint32_t x = 0, y = 0, width = 0, height = 0;
    bool on = false;
} g_overlay;
std::uint32_t g_field = 0, g_last_width = 0, g_last_height = 0;
bool g_have_previous = false;
std::uint32_t g_scale = 1, g_copy_words = 1u << 20, g_field_words = 1u << 19;
std::uint32_t g_sharp_2d = 1;

std::uint32_t field_base(std::uint32_t field) { return g_copy_words + (field & 1u) * g_field_words; }

bool g_timestamps = false;
std::uint32_t g_vendor_id = 0;  // PCI vendor id of the chosen adapter (0x8086 Intel), 0 when unknown
WGPUQuerySet g_queries = nullptr;
WGPUBuffer g_query_resolve = nullptr, g_query_read = nullptr;
bool g_query_pending = false, g_query_ready = false;
std::uint64_t g_measured_ns = 0, g_measured = 0, g_total_passes = 0;
WGPUDevice g_device = nullptr;
WGPUQueue g_queue = nullptr;
WGPUBuffer g_vram = nullptr, g_tables = nullptr, g_params = nullptr, g_data = nullptr, g_clut = nullptr, g_scratch = nullptr, g_readback = nullptr, g_decoded = nullptr;
std::uint32_t g_decoded_used = 0;
bool g_rendered[512] = {};
bool g_decode = true;
std::uint64_t g_params_size = 0, g_data_size = 0;
WGPUBindGroupLayout g_layout = nullptr;
WGPUPipelineLayout g_pipeline_layout = nullptr;
WGPUBindGroup g_group = nullptr;
WGPUComputePipeline g_pipelines[kPipelines] = {};
// The draw program with mipmap levels (HAS_MIP = true in gs.wgsl) takes as long to translate as all the others together on FXC, so it is built
// in the background after the plain programs; a batch that needs it waits for it (pipeline_of), every other batch never does.
WGPUComputePipeline g_raster_mip = nullptr;
std::thread *g_raster_mip_thread = nullptr;  // never destroyed: a running thread's destructor at exit would end the program
std::atomic<bool> g_raster_mip_ready{false};
std::mutex g_raster_mip_lock;

struct Command {
    Pipeline pipeline;
    std::uint32_t x, y;
    std::uint32_t param;
    bool mip = false;  // kRaster only: the batch carries a mipmap table, so it needs the draw program built with HAS_MIP (g_raster_mip)
};

// The program a draw command runs. A batch with a mipmap table runs the HAS_MIP program, and waits here, once, until it is built.
WGPUComputePipeline pipeline_of(const Command &c) {
    if (!c.mip || c.pipeline != kRaster) return g_pipelines[c.pipeline];
    if (!g_raster_mip_ready.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> lock(g_raster_mip_lock);
        if (!g_raster_mip_ready.load(std::memory_order_acquire)) {
            const auto start = std::chrono::steady_clock::now();
            if (g_raster_mip_thread && g_raster_mip_thread->joinable()) g_raster_mip_thread->join();
            g_raster_mip_ready.store(true, std::memory_order_release);
            std::fprintf(stderr, "[gpu] waited %.0f ms for the mipmap draw program\n", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
        }
    }
    return g_raster_mip ? g_raster_mip : g_pipelines[kRaster];
}

struct CommandInfo {
    std::size_t index;
    std::uint32_t values[8];
};
std::vector<CommandInfo> g_command_info;

// PS2_HITCH_LOG=<ms>: what the graphics chip was given between two pictures (commands and workgroups per kind, bytes of data), kept for the
// last few pictures, so a slow hand-over can say what the chip was busy with (the Intel driver has no timestamp queries here).
struct HitchWork {
    std::uint32_t commands[16] = {};
    std::uint64_t groups[16] = {};
    std::uint64_t arena_bytes = 0;
    std::uint32_t submits = 0;
};
HitchWork g_hitch_now, g_hitch_before[3];

// PS2_GPU_BENCH=<vblank>,<reps>: measuring aid (docs/speed.md, "Measuring the chip"). Keeps the chip work of the frame ending at that vblank, then replays it
// many times from the same starting memory and times it, kind of work by kind of work and drawing run by drawing run.
struct BenchInfo {
    std::size_t index = 0;
    std::uint32_t prims = 0, tiles = 0, entries = 0, snapshot = 0;
    std::uint32_t kinds[4] = {};  // triangles, sprites, lines, points
    std::uint32_t textured = 0, blended = 0, ztest = 0, zwrite = 0, atest = 0, bilinear = 0, fog = 0, date = 0;
    std::uint32_t fpsm = 0, tpsm = 0, setups = 0;
    double area = 0, bbox = 0;  // pixels the shapes cover (triangles: from their area, sprites: the rectangle) and pixels in their bounding boxes
};
struct BenchSubmit {
    std::vector<Command> commands;
    std::vector<std::uint32_t> params, arena;
    std::vector<BenchInfo> info;
};
struct Bench {
    bool configured = false, capturing = false, done = false;
    std::uint64_t vblank = 0, reps = 20;
    std::vector<BenchSubmit> subs;
    WGPUBuffer vram_copy = nullptr;
} g_bench;
std::vector<BenchInfo> g_bench_info;
std::uint64_t g_vblanks = 0;

// PS2_BATCH_LOG=1: once per frame, why the drawing batches ended (docs/speed.md, "The arena dip")
enum Why { kWhyKey, kWhyTexel, kWhyUnder, kWhyOverlap, kWhyZ, kWhyUpload, kWhyClut, kWhyCopy, kWhySize, kWhyFlush,
kWhyRoute,
kWhys };
const char *const kWhyName[kWhys] = {"buffers", "texel", "drawn-under", "overlap", "depth", "upload", "palette", "copy", "size", "other"
, "route"
};
Why g_why = kWhyFlush;
std::uint32_t g_why_runs[kWhys] = {}, g_why_prims[kWhys] = {};
const bool g_batch_log = std::getenv("PS2_BATCH_LOG") != nullptr;
const unsigned long long g_batch_log_frame = std::getenv("PS2_BATCH_LOG") ? std::strtoull(std::getenv("PS2_BATCH_LOG"), nullptr, 10) : 0;  // >1: print every "drawn-under" end of that frame
// PS2_GPU_WGPU_TIME=<frames>: the drawing thread's time inside each kind of wgpu call (wall clock, so the waits show as waits), and the
// calls, per frame, printed every <frames> vblanks (default 300). Off: one predictable branch per call site.
namespace wt {
enum Kind { kEncoder, kBeginPass, kSetPipeline, kSetGroup, kDispatch, kEndPass, kRenderPass, kFinish, kSubmit, kWrite, kPoll, kAcquire, kPresent,
            kWait, kCopy, kDrain, kKinds };
const char *const kName[kKinds] = {"create encoder", "begin compute pass", "set pipeline", "set bind group", "dispatch", "end compute pass", "present pass encode",
                                   "finish", "submit", "write buffer", "poll", "acquire (wait)", "present", "read-back waits", "copy/map other",
                                   "waiting for the submit thread"};
std::uint64_t g_post_ns = 0, g_post_jobs = 0;  // the submit thread's totals at the block's start (post::State)
bool post_totals(std::uint64_t &ns, std::uint64_t &jobs);
const unsigned g_every = [] {
    const char *v = std::getenv("PS2_GPU_WGPU_TIME");
    return v ? std::max(1u, static_cast<unsigned>(std::strtoul(v, nullptr, 10) ? std::strtoul(v, nullptr, 10) : 300)) : 0u;
}();
const bool g_on = g_every != 0;
std::uint64_t g_ns[kKinds] = {}, g_cpu[kKinds] = {}, g_calls[kKinds] = {}, g_bytes = 0, g_frames = 0, g_submits = 0;
std::chrono::steady_clock::time_point g_since;
double g_cpu_since = 0;
// The thread's own processor clock for the calls that may wait (not per dispatch: the clock costs a little): on Windows the thread's
// cycle counter, which counts at the time-stamp counter's rate (calibrated against the wall clock below), elsewhere CLOCK_THREAD_CPUTIME_ID in ns.
#if defined(_WIN32) && defined(__x86_64__)
extern "C" __declspec(dllimport) int __stdcall QueryThreadCycleTime(void *thread, unsigned long long *cycles);
extern "C" __declspec(dllimport) void *__stdcall GetCurrentThread();
std::uint64_t cpu_ticks() {
    unsigned long long c = 0;
    QueryThreadCycleTime(GetCurrentThread(), &c);
    return c;
}
double ticks_per_ns() {  // the time-stamp counter against the wall clock, measured once over 50 ms
    static const double rate = [] {
        const auto t0 = std::chrono::steady_clock::now();
        const std::uint64_t c0 = __builtin_ia32_rdtsc();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const std::uint64_t c1 = __builtin_ia32_rdtsc();
        return static_cast<double>(c1 - c0) / static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
    }();
    return rate;
}
#elif defined(CLOCK_THREAD_CPUTIME_ID)
std::uint64_t cpu_ticks() {
    timespec t{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
    return static_cast<std::uint64_t>(t.tv_sec) * 1000000000ull + static_cast<std::uint64_t>(t.tv_nsec);
}
double ticks_per_ns() { return 1.0; }
#else
std::uint64_t cpu_ticks() { return 0; }
double ticks_per_ns() { return 1.0; }
#endif
constexpr bool kClocked[kKinds] = {false, false, false, false, false, true, true, true, true, true, true, true, true, true, true, true};
struct Scope {
    Kind kind;
    std::chrono::steady_clock::time_point from;
    std::uint64_t cpu_from = 0;
    explicit Scope(Kind k) : kind(k) {
        if (!g_on) return;
        from = std::chrono::steady_clock::now();
        if (kClocked[k]) cpu_from = cpu_ticks();
    }
    ~Scope() {
        if (!g_on) return;
        if (kClocked[kind]) g_cpu[kind] += cpu_ticks() - cpu_from;
        g_ns[kind] += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - from).count());
        ++g_calls[kind];
    }
};
void restart() {
    for (int k = 0; k < kKinds; ++k) g_ns[k] = g_cpu[k] = g_calls[k] = 0;
    g_bytes = g_submits = 0;
    g_since = std::chrono::steady_clock::now();
    g_cpu_since = ps2::hitch::thread_cpu_ms();
    post_totals(g_post_ns, g_post_jobs);
}
// One line per block: wall ms a vblank, the thread's processor ms a vblank (GetThreadTimes / CLOCK_THREAD_CPUTIME_ID), and per kind of call
// "wall/processor ms (calls)" a vblank; the processor part only for the calls that may wait. Own work = thread processor ms - the calls' processor ms.
void frame() {
    if (!g_on) return;
    ++g_frames;
    if (g_frames <= g_every) {  // the first block only starts the clock: start-up and warm-up are left out
        if (g_frames == g_every) ticks_per_ns(), restart();
        return;
    }
    if (g_frames % g_every) return;
    const double f = g_every, wall = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - g_since).count() / f;
    const double thread = (ps2::hitch::thread_cpu_ms() - g_cpu_since) / f, rate = ticks_per_ns();
    double sum = 0, sum_cpu = 0;
    for (int k = 0; k < kKinds; ++k) {
        sum += static_cast<double>(g_ns[k]) / 1e6 / f;
        sum_cpu += kClocked[k] ? static_cast<double>(g_cpu[k]) / rate / 1e6 / f : static_cast<double>(g_ns[k]) / 1e6 / f;
    }
    std::fprintf(stderr, "[wgpu-time] %u vblanks: %.2f ms a vblank, thread processor %.2f ms, in wgpu %.3f ms (processor %.3f), own work %.2f ms, %.1f submits, %.0f KiB written;",
                 g_every, wall, thread, sum, sum_cpu, thread - sum_cpu, static_cast<double>(g_submits) / f, static_cast<double>(g_bytes) / 1024.0 / f);
    for (int k = 0; k < kKinds; ++k) {
        if (!g_calls[k]) continue;
        if (kClocked[k]) std::fprintf(stderr, " %s %.3f/%.3f ms (%.1f),", kName[k], static_cast<double>(g_ns[k]) / 1e6 / f, static_cast<double>(g_cpu[k]) / rate / 1e6 / f, static_cast<double>(g_calls[k]) / f);
        else std::fprintf(stderr, " %s %.3f ms (%.1f),", kName[k], static_cast<double>(g_ns[k]) / 1e6 / f, static_cast<double>(g_calls[k]) / f);
    }
    std::uint64_t post_ns = 0, post_jobs = 0;
    if (post_totals(post_ns, post_jobs))
        std::fprintf(stderr, " on the submit thread %.3f ms (%.1f jobs: writes, finish, submit, poll, present)", static_cast<double>(post_ns - g_post_ns) / 1e6 / f,
                     static_cast<double>(post_jobs - g_post_jobs) / f);
    std::fprintf(stderr, "\n");
    restart();
}
inline void write(WGPUQueue queue, WGPUBuffer buffer, std::uint64_t offset, const void *data, std::size_t size) {
    Scope s(kWrite);
    if (g_on) g_bytes += size;
    wgpuQueueWriteBuffer(queue, buffer, offset, data, size);
}
}  // namespace wt

std::FILE *g_profile_out = nullptr;
std::uint64_t g_profile_from = 0, g_profile_to = 0;
WGPUQuerySet g_profile_queries = nullptr;
WGPUBuffer g_profile_resolve = nullptr, g_profile_read = nullptr;

std::vector<Command> g_commands;
std::vector<std::array<std::uint32_t, kParamStride / 4>> g_param_records;
struct Arena {
    std::unique_ptr<std::uint32_t[]> words;
    std::size_t used = 0, capacity = 0;

    std::uint32_t *grow(std::size_t count) {
        if (used + count > capacity) {
            std::size_t next = std::max<std::size_t>(capacity * 2, 1u << 20);
            while (next < used + count) next *= 2;
            auto bigger = std::make_unique_for_overwrite<std::uint32_t[]>(next);
            if (used) std::memcpy(bigger.get(), words.get(), used * 4);
            words = std::move(bigger);
            capacity = next;
        }
        std::uint32_t *at = words.get() + used;
        used += count;
        return at;
    }
    std::size_t size() const { return used; }
    bool empty() const { return !used; }
    void clear() { used = 0; }
    std::uint32_t *data() { return words.get(); }
    std::uint32_t &operator[](std::size_t i) { return words[i]; }
} g_arena;
std::uint32_t g_clut_slot = 0, g_clut_loads = 0, g_texflushes = 0;
std::uint32_t g_setup_serials = 0;  // numbers the rebuilds of g_setup_cache

struct Image {
    bool active = false;
    std::uint32_t dbp = 0, dbw = 0, dpsm = 0, dsax = 0, dsay = 0, rrw = 0, total = 0, done = 0, bpp = 32, lead_bits = 0, id = 0;
    std::vector<std::uint8_t> pending;
} g_image;
std::uint32_t g_image_ids = 0;

struct Range {
    std::uint64_t lo = ~0ull, hi = 0;
    bool overlaps(std::uint64_t a, std::uint64_t b) const { return lo < b && a < hi; }
    void add(std::uint64_t a, std::uint64_t b) {
        lo = std::min(lo, a);
        hi = std::max(hi, b);
    }
};

using tiles::Words;

// The shape records of the frame, in the order the batches were flushed, followed by the records of the open batch. draw() writes a record here once
// and the chip reads it from here: at submit this array goes into the data buffer right after the arena (place_shapes), so the records are not copied
// into the arena at flush any more. The open batch sees only its own records (`data()`, `size()` count from `begin`).
// The tile helper reads the records of the batches it builds while this thread goes on appending; when the array has to move to grow, the old block is
// kept until the next submit, where every job is finished (`retired`).
struct Shapes {
    Words words;
    std::size_t begin = 0;  // the first word of the open batch
    std::vector<std::unique_ptr<std::uint32_t[]>> retired;

    void append(const std::uint32_t *from, std::size_t count) {
        if (words.used + count > words.capacity) {
            std::size_t next = std::max<std::size_t>(words.capacity * 2, 1u << 20);
            while (next < words.used + count) next *= 2;
            auto bigger = std::make_unique_for_overwrite<std::uint32_t[]>(next);
            if (words.used) std::memcpy(bigger.get(), words.words.get(), words.used * 4);
            if (tiles::backlog()) retired.push_back(std::move(words.words));  // a job may still read the old block: keep it until submit
            words.words = std::move(bigger);
            words.capacity = next;
        }
        std::memcpy(words.words.get() + words.used, from, count * 4);
        words.used += count;
    }
    // the open batch
    std::size_t size() const { return words.used - begin; }
    bool empty() const { return words.used == begin; }
    std::uint32_t *data() { return words.data() + begin; }
    const std::uint32_t *data() const { return words.data() + begin; }
    std::uint32_t &operator[](std::size_t i) { return words[begin + i]; }
    const std::uint32_t &operator[](std::size_t i) const { return words[begin + i]; }
    void clear() { words.used = begin; }  // drops the open batch's records (a batch the card took, a test)
    // flush: the open batch's records stay for the chip; returns their first word
    std::uint32_t commit() {
        const std::size_t at = begin;
        begin = words.used;
        return static_cast<std::uint32_t>(at);
    }
    // submit, once every tile job is finished: the flushed records have been uploaded; the open batch's (if any) move to the front
    void restart() {
        const std::size_t open = size();
        if (open && begin) std::memmove(words.data(), words.data() + begin, open * 4);
        words.used = open;
        begin = 0;
        retired.clear();
    }
} g_shapes;
std::vector<std::uint32_t> g_shape_params;  // the parameter records of this submit's raster commands: word 1 (the records' offset) gets the arena's size added at submit

// The submit thread. wgpu's part of a submit is the same work every frame: copying the frame's data into its staging memory
// (wgpuQueueWriteBuffer, 5 to 8 MB a frame in the fights), turning the recorded commands into the backend's command list
// (wgpuCommandEncoderFinish), the submit, and the present. On Direct3D 12 that is 1.5 to 2.5 ms of the drawing thread a frame. This
// thread does it instead, while the drawing thread goes on with the next frame: submit() records the commands and hands the encoder over,
// with the frame's arena, shape records and parameter records (swapped, not copied), and the surface texture to present.
// Exactness: the queue sees the same operations in the same order. Only one hand-over is in flight, and every other queue or surface
// operation (a buffer write, a read-back, the next acquire, a resize) waits for it first (drain()), so nothing can slip in between.
// PS2_GPU_SUBMIT_THREAD=1 turns it on (off by default, see mode()). Always off in the browser build, with the card path, and with the
// diagnostics that time the present (PS2_PRESENT_LOG, PS2_HITCH_LOG).
namespace post {
struct Job {
    WGPUCommandEncoder enc = nullptr;
    WGPUTexture texture = nullptr;  // present this surface texture after the submit (with its view)
    WGPUTextureView view = nullptr;
    WGPUBuffer data = nullptr, params = nullptr;
    bool measure = false, writes = false;  // writes: the job writes the arena, shape and parameter records first
    bool blit = false;                     // and before them present()'s picture parameters
    std::uint32_t blit_words[8] = {};
    Arena arena;
    Words shapes;
    std::size_t shape_words = 0;
    std::vector<std::array<std::uint32_t, kParamStride / 4>> records;
};
struct State {
    std::mutex mutex;
    std::condition_variable cv;
    bool busy = false;
    Job job;
    std::atomic<std::uint64_t> ns{0}, jobs{0};  // the thread's time in jobs, for PS2_GPU_WGPU_TIME
};
State *g_state = nullptr;  // made at the first hand-over and never freed (a detached thread may still wait on it at exit)
void drain();
WGPUTexture g_present_texture = nullptr;  // present() sets it around submit(view): the texture the job presents and releases
bool g_presented_async = false;           // the last submit() handed its picture over: the job presents it
std::uint32_t g_blit[8] = {};             // present()'s picture parameters for the present pass, written by the next submit
bool g_blit_pending = false;
// 0 off, 1 everything on the submit thread, 2 the writes stay on the drawing thread. Off by default on Windows and macOS: on a laptop with Intel graphics it frees
// the drawing thread but costs the emulation thread 1 to 2 ms a frame (two fast cores, now four busy threads), and Silent Hill 2, held by that
// thread, lost 1.5 % with it on (the God of War fights, held by the chip, gained 0.5 to 1 %). On by default on Linux (measured on a 16-thread
// desktop in a window, limit off: God of War ship +14 %, film +7 %, Silent Hill 2 a tie); PS2_GPU_SUBMIT_THREAD=0 turns it off there.
int mode() {
#ifdef __EMSCRIPTEN__
    return 0;
#else
    static const int m = [] {
        const char *v = std::getenv("PS2_GPU_SUBMIT_THREAD");
        if (std::getenv("PS2_PRESENT_LOG")) return 0;
#ifdef __linux__
        const int def = 1;
#else
        const int def = 0;
#endif
        const int r = v ? std::atoi(v) : def;
        if (r != 0 && !v) std::fprintf(stderr, "[gpu] submit thread on by default (PS2_GPU_SUBMIT_THREAD=0 turns it off)\n");
        return r;
    }();
    return m;
#endif
}
bool enabled() { return mode() != 0; }
}  // namespace post

struct Batch {
    Shapes &prims = g_shapes;
    std::vector<std::uint32_t> setups;
    std::vector<std::uint32_t> mips;  // kMipWords per setup, the same index as `setups` (make_mip)
    bool any_mip = false;             // a setup of the batch has mipmap words that are not zero: the table goes to the chip
    std::uint32_t setup_serial = 0;  // the cache serial of the setup that is the last one of `setups`; 0: none (see draw)
    std::uint64_t key = ~0ull;
    int minx = 1 << 30, miny = 1 << 30, maxx = -1, maxy = -1;
    std::uint32_t fbp = 0, fbw = 0, fpsm = 0, zbp = 0, zpsm = 0;
    bool zte = false, zwrite = false;
    int route = 0;  // 1 when the card path draws this batch, else the painter
    std::uint32_t texflushes = 0;
    int rx0 = 0, ry0 = 0, rx1 = -2, ry1 = -2;
    Range fb_range, z_range;
} g_batch;

struct SetupCache {
    std::uint32_t generation = ~0u, clut = ~0u, serial = 0;
    std::uint64_t attr = ~0ull;
    std::uint32_t setup[kSetupWords] = {};
    std::uint32_t mip[kMipWords] = {};
    std::uint32_t mip_top = 0;  // the highest mip level a pixel of this setup can read (make_mip); 0: only the base texture
    std::uint32_t flags = 0;
    std::uint64_t key = 0;
    Range tex;
    int route = 0;  // hw::live_route of this setup: 0 painter, 1 card, 2 either
    int look = -1;  // hw::live_look of this setup
    bool waits = false;  // routed to the painter only because a pipeline was still being made: ask again when one finishes
    std::uint64_t ready_seen = 0;
} g_setup_cache;

struct PreOp {
    bool clut = false;
    std::uint32_t rec[12] = {};
    Range range;
    std::uint32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0, image = 0;
};

bool uploads_overlap(const PreOp &a, const PreOp &b) {
    if (a.image && a.image == b.image) return false;
    if (!a.range.overlaps(b.range.lo, b.range.hi)) return false;
    if (a.rec[0] != b.rec[0] || a.rec[1] != b.rec[1] || a.rec[2] != b.rec[2]) return true;
    return a.x0 <= b.x1 && b.x0 <= a.x1 && a.y0 <= b.y1 && b.y0 <= a.y1;
}

// Uploads and CLUT loads run before the pending primitive batch unless they touch memory it reads or writes.
std::vector<PreOp> g_pre;
std::vector<Range> g_batch_reads;

// The words of graphics memory that the sprites of the open batch write, one bit per 32-bit word. The page and box tests that cut a batch
// before a shape that reads what the batch draws are all that most shapes need, but a sprite that reads memory in another layout than the frame (Metal
// Gear Solid 3 expands 8-bit indices in place into 32-bit pixels, 7,168 sprites of 16 pixels per frame) touches the same pages as the sprites before it
// and almost never the same words. The set is built when the first such sprite asks (from every shape of the batch, which must all be small sprites)
// and then grows with each sprite that joins; state 2 means a shape the set cannot hold is in the batch, and then the old tests decide alone.
struct Exact {
    std::vector<std::uint64_t> written;
    std::vector<std::uint32_t> dirty;  // the parts of `written` that are not zero, so a reset does not clear 128 KB
    int state = 0;                     // 0 not built, 1 holds every shape of the batch, 2 cannot
    Range reads;                       // state 1: the range of bytes that the reads of the region-repeat sprites of the batch span (what their snapshot has to cover)
    bool reads_ok = true;              // false when one of those sprites cannot be listed: the snapshot then takes the whole texture
    Range pending;                     // the reads of the sprite exact_clear just allowed, until exact_follow has seen it appended
    bool pending_valid = false;
    std::uint32_t tests = 0, merged = 0, hazards = 0, refused = 0;  // PS2_BATCH_LOG: sprites asked, joined, kept out by a real overlap, kept out because no exact set could be made
    std::uint64_t ns[2] = {};                                         // PS2_BATCH_LOG: processor time spent asking (exact_clear) and following (exact_follow)
} g_exact;

// Adds the time of its scope to g_exact.ns[which] when PS2_BATCH_LOG is on.
struct ExactTimer {
    bool on;
    int which;
    std::chrono::steady_clock::time_point start;
    explicit ExactTimer(int w);
    ~ExactTimer();
};
std::vector<std::uint32_t> g_swizzle;  // the processor's copy of the shader's address tables (swizzle_tables)

ExactTimer::ExactTimer(int w) : on(g_batch_log), which(w), start(g_batch_log ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{}) {}
ExactTimer::~ExactTimer() {
    if (on) {
        const auto d = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
        g_exact.ns[which] += d;
    }
}

void exact_reset() {
    for (const std::uint32_t i : g_exact.dirty) g_exact.written[i] = 0;
    g_exact.dirty.clear();
    g_exact.state = 0;
    g_exact.reads = Range{};
    g_exact.reads_ok = true;
    g_exact.pending_valid = false;
}

// The shader's addr() (gs.wgsl) for the format words `f` of a setup (fmt_words): the word of graphics memory that texel or pixel (x, y) lives in.
std::uint32_t word_at(const std::uint32_t *f, std::uint32_t x, std::uint32_t y) {
    const std::uint32_t pws = f[3] & 15u, phs = (f[3] >> 4) & 15u, nbit = (f[3] >> 10) & 1u;
    const std::uint32_t n = g_swizzle[f[2] + ((y & ((1u << phs) - 1u)) << pws) + (x & ((1u << pws) - 1u))];
    const std::uint32_t a = f[0] + ((x >> pws) + (y >> phs) * f[1]) * 8192u + (n >> nbit);
    return (a & 0x3FFFFFu) >> 2;
}

constexpr std::uint32_t kExactPixels = 1024;  // the largest sprite whose pixels are listed word by word, and the most texels one sprite is asked about

// The whole texel coordinates (before the wrap mode, `lo` to `hi` on each axis) that the pixels of a sprite can read, worked out the way the draw program does it
// (shade_at, sample in gs.wgsl): a pixel's position is x + j / SCALE (upscaled, moved back by (1 - 1 / SCALE) / 2), the texture coordinate is linear in it, a nearest read takes floor(u) and a bilinear one the
// four texels from floor(u - 0.5) on. A texel more on each side only where the 32-bit float arithmetic of the draw program may not give exactly the number worked out here: in
// the plain case (texture coordinates in 12.4 fixed point, a sprite whose width in pixels is a power of two) every step is exact and a coordinate that lies on a texel edge
// (the usual half-texel offset of a bilinear sprite) is not widened, so the sprite does not touch the row above it.
bool sprite_taps(const std::uint32_t *rec, const std::uint32_t *setup, int minx, int miny, int maxx, int maxy, int lo[2], int hi[2]) {
    if ((rec[1] & kKindMask) != 1 || !(setup[0] & 2u)) return false;
    const bool fst = setup[0] & 16u, bilinear = setup[0] & 16384u;
    float f[4], uv[6];
    std::memcpy(f, rec + 4, sizeof f);
    std::memcpy(uv, rec + 15, sizeof uv);
    const double samples = static_cast<double>(g_scale);
    for (int axis = 0; axis < 2; ++axis) {
        const double origin = f[axis], d = f[2 + axis], a = uv[axis], b = uv[3 + axis];
        const int from = axis ? miny : minx, to = axis ? maxy : maxx;
        double k = 1.0 / 16.0;
        bool exact = true;
        if (!fst) {
            double q = uv[5];
            if (q == 0.0) q = 1.0;
            k = static_cast<double>(1u << (axis ? (setup[15] >> 8) : (setup[15] & 0xFFu))) / q;
            exact = false;
        }
        int exponent;
        if (!(std::fabs(d) < 4096.0) || (d != 0.0 && std::frexp(std::fabs(d), &exponent) != 0.5) || to - from > 63 || !(std::fabs(a) < 32768.0) || !(std::fabs(b) < 32768.0) || (g_scale != 1 && g_scale != 2)) exact = false;
        const auto at = [&](double p) { return (a + (b - a) * (d != 0.0 ? (p - origin) / d : 0.0)) * k; };
        // upscaled, a sample stands for the centre of its place less half a pixel (shade_at), up to (1 - 1 / SCALE) / 2 before x
        const double centre = g_scale > 1 ? 0.5 / samples - 0.5 : 0.0;
        double u0 = at(static_cast<double>(from) + centre), u1 = at(static_cast<double>(to) + (samples - 1.0) / samples);
        if (u0 > u1) std::swap(u0, u1);
        const double m = exact ? 0.0 : 1.0 / 64.0;
        if (!(std::fabs(u0) < 1e6) || !(std::fabs(u1) < 1e6)) return false;
        int l = static_cast<int>(std::floor(u0 - m)), h = static_cast<int>(std::floor(u1 + m));
        if (bilinear) {
            l = std::min(l, static_cast<int>(std::floor(u0 - 0.5 - m)));
            h = std::max(h, static_cast<int>(std::floor(u1 - 0.5 + m)) + 1);
        }
        lo[axis] = l;
        hi[axis] = h;
    }
    return true;
}

// The coordinates one axis reaches through the wrap mode (the shader's wrap_coord) when the whole coordinates `a` to `b` are asked for. False when there would be more than 32.
bool tap_axis(std::uint32_t mode, int a, int b, int size, std::uint32_t minv, std::uint32_t maxv, int *out, int &n) {
    n = 0;
    if (b - a > 31 || (mode == 2 && maxv < minv)) return false;
    for (int t = a; t <= b; ++t) {
        int r;
        switch (mode) {
        case 0: r = t & (size - 1); break;
        case 1: r = std::clamp(t, 0, size - 1); break;
        case 2: r = std::clamp(t, static_cast<int>(minv), static_cast<int>(maxv)); break;
        default: r = static_cast<int>((static_cast<std::uint32_t>(t) & minv) | maxv); break;
        }
        bool seen = false;
        for (int i = 0; i < n && !seen; ++i) seen = out[i] == r;
        if (!seen) out[n++] = r;
    }
    return true;
}

// Calls visit(word) for every word of graphics memory that the texture reads of a sprite can touch (the same word may come more than once). False, with nothing
// visited, when they cannot be listed: not a sprite, not textured, or more than kExactPixels texels.
template <typename Visit>
bool sprite_reads(const std::uint32_t *rec, const std::uint32_t *setup, int minx, int miny, int maxx, int maxy, Visit visit) {
    if (g_swizzle.empty()) return false;
    int lo[2], hi[2], us[32], vs[32], nu, nv;
    if (!sprite_taps(rec, setup, minx, miny, maxx, maxy, lo, hi) ||
        !tap_axis((setup[7] >> 24) & 3u, lo[0], hi[0], 1 << (setup[15] & 0xFFu), setup[18] & 0xFFFFu, setup[18] >> 16, us, nu) ||
        !tap_axis((setup[7] >> 26) & 3u, lo[1], hi[1], 1 << (setup[15] >> 8), setup[19] & 0xFFFFu, setup[19] >> 16, vs, nv) || nu * nv > static_cast<int>(kExactPixels))
        return false;
    for (int j = 0; j < nv; ++j)
        for (int i = 0; i < nu; ++i) visit(word_at(setup + 25, static_cast<std::uint32_t>(us[i]), static_cast<std::uint32_t>(vs[j])));
    return true;
}

// A textured sprite whose draw program reads (u & MINU) | MAXU on an axis: the range its reads span is not the range of its corners.
bool region_repeat(const std::uint32_t *setup) { return (setup[0] & 2u) && ((((setup[7] >> 24) & 3u) == 3u) || (((setup[7] >> 26) & 3u) == 3u)); }

// The words from the first to the last that the texture reads of a sprite can touch, as a range of bytes; false when they cannot be listed.
bool sprite_read_range(const std::uint32_t *rec, const std::uint32_t *setup, int minx, int miny, int maxx, int maxy, Range &out) {
    std::uint32_t first = ~0u, last = 0;
    if (!sprite_reads(rec, setup, minx, miny, maxx, maxy, [&](std::uint32_t word) {
            first = std::min(first, word);
            last = std::max(last, word);
        }))
        return false;
    out = Range{};
    out.add(static_cast<std::uint64_t>(first) * 4, static_cast<std::uint64_t>(last) * 4 + 4);
    return true;
}

// Adds the words a sprite of the batch writes in the frame (every pixel of its box). False when the shape is no sprite, is larger than kExactPixels, or the frame is not a
// colour format. The depth buffer is not listed: a read that comes near it cuts the batch by pages (draw).
bool exact_add(const std::uint32_t *rec, const std::uint32_t *setup) {
    const std::uint32_t fpsm = setup[3];
    if ((rec[1] & kKindMask) != 1 || (fpsm != PSMCT32 && fpsm != PSMCT24 && fpsm != PSMCT16 && fpsm != PSMCT16S)) return false;
    const std::uint32_t x0 = rec[2] & 0xFFFFu, y0 = rec[2] >> 16, x1 = rec[3] & 0xFFFFu, y1 = rec[3] >> 16;
    if (x1 < x0 || y1 < y0 || static_cast<std::uint64_t>(x1 - x0 + 1) * (y1 - y0 + 1) > kExactPixels) return false;
    Exact &e = g_exact;
    for (std::uint32_t y = y0; y <= y1; ++y)
        for (std::uint32_t x = x0; x <= x1; ++x) {
            const std::uint32_t word = word_at(setup + 30, x, y);
            std::uint64_t &part = e.written[word >> 6];
            if (!part) e.dirty.push_back(word >> 6);
            part |= 1ull << (word & 63u);
        }
    return true;
}

void exact_fail() {
    exact_reset();
    g_exact.state = 2;
}

// Adds a region-repeat sprite's reads to the range the batch's snapshot has to cover; with `known` the range was just worked out by exact_clear.
void exact_reads(const std::uint32_t *rec, const std::uint32_t *setup, bool known) {
    Exact &e = g_exact;
    if (!region_repeat(setup)) return;
    Range r;
    if (known) r = e.pending;
    else if (!sprite_read_range(rec, setup, static_cast<int>(rec[2] & 0xFFFFu), static_cast<int>(rec[2] >> 16), static_cast<int>(rec[3] & 0xFFFFu), static_cast<int>(rec[3] >> 16), r)) {
        e.reads_ok = false;
        return;
    }
    e.reads.add(r.lo, r.hi);
}

// Called after a shape joined the batch: the set follows it, or is given up for this batch.
void exact_follow(const std::uint32_t *rec, const std::uint32_t *setup) {
    Exact &e = g_exact;
    const bool known = e.pending_valid;
    e.pending_valid = false;
    if (e.state != 1) return;
    const ExactTimer timer(1);
    if (!exact_add(rec, setup)) exact_fail();
    else exact_reads(rec, setup, known);
}

// True when the sprite about to join the batch (`rec` with its box, before it is appended) reads no word that a shape of the batch writes in the frame, so that every read of
// the batch copy is also what the shapes in drawing order would have read; `reads` is the range of bytes its reads span. False means a word is shared, or "cannot say": the old
// tests decide. (The depth buffer is judged by pages by the caller.)
bool exact_clear(const std::uint32_t *rec, const std::uint32_t *setup, int minx, int miny, int maxx, int maxy, Range &reads) {
    Exact &e = g_exact;
    const ExactTimer timer(0);
    ++e.tests;
    e.pending_valid = false;
    if (g_swizzle.empty()) {
        ++e.refused;
        return false;
    }
    if (e.state == 0) {
        Batch &b = g_batch;
        if (e.written.empty()) e.written.assign(kMemorySize / 4 / 64, 0);
        e.state = 1;
        const std::size_t prims = b.prims.size() / kPrimWords;
        for (std::size_t i = 0; i < prims && e.state == 1; ++i) {
            const std::uint32_t *r = b.prims.data() + i * kPrimWords;
            const std::uint32_t *rs = b.setups.data() + r[0] * kSetupWords;
            if (!exact_add(r, rs)) exact_fail();
            else exact_reads(r, rs, false);
        }
    }
    bool shared = false;
    std::uint32_t first = ~0u, last = 0;
    if (e.state != 1 || !sprite_reads(rec, setup, minx, miny, maxx, maxy, [&](std::uint32_t word) {
            shared = shared || (e.written[word >> 6] >> (word & 63u)) & 1u;
            first = std::min(first, word);
            last = std::max(last, word);
        })) {
        ++e.refused;
        return false;
    }
    if (shared) {
        ++e.hazards;
        return false;
    }
    reads = Range{};
    reads.add(static_cast<std::uint64_t>(first) * 4, static_cast<std::uint64_t>(last) * 4 + 4);
    e.pending = reads;
    e.pending_valid = true;
    ++e.merged;
    return true;
}

int g_tile = 8;  // pixels along one side of a screen tile (one workgroup of the raster program): 8 was the cheaper of 16, 8 and 4 on the arena and hall test frames; PS2_TILE=4, 8 or 16 to try others

WGPUStringView view(const char *s) { return WGPUStringView{s, WGPU_STRLEN}; }

std::uint32_t bits_per_pixel(std::uint32_t psm) {
    switch (psm) {
    case PSMCT24: case PSMZ24: return 24;
    case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S: return 16;
    case PSMT8: case PSMT8H: return 8;
    case PSMT4: case PSMT4HL: case PSMT4HH: return 4;
    default: return 32;
    }
}

void page_size(std::uint32_t psm, std::uint32_t &pw, std::uint32_t &ph) {
    switch (psm) {
    case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S: pw = 64; ph = 64; break;
    case PSMT8: pw = 128; ph = 64; break;
    case PSMT4: pw = 128; ph = 128; break;
    default: pw = 64; ph = 32; break;
    }
}

Range region(std::uint32_t bp, std::uint32_t bw, std::uint32_t psm, int x0, int y0, int x1, int y1) {
    std::uint32_t pw, ph;
    page_size(psm, pw, ph);
    const std::uint64_t per_row = std::max<std::uint32_t>(1, bw * 64 / pw);
    const auto page = [&](int x, int y) {
        return static_cast<std::uint64_t>(bp) * 256 + (static_cast<std::uint64_t>(std::max(x, 0)) / pw + static_cast<std::uint64_t>(std::max(y, 0)) / ph * per_row) * 8192;
    };
    Range r;
    r.add(std::min(page(x0, y0), static_cast<std::uint64_t>(bp) * 256), page(x1, y1) + 8192);
    return r;
}

std::uint32_t z_max(std::uint32_t psm) {
    switch (psm) {
    case PSMZ24: return 0xFFFFFFu;
    case PSMZ16: case PSMZ16S: return 0xFFFFu;
    default: return 0xFFFFFFFFu;
    }
}

std::uint32_t f2u(float f) {
    std::uint32_t v;
    std::memcpy(&v, &f, 4);
    return v;
}

std::uint32_t push_params(const std::initializer_list<std::uint32_t> values) {
    std::array<std::uint32_t, kParamStride / 4> record{};
    std::size_t i = 0;
    for (std::uint32_t v : values) record[i++] = v;
    g_param_records.push_back(record);
    return static_cast<std::uint32_t>(g_param_records.size() - 1);
}

void dispatch_linear(Pipeline p, std::uint32_t items, std::uint32_t param) {
    const std::uint32_t groups = (items + 63) / 64;
    if (!groups) return;
    g_commands.push_back({p, std::min(groups, 1024u), (groups + 1023) / 1024, param});
}

constexpr std::uint32_t kCacheWords = 4u << 20;
constexpr std::size_t kCacheMinBytes = 256;

struct CacheEntry {
    std::uint64_t key;
    std::uint32_t offset, words;
    std::size_t bytes;
    std::uint64_t used;
    std::uint64_t uid;  // a number no other entry ever had: two uploads that find the same entry uploaded the same bytes (the decode cache's identities)
};

WGPUBuffer g_cache = nullptr;
std::vector<std::uint32_t> g_cache_copy;
std::deque<CacheEntry> g_cache_ring;
std::unordered_multimap<std::uint64_t, CacheEntry *> g_cache_index;
std::unordered_map<std::uint64_t, std::uint32_t> g_cache_seen;
std::uint32_t g_cache_head = 0;
std::uint64_t g_serial = 1;
std::uint64_t g_cache_uids = 0, g_cache_uid = 0;  // entries made so far; the uid of the entry the last cache_find / cache_insert gave

std::uint64_t content_key(const std::uint8_t *p, std::size_t n) {
    std::uint64_t h = static_cast<std::uint64_t>(n) * 0x9E3779B97F4A7C15ull;
    const auto mix = [&h](std::uint64_t v) {
        h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
        h *= 0xBF58476D1CE4E5B9ull;
    };
    const std::size_t step = std::max<std::size_t>(8, (n / 64) & ~static_cast<std::size_t>(7));
    std::uint64_t v;
    for (std::size_t i = 0; i + 8 <= n; i += step) {
        std::memcpy(&v, p + i, 8);
        mix(v);
    }
    v = 0;
    std::memcpy(&v, p + n - std::min<std::size_t>(n, 8), std::min<std::size_t>(n, 8));
    mix(v);
    return h;
}

bool cache_find(std::uint64_t key, const std::uint8_t *bytes, std::size_t n, std::uint32_t &offset) {
    const auto [first, last] = g_cache_index.equal_range(key);
    for (auto it = first; it != last; ++it) {
        CacheEntry &e = *it->second;
        if (e.bytes != n || std::memcmp(g_cache_copy.data() + e.offset, bytes, n) != 0) continue;
        e.used = g_serial;
        offset = e.offset;
        g_cache_uid = e.uid;
        return true;
    }
    return false;
}

bool cache_insert(std::uint64_t key, const std::uint8_t *bytes, std::size_t n, std::uint32_t &offset) {
    const auto words = static_cast<std::uint32_t>((n + 3) / 4 + 1);
    if (words > kCacheWords / 4) return false;
    const auto evict_front = [] {
        CacheEntry &front = g_cache_ring.front();
        if (front.used == g_serial) return false;
        const auto [first, last] = g_cache_index.equal_range(front.key);
        for (auto it = first; it != last; ++it)
            if (it->second == &front) {
                g_cache_index.erase(it);
                break;
            }
        g_cache_ring.pop_front();
        return true;
    };
    if (g_cache_head + words > kCacheWords) {
        while (!g_cache_ring.empty() && g_cache_ring.front().offset >= g_cache_head)
            if (!evict_front()) return false;
        g_cache_head = 0;
    }
    while (!g_cache_ring.empty() && g_cache_ring.front().offset < g_cache_head + words)
        if (!evict_front()) return false;
    offset = g_cache_head;
    std::uint32_t *to = g_cache_copy.data() + offset;
    to[words - 1] = 0;
    to[words - 2] = 0;
    std::memcpy(to, bytes, n);
    post::drain();
    wt::write(g_queue, g_cache, static_cast<std::uint64_t>(offset) * 4, to, static_cast<std::uint64_t>(words) * 4);
    g_cache_ring.push_back({key, offset, words, n, g_serial, ++g_cache_uids});
    g_cache_uid = g_cache_uids;
    g_cache_index.emplace(key, &g_cache_ring.back());
    g_cache_head += words;
    return true;
}

std::uint32_t arena_append(const void *bytes, std::size_t size) {
    const std::uint32_t offset = static_cast<std::uint32_t>(g_arena.size());
    const std::size_t words = (size + 3) / 4 + 1;
    std::uint32_t *to = g_arena.grow(words);
    to[words - 2] = 0;
    to[words - 1] = 0;
    std::memcpy(to, bytes, size);
    return offset;
}

float u2f(std::uint32_t u) {
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}

bool reads_own_word(const std::uint32_t *rec, const std::uint32_t *setup) {
    const std::uint32_t fpsm = setup[3], tpsm = setup[14];
    if ((rec[1] & kKindMask) != 1 || (setup[0] & (16u | 16384u)) != 16u || setup[12] != setup[1] || setup[13] != setup[2]) return false;
    float f[6], uv[6];
    std::memcpy(f, rec + 4, sizeof f);
    std::memcpy(uv, rec + 15, sizeof uv);
    const float du = (uv[3] - uv[0]) / 16.0f, dv = (uv[4] - uv[1]) / 16.0f;
    if (f[2] == 0.0f || f[3] == 0.0f || std::fabs(du - f[2]) > 0.0005f * std::fabs(f[2]) || std::fabs(dv - f[3]) > 0.0005f * std::fabs(f[3])) return false;
    const int x0 = static_cast<int>(rec[2] & 0xFFFFu), y0 = static_cast<int>(rec[2] >> 16), x1 = static_cast<int>(rec[3] & 0xFFFFu), y1 = static_cast<int>(rec[3] >> 16);
    const auto texel = [](float a, float b, float origin, float span, int p) {
        return static_cast<int>(std::floor((a + (b - a) * ((static_cast<float>(p) - origin) / span)) / 16.0f));
    };
    for (const int y : {y0, y1})
        if (texel(uv[1], uv[4], f[1], f[3], y) != y) return false;
    const int ox = texel(uv[0], uv[3], f[0], f[2], x0) - x0;
    if (texel(uv[0], uv[3], f[0], f[2], x1) - x1 != ox) return false;
    const auto wide = [](std::uint32_t psm) { return psm == PSMCT32 || psm == PSMCT24 || psm == PSMT8H || psm == PSMT4HL || psm == PSMT4HH; };
    if (ox == 0) return tpsm == fpsm || (wide(fpsm) && wide(tpsm));
    const bool half = fpsm == PSMCT16 || fpsm == PSMCT16S;
    return half && tpsm == fpsm && (ox == 8 || ox == -8) && x1 - x0 < 8 && (x0 & 8) == (x1 & 8) && ((x0 ^ 8) == x0 + ox);
}

Range read_range(const std::uint32_t *rec, const std::uint32_t *setup, const Range &whole) {
    if ((rec[1] & kKindMask) != 1 || !(setup[0] & 16u)) return whole;
    // Wrap mode 3 (region repeat) reads (u & MINU) | MAXU, which need not lie between the corners of the sprite: the range is that of the words it can touch, else the whole texture.
    if (region_repeat(setup)) {
        Range r;
        return sprite_read_range(rec, setup, static_cast<int>(rec[2] & 0xFFFFu), static_cast<int>(rec[2] >> 16), static_cast<int>(rec[3] & 0xFFFFu), static_cast<int>(rec[3] >> 16), r) ? r : whole;
    }
    float uv[6];
    std::memcpy(uv, rec + 15, sizeof uv);
    const int u0 = static_cast<int>(std::floor(std::min(uv[0], uv[3]) / 16.0f)) - 1, u1 = static_cast<int>(std::ceil(std::max(uv[0], uv[3]) / 16.0f)) + 1;
    const int v0 = static_cast<int>(std::floor(std::min(uv[1], uv[4]) / 16.0f)) - 1, v1 = static_cast<int>(std::ceil(std::max(uv[1], uv[4]) / 16.0f)) + 1;
    const int tw = std::max(1 << (setup[15] & 0xFFu), static_cast<int>(setup[18] >> 16) + 1), th = std::max(1 << (setup[15] >> 8), static_cast<int>(setup[19] >> 16) + 1);
    if (u0 < 0 || v0 < 0 || u1 >= tw || v1 >= th) return whole;
    return region(setup[12], setup[13], setup[14], u0, v0, u1, v1);
}

Range pages(std::uint32_t bp, std::uint32_t bw, std::uint32_t psm, int x0, int y0, int x1, int y1) {
    std::uint32_t pw, ph;
    page_size(psm, pw, ph);
    const std::uint64_t per_row = std::max<std::uint32_t>(1, bw * 64 / pw);
    const auto page = [&](int x, int y) {
        return static_cast<std::uint64_t>(bp) * 256 + (static_cast<std::uint64_t>(std::max(x, 0)) / pw + static_cast<std::uint64_t>(std::max(y, 0)) / ph * per_row) * 8192;
    };
    Range r;
    r.add(page(x0, y0), page(x1, y1) + 8192);
    return r;
}

void mark_rendered(const Range &r) {
    for (std::uint64_t at = r.lo; at < r.hi; at += 8192) g_rendered[(at >> 13) & 511u] = true;
}

bool rendered(const Range &r) {
    for (std::uint64_t at = r.lo; at < r.hi; at += 8192)
        if (g_rendered[(at >> 13) & 511u]) return true;
    return false;
}

// ---- The decode cache -----------------------------------------------------------------------------------------------------
// Decoded textures stay in the first part of the decoded buffer (kCachedWords) across batches, submits and frames, and are used again while what
// they were decoded from is unchanged. "Unchanged" is decided by identities, the idea of the card path's texture identities (hwcache.cpp) written
// again here for the painter: every 256-byte block of graphics memory has a number that is equal when its bytes are equal.
// - A host-to-local upload gives every block it writes completely a number made of where it wrote and which bytes: the upload cache's entry when
//   the bytes are in it (found there byte for byte), a hash of the bytes when the upload is too small for that cache, else a new number. So the same
//   upload again gives the same number whatever happened in between. A block it writes in part (an edge of the rectangle, or a format that keeps
//   bits of the word: 24-bit, 8H, 4HL, 4HH) mixes its old number in.
// - A draw, a local-to-local copy and a loaded state give every page they write a number nobody had (whole pages, a superset of what they write:
//   cheaper than blocks for the full-screen passes). Pages last written by a draw or a copy hold pictures: textures there are decoded per submit as
//   before (the transient part of the buffer) and not kept.
// - A palette load gives the groups of 16 entries it writes in its slot a number made of its parameters and the numbers of the blocks it reads; the
//   slot starts as a copy of the slot before it, as the load program does.
// A decoded texture is found again by its set-up words (base, width, format, size, palette format and start, TEXA, AEM), the folded numbers of every
// block its texels lie in and the numbers of the palette groups it reads. The numbers change in the order of the command stream: an upload or a palette
// load when it is queued (emit_all dispatches every queued one before the batch whose decodes are then planned), a draw after its batch, a copy when it
// is dispatched. Off with PS2_DECODE_CACHE=0; PS2_DECODE_CACHE_STAT=1 prints what it did every 300 frames.
constexpr std::uint32_t kCachedOrder = 23;  // 8 Mi words (32 MB), handed out in powers of two (a decoded texture is 2^(tw + th) words)
constexpr std::uint32_t kCachedWords = 1u << kCachedOrder;
bool g_dec_on = true, g_dec_small = true, g_dec_stat = false;

void fmt_words(std::uint32_t psm, std::uint32_t bp, std::uint32_t bw, std::uint32_t *out);

std::uint64_t mix64(std::uint64_t a, std::uint64_t b) {
    std::uint64_t z = a ^ (b + 0x9E3779B97F4A7C15ull + (a << 6) + (a >> 2));
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

struct DecStat {
    std::uint64_t walks = 0, cells = 0, picture_walks = 0, asked = 0, found = 0, decoded = 0, decoded_words = 0, pictures = 0, picture_words = 0, evicted = 0, refused = 0, in_place = 0;
} g_dec_count;

struct Identities {
    std::uint64_t block[16384] = {}, block_seq[16384] = {};  // a block's own number and when it was set
    std::uint64_t page[512] = {}, page_seq[512] = {};        // a page's number and when it was set: newer than a block's own, it is the block's
    std::uint64_t touched[512] = {};                         // the last write of any kind into the page
    std::uint64_t block_write[512] = {};                     // the last write of a block's own number in the page
    bool page_drawn[512] = {};
    std::uint64_t seq = 0, fresh = 0;
    std::vector<std::array<std::uint64_t, 32>> slot;  // palette slots: the number of each group of 16 entries
} g_id;

std::uint64_t fresh_id() { return mix64(0xF7E5Bull, ++g_id.fresh) | 1; }

inline std::uint64_t block_id(std::uint32_t b, bool &drawn) {
    const std::uint32_t p = b >> 5;
    if (g_id.page_seq[p] > g_id.block_seq[b]) {
        drawn = drawn || g_id.page_drawn[p];
        return mix64(g_id.page[p], b);
    }
    return g_id.block[b];
}

// Every page the bytes lo .. hi - 1 lie in gets a new number.
void stamp_pages(const Range &r, bool drawn) {
    if (r.hi <= r.lo) return;
    const std::uint64_t first = r.lo >> 13, last = (r.hi - 1) >> 13;
    for (std::uint64_t q = first; q <= last && q - first < 512; ++q) {
        const std::uint32_t p = static_cast<std::uint32_t>(q & 511u);
        g_id.page[p] = fresh_id();
        g_id.page_seq[p] = g_id.touched[p] = ++g_id.seq;
        g_id.page_drawn[p] = drawn;
    }
}

// The blocks of the rectangle x0..x1, y0..y1 of a format (fmt_words `f`), one call per block-sized cell: fn(block, cell x0, y0, x1, y1). A block of
// the GS holds a whole aligned cell of 8 x 8 (32-bit), 16 x 8 (16-bit), 16 x 16 (8-bit) or 32 x 16 (4-bit) pixels.
template <typename Fn> void for_cells(const std::uint32_t *f, std::uint32_t x0, std::uint32_t y0, std::uint32_t x1, std::uint32_t y1, Fn &&fn) {
    const std::uint32_t pws = f[3] & 15u, phs = (f[3] >> 4) & 15u;
    const std::uint32_t bw = pws == 7 ? (phs == 7 ? 32u : 16u) : (phs == 6 ? 16u : 8u), bh = pws == 7 ? 16u : 8u;
    for (std::uint32_t cy = y0 / bh; cy <= y1 / bh; ++cy)
        for (std::uint32_t cx = x0 / bw; cx <= x1 / bw; ++cx) fn(word_at(f, cx * bw, cy * bh) >> 6, cx * bw, cy * bh, cx * bw + bw - 1, cy * bh + bh - 1);
}

void everything_fresh() {
    stamp_pages(Range{0, kMemorySize}, false);
    for (auto &s : g_id.slot)
        for (auto &g : s) g = fresh_id();
}

// A host-to-local upload (`rec` as queue_upload builds it: dbp, dbw, dpsm, dsax, dsay, rrw, done, pixels, offset, lead bits, bits per pixel) of
// bytes whose number is `content`.
void id_upload(const std::uint32_t *rec, std::uint64_t content) {
    const std::uint32_t rrw = rec[5], done = rec[6], pixels = rec[7], psm = rec[2];
    if (!rrw || !pixels) return;
    const std::uint32_t x0 = rec[3], x1 = rec[3] + rrw - 1, y0 = rec[4] + done / rrw, y1 = rec[4] + (done + pixels - 1) / rrw;
    if (x1 > 2047 || y1 > 2047) {  // wraps around the 2048 x 2048 space: not followed, everything counts as written
        everything_fresh();
        return;
    }
    const std::uint32_t full0 = rec[4] + (done + rrw - 1) / rrw, full_end = rec[4] + (done + pixels) / rrw;  // the rows written whole
    const bool whole_words = psm != 0x01 && psm != 0x31 && psm != 0x1B && psm != 0x24 && psm != 0x2C;
    std::uint64_t chunk = mix64(content, 0x0B10Cull);
    for (int k : {0, 1, 2, 3, 4, 5, 6, 7, 9}) chunk = mix64(chunk, rec[k]);
    chunk = mix64(chunk, rec[10] & 0x7FFFFFFFu);
    std::uint32_t f[5];
    fmt_words(psm, rec[0], rec[1], f);
    for_cells(f, x0, y0, x1, y1, [&](std::uint32_t b, std::uint32_t cx0, std::uint32_t cy0, std::uint32_t cx1, std::uint32_t cy1) {
        const bool full = whole_words && cx0 >= x0 && cx1 <= x1 && cy0 >= full0 && cy1 < full_end;
        bool drawn = false;
        g_id.block[b] = (full ? mix64(chunk, b) : mix64(mix64(block_id(b, drawn), chunk), b)) | 1;
        g_id.block_seq[b] = ++g_id.seq;
        g_id.touched[b >> 5] = g_id.block_write[b >> 5] = g_id.seq;
    });
}

std::uint64_t bytes_id(const std::uint8_t *p, std::size_t n) {
    std::uint64_t h = mix64(0xB17E5ull, n), v;
    std::size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        std::memcpy(&v, p + i, 8);
        h = mix64(h, v);
    }
    v = 0;
    std::memcpy(&v, p + i, n - i);
    return mix64(h, v) | 1;
}

// A palette load (`rec` as load_clut builds it: from slot, to slot, cbp, cpsm, csm, csa, 8-bit, cbw, cou, cov), as the load program in gs.wgsl does it.
void id_clut(const std::uint32_t *rec) {
    const std::uint32_t cpsm = rec[3], csm = rec[4], csa = rec[5], entries = rec[6] ? 256u : 16u;
    std::uint64_t h = 0xC1u;
    for (int k = 2; k < 10; ++k) h = mix64(h, rec[k]);
    std::uint32_t f[5];
    fmt_words(cpsm, rec[2], csm ? rec[7] : 1u, f);
    const auto fold = [&](std::uint32_t b, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t) {
        bool drawn = false;
        h = mix64(h, block_id(b, drawn));
    };
    if (csm) for_cells(f, rec[8] * 16, rec[9], rec[8] * 16 + entries - 1, rec[9], fold);
    else if (entries == 256) for_cells(f, 0, 0, 15, 15, fold);
    else for_cells(f, 0, 0, 7, 1, fold);
    h |= 1;
    std::array<std::uint64_t, 32> next = g_id.slot[rec[0] % kClutSlots];
    if (csm)
        for (std::uint32_t g = 0; g < entries / 16; ++g) next[(csa + g) & 31u] = mix64(h, g);
    else if (entries == 256)
        for (std::uint32_t g = 0; g < 16; ++g) next[g] = mix64(h, g);
    else next[csa & 15u] = h;
    g_id.slot[rec[1] % kClutSlots] = next;
}

// The palette groups a set-up's texture reads (convert in gs.wgsl): 0 to 255 for 8-bit indices, the 16 entries at CSA for 4-bit ones.
std::uint64_t palette_id(const std::uint32_t *w) {
    const std::uint32_t psm = w[14];
    const bool eight = psm == 0x13 || psm == 0x1B, four = psm == 0x14 || psm == 0x24 || psm == 0x2C;
    if (!eight && !four) return 0;
    const std::array<std::uint64_t, 32> &s = g_id.slot[w[22] % kClutSlots];
    if (four) return mix64(s[w[17] & 15u], 4);
    std::uint64_t h = 8;
    for (std::uint32_t g = 0; g < 16; ++g) h = mix64(h, s[g]);
    return h;
}

// The folded numbers of the blocks a texture's texels lie in (2^tw x 2^th from its base); false when one of them holds a picture. A texture asked for
// again while none of its pages was written since gives the remembered answer; one that has a page drawn over as a whole since is a picture at once
// (the pages a texture lies in are remembered, they depend only on its base, width, format and size).
struct TexMemo {
    std::uint64_t ident = 0, seq = 0;
    bool picture = false;
    std::vector<std::uint16_t> pages;
};
std::unordered_map<std::uint64_t, TexMemo> g_tex_memo;

bool texture_id(const std::uint32_t *w, std::uint64_t &ident) {
    const std::uint64_t key = mix64(mix64(mix64(w[12], w[13]), w[14]), w[15]);
    auto it = g_tex_memo.find(key);
    if (it != g_tex_memo.end()) {
        const TexMemo &m = it->second;
        bool valid = true;
        for (std::uint16_t p : m.pages) {
            if (g_id.page_drawn[p] && g_id.page_seq[p] > g_id.block_write[p]) return false;  // every block of the page is drawn
            valid = valid && g_id.touched[p] <= m.seq;
        }
        if (valid) {
            ident = m.ident;
            return !m.picture;
        }
    } else {
        if (g_tex_memo.size() >= 4096) g_tex_memo.clear();
        it = g_tex_memo.emplace(key, TexMemo{}).first;
    }
    TexMemo &m = it->second;
    m.pages.clear();
    m.seq = g_id.seq;
    std::uint64_t h = key;
    bool drawn = false;
    const std::uint32_t tw = w[15] & 0xFFu, th = w[15] >> 8;
    const std::uint32_t *f = w + 25;  // the texture's format words (fmt_words)
    for_cells(f, 0, 0, (1u << tw) - 1, (1u << th) - 1, [&](std::uint32_t b, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t) {
        h = mix64(h, block_id(b, drawn));
        const std::uint16_t p = static_cast<std::uint16_t>(b >> 5);
        if (m.pages.empty() || m.pages.back() != p) m.pages.push_back(p);
    });
    std::sort(m.pages.begin(), m.pages.end());
    m.pages.erase(std::unique(m.pages.begin(), m.pages.end()), m.pages.end());
    m.ident = ident = h;
    m.picture = drawn;
    ++g_dec_count.walks;
    g_dec_count.picture_walks += drawn;
    g_dec_count.cells += (1ull << (tw + th)) * 32 / ((1ull << (f[3] & 15u)) << ((f[3] >> 4) & 15u));
    return !drawn;
}

// The decoded textures kept, and the free space of the kept part (a buddy allocator: a free block of 2^k words at an offset that is a multiple of it).
struct Kept {
    std::uint32_t key[8];
    std::uint64_t pal, ident, used, batch;
    std::uint32_t offset, order;
    bool live;
};
std::vector<Kept> g_kept;
std::vector<std::uint32_t> g_kept_free;
std::unordered_multimap<std::uint64_t, std::uint32_t> g_kept_index;
std::vector<std::uint32_t> g_space[kCachedOrder + 1];
std::uint64_t g_dec_batch = 0;

void key_of(const std::uint32_t *w, std::uint32_t *key) {
    static const int kWords[] = {12, 13, 14, 15, 16, 17, 20};
    for (int k = 0; k < 7; ++k) key[k] = w[kWords[k]];
    key[7] = w[0] & 32768u;  // AEM
}

std::uint64_t kept_hash(const std::uint32_t *key, std::uint64_t pal, std::uint64_t ident) {
    std::uint64_t h = mix64(pal, ident);
    for (int k = 0; k < 8; ++k) h = mix64(h, key[k]);
    return h;
}

void space_free(std::uint32_t offset, std::uint32_t order) {
    while (order < kCachedOrder) {
        std::vector<std::uint32_t> &list = g_space[order];
        const auto buddy = std::find(list.begin(), list.end(), offset ^ (1u << order));
        if (buddy == list.end()) break;
        *buddy = list.back();
        list.pop_back();
        offset &= ~(1u << order);
        ++order;
    }
    g_space[order].push_back(offset);
}

bool space_take(std::uint32_t order, std::uint32_t &offset) {
    std::uint32_t k = order;
    while (k <= kCachedOrder && g_space[k].empty()) ++k;
    if (k > kCachedOrder) return false;
    offset = g_space[k].back();
    g_space[k].pop_back();
    while (k > order) {  // split, keep the lower half
        --k;
        g_space[k].push_back(offset + (1u << k));
    }
    return true;
}

void kept_drop(std::uint32_t i) {
    Kept &e = g_kept[i];
    const auto [first, last] = g_kept_index.equal_range(kept_hash(e.key, e.pal, e.ident));
    for (auto it = first; it != last; ++it)
        if (it->second == i) {
            g_kept_index.erase(it);
            break;
        }
    space_free(e.offset, e.order);
    e.live = false;
    g_kept_free.push_back(i);
}

void dec_reset() {
    g_kept.clear();
    g_kept_free.clear();
    g_kept_index.clear();
    for (auto &list : g_space) list.clear();
    g_space[kCachedOrder].push_back(0);
    g_tex_memo.clear();
    if (g_id.slot.empty()) g_id.slot.assign(kClutSlots, {});
    everything_fresh();
}

// Room for 2^order words: the least recently used textures leave as needed (never one this batch reads; none at all while the frame is captured for
// PS2_GPU_BENCH, whose replays use what was kept before the frame and must find it unchanged).
bool kept_room(std::uint32_t order, std::uint32_t &offset) {
    for (;;) {
        if (space_take(order, offset)) return true;
        if (g_bench.capturing) return false;
        std::uint32_t victim = ~0u;
        for (std::uint32_t i = 0; i < g_kept.size(); ++i) {
            const Kept &e = g_kept[i];
            if (e.live && e.batch != g_dec_batch && (victim == ~0u || e.used < g_kept[victim].used)) victim = i;
        }
        if (victim == ~0u) return false;
        kept_drop(victim);
        ++g_dec_count.evicted;
    }
}

void dec_stat_frame() {
    if (!g_dec_stat || frame_count() % 300 != 0) return;
    const DecStat &c = g_dec_count;
    std::uint64_t live = 0, words = 0;
    for (const Kept &e : g_kept)
        if (e.live) ++live, words += 1ull << e.order;
    std::fprintf(stderr, "[decode-cache] frame %llu, last 300 frames: %llu asked, %llu found (%.1f %%), %llu decoded (%.1f MB), %llu pictures decoded (%.1f MB), %llu evicted, "
                         "%llu refused, %llu read in place; kept %llu (%.1f MB); %llu walks (%llu of pictures), %llu cells\n",
                 static_cast<unsigned long long>(frame_count()), static_cast<unsigned long long>(c.asked), static_cast<unsigned long long>(c.found),
                 c.asked ? 100.0 * static_cast<double>(c.found) / static_cast<double>(c.asked) : 0.0, static_cast<unsigned long long>(c.decoded),
                 static_cast<double>(c.decoded_words) * 4 / 1e6, static_cast<unsigned long long>(c.pictures), static_cast<double>(c.picture_words) * 4 / 1e6,
                 static_cast<unsigned long long>(c.evicted), static_cast<unsigned long long>(c.refused), static_cast<unsigned long long>(c.in_place), static_cast<unsigned long long>(live),
                 static_cast<double>(words) * 4 / 1e6, static_cast<unsigned long long>(c.walks), static_cast<unsigned long long>(c.picture_walks),
                 static_cast<unsigned long long>(c.cells));
    g_dec_count = DecStat{};
}

struct Decode {
    std::uint32_t setup, offset, words;
};
std::vector<Decode> g_decodes;   // the decodes this batch dispatches
std::vector<Decode> g_assigned;  // every decoded texture a setup of this batch reads, dispatched or kept, for setups that share it
std::vector<double> g_setup_taps;  // per setup of the batch: pixels in the boxes of its shapes (plan_decodes, filled when first asked)

void plan_decodes(std::uint32_t prims) {
    Batch &b = g_batch;
    g_decodes.clear();
    g_assigned.clear();
    g_setup_taps.clear();
    if (!g_decode) return;
    const bool big = prims > kDecodePrims, cache = g_dec_on && !hw::enabled();
    if (!big && !(cache && g_dec_small)) return;  // a small batch decodes nothing, but may read what is kept
    ++g_dec_batch;
    const std::uint32_t setups = static_cast<std::uint32_t>(b.setups.size() / kSetupWords);
    static const int kKey[] = {12, 13, 14, 15, 16, 17, 20, 22};
    for (std::uint32_t i = 0; i < setups; ++i) {
        std::uint32_t *w = b.setups.data() + i * kSetupWords;
        if (!(w[0] & 2u)) continue;
        const std::uint32_t tw = w[15] & 0xFFu, th = w[15] >> 8;
        const std::uint32_t mu = (w[7] >> 24) & 3u, mv = (w[7] >> 26) & 3u;
        const auto inside = [](std::uint32_t mode, std::uint32_t region, std::uint32_t size) { return mode < 2 || ((region & 0x3FFu) < (1u << size) && (region >> 16) < (1u << size)); };
        // every set-up checks its own clamp or repeat region against the texture, also one that would share a decode (it reads past it otherwise)
        if (!inside(mu, w[18], tw) || !inside(mv, w[19], th)) continue;
        bool shared = false;
        for (const Decode &d : g_assigned) {
            const std::uint32_t *o = b.setups.data() + d.setup * kSetupWords;
            bool same = (o[0] & 32768u) == (w[0] & 32768u);
            for (int k : kKey) same = same && o[k] == w[k];
            if (same) {
                w[24] = d.offset;
                shared = true;
                break;
            }
        }
        if (shared) continue;
        const std::uint32_t words = 1u << (tw + th);
        // Decoding costs the chip about one read per texel of the whole texture, reading in place about one per tap of the pixels that use it.
        // A texture whose shapes cover few pixels is read in place, as a small batch's are (the same texels either way): Silent Hill 2 draws
        // its 512 x 512 palette pages with a few pixels per set-up and many palettes, and decoded 6 to 8 million texels a frame for that.
        // The pixels are the shapes' boxes (more than they cover), four taps each when filtered, and decoding wins from an eighth of the texels on.
        const auto worth = [&] {
            if (g_setup_taps.empty()) {
                g_setup_taps.assign(setups, 0.0);
                for (std::uint32_t q = 0; q < prims; ++q) {
                    const std::uint32_t *rec = b.prims.data() + q * kPrimWords;
                    if (rec[0] < setups)
                        g_setup_taps[rec[0]] += static_cast<double>((rec[3] & 0xFFFFu) - (rec[2] & 0xFFFFu) + 1) * static_cast<double>((rec[3] >> 16) - (rec[2] >> 16) + 1);
                }
            }
            const double taps = g_setup_taps[i] * ((w[0] & 16384u) ? 4.0 : 1.0) * g_scale * g_scale;
            return taps * 8.0 >= static_cast<double>(words);
        };
        if (cache && tw <= 10 && th <= 10) {
            std::uint64_t ident;
            if (texture_id(w, ident) && !(g_scale > 1 && rendered(pages(w[12], w[13], w[14], 0, 0, (1 << tw) - 1, (1 << th) - 1)))) {
                ++g_dec_count.asked;
                std::uint32_t key[8];
                key_of(w, key);
                const std::uint64_t pal = palette_id(w);
                const auto [first, last] = g_kept_index.equal_range(kept_hash(key, pal, ident));
                Kept *found = nullptr;
                for (auto it = first; it != last && !found; ++it) {
                    Kept &e = g_kept[it->second];
                    if (e.pal == pal && e.ident == ident && std::equal(key, key + 8, e.key)) found = &e;
                }
                if (found) {
                    ++g_dec_count.found;
                    found->used = g_serial;
                    found->batch = g_dec_batch;
                    w[24] = found->offset;
                    g_assigned.push_back({i, found->offset, words});
                    continue;
                }
                if (!big) continue;
                if (!worth()) {
                    ++g_dec_count.in_place;
                    continue;
                }
                std::uint32_t offset;
                if (kept_room(tw + th, offset)) {
                    std::uint32_t at;
                    if (!g_kept_free.empty()) {
                        at = g_kept_free.back();
                        g_kept_free.pop_back();
                    } else {
                        at = static_cast<std::uint32_t>(g_kept.size());
                        g_kept.emplace_back();
                    }
                    Kept &e = g_kept[at];
                    std::copy(key, key + 8, e.key);
                    e.pal = pal, e.ident = ident, e.used = g_serial, e.batch = g_dec_batch, e.offset = offset, e.order = tw + th, e.live = true;
                    g_kept_index.emplace(kept_hash(key, pal, ident), at);
                    ++g_dec_count.decoded;
                    g_dec_count.decoded_words += words;
                    w[24] = offset;
                    g_decodes.push_back({i, offset, words});
                    g_assigned.push_back({i, offset, words});
                    continue;
                }
                ++g_dec_count.refused;
            }
        }
        if (!big) continue;
        // a picture (or no room): decoded for this submit only, after the kept part
        if (!worth()) {
            ++g_dec_count.in_place;
            continue;
        }
        if (g_decoded_used + words > kDecodedWords) continue;
        if (g_scale > 1 && rendered(pages(w[12], w[13], w[14], 0, 0, (1 << tw) - 1, (1 << th) - 1))) continue;
        ++g_dec_count.pictures;
        g_dec_count.picture_words += words;
        w[24] = kCachedWords + g_decoded_used;
        g_decodes.push_back({i, w[24], words});
        g_assigned.push_back({i, w[24], words});
        g_decoded_used += words;
    }
}

std::uint32_t mip_top_of(const std::uint32_t *mip);
Range mip_range(const std::uint32_t *setup, const std::uint32_t *mip, std::uint32_t top);

std::uint32_t snapshot_batch(std::uint32_t prims, std::uint32_t &lo, std::uint32_t &hi) {
    Batch &b = g_batch;
    lo = hi = 0;
    if (g_batch_reads.empty()) return 0;
    const Range fb = pages(b.fbp, b.fbw, b.fpsm, b.minx, b.miny, b.maxx, b.maxy);
    const Range z = pages(b.zbp, b.fbw, b.zpsm, b.minx, b.miny, b.maxx, b.maxy);
    const auto written = [&](const Range &t) { return t.overlaps(fb.lo, fb.hi) || (b.zwrite && t.overlaps(z.lo, z.hi)); };
    bool any = false;
    for (const Range &t : g_batch_reads) any = any || written(t);
    if (!any) return 0;
    Range shared;
    for (std::uint32_t i = 0; i < prims; ++i) {
        const std::uint32_t *rec = b.prims.data() + i * kPrimWords;
        const std::uint32_t *setup = b.setups.data() + rec[0] * kSetupWords;
        const std::uint32_t *mip = b.any_mip && (rec[0] + 1) * kMipWords <= b.mips.size() ? b.mips.data() + rec[0] * kMipWords : nullptr;
        const std::uint32_t top = mip ? mip_top_of(mip) : 0u;
        if (!(setup[0] & 2u) || (!top && reads_own_word(rec, setup))) continue;
        const int tw = std::max(1 << (setup[15] & 0xFFu), static_cast<int>(setup[18] >> 16) + 1), th = std::max(1 << (setup[15] >> 8), static_cast<int>(setup[19] >> 16) + 1);
        const Range whole = region(setup[12], setup[13], setup[14], 0, 0, tw - 1, th - 1);
        const bool listed = !top && (rec[1] & kKindMask) == 1 && region_repeat(setup) && g_exact.state == 1 && g_exact.reads_ok;  // the word sets know what such sprites read (exact_reads)
        Range t = top ? whole : listed ? g_exact.reads : read_range(rec, setup, whole);
        if (top) {  // a shape that reads mip levels: the whole of every level it can reach (mip_range)
            const Range levels = mip_range(setup, mip, top);
            t.add(levels.lo, levels.hi);
        }
        for (const Range *w : {&fb, b.zwrite ? &z : nullptr})
            if (w && t.overlaps(w->lo, w->hi)) shared.add(std::max(t.lo, w->lo), std::min(t.hi, w->hi));
    }
    if (shared.lo >= shared.hi) return 0;
    lo = static_cast<std::uint32_t>(std::min<std::uint64_t>(shared.lo, kMemorySize) / 4);
    hi = static_cast<std::uint32_t>(std::min<std::uint64_t>(shared.hi, kMemorySize) / 4);
    if (hi <= lo) return 0;
    const std::uint32_t words = (hi - lo) * g_scale * g_scale;
    dispatch_linear(kSnapshot, words, push_params({lo, words}));
    return words;
}

void note_raster(std::uint32_t prims, std::uint32_t tiles, std::uint32_t entries, std::uint32_t snapshot) {
    if (g_bench.capturing) {
        const Batch &b = g_batch;
        BenchInfo info;
        info.index = g_commands.size();
        info.prims = prims;
        info.tiles = tiles;
        info.entries = entries;
        info.snapshot = snapshot;
        info.setups = static_cast<std::uint32_t>(b.setups.size() / kSetupWords);
        info.fpsm = b.setups[3];
        info.tpsm = b.setups[14];
        for (std::uint32_t i = 0; i < prims; ++i) {
            const std::uint32_t *rec = b.prims.data() + i * kPrimWords;
            const std::uint32_t flags = b.setups[rec[0] * kSetupWords];
            ++info.kinds[rec[1] & 3u];
            const double bw = static_cast<double>((rec[3] & 0xFFFFu) - (rec[2] & 0xFFFFu) + 1), bh = static_cast<double>((rec[3] >> 16) - (rec[2] >> 16) + 1);
            info.bbox += bw * bh;
            info.area += (rec[1] & kKindMask) == 0 ? std::fabs(0.5 / static_cast<double>(u2f(rec[10]))) : bw * bh;
            info.textured += (flags & 2u) ? 1u : 0u;
            info.blended += (flags & 8u) ? 1u : 0u;
            info.ztest += (flags & 512u) ? 1u : 0u;
            info.zwrite += ((flags & 512u) && !(flags & 32u)) ? 1u : 0u;
            info.atest += (flags & 64u) ? 1u : 0u;
            info.bilinear += ((flags & 2u) && (flags & 16384u)) ? 1u : 0u;
            info.fog += (flags & 4u) ? 1u : 0u;
            info.date += (flags & 128u) ? 1u : 0u;
        }
        g_bench_info.push_back(info);
    }
    if (!g_profile_out) return;
    const Batch &b = g_batch;
    const std::uint32_t *first = b.setups.data();
    std::uint32_t textured = 0;
    for (std::uint32_t i = 0; i < prims; ++i) textured += (b.setups[b.prims[i * kPrimWords] * kSetupWords] & 2u) ? 1u : 0u;
    g_command_info.push_back({g_commands.size(), {prims, tiles, entries, snapshot, b.prims[1], first[3] | (first[14] << 8) | (first[6] << 16), first[0], textured}});
}

// The open batch's lists, started on the tile helper while the batch is still being drawn (tiles.hpp, kStreamStep); nullptr when there is none.
tiles::Job *g_stream = nullptr;

// The open batch's records are dropped, or move at a submit: its started lists are given up (flush_batch then builds them another way).
void drop_stream() {
    if (!g_stream) return;
    tiles::cancel(g_stream);
    g_stream = nullptr;
}

void reset_batch() {
    drop_stream();
    Batch &b = g_batch;
    exact_reset();
    b.prims.clear();
    b.setups.clear();
    b.mips.clear();
    b.any_mip = false;
    b.setup_serial = 0;
    g_batch_reads.clear();
    b.key = ~0ull;
    b.minx = b.miny = 1 << 30;
    b.maxx = b.maxy = -1;
}

// Puts the lists of a job the helper has finished into the arena and fills in the parameter record and the command that flush_batch left open for them.
void place_tiles(tiles::Job *job) {
    const tiles::Lists &lists = job->result;
    if (job->param >= g_param_records.size() || job->command >= g_commands.size()) {  // cannot happen; a started job always has its slots before it is finished
        std::fprintf(stderr, "[tiles] BAD job: parameter record %u of %zu, command %u of %zu\n", job->param, g_param_records.size(), job->command, g_commands.size());
        std::abort();
    }
    const std::uint32_t tile_offset = static_cast<std::uint32_t>(g_arena.size());
    std::memcpy(g_arena.grow(lists.words), job->lists.data(), static_cast<std::size_t>(lists.words) * 4);
    if (tiles::logging()) tiles::note_lists(job->lists.data(), lists);
    std::array<std::uint32_t, kParamStride / 4> &record = g_param_records[job->param];
    record[0] = tile_offset;
    record[3] = lists.tiles;
    record[4] = tile_offset + lists.tiles * 2 + 2;
    Command &c = g_commands[job->command];
    c.x = std::min(lists.tiles, 1024u);
    c.y = (lists.tiles + 1023) / 1024;
    tiles::release(job);
}

// Places the finished jobs, oldest first; with `all` it waits for the jobs that are not done yet.
void finish_tiles(bool all) {
    if (all) drop_stream();  // a submit while a batch is open: its records move to the front of the array (Shapes::restart)
    if (all) tiles::build_waiting();  // the jobs the helper has not reached: built here rather than waited for
    while (tiles::Job *job = tiles::next_finished(all)) place_tiles(job);
}

// The tile helper may build the lists (tiles.hpp); see flush_batch.
bool tiles_to_helper() { return tiles::threaded() && !g_live && !g_bench.capturing && !g_profile_out && !hw::enabled(); }

void flush_batch() {
    Batch &b = g_batch;
    if (b.prims.empty()) {
        b.key = ~0ull;
        return;
    }
    const std::uint32_t prims = static_cast<std::uint32_t>(b.prims.size() / kPrimWords);
    if (g_live) {  // the card takes the batch, or the painter does and the card's pictures are told what it reads and writes
        static_assert(sizeof(Range) == 16, "the card path reads the texture ranges as pairs of 64-bit numbers");
        hw::live_position(static_cast<std::uint32_t>(g_commands.size()));
        const hw::LiveBatch lb{b.prims.data(), prims, b.setups.data(), static_cast<std::uint32_t>(b.setups.size() / kSetupWords), b.fbp, b.fbw, b.fpsm, b.zbp, b.zpsm, b.zte, b.zwrite,
                               b.minx, b.miny, b.maxx, b.maxy, reinterpret_cast<const std::uint64_t *>(g_batch_reads.data()), static_cast<std::uint32_t>(g_batch_reads.size()), frame_count()};
        hw::pal_write(b.fbp, b.fbw, b.fpsm, b.minx, b.miny, b.maxx, b.maxy);  // the palette record sees what this batch overwrites
        if (b.zwrite) hw::pal_write(b.zbp, b.fbw, b.zpsm, b.minx, b.miny, b.maxx, b.maxy);
        if (b.route == 1 && hw::live_batch(lb)) {
            reset_batch();
            return;
        }
        hw::live_painter(lb);
    }
    // The tile lists (tiles.cpp): which tiles every shape touches, and the table and lists the raster program reads. On a threaded run the helper builds them
    // while this thread goes on with the batch, and finish_tiles fills in what they are for (the numbers in the parameter record and the command) when they are
    // done; else they are built here, as they always were. The card path and the replay capture (PS2_GPU_BENCH) want the numbers at once, so they build here.
    finish_tiles(false);
    // A big batch whose lists the helper started while it was drawn (draw, kStreamStep): it gets the last records now (unless the lists are now wanted at once).
    if (g_stream && !tiles_to_helper()) drop_stream();
    tiles::Job *const streamed = g_stream;
    if (streamed) tiles::close(streamed, b.prims.data(), prims);
    g_stream = nullptr;
    // With kMaxBacklog jobs waiting the helper is behind (mostly on one big batch while many small ones follow): this batch's lists are then built here
    // at once instead of waiting for the oldest job, so neither thread waits for the other. A batch of a few shapes is built here too: handing it
    // over (and waking the helper) costs more than building it (the God of War ship fight ends about 100 batches a frame after one shape).
    tiles::Job *const job = streamed ? streamed
                            : tiles_to_helper() && prims >= tiles::kMinJob && tiles::backlog() < tiles::kMaxBacklog ? tiles::start(b.prims.data(), prims) : nullptr;
    plan_decodes(prims);
    const std::uint32_t prim_offset = static_cast<std::uint32_t>(b.prims.begin);  // the records stay where draw() wrote them (g_shapes, kept by commit() below); place_shapes adds the arena's size at submit
    const std::uint32_t setup_offset = arena_append(b.setups.data(), b.setups.size() * 4);
    // The mipmap table only when a setup of the batch has one (the draw program then reads it for every setup; prm 9 is ~0 otherwise)
    const std::uint32_t mip_offset = b.any_mip && b.mips.size() / kMipWords == b.setups.size() / kSetupWords ? arena_append(b.mips.data(), b.mips.size() * 4) : 0xFFFFFFFFu;
    for (const Decode &d : g_decodes) dispatch_linear(kDecode, d.words, push_params({setup_offset + d.setup * kSetupWords, d.offset, d.words}));
    std::uint32_t tile_offset = 0;
    tiles::Lists lists;
    if (!job) {
        tile_offset = static_cast<std::uint32_t>(g_arena.size());
        lists = tiles::build_now(b.prims.data(), prims, [](void *, std::size_t words) { return g_arena.grow(words); }, nullptr);
        if (tiles::logging()) tiles::note_lists(g_arena.data() + tile_offset, lists);
    }
    const std::uint32_t list_offset = tile_offset + lists.tiles * 2 + 2;
    std::uint32_t snapshot_lo, snapshot_hi;
    const std::uint32_t snapshot = snapshot_batch(prims, snapshot_lo, snapshot_hi);
    const std::uint32_t param = push_params({tile_offset, prim_offset, setup_offset, lists.tiles, list_offset, snapshot_lo, snapshot_hi, 1u, 0u, mip_offset});
    note_raster(prims, lists.tiles, lists.total, snapshot);
    if (stat::g_on) {
        stat::batch(prims, kWhyName[g_why]);
        if (!g_batch_log) g_why = kWhyFlush;
    }
    if (g_batch_log) {
        if (g_batch_log_frame > 1 && frame_count() == g_batch_log_frame) std::fprintf(stderr, "[break] %s %u\n", kWhyName[g_why], prims);
        ++g_why_runs[g_why];
        g_why_prims[g_why] += prims;
        g_why = kWhyFlush;
    }
    if (g_bench.capturing && hw::enabled()) hw::capture_batch(static_cast<std::uint32_t>(g_bench.subs.size()), static_cast<std::uint32_t>(g_commands.size()), b.prims.data(), prims, b.setups.data(), static_cast<std::uint32_t>(b.setups.size() / kSetupWords));
    g_commands.push_back({kRaster, std::min(lists.tiles, 1024u), (lists.tiles + 1023) / 1024, param, mip_offset != 0xFFFFFFFFu});
    g_shape_params.push_back(param);
    if (job) {  // the helper has the records; the slots wait for its numbers
        job->param = param;
        job->command = static_cast<std::uint32_t>(g_commands.size() - 1);
    }
    mark_rendered(pages(b.fbp, b.fbw, b.fpsm, b.minx, b.miny, b.maxx, b.maxy));
    if (b.zwrite) mark_rendered(pages(b.zbp, b.fbw, b.zpsm, b.minx, b.miny, b.maxx, b.maxy));
    if (g_dec_on) {  // the decode cache: this batch's decodes saw memory as it was, its draws are writers
        stamp_pages(pages(b.fbp, b.fbw, b.fpsm, b.minx, b.miny, b.maxx, b.maxy), true);
        if (b.zwrite) stamp_pages(pages(b.zbp, b.fbw, b.zpsm, b.minx, b.miny, b.maxx, b.maxy), true);
    }
    if (hwcache::g_on) {  // the texture cache (hwcache.hpp): this batch reads its textures as they are now, then its draws are writers
        if (hwcache::counting()) hwcache::batch_textures(b.setups.data(), static_cast<std::uint32_t>(b.setups.size() / kSetupWords), kSetupWords);
        hwcache::write_region(b.fbp, b.fbw, b.fpsm, b.minx, b.miny, b.maxx, b.maxy, hwcache::kDraw);
        if (b.zwrite) hwcache::write_region(b.zbp, b.fbw, b.zpsm, b.minx, b.miny, b.maxx, b.maxy, hwcache::kDraw);
    }
    if (g_bench.capturing && hw::enabled() && hw::match())  // PS2_GPU_HW_MATCH=1: neutralise the shapes the trial does not draw, so both pictures lack them (after
        for (std::uint32_t i = 0; i < prims; ++i) {           // everything above has read the records as they were)
            std::uint32_t *rec = b.prims.data() + i * kPrimWords;
            if (!hw::left_out(rec, b.setups.data() + rec[0] * kSetupWords)) continue;
            rec[2] = 0x7FFF7FFFu;
            rec[3] = 0;
        }
    b.prims.commit();
    reset_batch();
}

void batch_ranges() {
    Batch &b = g_batch;
    if (b.rx0 == b.minx && b.ry0 == b.miny && b.rx1 == b.maxx && b.ry1 == b.maxy) return;
    b.rx0 = b.minx;
    b.ry0 = b.miny;
    b.rx1 = b.maxx;
    b.ry1 = b.maxy;
    b.fb_range = region(b.fbp, b.fbw, b.fpsm, b.minx, b.miny, b.maxx, b.maxy);
    b.z_range = region(b.zbp, b.fbw, b.zpsm, b.minx, b.miny, b.maxx, b.maxy);
}

bool batch_conflict(const Range &r, bool clut) {
    Batch &b = g_batch;
    if (b.prims.empty()) return false;
    batch_ranges();
    if (b.fb_range.overlaps(r.lo, r.hi)) return true;
    if (b.zte && b.z_range.overlaps(r.lo, r.hi)) return true;
    if (!clut)
        for (const Range &t : g_batch_reads)
            if (t.overlaps(r.lo, r.hi)) return true;
    return false;
}

void emit_pre() {
    std::size_t i = 0;
    while (i < g_pre.size()) {
        const bool clut = g_pre[i].clut;
        std::size_t j = i + 1;
        while (j < g_pre.size() && g_pre[j].clut == clut) {
            bool overlap = false;
            if (!clut)
                for (std::size_t k = i; k < j && !overlap; ++k) overlap = uploads_overlap(g_pre[k], g_pre[j]);
            if (overlap) break;
            ++j;
        }
        const std::uint32_t count = static_cast<std::uint32_t>(j - i);
        std::uint32_t *records = g_arena.grow(count * 12u);
        const std::uint32_t offset = static_cast<std::uint32_t>(records - g_arena.data());
        std::uint32_t total = 0;
        for (std::size_t k = i; k < j; ++k) {
            std::memcpy(records + (k - i) * 12, g_pre[k].rec, sizeof g_pre[k].rec);
            if (!clut) {
                records[(k - i) * 12 + 11] = total;
                total += g_pre[k].rec[7];
            }
        }
        if (g_live) {  // the card's pictures are told at the point of the command stream where the upload (a writer) or the palette load (a reader) runs
            hw::live_position(static_cast<std::uint32_t>(g_commands.size()));
            for (std::size_t k = i; k < j; ++k) clut ? hw::live_reader(g_pre[k].range.lo, g_pre[k].range.hi) : hw::live_writer(g_pre[k].range.lo, g_pre[k].range.hi);
        }
        const std::size_t before = g_commands.size();
        if (clut) g_commands.push_back({kLoadClut, (count + 15) / 16, 1, push_params({offset, count})});  // one workgroup per 16 palette loads (CLUT_CHUNK in gs.wgsl)
        else dispatch_linear(kUpload, total, push_params({offset, count, total}));
        if (g_bench.capturing && hw::enabled() && g_commands.size() > before) {
            Range both;
            for (std::size_t k = i; k < j; ++k) both.add(g_pre[k].range.lo, g_pre[k].range.hi);
            hw::capture_pre(static_cast<std::uint32_t>(g_bench.subs.size()), static_cast<std::uint32_t>(before), clut, both.lo, both.hi);
            if (hwcache::g_on) hwcache::capture_pre(static_cast<std::uint32_t>(g_bench.subs.size()), static_cast<std::uint32_t>(before), i, j);
        }
        i = j;
    }
    g_pre.clear();
    if (hwcache::g_on) hwcache::pending_clear();
}

void emit_all() {
    emit_pre();
    flush_batch();
}

void queue_pre(const PreOp &op) {
    g_why = op.clut ? kWhyClut : kWhyUpload;
    if (batch_conflict(op.range, op.clut)) emit_all();
    g_why = kWhyFlush;
    g_pre.push_back(op);
}

void submit(WGPUTextureView present_view);

std::uint64_t queue_upload(const std::uint8_t *bytes, std::size_t size, std::uint32_t lead_bits) {
    Image &im = g_image;
    const std::uint64_t bits = static_cast<std::uint64_t>(size) * 8;
    if (bits <= lead_bits) return 0;
    const std::uint32_t pixels = static_cast<std::uint32_t>(std::min<std::uint64_t>((bits - lead_bits) / im.bpp, im.total - im.done));
    if (!pixels) return 0;
    const std::uint64_t used = lead_bits + static_cast<std::uint64_t>(pixels) * im.bpp;
    PreOp op;
    const auto n = static_cast<std::size_t>((used + 7) / 8);
    std::uint32_t offset = 0, source = 0;
    if (n >= kCacheMinBytes) {
        const std::uint64_t key = content_key(bytes, n);
        bool cached = cache_find(key, bytes, n, offset);
        if (!cached && ++g_cache_seen[key] >= 2) {
            cached = cache_insert(key, bytes, n, offset);
            // The ring cannot evict its oldest entry while the pending submit reads it. A texture drawn every frame
            // is read again before the next insert, so without a submit here the cache would take nothing new
            // once it wraps, and every upload from then on goes through the per-submit buffer.
            if (!cached && !g_cache_ring.empty() && g_cache_ring.front().used == g_serial) {
                emit_all();
                submit(nullptr);
                cached = cache_insert(key, bytes, n, offset);
            }
        }
        if (cached) source = 0x80000000u;
        if (g_cache_seen.size() > 65536) g_cache_seen.clear();
    }
    if (!source) offset = arena_append(bytes, n);
    const std::uint32_t rec[11] = {im.dbp, im.dbw, im.dpsm, im.dsax, im.dsay, im.rrw, im.done, pixels, offset, lead_bits, im.bpp | source};
    std::memcpy(op.rec, rec, sizeof rec);
    const std::uint32_t first = im.done / im.rrw, last = (im.done + pixels - 1) / im.rrw;
    op.range = region(im.dbp, im.dbw, im.dpsm, static_cast<int>(im.dsax), static_cast<int>(im.dsay + first), static_cast<int>(im.dsax + im.rrw - 1),
                      static_cast<int>(im.dsay + last));
    op.x0 = im.dsax;
    op.x1 = im.dsax + im.rrw - 1;
    op.y0 = im.dsay + first;
    op.y1 = im.dsay + last;
    op.image = im.id;
    const Range pal_range_of_upload = op.range;
    queue_pre(op);
    if (g_dec_on) id_upload(op.rec, source ? mix64(g_cache_uid, 0xCAC4Eull) : n < kCacheMinBytes ? bytes_id(bytes, n) : fresh_id());  // after queue_pre, as hwcache below
    if (g_live) hw::pal_upload(im.dbp, im.dbw, im.dpsm, im.dsax, im.dsay, im.rrw, im.done, pixels, static_cast<std::uint32_t>(lead_bits), im.bpp, bytes, size, pal_range_of_upload.lo, pal_range_of_upload.hi);
    if (hwcache::g_on)  // after queue_pre: a batch that reads this memory was flushed by it and saw the old numbers
        hwcache::write_upload(im.dbp, im.dbw, im.dpsm, static_cast<int>(im.dsax), static_cast<int>(im.dsay + first), static_cast<int>(im.dsax + im.rrw - 1), static_cast<int>(im.dsay + last),
                               lead_bits == 0 && im.done % im.rrw == 0 && (pixels % im.rrw == 0 || im.done + pixels >= im.total), bytes, size);
    im.done += pixels;
    return used;
}

void finish_image_if_done() {
    Image &im = g_image;
    if (im.done < im.total) return;
    im.active = false;
    im.pending.clear();
    const std::uint32_t psm = im.dpsm;
    if (psm != PSMCT32 && psm != PSMCT16 && psm != PSMCT16S && psm != PSMT8 && psm != PSMT4 && psm != PSMZ32 && psm != PSMZ16 && psm != PSMZ16S) return;
    std::uint32_t pw, ph;
    page_size(psm, pw, ph);
    // every page the upload touches, even in part: a texture sheet smaller than a page (Urban Reign's 128 x 64 4-bit button icons, put
    // where the fights draw their glow) must not be read as an upscaled picture (kUpscaledSource)
    const std::uint32_t per_row = std::max<std::uint32_t>(1, im.dbw * 64 / pw), rows = im.rrw ? im.total / im.rrw : 0;
    if (!rows) return;
    for (std::uint32_t py = im.dsay / ph; py <= (im.dsay + rows - 1) / ph; ++py)
        for (std::uint32_t px = im.dsax / pw; px <= (im.dsax + im.rrw - 1) / pw; ++px)
            g_rendered[((im.dbp * 256 + (px + py * per_row) * 8192) >> 13) & 511u] = false;
}

void flush_image() {
    Image &im = g_image;
    if (!im.active || im.pending.empty()) return;
    const std::uint64_t used = queue_upload(im.pending.data(), im.pending.size(), im.lead_bits);
    if (!used) return;
    im.pending.erase(im.pending.begin(), im.pending.begin() + static_cast<std::ptrdiff_t>(used / 8));
    im.lead_bits = static_cast<std::uint32_t>(used % 8);
    finish_image_if_done();
}

void flush_all() {
    flush_image();
    emit_all();
}

void on_error(WGPUDevice const *, WGPUErrorType type, WGPUStringView message, void *, void *) {
    std::fprintf(stderr, "[gpu] error %d: %.*s\n", static_cast<int>(type), static_cast<int>(message.length == WGPU_STRLEN ? std::strlen(message.data) : message.length), message.data);
}

void pump() {
#ifdef __EMSCRIPTEN__
    browser_yield();
    wgpuInstanceProcessEvents(g_instance);
#else
    if (g_device) wgpuDevicePoll(g_device, true, nullptr);
    else wgpuInstanceProcessEvents(g_instance);
#endif
}

void wait_until(const bool &flag) {
    while (!flag) pump();
}

void query_mapped(WGPUMapAsyncStatus status, WGPUStringView, void *, void *) {
    if (status == WGPUMapAsyncStatus_Success) g_query_ready = true;
    else g_query_pending = false;
}

namespace post {
// The calls submit() makes after recording, in the same order (upload_data, the parameter records, finish, submit, the timestamps' map,
// the poll; then present()'s present and releases).
void run(Job &j) {
    const auto from = std::chrono::steady_clock::now();
    if (j.blit) wgpuQueueWriteBuffer(g_queue, g_blit_params, 0, j.blit_words, sizeof j.blit_words);
    j.blit = false;
    if (j.writes) {
        wgpuQueueWriteBuffer(g_queue, j.data, 0, j.arena.data(), j.arena.size() * 4);
        if (j.shape_words) wgpuQueueWriteBuffer(g_queue, j.data, j.arena.size() * 4, j.shapes.data(), j.shape_words * 4);
        wgpuQueueWriteBuffer(g_queue, j.params, 0, j.records.data(), j.records.size() * kParamStride);
    }
    WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(j.enc, nullptr);
    wgpuQueueSubmit(g_queue, 1, &cmd);
    wgpuCommandBufferRelease(cmd);
    wgpuCommandEncoderRelease(j.enc);
    if (j.measure) {
        WGPUBufferMapCallbackInfo cb = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
        cb.mode = WGPUCallbackMode_AllowProcessEvents;
        cb.callback = query_mapped;
        wgpuBufferMapAsync(g_query_read, WGPUMapMode_Read, 0, 16, cb);
    }
    wgpuDevicePoll(g_device, false, nullptr);
    if (j.view) {
        wgpuSurfacePresent(g_surface);
        wgpuTextureViewRelease(j.view);
        wgpuTextureRelease(j.texture);
    }
    j.enc = nullptr;
    j.view = nullptr;
    j.texture = nullptr;
    j.arena.clear();
    j.shapes.used = 0;
    j.shape_words = 0;
    j.records.clear();
    g_state->ns += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - from).count());
    ++g_state->jobs;
}

#ifdef _WIN32
extern "C" __declspec(dllimport) void *__stdcall GetModuleHandleW(const wchar_t *name);
extern "C" __declspec(dllimport) void *__stdcall GetCurrentThread();
extern "C" __declspec(dllimport) void *__stdcall GetProcAddress(void *module, const char *name);
#endif
void loop() {
#ifdef _WIN32  // the name shows in profilers and speedcheck (SetThreadDescription, Windows 10 1607 on, looked up so older systems still start)
    using SetDescription = long(__stdcall *)(void *, const wchar_t *);
    if (void *kernel = GetModuleHandleW(L"kernel32.dll"))
        if (const auto set = reinterpret_cast<SetDescription>(GetProcAddress(kernel, "SetThreadDescription"))) set(GetCurrentThread(), L"ps2 submit");
#elif defined(__linux__)
    pthread_setname_np(pthread_self(), "ps2 submit");
#endif
    std::unique_lock<std::mutex> lock(g_state->mutex);
    for (;;) {
        g_state->cv.wait(lock, [] { return g_state->busy; });
        lock.unlock();
        run(g_state->job);
        lock.lock();
        g_state->busy = false;
        g_state->cv.notify_all();
    }
}

void drain() {
    if (!g_state) return;
    std::unique_lock<std::mutex> lock(g_state->mutex);
    if (!g_state->busy) return;
    wt::Scope s(wt::kDrain);
    g_state->cv.wait(lock, [] { return !g_state->busy; });
}

// The job to fill: the last one is done, the thread exists.
Job &slot() {
    if (!g_state) {
        g_state = new State;
        std::thread(loop).detach();
        std::atexit([] { drain(); });  // the last picture is submitted before the library is torn down
    }
    drain();
    return g_state->job;
}

void go() {
    std::lock_guard<std::mutex> lock(g_state->mutex);
    g_state->busy = true;
    g_state->cv.notify_all();
}
}  // namespace post

bool wt::post_totals(std::uint64_t &ns, std::uint64_t &jobs) {
    if (!post::g_state) return false;
    ns = post::g_state->ns;
    jobs = post::g_state->jobs;
    return true;
}

WGPUBuffer make_buffer(std::uint64_t size, WGPUBufferUsage usage) {
    WGPUBufferDescriptor d = WGPU_BUFFER_DESCRIPTOR_INIT;
    d.size = size;
    d.usage = usage;
    return wgpuDeviceCreateBuffer(g_device, &d);
}

WGPUBindGroup make_group(WGPUBuffer params, WGPUBuffer data) {
    WGPUBindGroupEntry e[8] = {};
    WGPUBuffer buffers[8] = {g_vram, g_tables, params, data, g_clut, g_scratch, g_cache, g_decoded};
    for (int i = 0; i < 8; ++i) {
        e[i] = WGPU_BIND_GROUP_ENTRY_INIT;
        e[i].binding = static_cast<std::uint32_t>(i);
        e[i].buffer = buffers[i];
        e[i].size = i == 2 ? kParamStride : WGPU_WHOLE_SIZE;
    }
    WGPUBindGroupDescriptor d = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    d.layout = g_layout;
    d.entryCount = 8;
    d.entries = e;
    return wgpuDeviceCreateBindGroup(g_device, &d);
}

void rebuild_group() {
    if (g_group) wgpuBindGroupRelease(g_group);
    g_group = make_group(g_params, g_data);
}

// The data buffer holds the arena and then the frame's shape records (g_shapes): the raster commands' parameter word 1 (where their records start)
// gets the arena's size added. Called once per submit, after the tile jobs are placed (they add to the arena).
void place_shapes() {
    const std::uint32_t base = static_cast<std::uint32_t>(g_arena.size());
    for (const std::uint32_t p : g_shape_params) g_param_records[p][1] += base;
    g_shape_params.clear();
}

// One write each: the arena, and the shape records right after it, from where draw() put them.
void upload_data() {
    wt::write(g_queue, g_data, 0, g_arena.data(), g_arena.size() * 4);
    if (g_shapes.begin) wt::write(g_queue, g_data, g_arena.size() * 4, g_shapes.words.data(), g_shapes.begin * 4);
}

void ensure_capacity() {
    bool changed = false;
    const std::uint64_t data_bytes = std::max<std::uint64_t>((g_arena.size() + g_shapes.begin) * 4, 16);
    if (data_bytes > g_data_size) {
        if (g_data) wgpuBufferRelease(g_data);
        g_data_size = std::max<std::uint64_t>(g_data_size * 2, (data_bytes + (4u << 20) - 1) & ~((4ull << 20) - 1));
        g_data = make_buffer(g_data_size, WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst);
        changed = true;
    }
    const std::uint64_t param_bytes = std::max<std::uint64_t>(g_param_records.size(), 1) * kParamStride;
    if (param_bytes > g_params_size) {
        if (g_params) wgpuBufferRelease(g_params);
        g_params_size = std::max<std::uint64_t>(g_params_size * 2, std::max<std::uint64_t>(param_bytes, 64 * 1024));
        g_params = make_buffer(g_params_size, WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst);
        changed = true;
    }
    if (changed || !g_group) rebuild_group();
}

void profile_submit() {
    constexpr std::uint32_t kMax = 2048;
    if (!g_profile_queries) {
        WGPUQuerySetDescriptor qd = WGPU_QUERY_SET_DESCRIPTOR_INIT;
        qd.type = WGPUQueryType_Timestamp;
        qd.count = kMax * 2;
        g_profile_queries = wgpuDeviceCreateQuerySet(g_device, &qd);
        g_profile_resolve = make_buffer(kMax * 16, WGPUBufferUsage_QueryResolve | WGPUBufferUsage_CopySrc);
        g_profile_read = make_buffer(kMax * 16, WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst);
    }
    ensure_capacity();
    upload_data();
    wt::write(g_queue, g_params, 0, g_param_records.data(), g_param_records.size() * kParamStride);
    WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(g_device, nullptr);
    const std::uint32_t n = static_cast<std::uint32_t>(std::min<std::size_t>(g_commands.size(), kMax));
    for (std::uint32_t i = 0; i < g_commands.size(); ++i) {
        const Command &c = g_commands[i];
        WGPUPassTimestampWrites writes = WGPU_PASS_TIMESTAMP_WRITES_INIT;
        writes.querySet = g_profile_queries;
        writes.beginningOfPassWriteIndex = i * 2;
        writes.endOfPassWriteIndex = i * 2 + 1;
        WGPUComputePassDescriptor cd = WGPU_COMPUTE_PASS_DESCRIPTOR_INIT;
        if (i < n) cd.timestampWrites = &writes;
        WGPUComputePassEncoder pass = wgpuCommandEncoderBeginComputePass(enc, &cd);
        wgpuComputePassEncoderSetPipeline(pass, pipeline_of(c));
        const std::uint32_t offset = c.param * kParamStride;
        wgpuComputePassEncoderSetBindGroup(pass, 0, g_group, 1, &offset);
        wgpuComputePassEncoderDispatchWorkgroups(pass, c.x, c.y, 1);
        wgpuComputePassEncoderEnd(pass);
        wgpuComputePassEncoderRelease(pass);
    }
    if (n) {
        wgpuCommandEncoderResolveQuerySet(enc, g_profile_queries, 0, n * 2, g_profile_resolve, 0);
        wgpuCommandEncoderCopyBufferToBuffer(enc, g_profile_resolve, 0, g_profile_read, 0, n * 16);
    }
    WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(enc, nullptr);
    wgpuQueueSubmit(g_queue, 1, &cmd);
    wgpuCommandBufferRelease(cmd);
    wgpuCommandEncoderRelease(enc);
    if (n) {
        bool done = false;
        WGPUBufferMapCallbackInfo cb = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
        cb.mode = WGPUCallbackMode_AllowProcessEvents;
        cb.callback = [](WGPUMapAsyncStatus, WGPUStringView, void *flag, void *) { *static_cast<bool *>(flag) = true; };
        cb.userdata1 = &done;
        wgpuBufferMapAsync(g_profile_read, WGPUMapMode_Read, 0, n * 16, cb);
        wait_until(done);
        const auto *t = static_cast<const std::uint64_t *>(wgpuBufferGetConstMappedRange(g_profile_read, 0, n * 16));
        std::size_t info = 0;
        for (std::uint32_t i = 0; t && i < n; ++i) {
            while (info < g_command_info.size() && g_command_info[info].index < i) ++info;
            std::fprintf(g_profile_out, "%llu %u %s %llu %u %u", static_cast<unsigned long long>(frame_count()), i, kEntryPoints[g_commands[i].pipeline],
                         static_cast<unsigned long long>(t[i * 2 + 1] > t[i * 2] ? t[i * 2 + 1] - t[i * 2] : 0), g_commands[i].x, g_commands[i].y);
            if (info < g_command_info.size() && g_command_info[info].index == i)
                for (std::uint32_t v : g_command_info[info].values) std::fprintf(g_profile_out, " %u", v);
            std::fprintf(g_profile_out, "\n");
        }
        wgpuBufferUnmap(g_profile_read);
    }
    g_commands.clear();
    g_command_info.clear();
    ++g_serial;
    g_param_records.clear();
    g_arena.clear();
    g_shapes.restart();
    g_decoded_used = 0;
    g_clut_loads = 0;
}

void bench_capture() {
    BenchSubmit &sub = g_bench.subs.emplace_back();
    sub.commands = g_commands;
    sub.params.resize(g_param_records.size() * (kParamStride / 4));
    if (!g_param_records.empty()) std::memcpy(sub.params.data(), g_param_records.data(), g_param_records.size() * kParamStride);
    sub.arena.assign(g_arena.data(), g_arena.data() + g_arena.size());
    sub.arena.insert(sub.arena.end(), g_shapes.words.data(), g_shapes.words.data() + g_shapes.begin);  // as the chip gets them (place_shapes)
    sub.info = std::move(g_bench_info);
    g_bench_info.clear();
}

// Browsers copy writes above about 1 MiB through a slower path, so large uploads go in pieces.
void submit(WGPUTextureView present_view = nullptr) {
    const auto write_blit = [] {  // present()'s parameters, when this submit does not hand them to the job
        if (!post::g_blit_pending) return;
        post::drain();
        wt::write(g_queue, g_blit_params, 0, post::g_blit, sizeof post::g_blit);
        post::g_blit_pending = false;
    };
    if (g_commands.empty() && !present_view
        && !(g_live && hw::live_pending())
        ) {
        write_blit();
        return;
    }
    post::drain();  // the last hand-over is submitted (and its picture presented) before anything of this one reaches the queue
    finish_tiles(true);  // the helper's lists go into the arena before it is measured, copied or uploaded
    place_shapes();
    if (g_bench.capturing) bench_capture();
    if (g_profile_out && !present_view && g_timestamps && frame_count() >= g_profile_from && frame_count() < g_profile_to) {
        write_blit();
        profile_submit();
        return;
    }
    ensure_capacity();
    const bool card_work = g_live && hw::live_pending();
    if (card_work) hw::live_prepare();
    if (ps2::hitch::on()) {
        for (const Command &c : g_commands) {
            ++g_hitch_now.commands[c.pipeline];
            g_hitch_now.groups[c.pipeline] += static_cast<std::uint64_t>(c.x) * c.y;
        }
        g_hitch_now.arena_bytes += (g_arena.size() + g_shapes.begin) * 4;
        ++g_hitch_now.submits;
    }
    // the submit thread takes the writes, finish, submit and present (post above); a present only with its surface texture (present())
    const bool async = post::enabled() && !g_live && !hwcache::g_on && !ps2::hitch::on() && !g_bench.configured && !hw::g_memlog
        && (!present_view || post::g_present_texture);
    post::g_presented_async = false;
    const bool post_writes = async && post::mode() == 1;
    const bool post_blit = post::g_blit_pending && post_writes && present_view;
    if (!post_blit) write_blit();  // first, as present() did before
    if (!post_writes) {
        upload_data();
        wt::write(g_queue, g_params, 0, g_param_records.data(), g_param_records.size() * kParamStride);
    }
    if (g_query_ready) {
        wt::Scope s(wt::kCopy);
        const void *mapped = wgpuBufferGetConstMappedRange(g_query_read, 0, 16);
        if (mapped) {
            std::uint64_t t[2];
            std::memcpy(t, mapped, sizeof t);
            if (t[1] > t[0]) {
                g_measured_ns += t[1] - t[0];
                ++g_measured;
            }
        }
        wgpuBufferUnmap(g_query_read);
        g_query_ready = g_query_pending = false;
    }
    const bool measure = g_timestamps && !g_query_pending
        && !card_work
        ;
    if (wt::g_on) ++wt::g_submits;
    WGPUCommandEncoder enc;
    {
        wt::Scope s(wt::kEncoder);
        enc = wgpuDeviceCreateCommandEncoder(g_device, nullptr);
    }
    WGPUPassTimestampWrites writes = WGPU_PASS_TIMESTAMP_WRITES_INIT;
    writes.querySet = g_queries;
    writes.beginningOfPassWriteIndex = 0;
    writes.endOfPassWriteIndex = 1;
    WGPUComputePassDescriptor cd = WGPU_COMPUTE_PASS_DESCRIPTOR_INIT;
    if (measure) cd.timestampWrites = &writes;
    WGPUComputePassEncoder pass;
    {
        wt::Scope s(wt::kBeginPass);
        pass = wgpuCommandEncoderBeginComputePass(enc, &cd);
    }
    Pipeline current = kPipelines;
    bool current_mip = false;
    ++g_submits;
    ++g_total_passes;
    static const unsigned skip = [] {  // PS2_GPU_SKIP=raster,snapshot: leave these kinds of work out (the picture is wrong; only to see what they cost)
        unsigned mask = 0;
        const std::string list = os::env("PS2_GPU_SKIP");
        for (int k = 0; k < kPipelines && !list.empty(); ++k)
            if (list.find(kEntryPoints[k]) != std::string::npos) mask |= 1u << k;
        return mask;
    }();
    std::uint32_t command_index = 0;
    for (const Command &c : g_commands) {
        if (card_work && hw::live_before(command_index)) {  // the card's work that goes before this command (the order of the command stream)
            pass = hw::live_encode(enc, command_index, pass);  // may go on in this compute pass or end it
            if (!pass) pass = wgpuCommandEncoderBeginComputePass(enc, &cd);
            current = kPipelines;
        }
        ++command_index;
        ++g_dispatches[c.pipeline];
        if (skip & (1u << c.pipeline)) continue;
        if (c.pipeline != current || c.mip != current_mip) {
            wt::Scope s(wt::kSetPipeline);
            wgpuComputePassEncoderSetPipeline(pass, pipeline_of(c));
            current = c.pipeline;
            current_mip = c.mip;
        }
        const std::uint32_t offset = c.param * kParamStride;
        {
            wt::Scope s(wt::kSetGroup);
            wgpuComputePassEncoderSetBindGroup(pass, 0, g_group, 1, &offset);
        }
        wt::Scope s(wt::kDispatch);
        wgpuComputePassEncoderDispatchWorkgroups(pass, c.x, c.y, 1);
    }
    if (card_work) pass = hw::live_encode(enc, ~0u, pass);
    if (pass) {
        wt::Scope s(wt::kEndPass);
        wgpuComputePassEncoderEnd(pass);
        wgpuComputePassEncoderRelease(pass);
    }
    if (measure) {
        wgpuCommandEncoderResolveQuerySet(enc, g_queries, 0, 2, g_query_resolve, 0);
        wgpuCommandEncoderCopyBufferToBuffer(enc, g_query_resolve, 0, g_query_read, 0, 16);
    }
    if (present_view) {
        wt::Scope s(wt::kRenderPass);
        WGPURenderPassColorAttachment color = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
        color.view = present_view;
        color.loadOp = WGPULoadOp_Clear;
        color.storeOp = WGPUStoreOp_Store;
        color.clearValue = WGPUColor{0.0, 0.0, 0.0, 1.0};
        WGPURenderPassDescriptor rd = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
        rd.colorAttachmentCount = 1;
        rd.colorAttachments = &color;
        WGPURenderPassEncoder rp = wgpuCommandEncoderBeginRenderPass(enc, &rd);
        const float w = static_cast<float>(g_surface_width), h = static_cast<float>(g_surface_height);
        const float aspect = g_options.widescreen ? 16.0f / 9.0f : 4.0f / 3.0f;
        const float vw = std::min(w, h * aspect), vh = vw / aspect;
        wgpuRenderPassEncoderSetViewport(rp, (w - vw) * 0.5f, (h - vh) * 0.5f, vw, vh, 0.0f, 1.0f);
        wgpuRenderPassEncoderSetPipeline(rp, g_blit);
        wgpuRenderPassEncoderSetBindGroup(rp, 0, g_blit_group, 0, nullptr);
        wgpuRenderPassEncoderDraw(rp, 3, 1, 0, 0);
        if (g_overlay.on && g_overlay.group && g_surface_width && g_surface_height) {
            // the panel's own rectangle, cut to the window (a window made smaller than the panel shows its top left part)
            const std::uint32_t ox = std::min(g_overlay.x, g_surface_width - 1), oy = std::min(g_overlay.y, g_surface_height - 1);
            const std::uint32_t ow = std::min(g_overlay.width, g_surface_width - ox), oh = std::min(g_overlay.height, g_surface_height - oy);
            wgpuRenderPassEncoderSetViewport(rp, static_cast<float>(ox), static_cast<float>(oy), static_cast<float>(ow), static_cast<float>(oh), 0.0f, 1.0f);
            wgpuRenderPassEncoderSetPipeline(rp, g_overlay.pipeline);
            wgpuRenderPassEncoderSetBindGroup(rp, 0, g_overlay.group, 0, nullptr);
            wgpuRenderPassEncoderDraw(rp, 3, 1, 0, 0);
        }
        wgpuRenderPassEncoderEnd(rp);
        wgpuRenderPassEncoderRelease(rp);
    }
    if (async) {
        post::Job &j = post::slot();
        j.enc = enc;
        j.measure = measure;
        j.data = g_data;
        j.params = g_params;
        if (present_view) j.view = present_view, j.texture = post::g_present_texture;
        j.writes = post_writes;
        j.blit = post_blit;
        if (post_blit) std::memcpy(j.blit_words, post::g_blit, sizeof post::g_blit), post::g_blit_pending = false;
        if (post_writes) {
        if (wt::g_on) wt::g_bytes += (g_arena.size() + g_shapes.begin) * 4 + g_param_records.size() * kParamStride;
        // the frame's words go with the job and the drawing thread goes on with the job's emptied ones from last time
        std::swap(j.arena, g_arena);
        std::swap(j.records, g_param_records);
        j.shape_words = g_shapes.begin;
        if (g_shapes.begin) {
            const std::size_t open = g_shapes.size();  // the open batch's records stay here, at the front (as Shapes::restart)
            std::swap(j.shapes, g_shapes.words);
            g_shapes.words.used = 0;
            if (open) g_shapes.words.append(j.shapes.data() + j.shape_words, open);
            g_shapes.begin = 0;
        }
        }
        if (measure) g_query_pending = true;
        post::go();
        post::g_presented_async = present_view != nullptr;
    } else {
    const std::uint64_t t_finish = g_live ? hw::live_clock() : 0;  // PS2_GPU_HW_TIME=1 times wgpu's finish and submit
    WGPUCommandBuffer cmd;
    {
        wt::Scope s(wt::kFinish);
        cmd = wgpuCommandEncoderFinish(enc, nullptr);
    }
    const std::uint64_t t_submit = g_live ? hw::live_clock() : 0;
    {
        wt::Scope s(wt::kSubmit);
        wgpuQueueSubmit(g_queue, 1, &cmd);
    }
    if (g_live) hw::live_wgpu_time(card_work, t_finish, t_submit, hw::live_clock());
    {
        wt::Scope s(wt::kSubmit);  // the releases belong to the submit's cost (wgpu frees the recorded commands here)
        wgpuCommandBufferRelease(cmd);
        wgpuCommandEncoderRelease(enc);
    }
    if (wt::g_on) --wt::g_calls[wt::kSubmit];
    if (g_live) hw::live_submitted();
    hw::memlog_tick(g_instance, g_queue, frame_count());  // PS2_GPU_HW_MEMLOG=<n>
    if (measure) {
        wt::Scope s(wt::kCopy);
        g_query_pending = true;
        WGPUBufferMapCallbackInfo cb = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
        cb.mode = WGPUCallbackMode_AllowProcessEvents;
        cb.callback = query_mapped;
        wgpuBufferMapAsync(g_query_read, WGPUMapMode_Read, 0, 16, cb);
    }
    {
        wt::Scope s(wt::kPoll);
#ifdef __EMSCRIPTEN__
        wgpuInstanceProcessEvents(g_instance);
#else
        wgpuDevicePoll(g_device, false, nullptr);
#endif
    }
    }
    g_commands.clear();
    g_command_info.clear();
    ++g_serial;
    g_param_records.clear();
    g_arena.clear();
    g_shapes.restart();
    g_decoded_used = 0;
    g_clut_loads = 0;
}

bool readback_log_enabled() {
    static const bool enabled = std::getenv("PS2_READBACK_LOG") != nullptr;
    return enabled;
}

bool log_slow_readback(double ms) {
    if (ms < 10.0) return false;
    static std::uint64_t slow_reads = 0;
    ++slow_reads;
    return slow_reads <= 20 || slow_reads % 100 == 0;
}

void read_back(WGPUBuffer source, std::uint64_t bytes, void *out, std::uint64_t offset = 0) {
    post::drain();
    const bool diagnose = readback_log_enabled();
    const auto started = diagnose ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(g_device, nullptr);
    wgpuCommandEncoderCopyBufferToBuffer(enc, source, offset, g_readback, 0, bytes);
    WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(enc, nullptr);
    wgpuQueueSubmit(g_queue, 1, &cmd);
    wgpuCommandBufferRelease(cmd);
    wgpuCommandEncoderRelease(enc);
    bool done = false;
    WGPUBufferMapCallbackInfo cb = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
    cb.mode = WGPUCallbackMode_AllowProcessEvents;
    cb.callback = [](WGPUMapAsyncStatus status, WGPUStringView, void *flag, void *) {
        if (status != WGPUMapAsyncStatus_Success) std::fprintf(stderr, "[gpu] map failed %d\n", static_cast<int>(status));
        *static_cast<bool *>(flag) = true;
    };
    const auto submitted = diagnose ? std::chrono::steady_clock::now() : started;
    cb.userdata1 = &done;
    wt::Scope s(wt::kWait);
    wgpuBufferMapAsync(g_readback, WGPUMapMode_Read, 0, bytes, cb);
    wait_until(done);
    const auto ready = diagnose ? std::chrono::steady_clock::now() : started;
    const void *mapped = wgpuBufferGetConstMappedRange(g_readback, 0, bytes);
    if (mapped) std::memcpy(out, mapped, bytes);
    wgpuBufferUnmap(g_readback);
    if (diagnose && log_slow_readback(std::chrono::duration<double, std::milli>(ready - started).count())) std::fprintf(stderr, "[readback] sync frame %llu bytes %llu: encode/submit %.3f ms, map-wait %.3f ms, copy %.3f ms\n",
        static_cast<unsigned long long>(frame_count()), static_cast<unsigned long long>(bytes),
        std::chrono::duration<double, std::milli>(submitted - started).count(),
        std::chrono::duration<double, std::milli>(ready - submitted).count(),
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - ready).count());
}

void wait_gpu() {
    post::drain();
    bool done = false;
    WGPUQueueWorkDoneCallbackInfo cb = WGPU_QUEUE_WORK_DONE_CALLBACK_INFO_INIT;
    cb.mode = WGPUCallbackMode_AllowProcessEvents;
    cb.callback = [](WGPUQueueWorkDoneStatus, WGPUStringView, void *flag, void *) { *static_cast<bool *>(flag) = true; };
    cb.userdata1 = &done;
    wgpuQueueOnSubmittedWorkDone(g_queue, cb);
    wait_until(done);
}

void bench_start() {
    const std::uint64_t bytes = static_cast<std::uint64_t>(kMemorySize) * g_scale * g_scale;
    g_bench.vram_copy = make_buffer(bytes, WGPUBufferUsage_CopyDst | WGPUBufferUsage_CopySrc);
    WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(g_device, nullptr);
    wgpuCommandEncoderCopyBufferToBuffer(enc, g_vram, 0, g_bench.vram_copy, 0, bytes);
    WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(enc, nullptr);
    wgpuQueueSubmit(g_queue, 1, &cmd);
    wgpuCommandBufferRelease(cmd);
    wgpuCommandEncoderRelease(enc);
    g_bench.capturing = true;
    if (hw::enabled()) hw::capture_begin();
    g_bench.subs.clear();
    g_bench_info.clear();
    std::fprintf(stderr, "[bench] capturing the chip work of vblank %llu (game frame %llu)\n", static_cast<unsigned long long>(g_bench.vblank),
                 static_cast<unsigned long long>(frame_count()));
}

struct BenchBuffers {
    WGPUBuffer params = nullptr, data = nullptr;
    WGPUBindGroup group = nullptr;
};

// Per submit and command: 1 for a drawing run of sprites only that spans a good part of the screen (the glow, blur and copy passes of lane glow), else 0.
std::vector<std::vector<char>> g_bench_glow;

// Encodes `reps` copies of the frame (restore memory, then every captured submit as one compute pass) minus the kinds of work in `skip`, submits them all at once and
// returns the milliseconds per copy.
double bench_time(const std::vector<BenchBuffers> &bufs, unsigned skip, unsigned reps, bool restore, int size_filter = 0) {
    const std::uint64_t bytes = static_cast<std::uint64_t>(kMemorySize) * g_scale * g_scale;
    std::vector<WGPUCommandBuffer> cmds;
    for (unsigned r = 0; r < reps; ++r) {
        WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(g_device, nullptr);
        if (restore) wgpuCommandEncoderCopyBufferToBuffer(enc, g_bench.vram_copy, 0, g_vram, 0, bytes);
        for (std::size_t i = 0; i < g_bench.subs.size(); ++i) {
            WGPUComputePassEncoder pass = wgpuCommandEncoderBeginComputePass(enc, nullptr);
            Pipeline current = kPipelines;
            bool current_mip = false;
            for (std::size_t n = 0; n < g_bench.subs[i].commands.size(); ++n) {
                const Command &c = g_bench.subs[i].commands[n];
                if (skip & (1u << c.pipeline)) continue;
                if (size_filter && size_filter <= 2 && c.pipeline == kRaster && ((c.x * c.y <= 8u) == (size_filter == 1))) continue;  // 1: leave out the small runs (8 tiles or fewer), 2: leave out the big ones
                if (size_filter >= 3 && c.pipeline == kRaster && (g_bench_glow[i][n] != 0) == (size_filter == 3)) continue;  // 3: leave out the full-screen sprite passes (lane glow), 4: leave out all the others
                if (c.pipeline != current || c.mip != current_mip) {
                    wgpuComputePassEncoderSetPipeline(pass, pipeline_of(c));
                    current = c.pipeline;
                    current_mip = c.mip;
                }
                const std::uint32_t offset = c.param * kParamStride;
                wgpuComputePassEncoderSetBindGroup(pass, 0, bufs[i].group, 1, &offset);
                wgpuComputePassEncoderDispatchWorkgroups(pass, c.x, c.y, 1);
            }
            wgpuComputePassEncoderEnd(pass);
            wgpuComputePassEncoderRelease(pass);
        }
        cmds.push_back(wgpuCommandEncoderFinish(enc, nullptr));
        wgpuCommandEncoderRelease(enc);
    }
    wait_gpu();
    const auto t0 = std::chrono::steady_clock::now();
    wgpuQueueSubmit(g_queue, cmds.size(), cmds.data());
    wait_gpu();
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    for (WGPUCommandBuffer c : cmds) wgpuCommandBufferRelease(c);
    return ms / reps;
}

// `count` copies of one command, alone in a pass, in one submit: milliseconds for the lot.
double bench_one(const BenchBuffers &buf, const Command &c, unsigned count) {
    WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(g_device, nullptr);
    WGPUComputePassEncoder pass = wgpuCommandEncoderBeginComputePass(enc, nullptr);
    wgpuComputePassEncoderSetPipeline(pass, pipeline_of(c));
    const std::uint32_t offset = c.param * kParamStride;
    for (unsigned i = 0; i < count; ++i) {
        wgpuComputePassEncoderSetBindGroup(pass, 0, buf.group, 1, &offset);
        wgpuComputePassEncoderDispatchWorkgroups(pass, c.x, c.y, 1);
    }
    wgpuComputePassEncoderEnd(pass);
    wgpuComputePassEncoderRelease(pass);
    WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(enc, nullptr);
    wgpuCommandEncoderRelease(enc);
    wait_gpu();
    const auto t0 = std::chrono::steady_clock::now();
    wgpuQueueSubmit(g_queue, 1, &cmd);
    wait_gpu();
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    wgpuCommandBufferRelease(cmd);
    return ms;
}

std::uint64_t fnv_hash(const std::vector<std::uint32_t> &words, std::size_t first, std::size_t count) {
    std::uint64_t h = 1469598103934665603ull;
    for (std::size_t i = first; i < first + count && i < words.size(); ++i) {
        h ^= words[i];
        h *= 1099511628211ull;
    }
    return h;
}

// PS2_GPU_BENCH_SHOT=<prefix> (with PS2_GPU_HW=1): writes the picture in the frame buffer after a replay to <prefix>-<tag>.ppm. PS2_GPU_BENCH_SHOT_FB=fbp,fbw,width,height picks the buffer (256,8,512,448).
void bench_shot(const char *tag) {
    const std::string prefix = os::env("PS2_GPU_BENCH_SHOT");
    if (prefix.empty()) return;
    unsigned fbp = 256, fbw = 8, w = 512, h = 448;
    if (const std::string spec = os::env("PS2_GPU_BENCH_SHOT_FB"); !spec.empty()) std::sscanf(spec.c_str(), "%u,%u,%u,%u", &fbp, &fbw, &w, &h);
    std::vector<std::uint32_t> rgba;
    std::uint32_t pw = 0, ph = 0;
    display(DisplaySource{fbp, fbw, PSMCT32, 0, 0, w, h, false}, rgba, pw, ph);
    if (std::FILE *f = os::open((prefix + "-" + tag + ".ppm").c_str(), "wb")) {
        std::fprintf(f, "P6\n%u %u\n255\n", pw, ph);
        for (std::uint32_t c : rgba) {
            const unsigned char px[3] = {static_cast<unsigned char>(c), static_cast<unsigned char>(c >> 8), static_cast<unsigned char>(c >> 16)};
            std::fwrite(px, 1, 3, f);
        }
        std::fclose(f);
    }
}

std::uint64_t bytes_of_vram() { return static_cast<std::uint64_t>(kMemorySize) * g_scale * g_scale; }

// PS2_GPU_HW_CLASS=1 (with PS2_GPU_BENCH): sorts every shape of the captured frame into the classes of hw::shape_class
// (what a render pipeline could draw, and why not), counts shapes, covered pixels and drawing runs per class, and measures what the compute path
// spends on each class: the replay is timed again with that class's shapes made invisible (their screen box is emptied, so the drawing program
// skips them at the first test; the walk through the tile lists stays). The difference to the unchanged replay is the class's chip time.
void bench_classes(const std::vector<BenchBuffers> &bufs, unsigned reps) {
    const int kinds = hw::class_count();
    struct Total {
        std::uint64_t shapes = 0, runs = 0, only = 0;
        double area = 0, ms = 0, ms_median = 0;
    };
    std::vector<Total> tot(kinds);
    std::vector<std::vector<std::vector<std::uint32_t>>> where(g_bench.subs.size(), std::vector<std::vector<std::uint32_t>>(kinds));  // [submit][class] -> word offset of each shape's record
    std::uint64_t runs = 0, hw_runs = 0, shapes = 0;
    double area_all = 0;
    for (std::size_t i = 0; i < g_bench.subs.size(); ++i) {
        const BenchSubmit &sub = g_bench.subs[i];
        std::size_t info = 0;
        for (std::size_t n = 0; n < sub.commands.size(); ++n) {
            const Command &c = sub.commands[n];
            if (c.pipeline != kRaster) continue;
            while (info < sub.info.size() && sub.info[info].index < n) ++info;
            if (info >= sub.info.size() || sub.info[info].index != n) continue;
            const BenchInfo &bi = sub.info[info];
            const std::uint32_t *param = sub.params.data() + static_cast<std::size_t>(c.param) * (kParamStride / 4);
            const std::uint32_t prim_offset = param[1], setup_offset = param[2];
            std::vector<bool> seen(kinds, false);
            int classes_in_run = 0;
            for (std::uint32_t j = 0; j < bi.prims; ++j) {
                const std::uint32_t at = prim_offset + j * kPrimWords;
                const std::uint32_t *rec = sub.arena.data() + at;
                const int k = hw::shape_class(rec, sub.arena.data() + setup_offset + rec[0] * kSetupWords);
                const double bw = static_cast<double>((rec[3] & 0xFFFFu) - (rec[2] & 0xFFFFu) + 1), bh = static_cast<double>((rec[3] >> 16) - (rec[2] >> 16) + 1);
                const double area = (rec[1] & kKindMask) == 0 ? std::fabs(0.5 / static_cast<double>(u2f(rec[10]))) : bw * bh;
                ++tot[k].shapes;
                tot[k].area += area;
                area_all += area;
                ++shapes;
                where[i][k].push_back(at);
                if (!seen[k]) {
                    seen[k] = true;
                    ++tot[k].runs;
                    ++classes_in_run;
                }
            }
            ++runs;
            bool all_hw = true;
            for (int k = 0; k < kinds; ++k) all_hw = all_hw && (!seen[k] || hw::class_is_hardware(k));
            if (all_hw) ++hw_runs;
            if (classes_in_run == 1)
                for (int k = 0; k < kinds; ++k)
                    if (seen[k]) ++tot[k].only;
        }
    }
    const auto timed = [&](double &median) {
        std::vector<double> t;
        for (int trial = 0; trial < 5; ++trial) t.push_back(bench_time(bufs, 0, reps, true));
        std::sort(t.begin(), t.end());
        median = t[t.size() / 2];
        return t.front();
    };
    const auto upload = [&](std::size_t i, const std::vector<std::uint32_t> &arena) {
        if (!arena.empty()) wgpuQueueWriteBuffer(g_queue, bufs[i].data, 0, arena.data(), arena.size() * 4);
    };
    const auto hide = [&](std::vector<std::vector<std::uint32_t>> &copies, std::size_t i, const std::vector<std::uint32_t> &offsets) {
        for (std::uint32_t at : offsets) copies[i][at + 2] = 0x7FFF7FFFu, copies[i][at + 3] = 0;  // the box test of the drawing program fails everywhere
    };
    double base_median = 0;
    bench_time(bufs, 0, 3, true);
    const double base = timed(base_median);
    std::fprintf(stderr, "[class] the unchanged frame: best %.3f ms, median %.3f ms per frame (5 trials of %u replays)\n", base, base_median, reps);
    std::vector<std::vector<std::uint32_t>> copies;
    for (const BenchSubmit &sub : g_bench.subs) copies.push_back(sub.arena);
    const auto run_without = [&](const std::vector<int> &which, double &median) {
        for (std::size_t i = 0; i < copies.size(); ++i) {
            copies[i] = g_bench.subs[i].arena;
            for (int k : which) hide(copies, i, where[i][k]);
            upload(i, copies[i]);
        }
        wait_gpu();
        const double best = timed(median);
        for (std::size_t i = 0; i < copies.size(); ++i) upload(i, g_bench.subs[i].arena);
        wait_gpu();
        return best;
    };
    {  // control: the arena uploaded again with nothing hidden must time like the unchanged frame, or the differences below are an artefact of the upload
        double median = 0;
        const double control = run_without({}, median);
        std::fprintf(stderr, "[class] control (arena uploaded again, nothing hidden): best %.3f / median %.3f ms\n", control, median);
    }
    for (int k = 0; k < kinds; ++k) {
        if (!tot[k].shapes) continue;
        double median = 0;
        const double without = run_without({k}, median);
        tot[k].ms = base - without;
        tot[k].ms_median = base_median - median;
    }
    std::vector<int> tricky, friendly;
    for (int k = 0; k < kinds; ++k)
        if (tot[k].shapes) (hw::class_is_hardware(k) ? friendly : tricky).push_back(k);
    double only_hw = 0, only_hw_median = 0, only_tricky = 0, only_tricky_median = 0;
    if (!tricky.empty()) only_hw = run_without(tricky, only_hw_median);
    if (!friendly.empty()) only_tricky = run_without(friendly, only_tricky_median);
    std::fprintf(stderr, "[class] %zu drawing runs, %llu shapes, %.0f covered pixels (before the depth test); runs with only card-friendly shapes: %llu (%.1f%%)\n", static_cast<std::size_t>(runs), static_cast<unsigned long long>(shapes), area_all, static_cast<unsigned long long>(hw_runs),
                 100.0 * static_cast<double>(hw_runs) / static_cast<double>(std::max<std::uint64_t>(1, runs)));
    for (int k = 0; k < kinds; ++k) {
        if (!tot[k].shapes) continue;
        std::fprintf(stderr, "[class] %-62s runs %4llu (only this: %4llu) shapes %6llu (%5.1f%%) pixels %10.0f (%5.1f%%) chip ms %6.3f best / %6.3f median\n", hw::class_name(k), static_cast<unsigned long long>(tot[k].runs), static_cast<unsigned long long>(tot[k].only),
                     static_cast<unsigned long long>(tot[k].shapes), 100.0 * static_cast<double>(tot[k].shapes) / static_cast<double>(std::max<std::uint64_t>(1, shapes)), tot[k].area, 100.0 * tot[k].area / std::max(1.0, area_all), tot[k].ms, tot[k].ms_median);
    }
    std::fprintf(stderr, "[class] compute path, only the card-friendly shapes (the rest hidden): best %.3f / median %.3f ms; only the other shapes: best %.3f / median %.3f ms; the frame: best %.3f / median %.3f ms\n", only_hw, only_hw_median, only_tricky, only_tricky_median, base, base_median);
}

// The card path's replay of the captured frame; `vram` is the memory after the painter's replay of the same frame.
void bench_card(const std::vector<std::uint32_t> &vram, std::vector<BenchBuffers> &bufs, unsigned reps, void (*shot)(const char *)) {
    hw::Context ctx{g_device, g_queue, g_vram, g_tables, g_clut, g_scratch, g_cache, g_layout, {}, kUpload, kLoadClut, kRaster, kDecode, kSnapshot, g_scale, g_bench.vram_copy, bytes_of_vram(), kGsShader};
    ctx.pipelines.assign(g_pipelines, g_pipelines + kPipelines);
    ctx.read_vram = [](std::vector<std::uint32_t> &out) {
        out.resize(static_cast<std::size_t>(kMemorySize / 4) * g_scale * g_scale);
        read_back(g_vram, out.size() * 4, out.data());
    };
    ctx.read_before = [](std::vector<std::uint32_t> &out) {
        out.resize(static_cast<std::size_t>(kMemorySize / 4) * g_scale * g_scale);
        read_back(g_bench.vram_copy, out.size() * 4, out.data());
    };
    ctx.painter_vram = &vram;
    std::vector<hw::Sub> subs;
    for (std::size_t i = 0; i < g_bench.subs.size(); ++i) {
        hw::Sub sub;
        for (const Command &c : g_bench.subs[i].commands) sub.commands.push_back({static_cast<std::uint32_t>(c.pipeline), c.x, c.y, c.param});
        sub.group = bufs[i].group;
        subs.push_back(std::move(sub));
    }
    hw::run(ctx, subs, reps, [] { wait_gpu(); }, shot);
}

void bench_run() {
    g_bench.capturing = false;
    g_bench.done = true;
    if (g_bench.subs.empty()) {
        std::fprintf(stderr, "[bench] no chip work in that frame\n");
        if (hw::enabled() && hw::diff_enabled()) {  // arm for the next frame
            if (g_bench.vram_copy) wgpuBufferRelease(g_bench.vram_copy);
            g_bench.vram_copy = nullptr;
            g_bench.done = false;
            g_bench.vblank = g_vblanks + 2;
        }
        return;
    }
    wait_gpu();
    std::vector<BenchBuffers> bufs;
    std::size_t total_commands = 0;
    std::uint64_t kind_count[kPipelines] = {};
    for (const BenchSubmit &sub : g_bench.subs) {
        BenchBuffers b;
        b.params = make_buffer(std::max<std::uint64_t>(sub.params.size() * 4, kParamStride), WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst);
        b.data = make_buffer(std::max<std::uint64_t>(sub.arena.size() * 4, 16), WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst);
        if (!sub.params.empty()) wgpuQueueWriteBuffer(g_queue, b.params, 0, sub.params.data(), sub.params.size() * 4);
        if (!sub.arena.empty()) wgpuQueueWriteBuffer(g_queue, b.data, 0, sub.arena.data(), sub.arena.size() * 4);
        b.group = make_group(b.params, b.data);
        bufs.push_back(b);
        total_commands += sub.commands.size();
        for (const Command &c : sub.commands) ++kind_count[c.pipeline];
    }
    wait_gpu();
    if (hw::enabled() && hw::diff_enabled()) {  // tests/hwalpha.cpp: the painter once, the card path once, then arm the bench for the next frame
        bench_time(bufs, 0, 1, true);
        std::vector<std::uint32_t> painter(static_cast<std::size_t>(kMemorySize / 4) * g_scale * g_scale);
        read_back(g_vram, painter.size() * 4, painter.data());
        bench_card(painter, bufs, 1, nullptr);
        for (BenchBuffers &b : bufs) {
            wgpuBindGroupRelease(b.group);
            wgpuBufferRelease(b.params);
            wgpuBufferRelease(b.data);
        }
        wgpuBufferRelease(g_bench.vram_copy);
        g_bench.vram_copy = nullptr;
        g_bench.done = false;
        g_bench.vblank = g_vblanks + 2;
        return;
    }
    std::fprintf(stderr, "[bench] %zu submits, %zu commands:", g_bench.subs.size(), total_commands);
    for (int k = 0; k < kPipelines; ++k) std::fprintf(stderr, " %s %llu", kEntryPoints[k], static_cast<unsigned long long>(kind_count[k]));
    std::fprintf(stderr, "\n");
    const unsigned reps = static_cast<unsigned>(g_bench.reps);
    const unsigned all = (1u << kPipelines) - 1;
    struct Variant {
        const char *name;
        unsigned skip;
        bool restore;
        int size_filter = 0;
    };
    g_bench_glow.assign(g_bench.subs.size(), {});
    for (std::size_t i = 0; i < g_bench.subs.size(); ++i) {
        g_bench_glow[i].assign(g_bench.subs[i].commands.size(), 0);
        for (const BenchInfo &b : g_bench.subs[i].info)
            if (b.index < g_bench_glow[i].size() && b.prims && b.kinds[1] == b.prims && b.tiles >= 400) g_bench_glow[i][b.index] = 1;
    }
    const Variant variants[] = {{"restore only", all, true},
                                {"everything", 0, true},
                                {"without raster", 1u << kRaster, true},
                                {"without decode", 1u << kDecode, true},
                                {"without snapshot", 1u << kSnapshot, true},
                                {"without decode and snapshot", (1u << kDecode) | (1u << kSnapshot), true},
                                {"without raster and snapshot", (1u << kRaster) | (1u << kSnapshot), true},
                                {"raster only", all & ~(1u << kRaster), true},
                                {"snapshot only", all & ~(1u << kSnapshot), true},
                                {"decode only", all & ~(1u << kDecode), true},
                                {"upload and clut only", all & ~((1u << kUpload) | (1u << kLoadClut)), true},
                                {"upload only", all & ~(1u << kUpload), true},
                                {"clut only", all & ~(1u << kLoadClut), true},
                                {"copy and display only", all & ~((1u << kCopyRead) | (1u << kCopyWrite) | (1u << kDisplay)), true},
                                {"raster, big runs only (over 8 tiles)", all & ~(1u << kRaster), true, 1},
                                {"raster, small runs only (8 tiles or fewer)", all & ~(1u << kRaster), true, 2},
                                {"everything but the sprite-only runs", 0, true, 3},
                                {"everything but the sprite-only runs, raster only", all & ~(1u << kRaster), true, 3},
                                {"raster, sprite-only runs (glow) only", all & ~(1u << kRaster), true, 4}};
    bench_time(bufs, 0, 3, true);  // warm up
    for (const Variant &v : variants) {
        double best = 1e30;
        for (int trial = 0; trial < 3; ++trial) best = std::min(best, bench_time(bufs, v.skip, reps, v.restore, v.size_filter));
        std::fprintf(stderr, "[bench] %-32s %8.3f ms per frame\n", v.name, best);
    }
    if (hw::class_enabled()) bench_classes(bufs, reps);
    // what one dispatch costs when it does next to nothing: 64 words copied by a snapshot
    {
        BenchBuffers micro;
        micro.params = make_buffer(kParamStride, WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst);
        micro.data = make_buffer(16, WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst);
        const std::uint32_t record[kParamStride / 4] = {0, 64};
        wgpuQueueWriteBuffer(g_queue, micro.params, 0, record, sizeof record);
        micro.group = make_group(micro.params, micro.data);
        const Command tiny{kSnapshot, 1, 1, 0};
        double best = 1e30;
        for (int trial = 0; trial < 5; ++trial) best = std::min(best, bench_one(micro, tiny, 400));
        std::fprintf(stderr, "[bench] tiny dispatch: %.2f us each (400 in a row)\n", best * 1000.0 / 400.0);
        const double one = bench_one(micro, tiny, 1);
        std::fprintf(stderr, "[bench] submit and wait for one tiny dispatch: %.0f us\n", one * 1000.0);
        // per drawing run: the same run 8 times in a row, minus the same number of tiny dispatches
        const unsigned count = 8;
        double base = 1e30;
        for (int trial = 0; trial < 5; ++trial) base = std::min(base, bench_one(micro, tiny, count));
        if (os::env("PS2_GPU_BENCH_DETAIL") != "0")
            for (std::size_t i = 0; i < g_bench.subs.size(); ++i) {
                std::size_t info = 0;
                for (std::size_t n = 0; n < g_bench.subs[i].commands.size(); ++n) {
                    const Command &c = g_bench.subs[i].commands[n];
                    if (c.pipeline != kRaster && c.pipeline != kSnapshot) continue;
                    double t = 1e30;
                    for (int trial = 0; trial < 3; ++trial) t = std::min(t, bench_one(bufs[i], c, count));
                    const double us = std::max(0.0, t - base) * 1000.0 / count;
                    if (c.pipeline == kSnapshot) {
                        std::fprintf(stderr, "[bench-run] %zu.%zu snapshot groups %u us %.1f\n", i, n, c.x * c.y, us);
                        continue;
                    }
                    while (info < g_bench.subs[i].info.size() && g_bench.subs[i].info[info].index < n) ++info;
                    if (info >= g_bench.subs[i].info.size() || g_bench.subs[i].info[info].index != n) continue;
                    const BenchInfo &b = g_bench.subs[i].info[info];
                    std::fprintf(stderr,
                                 "[bench-run] %zu.%zu raster us %.1f tiles %u prims %u entries %u snapshot %u tri %u sprite %u line %u point %u tex %u blend %u ztest %u zwrite %u atest %u bilinear %u fog %u date %u fpsm %u tpsm %u setups %u area %.0f bbox %.0f\n",
                                 i, n, us, b.tiles, b.prims, b.entries, b.snapshot, b.kinds[0], b.kinds[1], b.kinds[2], b.kinds[3], b.textured, b.blended, b.ztest, b.zwrite,
                                 b.atest, b.bilinear, b.fog, b.date, b.fpsm, b.tpsm, b.setups, b.area, b.bbox);
                }
            }
        wgpuBindGroupRelease(micro.group);
        wgpuBufferRelease(micro.params);
        wgpuBufferRelease(micro.data);
    }
    // the memory after one more copy of the frame: a fingerprint to compare builds (and the whole memory in a file if asked for)
    bench_time(bufs, 0, 1, true);
    if (hw::enabled()) bench_shot("compute");
    std::vector<std::uint32_t> vram(static_cast<std::size_t>(kMemorySize / 4) * g_scale * g_scale);
    read_back(g_vram, vram.size() * 4, vram.data());
    std::fprintf(stderr, "[bench] memory after the frame: hash %016llx\n", static_cast<unsigned long long>(fnv_hash(vram, 0, vram.size())));
    if (const std::string path = os::env("PS2_GPU_BENCH_DUMP"); !path.empty()) {
        if (std::FILE *f = os::open(path.c_str(), "wb")) {
            std::fwrite(vram.data(), 4, vram.size(), f);
            std::fclose(f);
        }
    }
    if (hw::enabled()) {
        hw::survey_print();
        bench_card(vram, bufs, reps, bench_shot);
    }
    for (BenchBuffers &b : bufs) {
        wgpuBindGroupRelease(b.group);
        wgpuBufferRelease(b.params);
        wgpuBufferRelease(b.data);
    }
}

// The highest mip level the mipmap words `mip` (make_mip) let a pixel read: 0 when only the base texture is read. With LCM = 1 the level of
// detail is K alone, so at most the level after floor(K) is reached (the *_MIPMAP_LINEAR blend); K <= 0 magnifies (level 0).
std::uint32_t mip_top_of(const std::uint32_t *mip) {
    if (!mip[0]) return 0;
    const std::uint32_t mxl = (mip[0] >> 1) & 7u, mmin = (mip[0] >> 5) & 7u;
    if (mxl == 0 || mmin < 2) return 0;
    if (mip[0] & 1u) {
        const float lod = static_cast<float>(static_cast<std::int32_t>(((mip[0] >> 16) & 0xFFFu) << 20) >> 20) / 16.0f;
        if (lod <= 0.0f) return 0;
        return std::min(static_cast<std::uint32_t>(std::floor(lod)) + 1u, mxl);
    }
    return mxl;
}

// The shader's picture of a memory format: base address, pages per row, swizzle table, packed layout bits, pixel mask (what fmt_of in gs.wgsl used to compute per tile pass).
void fmt_words(std::uint32_t psm, std::uint32_t bp, std::uint32_t bw, std::uint32_t *out) {
    constexpr std::uint32_t kOff32 = 0, kOffZ32 = 2048, kOff16 = 4096, kOff16S = 8192, kOffZ16 = 12288, kOffZ16S = 16384, kOff8 = 20480, kOff4 = 28672;
    std::uint32_t pws = 6, phs = 5, table = kOff32, amask = 0, nbit = 0, fixed = 0, mask = 0xFFFFFFFFu;
    switch (psm) {
    case 0x30: table = kOffZ32; break;
    case 0x31: table = kOffZ32; mask = 0xFFFFFFu; break;
    case 0x01: mask = 0xFFFFFFu; break;
    case 0x02: phs = 6; table = kOff16; amask = 2; mask = 0xFFFFu; break;
    case 0x0A: phs = 6; table = kOff16S; amask = 2; mask = 0xFFFFu; break;
    case 0x32: phs = 6; table = kOffZ16; amask = 2; mask = 0xFFFFu; break;
    case 0x3A: phs = 6; table = kOffZ16S; amask = 2; mask = 0xFFFFu; break;
    case 0x13: pws = 7; phs = 6; table = kOff8; amask = 3; mask = 0xFFu; break;
    case 0x14: pws = 7; phs = 7; table = kOff4; amask = 3; nbit = 1; mask = 0xFu; break;
    case 0x1B: fixed = 24; mask = 0xFFu; break;
    case 0x24: fixed = 24; mask = 0xFu; break;
    case 0x2C: fixed = 28; mask = 0xFu; break;
    default: break;
    }
    out[0] = bp * 256u;
    out[1] = std::max(1u, (bw * 64u) >> pws);
    out[2] = table;
    out[3] = pws | (phs << 4) | (amask << 8) | (nbit << 10) | (fixed << 11);
    out[4] = mask;
}

std::uint32_t make_setup(const State &st, std::uint64_t attr, std::uint32_t *w) {
    const Context &c = st.ctx[(attr >> 9) & 1u];
    std::uint32_t flags = 0;
    if (attr & 0x8u) flags |= 1u;
    if (attr & 0x10u) flags |= 2u;
    if (attr & 0x20u) flags |= 4u;
    if (attr & 0x40u) flags |= 8u;
    if (attr & 0x100u) flags |= 16u;
    if ((c.zbuf >> 32) & 1u) flags |= 32u;
    if (c.test & 1u) flags |= 64u;
    if ((c.test >> 14) & 1u) flags |= 128u;
    if ((c.test >> 15) & 1u) flags |= 256u;
    if ((c.test >> 16) & 1u) flags |= 512u;
    if (st.pabe & 1u) flags |= 1024u;
    if (st.colclamp & 1u) flags |= 2048u;
    if (c.fba & 1u) flags |= 4096u;
    if ((c.tex0 >> 34) & 1u) flags |= 8192u;
    if ((c.tex1 >> 5) & 1u) flags |= 16384u;
    if ((st.texa >> 15) & 1u) flags |= 32768u;
    const std::uint32_t zpsm = 0x30u | (static_cast<std::uint32_t>(c.zbuf >> 24) & 0xFu);
    w[0] = flags;
    w[1] = (static_cast<std::uint32_t>(c.frame) & 0x1FFu) * 32u;
    w[2] = static_cast<std::uint32_t>(c.frame >> 16) & 0x3Fu;
    w[3] = static_cast<std::uint32_t>(c.frame >> 24) & 0x3Fu;
    w[4] = static_cast<std::uint32_t>(c.frame >> 32);
    w[5] = (static_cast<std::uint32_t>(c.zbuf) & 0x1FFu) * 32u;
    w[6] = zpsm;
    w[7] = (static_cast<std::uint32_t>(c.test >> 1) & 7u) | ((static_cast<std::uint32_t>(c.test >> 12) & 3u) << 4) |
           ((static_cast<std::uint32_t>(c.test >> 17) & 3u) << 8) | ((static_cast<std::uint32_t>(c.alpha) & 0xFFu) << 12) |
           ((static_cast<std::uint32_t>(c.tex0 >> 35) & 3u) << 20) | ((static_cast<std::uint32_t>(c.clamp) & 0xFu) << 24) |
           ((static_cast<std::uint32_t>(st.scanmsk) & 3u) << 28);
    w[8] = static_cast<std::uint32_t>(c.test >> 4) & 0xFFu;
    w[9] = static_cast<std::uint32_t>(c.alpha >> 32) & 0xFFu;
    w[10] = (static_cast<std::uint32_t>(c.scissor) & 0x7FFu) | ((static_cast<std::uint32_t>(c.scissor >> 16) & 0x7FFu) << 16);
    w[11] = (static_cast<std::uint32_t>(c.scissor >> 32) & 0x7FFu) | ((static_cast<std::uint32_t>(c.scissor >> 48) & 0x7FFu) << 16);
    w[12] = static_cast<std::uint32_t>(c.tex0) & 0x3FFFu;
    w[13] = static_cast<std::uint32_t>(c.tex0 >> 14) & 0x3Fu;
    w[14] = static_cast<std::uint32_t>(c.tex0 >> 20) & 0x3Fu;
    w[15] = std::min<std::uint32_t>(static_cast<std::uint32_t>(c.tex0 >> 26) & 0xFu, 10u) | (std::min<std::uint32_t>(static_cast<std::uint32_t>(c.tex0 >> 30) & 0xFu, 10u) << 8);
    w[16] = static_cast<std::uint32_t>(c.tex0 >> 51) & 0xFu;
    w[17] = static_cast<std::uint32_t>(c.tex0 >> 56) & 0x1Fu;
    w[18] = (static_cast<std::uint32_t>(c.clamp >> 4) & 0x3FFu) | ((static_cast<std::uint32_t>(c.clamp >> 14) & 0x3FFu) << 16);
    w[19] = (static_cast<std::uint32_t>(c.clamp >> 24) & 0x3FFu) | ((static_cast<std::uint32_t>(c.clamp >> 34) & 0x3FFu) << 16);
    w[20] = (static_cast<std::uint32_t>(st.texa) & 0xFFu) | ((static_cast<std::uint32_t>(st.texa >> 32) & 0xFFu) << 8);
    w[21] = static_cast<std::uint32_t>(st.fogcol) & 0xFFFFFFu;
    w[22] = g_clut_slot;
    w[23] = z_max(zpsm);
    w[24] = 0xFFFFFFFFu;
    fmt_words(w[14], w[12], w[13], w + 25);
    fmt_words(w[3], w[1], w[2], w + 30);
    fmt_words(w[6], w[5], w[2], w + 35);
    return flags;
}

// The mipmap words of a setup (docs/mipmap.md), kept out of the 40 setup words: [0] is 0 when the texture
// is sampled as before (one level and the same filter for magnification and reduction), else bit 31 plus TEX1's LCM (bit 0), MXL (bits 1-3,
// at most 6), MMAG (4), MMIN (5-7), L (8-9) and K (16-27, signed 7.4); [1..6] base pointer | buffer width << 14 of levels 1 to 6 (mip_levels).
// Returns the highest level a pixel can read: 0 when only the base texture is read.
std::uint32_t make_mip(const State &st, std::uint64_t attr, std::uint32_t *m) {
    std::fill(m, m + kMipWords, 0u);
    static const bool off = std::getenv("PS2_MIPMAP") && std::getenv("PS2_MIPMAP")[0] == '0';  // PS2_MIPMAP=0: level 0 and MMAG's filter, as before (an A/B aid)
    if (!(attr & 0x10u) || off) return 0;
    const Context &c = st.ctx[(attr >> 9) & 1u];
    const std::uint32_t lcm = c.tex1 & 1u, mxl = std::min<std::uint32_t>(static_cast<std::uint32_t>(c.tex1 >> 2) & 7u, 6u), mmag = static_cast<std::uint32_t>(c.tex1 >> 5) & 1u;
    const std::uint32_t mmin = static_cast<std::uint32_t>(c.tex1 >> 6) & 7u, l = static_cast<std::uint32_t>(c.tex1 >> 19) & 3u, kraw = static_cast<std::uint32_t>(c.tex1 >> 32) & 0xFFFu;
    const bool min_linear = mmin == 1 || mmin >= 4, levels = mxl > 0 && mmin >= 2;
    if (mmag == (min_linear ? 1u : 0u) && !levels) return 0;
    m[0] = 0x80000000u | lcm | (mxl << 1) | (mmag << 4) | (mmin << 5) | (l << 8) | (kraw << 16);
    std::uint32_t bp[7], bw[7];
    mip_levels(c, bp, bw);
    for (int n = 1; n <= 6; ++n) m[n] = bp[n] | (bw[n] << 14);
    return mip_top_of(m);
}

// The bytes the mip levels 1 to `top` of a setup span (the base texture is cache.tex, the region of setup words 12 to 14).
Range mip_range(const std::uint32_t *setup, const std::uint32_t *mip, std::uint32_t top) {
    Range r;
    const std::uint32_t tw = setup[15] & 0xFFu, th = setup[15] >> 8;
    for (std::uint32_t n = 1; n <= top; ++n) {
        const std::uint32_t lw = tw > n ? tw - n : 0u, lh = th > n ? th - n : 0u;
        const int w = std::max(1 << lw, static_cast<int>((setup[18] >> 16) >> n) + 1), h = std::max(1 << lh, static_cast<int>((setup[19] >> 16) >> n) + 1);
        const Range level = region(mip[n] & 0x3FFFu, (mip[n] >> 14) & 0x3Fu, setup[14], 0, 0, w - 1, h - 1);
        r.add(level.lo, level.hi);
    }
    return r;
}

// The smallest and largest texel position (u0, v0, u1, v1, in texels, before any wrap mode) that the corners of a shape name; false when they cannot be said.
bool texel_extent(const std::uint32_t *setup, const Vertex *v, int count, std::uint32_t prim, bool fst, float &u0, float &v0, float &u1, float &v1) {
    const int tw = 1 << (setup[15] & 0xFFu), th = 1 << (setup[15] >> 8);
    u0 = v0 = 1e30f;
    u1 = v1 = -1e30f;
    for (int i = 0; i < count; ++i) {
        float u, t;
        if (fst) {
            u = static_cast<float>(v[i].u) / 16.0f;
            t = static_cast<float>(v[i].v) / 16.0f;
        } else {
            const float q = prim == 6 ? v[count - 1].q : v[i].q;
            if (!(q > 0.0f) || !(v[0].q > 0.0f) || !(v[count - 1].q > 0.0f)) return false;
            u = v[i].s / q * static_cast<float>(tw);
            t = v[i].t / q * static_cast<float>(th);
        }
        if (!std::isfinite(u) || !std::isfinite(t)) return false;
        u0 = std::min(u0, u);
        u1 = std::max(u1, u);
        v0 = std::min(v0, t);
        v1 = std::max(v1, t);
    }
    return true;
}

bool texel_rect(const std::uint32_t *setup, const Vertex *v, int count, std::uint32_t prim, bool fst, int rect[4]) {
    const int tw = 1 << (setup[15] & 0xFFu), th = 1 << (setup[15] >> 8);
    float u0, v0, u1, v1;
    if (!texel_extent(setup, v, count, prim, fst, u0, v0, u1, v1)) return false;
    const auto axis = [](std::uint32_t mode, float lo, float hi, int size, std::uint32_t clamp, int &a, int &b) {
        a = static_cast<int>(std::floor(lo)) - 1;
        b = static_cast<int>(std::ceil(hi)) + 1;
        switch (mode) {
        case 0:
            if (a < 0 || b >= size) {
                a = 0;
                b = size - 1;
            }
            return true;
        case 1:
            a = std::clamp(a, 0, size - 1);
            b = std::clamp(b, 0, size - 1);
            return true;
        case 2: {
            const int minv = static_cast<int>(clamp & 0x3FFu), maxv = static_cast<int>(clamp >> 16);
            a = std::clamp(a, minv, std::max(minv, maxv));
            b = std::clamp(b, minv, std::max(minv, maxv));
            return true;
        }
        default: return false;
        }
    };
    int ua, ub, va, vb;
    if (!axis((setup[7] >> 24) & 3u, u0, u1, tw, setup[18], ua, ub) || !axis((setup[7] >> 26) & 3u, v0, v1, th, setup[19], va, vb)) return false;
    rect[0] = std::max(ua, 0);
    rect[1] = std::max(va, 0);
    rect[2] = std::max(ub, 0);
    rect[3] = std::max(vb, 0);
    return true;
}

bool shares_frame(const std::uint32_t *setup, const Batch &b) {
    const auto wide = [](std::uint32_t psm) { return psm == PSMCT32 || psm == PSMCT24 || psm == PSMT8H || psm == PSMT4HL || psm == PSMT4HH; };
    return setup[12] == b.fbp && setup[13] == b.fbw && (setup[14] == b.fpsm || (wide(setup[14]) && wide(b.fpsm)));
}

bool drawn_under(const Batch &b, const int rect[4]) {
    const auto hits = [&](int x0, int y0, int x1, int y1) { return rect[0] <= x1 && x0 <= rect[2] && rect[1] <= y1 && y0 <= rect[3]; };
    const std::size_t prims = b.prims.size() / kPrimWords;
    if (prims > 64) return hits(b.minx, b.miny, b.maxx, b.maxy);
    for (std::size_t i = 0; i < prims; ++i) {
        const std::uint32_t lo = b.prims.data()[i * kPrimWords + 2], hi = b.prims.data()[i * kPrimWords + 3];
        if (hits(static_cast<int>(lo & 0xFFFFu), static_cast<int>(lo >> 16), static_cast<int>(hi & 0xFFFFu), static_cast<int>(hi >> 16))) return true;
    }
    return false;
}

// PS2_EXACT_SELFTEST=1 (any program will do, no disc: docs/mgs3.md, "Checking the word sets"): runs sprite_taps and exact_clear on random sprites in 32-bit colour against a
// look at every pixel, the way the draw program samples (the float arithmetic of shade_at with and without a fused multiply-add, every sub-sample of the pixel, every tap of
// the bilinear filter, the wrap modes) and the addresses of ps2::gs::address32, which this code does not share. A sprite that the word sets let join although a pixel reads a word
// an earlier sprite of the batch wrote, or a tap outside the range sprite_taps gives, is a failure; a sprite kept out without a shared word is only counted.
void exact_selftest() {
    std::uint64_t seed = 0x9E3779B97F4A7C15ull;
    const auto rnd = [&seed](std::uint32_t n) {
        seed ^= seed << 13;
        seed ^= seed >> 7;
        seed ^= seed << 17;
        return static_cast<std::uint32_t>((seed >> 11) % n);
    };
    const std::uint32_t saved_scale = g_scale;
    int failures = 0, sprites = 0, joined = 0, kept_out = 0, false_alarms = 0;
    for (int round = 0; round < 6000; ++round) {
        g_scale = 1 + rnd(3);
        reset_batch();
        std::uint32_t setup[kSetupWords] = {};
        const std::uint32_t tw = 5 + rnd(4), th = 5 + rnd(4), wms = rnd(4), wmt = rnd(4);
        const bool bilinear = rnd(2) != 0;
        setup[0] = 2u | 16u | (bilinear ? 16384u : 0u);
        setup[1] = 32u * rnd(3);
        setup[2] = 1u << (1 + rnd(3));
        setup[3] = PSMCT32;
        setup[7] = (wms << 24) | (wmt << 26);
        setup[12] = setup[1] + 32u * rnd(4);
        setup[13] = 1u << rnd(4);
        setup[14] = PSMCT32;
        setup[15] = tw | (th << 8);
        const auto bounds = [&](std::uint32_t mode, std::uint32_t bits, std::uint32_t &lo, std::uint32_t &hi) {
            const std::uint32_t size = 1u << bits;
            if (mode == 2) {
                lo = rnd(size / 2);
                hi = lo + rnd(size / 2);
            } else {
                lo = rnd(size);
                hi = rnd(size);
            }
        };
        std::uint32_t minu, maxu, minv, maxv;
        bounds(wms, tw, minu, maxu);
        bounds(wmt, th, minv, maxv);
        setup[18] = minu | (maxu << 16);
        setup[19] = minv | (maxv << 16);
        fmt_words(setup[14], setup[12], setup[13], setup + 25);
        fmt_words(setup[3], setup[1], setup[2], setup + 30);
        Batch &b = g_batch;
        b.setups.assign(setup, setup + kSetupWords);
        std::vector<std::uint32_t> written;
        const auto wrap = [&](std::uint32_t mode, int t, int size, std::uint32_t lo, std::uint32_t hi) {
            switch (mode) {
            case 0: return t & (size - 1);
            case 1: return std::clamp(t, 0, size - 1);
            case 2: return std::clamp(t, static_cast<int>(lo), static_cast<int>(hi));
            default: return static_cast<int>((static_cast<std::uint32_t>(t) & lo) | hi);
            }
        };
        const int count = 2 + static_cast<int>(rnd(6));
        for (int n = 0; n < count; ++n) {
            const int w = 1 + static_cast<int>(rnd(12)), h = 1 + static_cast<int>(rnd(6));
            const float x0 = static_cast<float>(rnd(100)) + 0.25f * static_cast<float>(rnd(4)), y0 = static_cast<float>(rnd(50)) + 0.25f * static_cast<float>(rnd(4));
            const float ratios[] = {1.0f, 1.0f, 2.0f, 0.5f, 1.5f, 0.75f, 3.0f};
            const float rw = ratios[rnd(7)], rh = ratios[rnd(7)];
            const float u0 = static_cast<float>(rnd((1u << tw) * 16u)) + (rnd(2) ? 8.0f : 0.0f), v0 = static_cast<float>(rnd((1u << th) * 16u)) + (rnd(2) ? 8.0f : 0.0f);
            const float u1 = u0 + std::floor(static_cast<float>(w) * rw * 16.0f), v1 = v0 + std::floor(static_cast<float>(h) * rh * 16.0f);
            const int minx = static_cast<int>(std::ceil(x0)), miny = static_cast<int>(std::ceil(y0)), maxx = static_cast<int>(std::ceil(x0 + static_cast<float>(w))) - 1, maxy = static_cast<int>(std::ceil(y0 + static_cast<float>(h))) - 1;
            if (minx > maxx || miny > maxy) continue;
            std::uint32_t rec[kPrimWords] = {};
            const float geometry[4] = {x0, y0, static_cast<float>(w), static_cast<float>(h)}, uv[6] = {u0, v0, 1.0f, u1, v1, 1.0f};
            rec[1] = 1;
            std::memcpy(rec + 4, geometry, sizeof geometry);
            std::memcpy(rec + 15, uv, sizeof uv);
            // what the draw program reads: floats as in shade_at, every sub-sample position and the whole position of the pixel (the nearest read of a sprite that is not scaled)
            std::vector<int> us[2];
            int actual_lo[2] = {1 << 30, 1 << 30}, actual_hi[2] = {-(1 << 30), -(1 << 30)};
            for (int axis = 0; axis < 2; ++axis) {
                const float origin = geometry[axis], d = geometry[2 + axis], a = uv[axis], bb = uv[3 + axis];
                for (int p = axis ? miny : minx; p <= (axis ? maxy : maxx); ++p)
                    for (std::uint32_t j = 0; j <= g_scale; ++j) {
                        const float centre = g_scale > 1 ? 0.5f / static_cast<float>(g_scale) - 0.5f : 0.0f;
                        const float position = j == g_scale ? static_cast<float>(p) : static_cast<float>(p) + static_cast<float>(j) / static_cast<float>(g_scale) + centre;
                        const float t = d != 0.0f ? (position - origin) / d : 0.0f;
                        for (const bool fused : {false, true}) {
                            const float u = (fused ? std::fma(bb - a, t, a) : a + (bb - a) * t) / 16.0f;
                            const int first = static_cast<int>(std::floor(bilinear ? u - 0.5f : u));
                            for (const int tap : {first, first + (bilinear ? 1 : 0)}) {
                                us[axis].push_back(tap);
                                actual_lo[axis] = std::min(actual_lo[axis], tap);
                                actual_hi[axis] = std::max(actual_hi[axis], tap);
                            }
                        }
                    }
            }
            int lo[2], hi[2];
            if (!sprite_taps(rec, setup, minx, miny, maxx, maxy, lo, hi)) {
                ++failures;
                std::fprintf(stderr, "[exact-selftest] FAIL: sprite_taps gives nothing for a textured sprite\n");
                continue;
            }
            for (int axis = 0; axis < 2; ++axis)
                if (actual_lo[axis] < lo[axis] || actual_hi[axis] > hi[axis]) {
                    ++failures;
                    std::fprintf(stderr, "[exact-selftest] FAIL: axis %d reads %d..%d, sprite_taps says %d..%d (scale %u, bilinear %d, d %g, u %g..%g)\n", axis, actual_lo[axis], actual_hi[axis], lo[axis], hi[axis], g_scale, bilinear, geometry[2 + axis], uv[axis], uv[3 + axis]);
                }
            std::vector<std::uint32_t> reads;
            for (const int v : us[1])
                for (const int u : us[0])
                    reads.push_back(address32(setup[12], setup[13], static_cast<std::uint32_t>(wrap(wms, u, 1 << tw, minu, maxu)), static_cast<std::uint32_t>(wrap(wmt, v, 1 << th, minv, maxv))) / 4);
            bool hazard = false;
            for (const std::uint32_t word : reads) hazard = hazard || std::find(written.begin(), written.end(), word) != written.end();
            ++sprites;
            if (!b.prims.empty()) {
                Range span;
                if (exact_clear(rec, setup, minx, miny, maxx, maxy, span)) {
                    ++joined;
                    if (hazard) {
                        ++failures;
                        std::fprintf(stderr, "[exact-selftest] FAIL: a sprite joined the batch although it reads a word an earlier sprite writes (round %d)\n", round);
                    }
                } else {
                    ++kept_out;
                    if (!hazard) ++false_alarms;
                    reset_batch();
                    b.setups.assign(setup, setup + kSetupWords);
                    written.clear();
                }
            }
            rec[0] = 0;
            rec[2] = static_cast<std::uint32_t>(minx) | (static_cast<std::uint32_t>(miny) << 16);
            rec[3] = static_cast<std::uint32_t>(maxx) | (static_cast<std::uint32_t>(maxy) << 16);
            b.prims.append(rec, kPrimWords);
            exact_follow(rec, setup);
            for (int y = miny; y <= maxy; ++y)
                for (int x = minx; x <= maxx; ++x) written.push_back(address32(setup[1], setup[2], static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y)) / 4);
        }
    }
    reset_batch();
    g_scale = saved_scale;
    std::fprintf(stderr, "[exact-selftest] %s: %d sprites asked about, %d joined, %d kept out (%d of them with no shared word), %d failures\n", failures ? "FAIL" : "PASS", sprites, joined, kept_out, false_alarms, failures);
}

void attrs(std::uint32_t *rec, int n, const Vertex &v, bool fst, const Vertex *color) {
    const Vertex &c = color ? *color : v;
    rec[12 + n] = c.r | (static_cast<std::uint32_t>(c.g) << 8) | (static_cast<std::uint32_t>(c.b) << 16) | (static_cast<std::uint32_t>(c.a) << 24);
    rec[15 + n * 3] = f2u(fst ? static_cast<float>(v.u) : v.s);
    rec[16 + n * 3] = f2u(fst ? static_cast<float>(v.v) : v.t);
    rec[17 + n * 3] = f2u(fst ? 1.0f : v.q);
    rec[11] |= static_cast<std::uint32_t>(v.f) << (8 * n);
}

}

Mode g_current_mode = Mode::off;

#ifdef _WIN32
extern "C" unsigned long __stdcall GetModuleFileNameW(void *module, wchar_t *name, unsigned long size);
#endif

// The full path of dxcompiler.dll when it and dxil.dll sit next to the program (scripts/fetch_dxc.sh, copied by CMake), else "".
// PS2_DX12_COMPILER=fxc leaves DXC out, =dxc says so when it cannot be used; unset: DXC when the two files are there.
static std::string find_dxc() {
#ifdef _WIN32
    const std::string want = [] {
        std::string v = os::env("PS2_DX12_COMPILER");
        for (char &c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return v;
    }();
    if (!want.empty() && want != "fxc" && want != "dxc") std::fprintf(stderr, "[gpu] PS2_DX12_COMPILER=%s is not fxc or dxc: ignored\n", want.c_str());
    if (want == "fxc") return {};
    wchar_t buf[1024] = {};
    const unsigned long n = GetModuleFileNameW(nullptr, buf, 1024);
    if (n == 0 || n >= 1024) return {};
    const std::filesystem::path dir = std::filesystem::path(std::wstring(buf, n)).parent_path();
    std::error_code ec;
    if (std::filesystem::exists(dir / "dxcompiler.dll", ec) && std::filesystem::exists(dir / "dxil.dll", ec)) return os::utf8(dir / "dxcompiler.dll");
    if (want == "dxc") std::fprintf(stderr, "[gpu] PS2_DX12_COMPILER=dxc: dxcompiler.dll and dxil.dll are not next to the program (scripts/fetch_dxc.sh): DXC unavailable\n");
#endif
    return {};
}

static const char *g_dx12_compiler = nullptr;  // "DXC" or "FXC" once Direct3D 12 is open

// One try at starting a graphics API: an instance, an adapter and a device. On failure everything made is released again and false comes
// back, so the next API can be tried. A software adapter (Direct3D's WARP) is only taken when software_ok: a real graphics chip on
// another API is better.
static bool open_backend([[maybe_unused]] std::uint32_t backends, const char *name, bool software_ok, [[maybe_unused]] const std::string &dxc_dll) {
#ifdef __EMSCRIPTEN__
    g_instance = wgpuCreateInstance(nullptr);
#else
    WGPUInstanceExtras extras = {};
    extras.chain.sType = static_cast<WGPUSType>(WGPUSType_InstanceExtras);
    extras.backends = backends;
    // Direct3D 12 turns the WGSL into HLSL and compiles that: with Microsoft's DXC (dxcompiler.dll + dxil.dll) or, without it, with the
    // old FXC that comes with Windows. A non-empty dxc_dll is the full path of dxcompiler.dll (dxil.dll sits beside it).
    if (!dxc_dll.empty()) {
        extras.dx12ShaderCompiler = WGPUDx12Compiler_Dxc;
        extras.dxcPath = WGPUStringView{dxc_dll.data(), dxc_dll.size()};
    } else {
        extras.dx12ShaderCompiler = WGPUDx12Compiler_Fxc;
    }
    WGPUInstanceDescriptor id = WGPU_INSTANCE_DESCRIPTOR_INIT;
    id.nextInChain = &extras.chain;
    g_instance = wgpuCreateInstance(&id);
#endif
    if (!g_instance) {
        std::fprintf(stderr, "[gpu] %s: no instance\n", name);
        return false;
    }
    WGPUAdapter adapter = nullptr;
    const auto give_up = [&] {
        if (g_device) wgpuDeviceRelease(g_device);
        if (adapter) wgpuAdapterRelease(adapter);
        wgpuInstanceRelease(g_instance);
        g_device = nullptr;
        g_instance = nullptr;
        return false;
    };
    bool adapter_done = false;
    WGPURequestAdapterOptions options = WGPU_REQUEST_ADAPTER_OPTIONS_INIT;
    options.powerPreference = std::getenv("PS2_GPU_LOW_POWER") ? WGPUPowerPreference_LowPower : WGPUPowerPreference_HighPerformance;
    options.forceFallbackAdapter = std::getenv("PS2_GPU_FALLBACK") != nullptr;
    WGPURequestAdapterCallbackInfo acb = WGPU_REQUEST_ADAPTER_CALLBACK_INFO_INIT;
    acb.mode = WGPUCallbackMode_AllowSpontaneous;
    acb.callback = [](WGPURequestAdapterStatus status, WGPUAdapter a, WGPUStringView, void *out, void *done) {
        if (status == WGPURequestAdapterStatus_Success) *static_cast<WGPUAdapter *>(out) = a;
        *static_cast<bool *>(done) = true;
    };
    acb.userdata1 = &adapter;
    acb.userdata2 = &adapter_done;
    wgpuInstanceRequestAdapter(g_instance, &options, acb);
    while (!adapter_done) pump();
    if (!adapter) {
        std::fprintf(stderr, "[gpu] no adapter for %s\n", name);
        return give_up();
    }
    bool selected_dx12 = false;
    g_vendor_id = 0;
    WGPUAdapterInfo info = WGPU_ADAPTER_INFO_INIT;
    if (wgpuAdapterGetInfo(adapter, &info) == WGPUStatus_Success) {
        selected_dx12 = info.backendType == WGPUBackendType_D3D12;
        g_vendor_id = info.vendorID;
        const char *api = info.backendType == WGPUBackendType_Vulkan ? "Vulkan" : info.backendType == WGPUBackendType_D3D12 ? "Direct3D 12" : info.backendType == WGPUBackendType_Metal ? "Metal" : "other";
        const bool software = info.adapterType == WGPUAdapterType_CPU;
        if (software && !software_ok)
            std::fprintf(stderr, "[gpu] %s has only a software adapter (%.*s): looking for a real graphics chip on another API first\n", name, static_cast<int>(info.device.length), info.device.data);
        else
            std::fprintf(stderr, "[gpu] %.*s (%.*s) on %s\n", static_cast<int>(info.device.length), info.device.data, static_cast<int>(info.description.length), info.description.data, api);
        wgpuAdapterInfoFreeMembers(info);
        if (software && !software_ok) return give_up();
    }
    WGPULimits supported = WGPU_LIMITS_INIT;
    wgpuAdapterGetLimits(adapter, &supported);
    WGPULimits limits = WGPU_LIMITS_INIT;
    limits.maxStorageBufferBindingSize = supported.maxStorageBufferBindingSize;
    limits.maxBufferSize = supported.maxBufferSize;
    WGPUDeviceDescriptor dd = WGPU_DEVICE_DESCRIPTOR_INIT;
    dd.requiredLimits = &limits;
    WGPUFeatureName features[2];
    g_timestamps = wgpuAdapterHasFeature(adapter, WGPUFeatureName_TimestampQuery);
    if (g_timestamps) features[dd.requiredFeatureCount++] = WGPUFeatureName_TimestampQuery;
    if (hw::enabled() && wgpuAdapterHasFeature(adapter, WGPUFeatureName_DualSourceBlending)) features[dd.requiredFeatureCount++] = WGPUFeatureName_DualSourceBlending;  // stored alpha in one draw; asked for only when the card path is on and the adapter offers it
    dd.requiredFeatures = features;
    dd.uncapturedErrorCallbackInfo.callback = on_error;
    bool device_done = false;
    WGPURequestDeviceCallbackInfo dcb = WGPU_REQUEST_DEVICE_CALLBACK_INFO_INIT;
    dcb.mode = WGPUCallbackMode_AllowSpontaneous;
    dcb.callback = [](WGPURequestDeviceStatus status, WGPUDevice d, WGPUStringView message, void *out, void *done) {
        if (status == WGPURequestDeviceStatus_Success) *static_cast<WGPUDevice *>(out) = d;
        else std::fprintf(stderr, "[gpu] device: %.*s\n", static_cast<int>(message.length), message.data);
        *static_cast<bool *>(done) = true;
    };
    dcb.userdata1 = &g_device;
    dcb.userdata2 = &device_done;
    wgpuAdapterRequestDevice(adapter, &dd, dcb);
    while (!device_done) pump();
    if (!g_device) {
        std::fprintf(stderr, "[gpu] no device on %s\n", name);
        return give_up();
    }
    g_adapter = adapter;
    // The requested backend mask may include DX12 even when wgpu selects Metal or Vulkan.
    g_dx12_compiler = selected_dx12 ? (dxc_dll.empty() ? "FXC" : "DXC") : nullptr;
    return true;
}

bool init(Mode m) {
    if (m == Mode::off) return false;
    stat::init();
    hwcache::init();
    struct Try {
        std::uint32_t backends;  // WGPUInstanceBackend flags (the web build has none)
        const char *name;
    };
    std::vector<Try> order;
    bool forced = false;
#ifdef __EMSCRIPTEN__
    order.push_back({0, "WebGPU"});
#else
    // PS2_GPU_BACKEND=vulkan, dx12 or d3d12 (any letter case) picks the graphics API and nothing else is tried. Another value is said so
    // and ignored. Unset: Windows tries Direct3D 12 first and Vulkan if that cannot start (docs/speed.md: on the Intel laptop Direct3D 12
    // draws the same pictures 18 to 52 % faster than Vulkan); elsewhere wgpu picks. The log line "[gpu] ... on <API>" names the API used.
    if (std::string backend = os::env("PS2_GPU_BACKEND"); !backend.empty()) {
        for (char &c : backend) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (backend == "vulkan") order.push_back({WGPUInstanceBackend_Vulkan, "Vulkan"}), forced = true;
        else if (backend == "dx12" || backend == "d3d12") order.push_back({WGPUInstanceBackend_DX12, "Direct3D 12"}), forced = true;
        else std::fprintf(stderr, "[gpu] PS2_GPU_BACKEND=%s is not vulkan, dx12 or d3d12: ignored\n", os::env("PS2_GPU_BACKEND").c_str());
    }
    if (order.empty()) {
#ifdef _WIN32
        if (hw::enabled()) {  // PS2_GPU_HW=1: on the one Windows laptop tried the card path ran faster on Vulkan while the painter is faster on Direct3D 12; PS2_GPU_BACKEND still wins.
            order.push_back({WGPUInstanceBackend_Vulkan, "Vulkan"});
            order.push_back({WGPUInstanceBackend_DX12, "Direct3D 12"});
        } else
        {
            order.push_back({WGPUInstanceBackend_DX12, "Direct3D 12"});
            order.push_back({WGPUInstanceBackend_Vulkan, "Vulkan"});
        }
#else
        order.push_back({WGPUInstanceBackend_Primary, "the best API"});
#endif
    }
#endif
    // First round: real graphics chips only; second round (when there is more than one API to try): software adapters too.
    const Try *chosen = nullptr;
    const std::string dxc = find_dxc();
    for (int round = 0; round < 2 && !chosen; ++round) {
        for (const Try &t : order) {
#ifdef __EMSCRIPTEN__
            const bool dx12 = false;  // the browser picks the graphics API; the web build has no WGPUInstanceBackend flags
#else
            const bool dx12 = (t.backends & WGPUInstanceBackend_DX12) != 0;
#endif
            // DXC where Direct3D 12 may run; if that cannot start with it, the same API is tried again with FXC before the next API.
            bool opened = dx12 && !dxc.empty() && open_backend(t.backends, t.name, round == 1 || order.size() == 1, dxc);
            if (!opened) {
                if (dx12 && !dxc.empty()) std::fprintf(stderr, "[gpu] Direct3D 12 could not start with DXC: trying FXC\n");
                opened = open_backend(t.backends, t.name, round == 1 || order.size() == 1, std::string());
            }
            if (opened) {
                chosen = &t;
                break;
            }
        }
        if (order.size() == 1) break;
    }
    if (!chosen) return false;
    if (forced) std::fprintf(stderr, "[gpu] graphics API: %s (asked for by PS2_GPU_BACKEND)\n", chosen->name);
    else if (chosen == &order[0] && order.size() > 1) std::fprintf(stderr, "[gpu] graphics API: %s (the default on Windows%s; PS2_GPU_BACKEND=vulkan or dx12 picks)\n", chosen->name,
                 hw::enabled() ? " with PS2_GPU_HW=1" :
                 "");
    else if (order.size() > 1) std::fprintf(stderr, "[gpu] graphics API: %s (%s could not be used, see above)\n", chosen->name, order[0].name);
    if (g_dx12_compiler) std::fprintf(stderr, "[gpu] Direct3D 12 shader compiler: %s (PS2_DX12_COMPILER=fxc|dxc picks)\n", g_dx12_compiler);
    g_queue = wgpuDeviceGetQueue(g_device);

    if (const std::string spec = os::env("PS2_GPU_PROFILE"); !spec.empty()) {
        char path[512] = {};
        unsigned long long a = 0, z = 0;
        if (std::sscanf(spec.c_str(), "%llu,%llu,%511s", &a, &z, path) == 3) {
            g_profile_out = os::open(path, "w");
            g_profile_from = a;
            g_profile_to = z;
        }
    }
    if (const std::string spec = os::env("PS2_GPU_BENCH"); !spec.empty()) {
        unsigned long long at = 0, reps = 20;
        if (std::sscanf(spec.c_str(), "%llu,%llu", &at, &reps) >= 1 && at > 1) {
            g_bench.configured = true;
            g_bench.vblank = at;
            g_bench.reps = std::clamp<unsigned long long>(reps, 1, 500);
        }
    }
    if (const char *scale = std::getenv("PS2_SCALE")) g_scale = std::clamp<std::uint32_t>(static_cast<std::uint32_t>(std::atoi(scale)), 1u, 4u);
    // PS2_SHARP_2D: how upscaled 2D art (text, icons, menus) is filtered, see SHARP in gs.wgsl: 0 plain bilinear, 1 sharp (the
    // default), 2 sharpest
    if (const char *sharp = std::getenv("PS2_SHARP_2D")) g_sharp_2d = std::clamp<std::uint32_t>(static_cast<std::uint32_t>(std::atoi(sharp)), 0u, 2u);
    g_copy_words = (1u << 20) * g_scale * g_scale;
    if (const int tile = std::atoi(os::env("PS2_TILE").c_str()); tile == 4 || tile == 8 || tile == 16) g_tile = tile;
    tiles::configure(static_cast<int>(g_scale), g_tile);
    g_field_words = (1u << 19) * g_scale * g_scale;
    g_vram = make_buffer(static_cast<std::uint64_t>(kMemorySize) * g_scale * g_scale, WGPUBufferUsage_Storage | WGPUBufferUsage_CopySrc | WGPUBufferUsage_CopyDst);
    std::vector<std::uint32_t> tables;
    swizzle_tables(tables);
    g_swizzle = tables;
    if (!os::env("PS2_EXACT_SELFTEST").empty()) exact_selftest();
    g_tables = make_buffer(tables.size() * 4, WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst);
    wgpuQueueWriteBuffer(g_queue, g_tables, 0, tables.data(), tables.size() * 4);
    g_clut = make_buffer(static_cast<std::uint64_t>(kClutSlots) * kClutEntries * 4, WGPUBufferUsage_Storage | WGPUBufferUsage_CopySrc | WGPUBufferUsage_CopyDst);
    g_scratch = make_buffer((static_cast<std::uint64_t>(g_copy_words) + 2ull * g_field_words) * 4, WGPUBufferUsage_Storage | WGPUBufferUsage_CopySrc);
    g_cache = make_buffer(static_cast<std::uint64_t>(kCacheWords) * 4, WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst);
    g_decoded = make_buffer((static_cast<std::uint64_t>(kCachedWords) + kDecodedWords) * 4, WGPUBufferUsage_Storage);  // the kept textures, then this submit's pictures
    if (const char *decode = std::getenv("PS2_DECODE")) g_decode = std::atoi(decode) != 0;
    if (const char *cache = std::getenv("PS2_DECODE_CACHE")) g_dec_on = std::atoi(cache) != 0, g_dec_small = std::atoi(cache) != 2;  // 0 off, 2 not for small batches
    g_dec_stat = !os::env("PS2_DECODE_CACHE_STAT").empty();
    dec_reset();
    g_cache_copy.assign(kCacheWords, 0);
    g_readback = make_buffer(static_cast<std::uint64_t>(g_copy_words) * 4, WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst);
    if (g_timestamps) {
        WGPUQuerySetDescriptor qd = WGPU_QUERY_SET_DESCRIPTOR_INIT;
        qd.type = WGPUQueryType_Timestamp;
        qd.count = 2;
        g_queries = wgpuDeviceCreateQuerySet(g_device, &qd);
        g_query_resolve = make_buffer(16, WGPUBufferUsage_QueryResolve | WGPUBufferUsage_CopySrc);
        g_query_read = make_buffer(16, WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst);
    }

    WGPUBindGroupLayoutEntry le[8] = {};
    const WGPUBufferBindingType types[8] = {WGPUBufferBindingType_Storage, WGPUBufferBindingType_ReadOnlyStorage, WGPUBufferBindingType_Uniform,
                                            WGPUBufferBindingType_ReadOnlyStorage, WGPUBufferBindingType_Storage, WGPUBufferBindingType_Storage,
                                            WGPUBufferBindingType_ReadOnlyStorage, WGPUBufferBindingType_Storage};
    for (int i = 0; i < 8; ++i) {
        le[i] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
        le[i].binding = static_cast<std::uint32_t>(i);
        le[i].visibility = WGPUShaderStage_Compute;
        le[i].buffer.type = types[i];
        le[i].buffer.hasDynamicOffset = i == 2;
        le[i].buffer.minBindingSize = i == 2 ? kParamStride : 0;
    }
    WGPUBindGroupLayoutDescriptor ld = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
    ld.entryCount = 8;
    ld.entries = le;
    g_layout = wgpuDeviceCreateBindGroupLayout(g_device, &ld);
    WGPUPipelineLayoutDescriptor pld = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
    pld.bindGroupLayoutCount = 1;
    pld.bindGroupLayouts = &g_layout;
    g_pipeline_layout = wgpuDeviceCreatePipelineLayout(g_device, &pld);

    // PS2_GS_SHADER=<file>: use this WGSL text instead of the built-in one (shader experiments without a rebuild)
    std::string shader_text;
    if (const std::string path = os::env("PS2_GS_SHADER"); !path.empty()) {
        if (std::FILE *f = os::open(path.c_str(), "rb")) {
            char buffer[65536];
            for (std::size_t n; (n = std::fread(buffer, 1, sizeof buffer, f)) > 0;) shader_text.append(buffer, n);
            const bool read_error = std::ferror(f) != 0;
            std::fclose(f);
            if (read_error || shader_text.empty()) {
                std::fprintf(stderr, "[gpu] PS2_GS_SHADER: %s %s, using the built-in shader\n", read_error ? "cannot finish reading" : "empty file", path.c_str());
                shader_text.clear();
            } else {
                std::fprintf(stderr, "[gpu] shader from %s (%zu bytes)\n", path.c_str(), shader_text.size());
            }
        } else {
            std::fprintf(stderr, "[gpu] PS2_GS_SHADER: cannot read %s, using the built-in shader\n", path.c_str());
        }
    }
    WGPUShaderSourceWGSL source = WGPU_SHADER_SOURCE_WGSL_INIT;
    source.code = shader_text.empty() ? view(kGsShader) : WGPUStringView{shader_text.data(), shader_text.size()};
    WGPUShaderModuleDescriptor md = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
    md.nextInChain = &source.chain;
    // DXC and the other compilers fuse a*b+c differently; with DXC the shader spells the fused sums out (EXPLICIT_FMA in gs.wgsl).
    const bool explicit_fma = g_dx12_compiler && std::strcmp(g_dx12_compiler, "DXC") == 0 && os::env("PS2_EXPLICIT_FMA") != "0";
    if (explicit_fma) std::fprintf(stderr, "[gpu] shader sums written as fma() (DXC)\n");
    const auto compile_start = std::chrono::steady_clock::now();
#ifdef __EMSCRIPTEN__
    WGPUShaderModule module = wgpuDeviceCreateShaderModule(g_device, &md);
#else
    // wgpu normally adds its own run-time checks to the shader: every buffer index clamped where the graphics API does not
    // bound it already, and a counter in every loop that breaks out after very many rounds. gs.wgsl keeps its indices inside
    // its buffers itself (the explicit bounds there) and all its loops end, so PS2_GPU_TRUSTED=1 makes
    // the built-in draw program without them (wgpu-native's wgpuDeviceCreateShaderModuleTrusted). On by default only for Intel graphics on Windows (trusted.hpp), where it was measured (a laptop); a shader loaded with PS2_GS_SHADER always keeps the checks.
#ifdef _WIN32
    const bool is_windows = true;
#else
    const bool is_windows = false;
#endif
    const char *trusted_why = "";
    // (PS2_GS_SHADER_TRUSTED=1 lets a shader loaded from a file go without them too, so a shader experiment is timed as the built-in one runs)
    const bool trusted = (shader_text.empty() || os::env("PS2_GS_SHADER_TRUSTED") == "1") &&
                         trusted_shader(g_vendor_id, is_windows, os::env("PS2_GPU_TRUSTED"), &trusted_why);
    if (trusted) std::fprintf(stderr, "[gpu] draw program without wgpu's run-time checks (%s)\n", trusted_why);
    WGPUShaderModule module = trusted ? wgpuDeviceCreateShaderModuleTrusted(g_device, &md, WGPUShaderRuntimeChecks_None) : wgpuDeviceCreateShaderModule(g_device, &md);
#endif
    // a shader loaded with PS2_GS_SHADER may lack the later constants; TILE, EXPLICIT_FMA, SHARP and HAS_MIP are listed only when it declares them
    const bool has_tile = shader_text.empty() || shader_text.find("override TILE") != std::string::npos;
    const bool has_fma = shader_text.empty() || shader_text.find("override EXPLICIT_FMA") != std::string::npos;
    const bool has_mip = shader_text.empty() || shader_text.find("override HAS_MIP") != std::string::npos;
    const bool has_sharp = shader_text.empty() || shader_text.find("override SHARP") != std::string::npos;
    const int scale = static_cast<int>(g_scale), tile = g_tile;
    const std::uint32_t sharp = g_sharp_2d;
    // `mip`: the draw program that reads mipmap levels (HAS_MIP in gs.wgsl); without it the program is the plain one
    auto make_pipeline = [=](int entry, bool mip) {
        WGPUComputePipelineDescriptor cd = WGPU_COMPUTE_PIPELINE_DESCRIPTOR_INIT;
        cd.layout = g_pipeline_layout;
        cd.compute.module = module;
        cd.compute.entryPoint = view(kEntryPoints[entry]);
        // a shader loaded with PS2_GS_SHADER may lack the later constants; each is listed only when the shader declares it
        WGPUConstantEntry constants[5];
        std::size_t count = 0;
        const auto set = [&](bool declared, const char *key, double value) {
            if (!declared) return;
            constants[count] = WGPU_CONSTANT_ENTRY_INIT;
            constants[count].key = view(key);
            constants[count].value = value;
            ++count;
        };
        set(true, "SCALE", static_cast<double>(scale));
        set(has_tile, "TILE", static_cast<double>(tile));
        set(has_fma, "EXPLICIT_FMA", explicit_fma ? 1.0 : 0.0);
        set(has_sharp, "SHARP", static_cast<double>(sharp));
        set(has_mip, "HAS_MIP", mip ? 1.0 : 0.0);
        cd.compute.constantCount = count;
        cd.compute.constants = constants;
        return wgpuDeviceCreateComputePipeline(g_device, &cd);
    };
    // The mipmap variant of the draw program: most games never use it, and on FXC it would double the start. Built in a thread beside the
    // plain programs and the game's own start (the browser build has none and does it after them); a batch with a mipmap table waits for
    // it (pipeline_of).
    if (g_raster_mip_thread) {
        if (g_raster_mip_thread->joinable()) g_raster_mip_thread->join();
        delete g_raster_mip_thread;
        g_raster_mip_thread = nullptr;
    }
    g_raster_mip = nullptr;
    g_raster_mip_ready.store(true, std::memory_order_release);  // nothing to wait for unless the thread below starts
    // the module is released by whoever finishes last: this function after the plain programs, the thread after its one
    auto users = std::make_shared<std::atomic<int>>(has_mip ? 2 : 1);
    const auto release_module = [users, module]() {
        if (users->fetch_sub(1) == 1) wgpuShaderModuleRelease(module);
    };
    const auto build_mip = [make_pipeline, release_module]() {
        const auto start = std::chrono::steady_clock::now();
        g_raster_mip = make_pipeline(kRaster, true);
        std::fprintf(stderr, "[gpu] mipmap draw program %s in %.0f ms\n", g_raster_mip ? "compiled" : "FAILED (levels are ignored)", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
        release_module();
    };
#ifndef __EMSCRIPTEN__
    if (has_mip) {
        g_raster_mip_ready.store(false, std::memory_order_release);
        g_raster_mip_thread = new std::thread(build_mip);
    }
#endif
    for (int i = 0; i < kPipelines; ++i) {
        g_pipelines[i] = make_pipeline(i, false);
        if (!g_pipelines[i]) return false;
    }
    std::fprintf(stderr, "[gpu] %d shaders compiled in %.0f ms\n", kPipelines, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - compile_start).count());
#ifdef __EMSCRIPTEN__
    if (has_mip) build_mip();  // the browser build has no threads
#endif
    release_module();
    ensure_capacity();
    if (hw::enabled() && !g_bench.configured && os::env("PS2_GPU_HW_LIVE") != "0") {  // PS2_GPU_HW=1 without the bench draws the running game's plain shapes on the card
        hw::Context hc{g_device, g_queue, g_vram, g_tables, g_clut, g_scratch, g_cache, g_layout, {}, kUpload, kLoadClut, kRaster, kDecode, kSnapshot, g_scale, nullptr, bytes_of_vram(), kGsShader};
        g_live = hw::live_init(hc);
    }
    g_mode = m;
    g_current_mode = m;
    return true;
}

void draw(const State &st, const Vertex *v, int count) {
    if (g_image.active && !g_image.pending.empty()) flush_image();
    std::uint64_t attr = st.prim;
    if (!(st.prmodecont & 1u)) attr = (st.prim & 7u) | (st.prmode & ~7ull);
    const std::uint32_t prim = static_cast<std::uint32_t>(st.prim) & 7u;
    const Context &c = st.ctx[(attr >> 9) & 1u];
    SetupCache &cache = g_setup_cache;
    if (cache.generation != g_setup_generation || cache.attr != attr || cache.clut != g_clut_slot
        || (cache.waits && hw::live_ready_changed(cache.ready_seen))
        ) {
        cache.generation = g_setup_generation;
        cache.attr = attr;
        cache.clut = g_clut_slot;
        cache.serial = ++g_setup_serials;
        const std::uint32_t *w = cache.setup;
        cache.flags = make_setup(st, attr, cache.setup);
        cache.route = g_live ? hw::live_route(cache.setup) : 0;
        cache.look = g_live ? hw::live_look() : -1;
        cache.waits = g_live && cache.route == 0 && hw::live_waits();
        cache.key = static_cast<std::uint64_t>(w[1]) | (static_cast<std::uint64_t>(w[2]) << 14) | (static_cast<std::uint64_t>(w[3]) << 20) |
                    (static_cast<std::uint64_t>(w[5]) << 26) | (static_cast<std::uint64_t>(w[6]) << 40);
        cache.mip_top = make_mip(st, attr, cache.mip);
        if (cache.flags & 2u) {
            const std::uint32_t tw = w[15] & 0xFFu, th = w[15] >> 8;
            const int tex_w = std::max(1 << tw, static_cast<int>(w[18] >> 16) + 1), tex_h = std::max(1 << th, static_cast<int>(w[19] >> 16) + 1);
            cache.tex = region(w[12], w[13], w[14], 0, 0, tex_w - 1, tex_h - 1);
            if (cache.mip_top) {  // the other levels are read too: uploads, palette loads and the cuts below must see their memory as well
                const Range levels = mip_range(w, cache.mip, cache.mip_top);
                cache.tex.add(levels.lo, levels.hi);
            }
        }
    }
    const std::uint32_t *setup = cache.setup;
    const std::uint32_t flags = cache.flags;
    const bool fst = flags & 16u, iip = flags & 1u;
    const int ofx = static_cast<int>(c.xyoffset & 0xFFFFu), ofy = static_cast<int>((c.xyoffset >> 32) & 0xFFFFu);
    const int scax0 = static_cast<int>(setup[10] & 0xFFFFu), scax1 = static_cast<int>(setup[10] >> 16);
    const int scay0 = static_cast<int>(setup[11] & 0xFFFFu), scay1 = static_cast<int>(setup[11] >> 16);
    const auto sx = [&](const Vertex &p) { return (static_cast<float>(p.x) - static_cast<float>(ofx)) / 16.0f; };
    const auto sy = [&](const Vertex &p) { return (static_cast<float>(p.y) - static_cast<float>(ofy)) / 16.0f; };
    const float samples = static_cast<float>(g_scale);

    std::uint32_t rec[kPrimWords] = {};
    int minx, miny, maxx, maxy;
    switch (prim) {
    case 3: case 4: case 5: {
        if (count < 3) return;
        // the corners, the edge rule and the box as vectors (setup.hpp; tests/draw_setup.cpp checks it against the scalar arithmetic)
        int box[4];
        if (!prim_setup::triangle(rec, box, v, ofx, ofy, samples, setup[10], setup[11], fst, iip)) return;
        minx = box[0];
        miny = box[1];
        maxx = box[2];
        maxy = box[3];
        break;
    }
    case 6: {
        const float x0 = sx(v[0]), y0 = sy(v[0]), x1 = sx(v[1]), y1 = sy(v[1]);
        minx = std::max(scax0, static_cast<int>(std::ceil(std::min(x0, x1))));
        maxx = std::min(scax1, static_cast<int>(std::ceil(std::max(x0, x1))) - 1);
        miny = std::max(scay0, static_cast<int>(std::ceil(std::min(y0, y1))));
        maxy = std::min(scay1, static_cast<int>(std::ceil(std::max(y0, y1))) - 1);
        rec[1] = 1;
        rec[4] = f2u(x0);
        rec[5] = f2u(y0);
        rec[6] = f2u(x1 - x0);
        rec[7] = f2u(y1 - y0);
        attrs(rec, 0, v[0], fst, nullptr);
        attrs(rec, 1, v[1], fst, nullptr);
        rec[24] = v[1].z;
        break;
    }
    case 1: case 2: {
        const float x0 = sx(v[0]), y0 = sy(v[0]), x1 = sx(v[1]), y1 = sy(v[1]);
        const int steps = static_cast<int>(std::max(std::fabs(x1 - x0), std::fabs(y1 - y0)));
        minx = std::max(scax0, static_cast<int>(std::floor(std::min(x0, x1))) - 1);
        maxx = std::min(scax1, static_cast<int>(std::ceil(std::max(x0, x1))) + 1);
        miny = std::max(scay0, static_cast<int>(std::floor(std::min(y0, y1))) - 1);
        maxy = std::min(scay1, static_cast<int>(std::ceil(std::max(y0, y1))) + 1);
        rec[1] = 2;
        rec[4] = f2u(x0);
        rec[5] = f2u(y0);
        rec[6] = f2u(x1);
        rec[7] = f2u(y1);
        rec[8] = static_cast<std::uint32_t>(steps);
        attrs(rec, 0, v[0], fst, iip ? nullptr : &v[1]);
        attrs(rec, 1, v[1], fst, nullptr);
        rec[24] = v[0].z;
        rec[25] = f2u(static_cast<float>(static_cast<std::int64_t>(v[1].z) - static_cast<std::int64_t>(v[0].z)));
        break;
    }
    case 0: {
        const int x = static_cast<int>(sx(v[0])), y = static_cast<int>(sy(v[0]));
        minx = maxx = x;
        miny = maxy = y;
        if (x < scax0 || x > scax1 || y < scay0 || y > scay1) return;
        rec[1] = 3;
        rec[4] = static_cast<std::uint32_t>(x);
        rec[5] = static_cast<std::uint32_t>(y);
        attrs(rec, 0, v[0], fst, nullptr);
        rec[24] = v[0].z;
        break;
    }
    default:
        return;
    }
    minx = std::max(minx, 0);
    miny = std::max(miny, 0);
    maxx = std::min(maxx, 2047);
    maxy = std::min(maxy, 2047);
    if (minx > maxx || miny > maxy) return;
    if (stat::g_on) stat::shape(st, attr, v, count, flags, minx, miny, maxx, maxy, cache.tex.lo, cache.tex.hi);
    if (g_batch_log_frame > 1 && frame_count() == g_batch_log_frame) {  // PS2_BATCH_LOG=<frame>: every shape of that frame, for working out how the batches could be cut differently offline
        int r[4] = {0, 0, 0, 0};
        const bool ok = (flags & 2u) && texel_rect(setup, v, count, prim, fst, r);
        std::fprintf(stderr, "[shape] box %d %d %d %d tex %d ok %d rect %d %d %d %d tbp %u tpsm %u fbp %u fpsm %u zbp %u zte %d zmsk %d key %llu\n", minx, miny, maxx, maxy, (flags & 2u) ? 1 : 0, ok, r[0], r[1], r[2], r[3], setup[12], setup[14],
                     setup[1], setup[3], setup[5], (flags & 512u) ? 1 : 0, (flags & 32u) ? 1 : 0, static_cast<unsigned long long>(cache.key));
    }

    Batch &b = g_batch;
    const bool zte = flags & 512u;
    const std::uint64_t key = cache.key;
    if (b.key != ~0ull && b.key != key) {
        g_why = kWhyKey;
        emit_all();
    }
    int sroute = 0;
    if (g_live) {  // a batch is wholly card or wholly painter, cut at a switch; lines and points are the painter's, a shape the chip draws nothing for goes with the batch it is in
        sroute = (rec[1] & kKindMask) > 1 ? 0 : cache.route;
        if (sroute == 2) sroute = b.prims.empty() ? 0 : b.route;
        if (!b.prims.empty() && sroute != b.route) {
            g_why = kWhyRoute;
            emit_all();
        }
    }
    const Range &tex = cache.tex;
    if (flags & 2u) {
        if (!b.prims.empty()) {
            batch_ranges();
            // A texture made of the upper byte of 32-bit words (PSMT8H, PSMT4HL, PSMT4HH) is not changed by a 24-bit depth buffer: the drawing
            // program writes a PSMZ24 depth value with a 24-bit mask (and/or on the word), so bits 24 to 31 stay as they were. God of War I
            // keeps such a texture in the upper byte of its depth buffer and reads it in hundreds of small draws below deck (docs/gow1.md).
            const std::uint32_t tpsm = setup[14];
            const bool upper_byte = tpsm == PSMT8H || tpsm == PSMT4HL || tpsm == PSMT4HH;
            const bool z_untouched = upper_byte && b.zpsm == PSMZ24;
            // The same holds for a 24-bit colour buffer: write_frame (gs.wgsl) keeps bits 24 to 31 of a PSMCT24 word. Gundam vs. Zeta Gundam
            // keeps its font in the upper byte of its 24-bit colour buffer and reads it in thousands of small draws per menu frame (docs/gvz.md).
            const bool fb_untouched = upper_byte && b.fpsm == PSMCT24;
            const auto written = [&](const Range &t) { return (!fb_untouched && t.overlaps(b.fb_range.lo, b.fb_range.hi)) || (b.zwrite && !z_untouched && t.overlaps(b.z_range.lo, b.z_range.hi)); };
            int rect[4];
            if (written(tex) && cache.mip_top) {
                // The rectangle and word-set tests below know only the base level: a shape that reads other levels too is cut off whenever any level was drawn on.
                g_why = kWhyTexel;
                emit_all();
            } else if (written(tex)) {
                // The tests below are page and box tests. A sprite that reads the memory of the frame in another layout (indices of 8 bits read as the pixels of 32 bits, a
                // texture window with wrap mode 3) is cut off by them although it often touches no word that an earlier shape wrote: exact_clear looks at the words.
                const bool exact_ok = prim == 6 && !shares_frame(setup, b);
                const auto exact_join = [&]() {
                    Range reads;
                    return exact_clear(rec, setup, minx, miny, maxx, maxy, reads) && !(b.zwrite && !z_untouched && reads.overlaps(b.z_range.lo, b.z_range.hi));
                };
                if (!texel_rect(setup, v, count, prim, fst, rect)) {
                    if (!exact_ok || !exact_join()) {
                        g_why = kWhyTexel;
                        emit_all();
                    }
                } else {
                    const Range used = region(setup[12], setup[13], setup[14], rect[0], rect[1], rect[2], rect[3]);
                    const bool small = rect[2] - rect[0] < 64 && rect[3] - rect[1] < 64;
                    const bool cached = b.texflushes == g_texflushes && small && shares_frame(setup, b);
                    const bool same = shares_frame(setup, b);
                    const bool frame = !cached && (same ? drawn_under(b, rect) : used.overlaps(b.fb_range.lo, b.fb_range.hi));
                    const bool zhit = b.zwrite && !z_untouched && used.overlaps(b.z_range.lo, b.z_range.hi);
                    if ((frame || zhit) && !(frame && !zhit && exact_ok && exact_join())) {
                        if (g_batch_log_frame > 1 && frame_count() == g_batch_log_frame) {
                            int hit = -1;
                            const std::size_t n = b.prims.size() / kPrimWords;
                            for (std::size_t i = 0; i < n && hit < 0; ++i) {
                                const std::uint32_t lo = b.prims.data()[i * kPrimWords + 2], hi = b.prims.data()[i * kPrimWords + 3];
                                if (rect[0] <= static_cast<int>(hi & 0xFFFFu) && static_cast<int>(lo & 0xFFFFu) <= rect[2] && rect[1] <= static_cast<int>(hi >> 16) && static_cast<int>(lo >> 16) <= rect[3]) hit = static_cast<int>(i);
                            }
                            const std::uint32_t hl = hit >= 0 ? b.prims.data()[hit * kPrimWords + 2] : 0, hh = hit >= 0 ? b.prims.data()[hit * kPrimWords + 3] : 0;
                            std::fprintf(stderr, "[why] prim %d of batch (%zu shapes) type %u: texel rect %d,%d-%d,%d tex bp %u psm %u fb bp %u psm %u same %d cached %d small %d hit shape %d box %u,%u-%u,%u zwrite %d\n", count, n, prim, rect[0], rect[1], rect[2], rect[3],
                                         setup[12], setup[14], setup[1], setup[3], same, cached, small, hit, hl & 0xFFFFu, hl >> 16, hh & 0xFFFFu, hh >> 16, b.zwrite);
                        }
                        g_why = frame ? (same ? kWhyUnder : kWhyOverlap) : kWhyZ;
                        emit_all();
                    }
                }
            }
        }
    }
    if (b.prims.empty()) {
        b.texflushes = g_texflushes;
        b.key = key;
        b.fbp = setup[1];
        b.fbw = setup[2];
        b.fpsm = setup[3];
        b.zbp = setup[5];
        b.zpsm = setup[6];
        b.zte = false;
        b.zwrite = false;
        b.route = sroute;
        b.rx1 = -2;
    }
    b.zte = b.zte || zte;
    b.zwrite = b.zwrite || (zte && !(flags & 32u));
    const std::size_t setups = b.setups.size() / kSetupWords;
    std::uint32_t setup_index;
    if (setups && b.setup_serial == cache.serial) {
        // the setup words did not change since the shape that wrote the last entry (the cache is only rebuilt in the block at the top)
        setup_index = static_cast<std::uint32_t>(setups - 1);
    } else {
        if (setups && std::memcmp(b.setups.data() + (setups - 1) * kSetupWords, setup, kSetupWords * 4) == 0 &&
            std::memcmp(b.mips.data() + (setups - 1) * kMipWords, cache.mip, kMipWords * 4) == 0) {
            setup_index = static_cast<std::uint32_t>(setups - 1);
        } else {
            setup_index = static_cast<std::uint32_t>(setups);
            b.setups.insert(b.setups.end(), setup, setup + kSetupWords);
            b.mips.insert(b.mips.end(), cache.mip, cache.mip + kMipWords);
            if (cache.mip[0]) {
                b.any_mip = true;
                ++(cache.mip_top ? g_mip_setups : g_filter_setups);
            }
        }
        b.setup_serial = cache.serial;
    }
    if (cache.mip[0]) ++(cache.mip_top ? g_mip_shapes : g_filter_shapes);
    if (flags & 2u) {
        if (g_batch_reads.empty() || g_batch_reads.back().lo != tex.lo || g_batch_reads.back().hi != tex.hi) {
            bool known = false;
            for (const Range &t : g_batch_reads) known = known || (t.lo <= tex.lo && tex.hi <= t.hi);
            if (!known) g_batch_reads.push_back(tex);
        }
    }
    // Upscaled, a bilinear texture the GS drew itself (a copy of the picture for an effect) holds SCALE x SCALE samples per texel: the
    // shader then filters between those samples instead of between whole texels (kUpscaledSource, sample_upscaled in gs.wgsl). Only
    // the texels this shape reads count: a big texture may reach round to the frame buffer without reading it.
    if (g_scale > 1 && (flags & 2u) && (flags & 16384u)) {
        int rect[4];
        if (texel_rect(setup, v, count, prim, fst, rect)) {
            const Range used = region(setup[12], setup[13], setup[14], rect[0], rect[1], rect[2], rect[3]);
            if (rendered(pages(setup[12], setup[13], setup[14], rect[0], rect[1], rect[2], rect[3])) ||
                (!b.prims.empty() && used.overlaps(b.fb_range.lo, b.fb_range.hi)))
                rec[1] |= kUpscaledSource;
        }
    }
    rec[0] = setup_index;
    rec[2] = static_cast<std::uint32_t>(minx) | (static_cast<std::uint32_t>(miny) << 16);
    rec[3] = static_cast<std::uint32_t>(maxx) | (static_cast<std::uint32_t>(maxy) << 16);
    const bool fresh_batch = b.prims.empty();
    b.prims.append(rec, kPrimWords);
    // Every kStreamStep shapes of a big batch go to the tile helper at once (tiles.hpp), so its lists are mostly built when the batch ends.
    if (const std::size_t words = b.prims.size(); words % (kPrimWords * tiles::kStreamStep) == 0) {
        const std::uint32_t shapes = static_cast<std::uint32_t>(words / kPrimWords);
        if (g_stream) tiles::feed(g_stream, b.prims.data(), shapes);
        else if (tiles_to_helper() && tiles::backlog() < tiles::kMaxBacklog) g_stream = tiles::open(b.prims.data(), shapes);
    }
    exact_follow(rec, setup);
    if (g_live && b.route == 1) hw::live_shape(cache.look, setup, rec, fresh_batch);  // the card's checks start now
    b.minx = std::min(b.minx, minx);
    b.miny = std::min(b.miny, miny);
    b.maxx = std::max(b.maxx, maxx);
    b.maxy = std::max(b.maxy, maxy);
    if (b.prims.size() >= kPrimWords * 65536) {
        g_why = kWhySize;
        emit_all();
    }
}

void image_begin(const State &s) {
    flush_image();
    Image &im = g_image;
    const std::uint64_t buf = s.bitbltbuf, pos = s.trxpos, reg = s.trxreg;
    im = Image{};
    im.dbp = static_cast<std::uint32_t>(buf >> 32) & 0x3FFFu;
    im.dbw = static_cast<std::uint32_t>(buf >> 48) & 0x3Fu;
    im.dpsm = static_cast<std::uint32_t>(buf >> 56) & 0x3Fu;
    im.dsax = static_cast<std::uint32_t>(pos >> 32) & 0x7FFu;
    im.dsay = static_cast<std::uint32_t>(pos >> 48) & 0x7FFu;
    im.rrw = static_cast<std::uint32_t>(reg) & 0xFFFu;
    im.total = im.rrw * (static_cast<std::uint32_t>(reg >> 32) & 0xFFFu);
    im.bpp = bits_per_pixel(im.dpsm);
    im.id = ++g_image_ids ? g_image_ids : ++g_image_ids;
    im.active = im.total != 0;
    if (stat::g_on && im.active) stat::upload(im.dbp, im.dbw, im.dpsm, static_cast<int>(im.dsax), static_cast<int>(im.dsay), im.rrw, im.total / im.rrw, static_cast<std::uint64_t>(im.total) * im.bpp / 8);
}

void image_data(const std::uint8_t *data, std::size_t bytes) {
    Image &im = g_image;
    if (!im.active) return;
    if (im.pending.empty() && !im.lead_bits) {
        const std::uint64_t used = queue_upload(data, bytes, 0);
        const std::size_t whole = static_cast<std::size_t>(used / 8);
        if (im.done < im.total && whole < bytes) im.pending.assign(data + whole, data + bytes);
        im.lead_bits = static_cast<std::uint32_t>(used % 8);
        finish_image_if_done();
        return;
    }
    im.pending.insert(im.pending.end(), data, data + bytes);
}

void local_copy(const State &s) {
    g_why = kWhyCopy;
    flush_all();
    g_why = kWhyFlush;
    const std::uint64_t buf = s.bitbltbuf, pos = s.trxpos, reg = s.trxreg;
    const std::uint32_t w = static_cast<std::uint32_t>(reg) & 0xFFFu, h = static_cast<std::uint32_t>(reg >> 32) & 0xFFFu;
    if (!w || !h) return;
    if (g_live) {  // the copy reads the source rectangle and overwrites the destination rectangle
        const int sx = static_cast<int>(pos) & 0x7FF, sy = static_cast<int>(pos >> 16) & 0x7FF, dx = static_cast<int>(pos >> 32) & 0x7FF, dy = static_cast<int>(pos >> 48) & 0x7FF;
        const Range from = pages(static_cast<std::uint32_t>(buf) & 0x3FFFu, static_cast<std::uint32_t>(buf >> 16) & 0x3Fu, static_cast<std::uint32_t>(buf >> 24) & 0x3Fu, sx, sy, sx + static_cast<int>(w) - 1, sy + static_cast<int>(h) - 1);
        const Range to = pages(static_cast<std::uint32_t>(buf >> 32) & 0x3FFFu, static_cast<std::uint32_t>(buf >> 48) & 0x3Fu, static_cast<std::uint32_t>(buf >> 56) & 0x3Fu, dx, dy, dx + static_cast<int>(w) - 1, dy + static_cast<int>(h) - 1);
        hw::live_position(static_cast<std::uint32_t>(g_commands.size()));
        hw::live_reader(from.lo, from.hi);
        hw::live_writer(to.lo, to.hi);
        hw::pal_write(static_cast<std::uint32_t>(buf >> 32) & 0x3FFFu, static_cast<std::uint32_t>(buf >> 48) & 0x3Fu, static_cast<std::uint32_t>(buf >> 56) & 0x3Fu, dx, dy, dx + static_cast<int>(w) - 1, dy + static_cast<int>(h) - 1);
    }
    if (hwcache::g_on)
        hwcache::write_region(static_cast<std::uint32_t>(buf >> 32) & 0x3FFFu, static_cast<std::uint32_t>(buf >> 48) & 0x3Fu, static_cast<std::uint32_t>(buf >> 56) & 0x3Fu,
                              static_cast<int>(pos >> 32) & 0x7FF, static_cast<int>(pos >> 48) & 0x7FF, (static_cast<int>(pos >> 32) & 0x7FF) + static_cast<int>(w) - 1,
                              (static_cast<int>(pos >> 48) & 0x7FF) + static_cast<int>(h) - 1, hwcache::kCopy);
    if (stat::g_on) stat::local_copy(static_cast<std::uint32_t>(buf >> 32) & 0x3FFFu, static_cast<std::uint32_t>(buf >> 48) & 0x3Fu, static_cast<std::uint32_t>(buf >> 56) & 0x3Fu,
                                     static_cast<int>(pos >> 32) & 0x7FF, static_cast<int>(pos >> 48) & 0x7FF, w, h);
    mark_rendered(pages(static_cast<std::uint32_t>(buf >> 32) & 0x3FFFu, static_cast<std::uint32_t>(buf >> 48) & 0x3Fu, static_cast<std::uint32_t>(buf >> 56) & 0x3Fu,
                        static_cast<int>(pos >> 32) & 0x7FF, static_cast<int>(pos >> 48) & 0x7FF, (static_cast<int>(pos >> 32) & 0x7FF) + static_cast<int>(w) - 1,
                        (static_cast<int>(pos >> 48) & 0x7FF) + static_cast<int>(h) - 1));
    if (g_dec_on)
        stamp_pages(pages(static_cast<std::uint32_t>(buf >> 32) & 0x3FFFu, static_cast<std::uint32_t>(buf >> 48) & 0x3Fu, static_cast<std::uint32_t>(buf >> 56) & 0x3Fu,
                          static_cast<int>(pos >> 32) & 0x7FF, static_cast<int>(pos >> 48) & 0x7FF, (static_cast<int>(pos >> 32) & 0x7FF) + static_cast<int>(w) - 1,
                          (static_cast<int>(pos >> 48) & 0x7FF) + static_cast<int>(h) - 1),
                    true);
    const std::uint32_t read = push_params({static_cast<std::uint32_t>(buf) & 0x3FFFu, static_cast<std::uint32_t>(buf >> 16) & 0x3Fu, static_cast<std::uint32_t>(buf >> 24) & 0x3Fu,
                                            static_cast<std::uint32_t>(pos) & 0x7FFu, static_cast<std::uint32_t>(pos >> 16) & 0x7FFu, w, h});
    const std::uint32_t write = push_params({static_cast<std::uint32_t>(buf >> 32) & 0x3FFFu, static_cast<std::uint32_t>(buf >> 48) & 0x3Fu, static_cast<std::uint32_t>(buf >> 56) & 0x3Fu,
                                             static_cast<std::uint32_t>(pos >> 32) & 0x7FFu, static_cast<std::uint32_t>(pos >> 48) & 0x7FFu, w, h,
                                             static_cast<std::uint32_t>(pos >> 59) & 3u});
    dispatch_linear(kCopyRead, w * h * g_scale * g_scale, read);
    dispatch_linear(kCopyWrite, w * h * g_scale * g_scale, write);
}

void load_clut(std::uint64_t tex0, std::uint64_t texclut) {
    flush_image();
    if (g_clut_loads + 2 >= kClutSlots) {
        emit_all();
        submit();
    }
    const std::uint32_t psm = static_cast<std::uint32_t>(tex0 >> 20) & 0x3Fu;
    const std::uint32_t cpsm = (static_cast<std::uint32_t>(tex0 >> 51) & 0xFu) == 0 ? PSMCT32 : static_cast<std::uint32_t>(tex0 >> 51) & 0xFu;
    const std::uint32_t next = (g_clut_slot + 1) % kClutSlots;
    const std::uint32_t tc = static_cast<std::uint32_t>(texclut);
    const std::uint32_t cbp = static_cast<std::uint32_t>(tex0 >> 37) & 0x3FFFu, csm = static_cast<std::uint32_t>(tex0 >> 55) & 1u;
    const std::uint32_t cbw = tc & 0x3Fu, cou = (tc >> 6) & 0x3Fu, cov = (tc >> 12) & 0x3FFu;
    if (stat::g_on) stat::clut_load();
    PreOp op;
    op.clut = true;
    const std::uint32_t rec[10] = {g_clut_slot, next, cbp, cpsm, csm, static_cast<std::uint32_t>(tex0 >> 56) & 0x1Fu, (psm == PSMT8 || psm == PSMT8H) ? 1u : 0u, cbw, cou, cov};
    std::memcpy(op.rec, rec, sizeof rec);
    {  // word 10: the slot entries the load writes, for load_clut in gs.wgsl (clut_entry): the first, and how many from there (wrapping at 512)
        const bool eight = rec[6] != 0;
        const std::uint32_t csa = rec[5], start = csm ? (csa * 16u) & 511u : eight ? 0u : (csa & 15u) * 16u;
        op.rec[10] = start | (eight ? 256u : 16u) << 16;
    }
    op.range = csm ? region(cbp, cbw, cpsm, static_cast<int>(cou * 16), static_cast<int>(cov), static_cast<int>(cou * 16 + 255), static_cast<int>(cov))
                   : region(cbp, 1, cpsm, 0, 0, 15, 15);
    queue_pre(op);
    if (g_dec_on) id_clut(op.rec);
    if (g_live) hw::pal_clut(g_clut_slot, next, cbp, cpsm, csm, static_cast<std::uint32_t>(tex0 >> 56) & 0x1Fu, psm == PSMT8 || psm == PSMT8H);  // after queue_pre (a pending batch that overwrites the palette was flushed by it)
    if (hwcache::g_on) hwcache::clut_load(g_clut_slot, next, cbp, cpsm, csm, static_cast<std::uint32_t>(tex0 >> 56) & 0x1Fu, (psm == PSMT8 || psm == PSMT8H) ? 1u : 0u, cbw, cou, cov);
    g_clut_slot = next;
    ++g_clut_loads;
}

void texflush() { ++g_texflushes; }

void flush() {
    if (g_mode == Mode::off) return;
    flush_all();
    submit();
    wt::frame();
    if (stat::g_on) stat::frame_end();
    dec_stat_frame();
    if (hwcache::g_on && hwcache::counting()) {
        if (hwcache::verify_on()) {
            static std::vector<std::uint8_t> memory(kMemorySize);
            download(memory.data(), 0, kMemorySize);
            hwcache::verify(memory.data());
        }
        hwcache::frame_end();
    }
    if (g_batch_log) {
        std::uint32_t runs = 0, prims = 0;
        for (int w = 0; w < kWhys; ++w) runs += g_why_runs[w], prims += g_why_prims[w];
        if (runs) {
            std::fprintf(stderr, "[batch] frame %llu: %u runs, %u shapes, ended by:", static_cast<unsigned long long>(frame_count()), runs, prims);
            for (int w = 0; w < kWhys; ++w)
                if (g_why_runs[w]) std::fprintf(stderr, " %s %u (%u shapes)", kWhyName[w], g_why_runs[w], g_why_prims[w]);
            std::fprintf(stderr, "; word test: %u asked, %u joined, %u real overlaps, %u not possible, processor time %.3f ms (asking %.3f, following %.3f)", g_exact.tests, g_exact.merged, g_exact.hazards, g_exact.refused, static_cast<double>(g_exact.ns[0] + g_exact.ns[1]) / 1e6, static_cast<double>(g_exact.ns[0]) / 1e6, static_cast<double>(g_exact.ns[1]) / 1e6);
            std::fprintf(stderr, "\n");
        }
        for (int w = 0; w < kWhys; ++w) g_why_runs[w] = g_why_prims[w] = 0;
        g_exact.tests = g_exact.merged = g_exact.hazards = g_exact.refused = 0;
        g_exact.ns[0] = g_exact.ns[1] = 0;
    }
    if (!g_bench.configured || g_bench.done) return;
    ++g_vblanks;
    if (g_bench.capturing) bench_run();
    else if (g_vblanks + 1 == g_bench.vblank) bench_start();
}

void wait_idle() {
    flush_all();
    submit();
    post::drain();
    bool done = false;
    WGPUQueueWorkDoneCallbackInfo cb = WGPU_QUEUE_WORK_DONE_CALLBACK_INFO_INIT;
    cb.mode = WGPUCallbackMode_AllowProcessEvents;
    cb.callback = [](WGPUQueueWorkDoneStatus, WGPUStringView, void *flag, void *) { *static_cast<bool *>(flag) = true; };
    cb.userdata1 = &done;
    wgpuQueueOnSubmittedWorkDone(g_queue, cb);
    wait_until(done);
}

struct PendingDownload {
    WGPUBuffer buffer = nullptr;
    std::uint64_t capacity = 0;
    std::uint8_t *memory = nullptr;
    std::uint32_t offset = 0, bytes = 0, serial = 0;
    bool busy = false, ready = false;
} g_pending;

void collect_download() {
    if (!g_pending.busy || !g_pending.ready) return;
    const void *mapped = wgpuBufferGetConstMappedRange(g_pending.buffer, 0, g_pending.bytes);
    if (mapped) std::memcpy(g_pending.memory + g_pending.offset, mapped, g_pending.bytes);
    wgpuBufferUnmap(g_pending.buffer);
    g_pending.busy = g_pending.ready = false;
}

bool download_async(std::uint8_t *memory, std::uint32_t offset, std::uint32_t bytes, std::uint32_t serial) {
    collect_download();
    if (g_pending.busy) return false;
    if (g_pending.capacity < bytes) {
        if (g_pending.buffer) wgpuBufferRelease(g_pending.buffer);
        g_pending.capacity = bytes;
        g_pending.buffer = make_buffer(bytes, WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst);
    }
    flush_all();
    if (g_live) hw::live_position(static_cast<std::uint32_t>(g_commands.size())), hw::live_reader(offset, static_cast<std::uint64_t>(offset) + bytes);
    if (g_scale != 1) dispatch_linear(kResolve, bytes / 4, push_params({offset / 4}));
    submit();
    post::drain();
    wt::Scope s(wt::kCopy);
    WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(g_device, nullptr);
    wgpuCommandEncoderCopyBufferToBuffer(enc, g_scale == 1 ? g_vram : g_scratch, offset, g_pending.buffer, 0, bytes);
    WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(enc, nullptr);
    wgpuQueueSubmit(g_queue, 1, &cmd);
    wgpuCommandBufferRelease(cmd);
    wgpuCommandEncoderRelease(enc);
    g_pending.memory = memory;
    g_pending.offset = offset;
    g_pending.bytes = bytes;
    g_pending.serial = serial;
    g_pending.busy = true;
    WGPUBufferMapCallbackInfo cb = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
    cb.mode = WGPUCallbackMode_AllowProcessEvents;
    cb.callback = [](WGPUMapAsyncStatus status, WGPUStringView, void *, void *) {
        if (status == WGPUMapAsyncStatus_Success) g_pending.ready = true;
        else g_pending.busy = false;
    };
    wgpuBufferMapAsync(g_pending.buffer, WGPUMapMode_Read, 0, bytes, cb);
    return true;
}

bool finish_download(std::uint32_t serial) {
    if (!g_pending.busy || g_pending.serial != serial) return false;
    const bool diagnose = readback_log_enabled();
    const auto started = diagnose ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    wt::Scope s(wt::kWait);
    while (g_pending.busy && !g_pending.ready) pump();
    if (diagnose && log_slow_readback(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count())) std::fprintf(stderr, "[readback] async frame %llu serial %u bytes %u: wait %.3f ms\n",
        static_cast<unsigned long long>(frame_count()), serial, g_pending.bytes,
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count());
    const bool done = g_pending.ready;
    collect_download();
    return done;
}

void download(std::uint8_t *memory, std::uint32_t offset, std::uint32_t bytes) {
    flush_all();
    if (g_live) hw::live_position(static_cast<std::uint32_t>(g_commands.size())), hw::live_reader(offset, static_cast<std::uint64_t>(offset) + bytes);
    if (g_scale == 1) {
        submit();
        read_back(g_vram, bytes, memory + offset, offset);
        return;
    }
    dispatch_linear(kResolve, bytes / 4, push_params({offset / 4}));
    submit();
    read_back(g_scratch, bytes, memory + offset, offset);
}

void read_state(std::vector<std::uint32_t> &vram, std::uint32_t &scale, std::vector<std::uint32_t> &clut) {
    flush_all();
    if (g_live) hw::live_position(static_cast<std::uint32_t>(g_commands.size())), hw::live_reader(0, kMemorySize);  // states are the judge: everything the card holds goes back first
    submit();
    scale = g_scale;
    vram.resize(static_cast<std::size_t>(kMemorySize / 4) * g_scale * g_scale);
    read_back(g_vram, vram.size() * 4, vram.data());
    clut.resize(kClutEntries);
    read_back(g_clut, kClutEntries * 4, clut.data(), static_cast<std::uint64_t>(g_clut_slot) * kClutEntries * 4);
}

void write_state(const std::vector<std::uint32_t> &vram, std::uint32_t scale, const std::vector<std::uint32_t> &clut) {
    wait_idle();
    if (stat::g_on) stat::state_loaded();
    if (hwcache::g_on) hwcache::state_loaded();
    dec_reset();  // memory and palette are new: nothing kept is valid
    const std::size_t words = kMemorySize / 4;
    const std::uint32_t from = scale * scale, to = g_scale * g_scale;
    // A page counts as drawn by the GS when its samples differ within a texel; one that is the same in all of them was uploaded (or
    // is flat, which reads the same either way).
    std::fill(std::begin(g_rendered), std::end(g_rendered), false);
    if (from == to && to > 1 && vram.size() == words * to)
        for (std::size_t page = 0; page < 512; ++page)
            for (std::size_t w = page * 2048; w < (page + 1) * 2048 && !g_rendered[page]; ++w)
                for (std::uint32_t k = 1; k < to; ++k)
                    if (vram[w * to + k] != vram[w * to]) {
                        g_rendered[page] = true;
                        break;
                    }
    if (from == to && vram.size() == words * to) {
        wgpuQueueWriteBuffer(g_queue, g_vram, 0, vram.data(), vram.size() * 4);
    } else {
        std::vector<std::uint32_t> layers(words * to);
        for (std::size_t w = 0; w < words; ++w)
            for (std::uint32_t k = 0; k < to; ++k) layers[w * to + k] = vram[w * from];
        wgpuQueueWriteBuffer(g_queue, g_vram, 0, layers.data(), layers.size() * 4);
    }
    if (clut.size() >= kClutEntries) wgpuQueueWriteBuffer(g_queue, g_clut, static_cast<std::uint64_t>(g_clut_slot) * kClutEntries * 4, clut.data(), kClutEntries * 4);
    if (g_live) hw::pal_state(from == to && to == 1 && vram.size() == words ? vram.data() : nullptr, vram.size());
    if (g_live) hw::live_state_loaded();  // memory is the truth: the card's pictures are dropped, nothing is written back
    wait_idle();
}

void stats() {
    if (g_mode == Mode::off) return;
    if (g_live) hw::live_stats();
    if (tiles::owner()) finish_tiles(true);  // the lists of the batches that were flushed but not submitted yet count in the fingerprint below; not from the emulation thread,
                                             // which calls this at the end of a run while the window thread may still be placing jobs (a crash at the end, 2026-11-11)
    tiles::report();
    stat::report();
    hwcache::report();
    std::fprintf(stderr, "[gpu] %llu submits, dispatches:", static_cast<unsigned long long>(g_submits));
    for (int i = 0; i < kPipelines; ++i) std::fprintf(stderr, " %s %llu", kEntryPoints[i], static_cast<unsigned long long>(g_dispatches[i]));
    std::fprintf(stderr, "\n");
    std::fprintf(stderr, "[gpu] mipmaps: %llu setups (%llu shapes) read levels above 0, %llu setups (%llu shapes) only pick the filter per pixel\n",
                 static_cast<unsigned long long>(g_mip_setups), static_cast<unsigned long long>(g_mip_shapes), static_cast<unsigned long long>(g_filter_setups),
                 static_cast<unsigned long long>(g_filter_shapes));
}

void configure_surface() {
    post::drain();  // no present of the old swap chain may be in flight
    WGPUSurfaceConfiguration config = WGPU_SURFACE_CONFIGURATION_INIT;
    config.device = g_device;
    config.format = g_surface_format;
    config.usage = WGPUTextureUsage_RenderAttachment;
    config.width = std::max(g_surface_width, 1u);
    config.height = std::max(g_surface_height, 1u);
    config.presentMode = g_present_mode;
    config.alphaMode = WGPUCompositeAlphaMode_Auto;
#ifndef __EMSCRIPTEN__
    WGPUSurfaceConfigurationExtras extras = {};
    if (g_frame_latency) {
        extras.chain.sType = static_cast<WGPUSType>(WGPUSType_SurfaceConfigurationExtras);
        extras.desiredMaximumFrameLatency = g_frame_latency;
        config.nextInChain = &extras.chain;
    }
#endif
    wgpuSurfaceConfigure(g_surface, &config);
}

const char *present_mode_name(WGPUPresentMode mode) {
    switch (mode) {
    case WGPUPresentMode_Fifo: return "Fifo";
    case WGPUPresentMode_FifoRelaxed: return "FifoRelaxed";
    case WGPUPresentMode_Immediate: return "Immediate";
    case WGPUPresentMode_Mailbox: return "Mailbox";
    default: return "?";
    }
}

bool attach_surface(const SurfaceTarget &target) {
    if (g_mode != Mode::primary || !g_device) return false;
    post::drain();
    WGPUSurfaceDescriptor sd = WGPU_SURFACE_DESCRIPTOR_INIT;
#ifdef __EMSCRIPTEN__
    WGPUEmscriptenSurfaceSourceCanvasHTMLSelector canvas = WGPU_EMSCRIPTEN_SURFACE_SOURCE_CANVAS_HTML_SELECTOR_INIT;
    canvas.selector = view(target.selector ? target.selector : "#canvas");
    sd.nextInChain = &canvas.chain;
#elif defined(_WIN32)
    WGPUSurfaceSourceWindowsHWND hwnd = WGPU_SURFACE_SOURCE_WINDOWS_HWND_INIT;
    if (target.kind != SurfaceTarget::Win32) return false;
    hwnd.hinstance = target.display;
    hwnd.hwnd = target.surface;
    sd.nextInChain = &hwnd.chain;
#elif defined(__APPLE__)
    if (target.kind != SurfaceTarget::Metal || !target.surface) return false;
    WGPUSurfaceSourceMetalLayer metal = WGPU_SURFACE_SOURCE_METAL_LAYER_INIT;
    metal.layer = target.surface;
    sd.nextInChain = &metal.chain;
#else
    WGPUSurfaceSourceXlibWindow xlib = WGPU_SURFACE_SOURCE_XLIB_WINDOW_INIT;
    WGPUSurfaceSourceWaylandSurface wayland = WGPU_SURFACE_SOURCE_WAYLAND_SURFACE_INIT;
    if (target.kind == SurfaceTarget::X11) {
        xlib.display = target.display;
        xlib.window = target.window;
        sd.nextInChain = &xlib.chain;
    } else if (target.kind == SurfaceTarget::Wayland) {
        wayland.display = target.display;
        wayland.surface = target.surface;
        sd.nextInChain = &wayland.chain;
    } else {
        return false;
    }
#endif
    g_surface = wgpuInstanceCreateSurface(g_instance, &sd);
    if (!g_surface) return false;
    WGPUSurfaceCapabilities caps = WGPU_SURFACE_CAPABILITIES_INIT;
    if (wgpuSurfaceGetCapabilities(g_surface, g_adapter, &caps) != WGPUStatus_Success || !caps.formatCount) {
        wgpuSurfaceRelease(g_surface);
        g_surface = nullptr;
        return false;
    }
    g_surface_format = caps.formats[0];
    for (std::size_t i = 0; i < caps.formatCount; ++i)
        if (caps.formats[i] == WGPUTextureFormat_BGRA8Unorm || caps.formats[i] == WGPUTextureFormat_RGBA8Unorm) {
            g_surface_format = caps.formats[i];
            break;
        }
    g_present_mode = WGPUPresentMode_Fifo;
    for (const WGPUPresentMode wanted : {WGPUPresentMode_Immediate, WGPUPresentMode_Mailbox})
        for (std::size_t i = 0; i < caps.presentModeCount; ++i)
            if (caps.presentModes[i] == wanted) g_present_mode = wanted;
    // PS2_PRESENT_MODE=fifo, relaxed, mailbox or immediate asks for one mode (when the surface offers it); PS2_FRAME_LATENCY=1..8 sets how many
    // pictures the swap chain keeps in flight. Both are for experiments (docs/speed.md); the log line below says what is used.
    if (std::string want = os::env("PS2_PRESENT_MODE"); !want.empty()) {
        for (char &c : want) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        const WGPUPresentMode asked = want == "fifo" ? WGPUPresentMode_Fifo : want == "relaxed" ? WGPUPresentMode_FifoRelaxed : want == "mailbox" ? WGPUPresentMode_Mailbox : want == "immediate" ? WGPUPresentMode_Immediate : WGPUPresentMode_Undefined;
        bool offered = false;
        for (std::size_t i = 0; i < caps.presentModeCount; ++i) offered |= caps.presentModes[i] == asked;
        if (offered) g_present_mode = asked;
        else std::fprintf(stderr, "[gpu] PS2_PRESENT_MODE=%s is not fifo, relaxed, mailbox or immediate, or this surface does not offer it: ignored\n", want.c_str());
    }
    if (const int latency = std::atoi(os::env("PS2_FRAME_LATENCY").c_str()); latency >= 1 && latency <= 8) g_frame_latency = static_cast<std::uint32_t>(latency);
    {
        std::string offers;
        for (std::size_t i = 0; i < caps.presentModeCount; ++i) offers += std::string(i ? ", " : "") + present_mode_name(caps.presentModes[i]);
        std::fprintf(stderr, "[gpu] presenting with %s (the surface offers %s), frame latency %s\n", present_mode_name(g_present_mode), offers.c_str(),
                     g_frame_latency ? std::to_string(g_frame_latency).c_str() : "default");
    }
    wgpuSurfaceCapabilitiesFreeMembers(caps);
    g_surface_width = target.width;
    g_surface_height = target.height;
    configure_surface();

    WGPUShaderSourceWGSL source = WGPU_SHADER_SOURCE_WGSL_INIT;
    source.code = view(kBlitShader);
    WGPUShaderModuleDescriptor md = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
    md.nextInChain = &source.chain;
    WGPUShaderModule module = wgpuDeviceCreateShaderModule(g_device, &md);
    WGPUBindGroupLayoutEntry le[2] = {WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT, WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT};
    le[0].binding = 0;
    le[0].visibility = WGPUShaderStage_Fragment;
    le[0].buffer.type = WGPUBufferBindingType_ReadOnlyStorage;
    le[1].binding = 1;
    le[1].visibility = WGPUShaderStage_Fragment;
    le[1].buffer.type = WGPUBufferBindingType_Uniform;
    WGPUBindGroupLayoutDescriptor ld = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
    ld.entryCount = 2;
    ld.entries = le;
    WGPUBindGroupLayout layout = wgpuDeviceCreateBindGroupLayout(g_device, &ld);
    WGPUPipelineLayoutDescriptor pld = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
    pld.bindGroupLayoutCount = 1;
    pld.bindGroupLayouts = &layout;
    WGPUPipelineLayout pipeline_layout = wgpuDeviceCreatePipelineLayout(g_device, &pld);
    WGPUColorTargetState color = WGPU_COLOR_TARGET_STATE_INIT;
    color.format = g_surface_format;
    WGPUFragmentState fragment = WGPU_FRAGMENT_STATE_INIT;
    fragment.module = module;
    fragment.entryPoint = view("fs");
    fragment.targetCount = 1;
    fragment.targets = &color;
    WGPURenderPipelineDescriptor rd = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
    rd.layout = pipeline_layout;
    rd.vertex.module = module;
    rd.vertex.entryPoint = view("vs");
    rd.fragment = &fragment;
    g_blit = wgpuDeviceCreateRenderPipeline(g_device, &rd);
    wgpuShaderModuleRelease(module);
    wgpuPipelineLayoutRelease(pipeline_layout);
    g_blit_params = make_buffer(32, WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst);
    WGPUBindGroupEntry be[2] = {WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT};
    be[0].binding = 0;
    be[0].buffer = g_scratch;
    be[0].size = WGPU_WHOLE_SIZE;
    be[1].binding = 1;
    be[1].buffer = g_blit_params;
    be[1].size = 32;
    WGPUBindGroupDescriptor bd = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    bd.layout = layout;
    bd.entryCount = 2;
    bd.entries = be;
    g_blit_group = wgpuDeviceCreateBindGroup(g_device, &bd);
    wgpuBindGroupLayoutRelease(layout);
    return g_blit != nullptr;
}

void resize_surface(std::uint32_t width, std::uint32_t height) {
    if (!g_surface || (width == g_surface_width && height == g_surface_height)) return;
    g_surface_width = width;
    g_surface_height = height;
    configure_surface();
}

bool has_surface() { return g_surface != nullptr; }

// PS2_PRESENT_LOG=1: once a second, where the window thread spent its time handing pictures to the screen (milliseconds per picture).
// "acquire" is the wait for the next picture of the swap chain: it is long when the graphics chip is still busy with the earlier ones.
struct PresentLog {
    using Clock = std::chrono::steady_clock;
    bool report = std::getenv("PS2_PRESENT_LOG") != nullptr;
    bool on = report || ps2::hitch::on();  // PS2_HITCH_LOG times the steps of every picture too
    Clock::time_point since = Clock::now(), mark = since;
    double flush = 0, acquire = 0, submit = 0, present = 0;
    std::uint32_t pictures = 0;
    void lap(double &into) {
        const auto now = Clock::now();
        into += std::chrono::duration<double, std::milli>(now - mark).count();
        mark = now;
    }
    void picture() {
        if (!report) return;
        ++pictures;
        const auto now = Clock::now();
        const double seconds = std::chrono::duration<double>(now - since).count();
        if (seconds < 1.0) return;
        std::fprintf(stderr, "[present] %u pictures in %.2f s; per picture: flush %.2f ms, acquire %.2f ms, submit %.2f ms, present %.2f ms (%s)\n", pictures, seconds,
                     flush / pictures, acquire / pictures, submit / pictures, present / pictures, present_mode_name(g_present_mode));
        since = now;
        flush = acquire = submit = present = 0;
        pictures = 0;
    }
};

// Getting the next picture of the swap chain can block: on NVIDIA with Wayland the driver waits for the compositor to give a buffer back.
// wgpu-native (wgpu-core, FRAME_TIMEOUT_MS) waits at most 1 s inside and then answers Timeout; Outdated and Lost mean the swap chain has
// to be made again; Error means it is not configured; Occluded (Metal only) means the window is covered and nothing was acquired.
// A picture that did not come is dropped (the game goes on, the screen keeps the old picture); the swap chain is made again only for
// Outdated, Lost and Error, or after kTimeoutsBeforeReconfigure Timeouts in a row. Every problem (and every wait over kSlowMs) leaves a
// line in the log: the first 20, then every 100th.
constexpr double kSlowMs = 500.0;
constexpr unsigned kTimeoutsBeforeReconfigure = 3;

const char *acquire_status_name(unsigned status) {
    switch (status) {
    case WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal: return "SuccessOptimal";
    case WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal: return "SuccessSuboptimal";
    case WGPUSurfaceGetCurrentTextureStatus_Timeout: return "Timeout";
    case WGPUSurfaceGetCurrentTextureStatus_Outdated: return "Outdated";
    case WGPUSurfaceGetCurrentTextureStatus_Lost: return "Lost";
    case WGPUSurfaceGetCurrentTextureStatus_Error: return "Error";
#ifndef __EMSCRIPTEN__
    case WGPUSurfaceGetCurrentTextureStatus_Occluded: return "Occluded";
#endif
    default: return "unknown";
    }
}

// Notes one acquire (status and how long it took); returns true when the swap chain should be configured again.
bool note_acquire(unsigned status, double ms) {
    static unsigned problems = 0, timeouts_in_row = 0;
    const bool success = status == WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal || status == WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal;
    timeouts_in_row = status == WGPUSurfaceGetCurrentTextureStatus_Timeout ? timeouts_in_row + 1 : 0;
    bool reconfigure = false;
    if (!success) {
        reconfigure = status != WGPUSurfaceGetCurrentTextureStatus_Timeout
#ifndef __EMSCRIPTEN__
                      && status != WGPUSurfaceGetCurrentTextureStatus_Occluded
#endif
                      ;
        if (timeouts_in_row >= kTimeoutsBeforeReconfigure) {
            reconfigure = true;
            timeouts_in_row = 0;
        }
    }
    if (!success || ms > kSlowMs) {
        if (problems < 20 || problems % 100 == 0)
            std::fprintf(stderr, "[present] getting the next screen picture: status %s after %.0f ms (problem %u)%s%s\n", acquire_status_name(status), ms, problems + 1,
                         success ? "" : ": this picture is dropped", reconfigure ? ", the swap chain is made again" : "");
        ++problems;
    }
    return reconfigure;
}

// PS2_PRESENT_FAKE=<status>:<n> (a test aid) makes every n-th picture answer with that status number (3 Timeout, 4 Outdated, 5 Lost,
// 6 Error) without asking the driver, to check the handling above. Status 0 stands still for 12 s inside the wait instead (a stuck
// driver: the watchdog must say where the window thread is) and then asks the driver as usual.
struct FakeStatus {
    unsigned status = 0, every = 0, count = 0;
    FakeStatus() {
        if (const char *t = std::getenv("PS2_PRESENT_FAKE")) std::sscanf(t, "%u:%u", &status, &every);
    }
    bool hit() { return every && ++count % every == 0; }
};

// The parameters of the display kernel (gs.wgsl): the picture of the main read circuit, where to put it in the scratch buffer, and the
// rectangle of the second read circuit that goes over it (all zero when there is none).
// The display kernel's work: the picture comes from a card target when one holds it current, else from memory (after what the card holds newer there is written back).
void push_display(const DisplaySource &src, std::uint32_t scratch_at, std::uint32_t param) {
    if (g_live) {
        const DisplayOverlay &o = src.overlay;
        hw::live_position(static_cast<std::uint32_t>(g_commands.size()));
        const std::uint32_t words[19] = {src.fbp, src.fbw, src.psm, src.dbx, src.dby, src.width, src.height, src.halved ? 1u : 0u, scratch_at,
                                         o.active ? 1u : 0u, o.fbp, o.fbw, o.psm, o.dbx, o.dby, static_cast<std::uint32_t>(o.x), static_cast<std::uint32_t>(o.y), o.width, o.height};
        const hw::LiveDisplay d{src.fbp, src.fbw, src.psm, src.dbx, src.dby, src.width, src.height, scratch_at, src.halved, o.active};
        if (hw::live_display(d, words, 19)) return;
        const int rows = src.halved ? static_cast<int>((src.height + 1) / 2) : static_cast<int>(src.height);
        const Range r = pages(src.fbp, src.fbw, src.psm, static_cast<int>(src.dbx), static_cast<int>(src.dby), static_cast<int>(src.dbx + src.width) - 1, static_cast<int>(src.dby) + rows - 1);
        hw::live_reader(r.lo, r.hi);
        if (o.active) {
            const Range q = pages(o.fbp, o.fbw, o.psm, static_cast<int>(o.dbx), static_cast<int>(o.dby), static_cast<int>(o.dbx + o.width) - 1, static_cast<int>(o.dby + o.height) - 1);
            hw::live_reader(q.lo, q.hi);
        }
    }
    g_commands.push_back({kDisplay, (src.width * g_scale + 7) / 8, (src.height * g_scale + 7) / 8, param});
}

std::uint32_t display_params(const DisplaySource &src, std::uint32_t scratch_at) {
    const DisplayOverlay &o = src.overlay;
    return push_params({src.fbp, src.fbw, src.psm, src.dbx, src.dby, src.width, src.height, src.halved ? 1u : 0u, scratch_at,
                        o.active ? 1u : 0u, o.fbp, o.fbw, o.psm, o.dbx, o.dby, static_cast<std::uint32_t>(o.x), static_cast<std::uint32_t>(o.y), o.width, o.height});
}

bool present(const DisplaySource &src) {
    if (!g_surface) return false;
    static PresentLog log;
    const double flush0 = log.flush, acquire0 = log.acquire, submit0 = log.submit, present0 = log.present;
    if (log.on) log.mark = PresentLog::Clock::now();
    flush_all();
    if (log.on) log.lap(log.flush);
    const std::uint32_t current = field_base(g_field), previous = field_base(g_field + 1);
    static const bool weave_off = std::getenv("PS2_NO_DEINTERLACE") != nullptr;
    const bool blend = src.halved && g_have_previous && g_last_width == src.width && g_last_height == src.height && !weave_off;
    const std::uint32_t param = display_params(src, current);
    push_display(src, current, param);
    const float sw = static_cast<float>(g_surface_width), sh = static_cast<float>(g_surface_height);
    const float aspect = g_options.widescreen ? 16.0f / 9.0f : 4.0f / 3.0f;
    const float vw = std::min(sw, sh * aspect), vh = vw / aspect;
    const std::uint32_t blit[8] = {src.width * g_scale, src.height * g_scale, current, previous, blend ? 1u : 0u, static_cast<std::uint32_t>(vw), static_cast<std::uint32_t>(vh), ps2::video_options()};
    post::drain();  // the last picture is presented before this one's acquire
    std::memcpy(post::g_blit, blit, sizeof blit);  // written by the next submit (its job with the submit thread), before the frame's other writes
    post::g_blit_pending = true;
    ++g_field;
    g_have_previous = true;
    g_last_width = src.width;
    g_last_height = src.height;
    if (log.on) log.lap(log.flush);
    static FakeStatus fake;
    WGPUSurfaceTexture st = WGPU_SURFACE_TEXTURE_INIT;
    const auto acquire_from = PresentLog::Clock::now();
    ps2::hitch::enter_present_stage("waiting for the next screen picture (wgpuSurfaceGetCurrentTexture)");
    const bool faked = fake.hit();
    if (faked && !fake.status) std::this_thread::sleep_for(std::chrono::seconds(12));
    if (faked && fake.status) st.status = static_cast<WGPUSurfaceGetCurrentTextureStatus>(fake.status);
    else {
        wt::Scope s(wt::kAcquire);
        wgpuSurfaceGetCurrentTexture(g_surface, &st);
    }
    ps2::hitch::enter_present_stage(nullptr);
    const double acquire_ms = std::chrono::duration<double, std::milli>(PresentLog::Clock::now() - acquire_from).count();
    if (log.on) log.lap(log.acquire);
    const bool reconfigure = note_acquire(st.status, acquire_ms);
    if (st.status != WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal && st.status != WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal) {
        if (st.texture) wgpuTextureRelease(st.texture);
        submit();  // the chip still gets the work; only the screen misses this picture
        if (reconfigure) configure_surface();
        return true;
    }
    WGPUTextureView target = wgpuTextureCreateView(st.texture, nullptr);
    const auto submit_from = PresentLog::Clock::now();
    ps2::hitch::enter_present_stage("submitting the picture (submit)");
    post::g_present_texture = st.texture;
    submit(target);
    post::g_present_texture = nullptr;
    if (log.on) log.lap(log.submit);
    if (!post::g_presented_async) {  // else the submit thread presents it and releases the texture (post)
#ifndef __EMSCRIPTEN__
        ps2::hitch::enter_present_stage("presenting the picture (wgpuSurfacePresent)");
        {
            wt::Scope s(wt::kPresent);
            wgpuSurfacePresent(g_surface);
        }
#endif
        ps2::hitch::enter_present_stage(nullptr);
        if (log.on) log.lap(log.present);
        if (const double ms = std::chrono::duration<double, std::milli>(PresentLog::Clock::now() - submit_from).count(); ms > kSlowMs) {
            static unsigned slow = 0;
            if (slow < 20 || slow % 100 == 0) std::fprintf(stderr, "[present] submitting and presenting a picture took %.0f ms (slow %u)\n", ms, slow + 1);
            ++slow;
        }
        wgpuTextureViewRelease(target);
        wgpuTextureRelease(st.texture);
    }
    ps2::hitch::enter_present_stage(nullptr);
    if (ps2::hitch::on()) {  // PS2_HITCH_LOG=<ms>: a picture whose hand-over took long, with the steps of it (docs/hitches.md)
        const double f = log.flush - flush0, a = log.acquire - acquire0, s = log.submit - submit0, p = log.present - present0;
        ps2::hitch::picture_times() = {f, a, s, p};
        if (f + a + s + p > ps2::hitch::threshold_ms())
            std::fprintf(stderr, "[hitch] hand-over of a picture took %.1f ms: flush %.1f, waiting for the next screen picture %.1f, submit %.1f, present %.1f\n", f + a + s + p, f, a, s, p);
        if (f + a + s + p > ps2::hitch::threshold_ms()) {
            const HitchWork *pictures[4] = {&g_hitch_now, &g_hitch_before[0], &g_hitch_before[1], &g_hitch_before[2]};
            for (int i = 0; i < 4; ++i) {
                const HitchWork &w = *pictures[i];
                std::fprintf(stderr, "[hitch]   given to the chip %d picture(s) ago: %u submits, %.2f MB of data;", i, w.submits, w.arena_bytes / 1048576.0);
                for (int k = 0; k < kPipelines; ++k)
                    if (w.commands[k]) std::fprintf(stderr, " %s %u cmds %llu groups;", kEntryPoints[k], w.commands[k], static_cast<unsigned long long>(w.groups[k]));
                std::fprintf(stderr, "\n");
            }
        }
        g_hitch_before[2] = g_hitch_before[1];
        g_hitch_before[1] = g_hitch_before[0];
        g_hitch_before[0] = g_hitch_now;
        g_hitch_now = HitchWork{};
    }
    log.picture();
    return true;
}

namespace {

// The overlay's shader: a triangle that covers the viewport (set to the panel's rectangle), each pixel read from the buffer.
constexpr const char *kOverlayShader = R"(
struct Rect { x: u32, y: u32, width: u32, height: u32 };
@group(0) @binding(0) var<storage, read> pixels: array<u32>;
@group(0) @binding(1) var<uniform> rect: Rect;
@vertex
fn vs(@builtin(vertex_index) i: u32) -> @builtin(position) vec4<f32> {
    let p = vec2<f32>(f32((i << 1u) & 2u), f32(i & 2u));
    return vec4<f32>(p * 2.0 - 1.0, 0.0, 1.0);
}
@fragment
fn fs(@builtin(position) at: vec4<f32>) -> @location(0) vec4<f32> {
    let x = min(u32(max(at.x - f32(rect.x), 0.0)), rect.width - 1u);
    let y = min(u32(max(at.y - f32(rect.y), 0.0)), rect.height - 1u);
    return unpack4x8unorm(pixels[y * rect.width + x]);
}
)";

bool make_overlay_pipeline() {
    WGPUShaderSourceWGSL source = WGPU_SHADER_SOURCE_WGSL_INIT;
    source.code = view(kOverlayShader);
    WGPUShaderModuleDescriptor md = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
    md.nextInChain = &source.chain;
    WGPUShaderModule module = wgpuDeviceCreateShaderModule(g_device, &md);
    WGPUBindGroupLayoutEntry le[2] = {WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT, WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT};
    le[0].binding = 0;
    le[0].visibility = WGPUShaderStage_Fragment;
    le[0].buffer.type = WGPUBufferBindingType_ReadOnlyStorage;
    le[1].binding = 1;
    le[1].visibility = WGPUShaderStage_Fragment;
    le[1].buffer.type = WGPUBufferBindingType_Uniform;
    WGPUBindGroupLayoutDescriptor ld = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
    ld.entryCount = 2;
    ld.entries = le;
    g_overlay.layout = wgpuDeviceCreateBindGroupLayout(g_device, &ld);
    WGPUPipelineLayoutDescriptor pld = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
    pld.bindGroupLayoutCount = 1;
    pld.bindGroupLayouts = &g_overlay.layout;
    WGPUPipelineLayout pipeline_layout = wgpuDeviceCreatePipelineLayout(g_device, &pld);
    WGPUBlendState blend = WGPU_BLEND_STATE_INIT;
    blend.color.operation = WGPUBlendOperation_Add;
    blend.color.srcFactor = WGPUBlendFactor_SrcAlpha;
    blend.color.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
    blend.alpha.operation = WGPUBlendOperation_Add;
    blend.alpha.srcFactor = WGPUBlendFactor_One;
    blend.alpha.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
    WGPUColorTargetState color = WGPU_COLOR_TARGET_STATE_INIT;
    color.format = g_surface_format;
    color.blend = &blend;
    WGPUFragmentState fragment = WGPU_FRAGMENT_STATE_INIT;
    fragment.module = module;
    fragment.entryPoint = view("fs");
    fragment.targetCount = 1;
    fragment.targets = &color;
    WGPURenderPipelineDescriptor rd = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
    rd.layout = pipeline_layout;
    rd.vertex.module = module;
    rd.vertex.entryPoint = view("vs");
    rd.fragment = &fragment;
    g_overlay.pipeline = wgpuDeviceCreateRenderPipeline(g_device, &rd);
    wgpuShaderModuleRelease(module);
    wgpuPipelineLayoutRelease(pipeline_layout);
    g_overlay.rect = make_buffer(16, WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst);
    if (!g_overlay.pipeline) std::fprintf(stderr, "[gpu] cannot make the overlay's pipeline: the quick menu is not shown\n");
    return g_overlay.pipeline != nullptr;
}

}  // namespace

void set_overlay(const std::uint32_t *rgba, std::uint32_t width, std::uint32_t height, std::uint32_t x, std::uint32_t y) {
    post::drain();
    if (!rgba || !width || !height || !g_surface) {
        g_overlay.on = false;
        return;
    }
    if (!g_overlay.pipeline && !make_overlay_pipeline()) return;
    const std::uint64_t bytes = static_cast<std::uint64_t>(width) * height * 4;
    if (bytes > g_overlay.capacity) {
        if (g_overlay.group) wgpuBindGroupRelease(g_overlay.group);
        if (g_overlay.pixels) wgpuBufferRelease(g_overlay.pixels);
        g_overlay.capacity = std::max<std::uint64_t>(bytes, 1u << 20);
        g_overlay.pixels = make_buffer(g_overlay.capacity, WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst);
        WGPUBindGroupEntry be[2] = {WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT};
        be[0].binding = 0;
        be[0].buffer = g_overlay.pixels;
        be[0].size = WGPU_WHOLE_SIZE;
        be[1].binding = 1;
        be[1].buffer = g_overlay.rect;
        be[1].size = 16;
        WGPUBindGroupDescriptor bd = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
        bd.layout = g_overlay.layout;
        bd.entryCount = 2;
        bd.entries = be;
        g_overlay.group = wgpuDeviceCreateBindGroup(g_device, &bd);
    }
    wgpuQueueWriteBuffer(g_queue, g_overlay.pixels, 0, rgba, bytes);
    const std::uint32_t rect[4] = {x, y, width, height};
    wgpuQueueWriteBuffer(g_queue, g_overlay.rect, 0, rect, sizeof rect);
    g_overlay.x = x;
    g_overlay.y = y;
    g_overlay.width = width;
    g_overlay.height = height;
    g_overlay.on = g_overlay.group != nullptr;
}

bool present_again() {
    if (!g_surface || !g_have_previous) return false;
    post::drain();
    // the same picture as the last present (its fields are still in the scratch buffer: the game draws nothing while it waits), with the
    // window's size and the picture look of now
    const float sw = static_cast<float>(g_surface_width), sh = static_cast<float>(g_surface_height);
    const float aspect = g_options.widescreen ? 16.0f / 9.0f : 4.0f / 3.0f;
    const float vw = std::min(sw, sh * aspect), vh = vw / aspect;
    const std::uint32_t tail[3] = {static_cast<std::uint32_t>(vw), static_cast<std::uint32_t>(vh), ps2::video_options()};
    wgpuQueueWriteBuffer(g_queue, g_blit_params, 20, tail, sizeof tail);
    WGPUSurfaceTexture st = WGPU_SURFACE_TEXTURE_INIT;
    wgpuSurfaceGetCurrentTexture(g_surface, &st);
    if (st.status != WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal && st.status != WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal) {
        if (st.texture) wgpuTextureRelease(st.texture);
        if (st.status == WGPUSurfaceGetCurrentTextureStatus_Outdated || st.status == WGPUSurfaceGetCurrentTextureStatus_Lost) configure_surface();
        return false;
    }
    WGPUTextureView target = wgpuTextureCreateView(st.texture, nullptr);
    submit(target);
#ifndef __EMSCRIPTEN__
    wgpuSurfacePresent(g_surface);
#endif
    wgpuTextureViewRelease(target);
    wgpuTextureRelease(st.texture);
    return true;
}

double busy_ms() {
    if (!g_measured) return 0.0;
    return static_cast<double>(g_measured_ns) / static_cast<double>(g_measured) * static_cast<double>(g_total_passes) / 1e6;
}

struct Copy {
    WGPUBuffer buffer = nullptr;
    std::uint64_t size = 0, order = 0;
    std::uint32_t width = 0, height = 0;
    bool busy = false, ready = false;
};
Copy g_copies[3];
std::uint64_t g_copy_order = 0;

bool display_async(const DisplaySource &src, std::vector<std::uint32_t> &rgba, std::uint32_t &width, std::uint32_t &height) {
    post::drain();
#ifdef __EMSCRIPTEN__
    wgpuInstanceProcessEvents(g_instance);
#else
    wgpuDevicePoll(g_device, false, nullptr);
#endif
    Copy *newest = nullptr;
    for (Copy &c : g_copies)
        if (c.ready && (!newest || c.order > newest->order)) newest = &c;
    if (newest) {
        const std::uint64_t bytes = static_cast<std::uint64_t>(newest->width) * newest->height * 4;
        const void *mapped = wgpuBufferGetConstMappedRange(newest->buffer, 0, bytes);
        if (mapped) {
            rgba.resize(static_cast<std::size_t>(newest->width) * newest->height);
            std::memcpy(rgba.data(), mapped, bytes);
            width = newest->width;
            height = newest->height;
        }
        for (Copy &c : g_copies)
            if (c.ready && c.order <= newest->order) {
                wgpuBufferUnmap(c.buffer);
                c.ready = c.busy = false;
            }
        if (!mapped) newest = nullptr;
    }
    Copy *slot = nullptr;
    for (Copy &c : g_copies)
        if (!c.busy) slot = &c;
    if (slot) {
        const std::uint64_t bytes = static_cast<std::uint64_t>(src.width) * src.height * g_scale * g_scale * 4;
        if (slot->size < bytes) {
            if (slot->buffer) wgpuBufferRelease(slot->buffer);
            slot->size = bytes;
            slot->buffer = make_buffer(bytes, WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst);
        }
        flush_all();
        const std::uint32_t param = display_params(src, 0);
        push_display(src, 0, param);
        submit();
        WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(g_device, nullptr);
        wgpuCommandEncoderCopyBufferToBuffer(enc, g_scratch, 0, slot->buffer, 0, bytes);
        WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(enc, nullptr);
        wgpuQueueSubmit(g_queue, 1, &cmd);
        wgpuCommandBufferRelease(cmd);
        wgpuCommandEncoderRelease(enc);
        slot->busy = true;
        slot->order = ++g_copy_order;
        slot->width = src.width * g_scale;
        slot->height = src.height * g_scale;
        WGPUBufferMapCallbackInfo cb = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
        cb.mode = WGPUCallbackMode_AllowProcessEvents;
        cb.callback = [](WGPUMapAsyncStatus status, WGPUStringView, void *copy, void *) {
            Copy &c = *static_cast<Copy *>(copy);
            if (status == WGPUMapAsyncStatus_Success) c.ready = true;
            else c.busy = false;
        };
        cb.userdata1 = slot;
        wgpuBufferMapAsync(slot->buffer, WGPUMapMode_Read, 0, bytes, cb);
    }
    return newest != nullptr;
}

bool display(const DisplaySource &src, std::vector<std::uint32_t> &rgba, std::uint32_t &width, std::uint32_t &height) {
    flush_all();
    const std::uint32_t param = display_params(src, 0);
    push_display(src, 0, param);
    submit();
    width = src.width * g_scale;
    height = src.height * g_scale;
    rgba.resize(static_cast<std::size_t>(width) * height);
    read_back(g_scratch, rgba.size() * 4, rgba.data());
    return true;
}

}
