#include "gpu.hpp"
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
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>

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
constexpr std::uint32_t kSetupWords = 40;  // 25 fields, then the texture, frame and depth formats as the shader needs them (fmt_words)
constexpr std::uint32_t kDecodedWords = 4u << 20;
constexpr std::uint32_t kDecodePrims = 64;

Mode g_mode = Mode::off;
std::uint64_t g_dispatches[kPipelines] = {}, g_submits = 0;

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
std::uint32_t g_field = 0, g_last_width = 0, g_last_height = 0;
bool g_have_previous = false;
std::uint32_t g_scale = 1, g_copy_words = 1u << 20, g_field_words = 1u << 19;

std::uint32_t field_base(std::uint32_t field) { return g_copy_words + (field & 1u) * g_field_words; }

bool g_timestamps = false;
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

struct Command {
    Pipeline pipeline;
    std::uint32_t x, y;
    std::uint32_t param;
};

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
enum Why { kWhyKey, kWhyTexel, kWhyUnder, kWhyOverlap, kWhyZ, kWhyUpload, kWhyClut, kWhyCopy, kWhySize, kWhyFlush, kWhys };
const char *const kWhyName[kWhys] = {"buffers", "texel", "drawn-under", "overlap", "depth", "upload", "palette", "copy", "size", "other"};
Why g_why = kWhyFlush;
std::uint32_t g_why_runs[kWhys] = {}, g_why_prims[kWhys] = {};
const bool g_batch_log = std::getenv("PS2_BATCH_LOG") != nullptr;
const unsigned long long g_batch_log_frame = std::getenv("PS2_BATCH_LOG") ? std::strtoull(std::getenv("PS2_BATCH_LOG"), nullptr, 10) : 0;  // >1: print every "drawn-under" end of that frame
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

struct Batch {
    std::vector<std::uint32_t> prims;
    std::vector<std::uint32_t> setups;
    std::vector<std::uint32_t> boxes;
    std::uint64_t key = ~0ull;
    int minx = 1 << 30, miny = 1 << 30, maxx = -1, maxy = -1;
    std::uint32_t fbp = 0, fbw = 0, fpsm = 0, zbp = 0, zpsm = 0;
    bool zte = false, zwrite = false;
    std::uint32_t texflushes = 0;
    int rx0 = 0, ry0 = 0, rx1 = -2, ry1 = -2;
    Range fb_range, z_range;
} g_batch;

struct SetupCache {
    std::uint32_t generation = ~0u, clut = ~0u;
    std::uint64_t attr = ~0ull;
    std::uint32_t setup[kSetupWords] = {};
    std::uint32_t flags = 0;
    std::uint64_t key = 0;
    Range tex;
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

int g_tile = 8;  // pixels along one side of a screen tile (one workgroup of the raster program): 8 was the cheaper of 16, 8 and 4 on the arena and hall test frames; PS2_TILE=4, 8 or 16 to try others
int g_tiles_per_row = 128;
std::vector<std::uint32_t> g_tile_count(128 * 128), g_tile_fill(128 * 128);
std::vector<std::uint32_t> g_touched;

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
};

WGPUBuffer g_cache = nullptr;
std::vector<std::uint32_t> g_cache_copy;
std::deque<CacheEntry> g_cache_ring;
std::unordered_multimap<std::uint64_t, CacheEntry *> g_cache_index;
std::unordered_map<std::uint64_t, std::uint32_t> g_cache_seen;
std::uint32_t g_cache_head = 0;
std::uint64_t g_serial = 1;

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
    wgpuQueueWriteBuffer(g_queue, g_cache, static_cast<std::uint64_t>(offset) * 4, to, static_cast<std::uint64_t>(words) * 4);
    g_cache_ring.push_back({key, offset, words, n, g_serial});
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

struct Span {
    std::int16_t ty, tx0, tx1;
};
std::vector<Span> g_spans;
std::vector<std::uint32_t> g_span_start;

void add_spans(const std::uint32_t *rec, std::uint32_t lo, std::uint32_t hi) {
    const int sc = static_cast<int>(g_scale);
    const int tx0 = static_cast<int>(lo & 0xFFFFu) * sc / g_tile, ty0 = static_cast<int>(lo >> 16) * sc / g_tile;
    const int tx1 = (static_cast<int>(hi & 0xFFFFu) * sc + sc - 1) / g_tile, ty1 = (static_cast<int>(hi >> 16) * sc + sc - 1) / g_tile;
    const auto whole = [&] {
        for (int ty = ty0; ty <= ty1; ++ty) g_spans.push_back({static_cast<std::int16_t>(ty), static_cast<std::int16_t>(tx0), static_cast<std::int16_t>(tx1)});
    };
    if (rec[1] != 0 || tx1 - tx0 < 2) {
        whole();
        return;
    }
    const double x[3] = {u2f(rec[4]), u2f(rec[6]), u2f(rec[8])}, y[3] = {u2f(rec[5]), u2f(rec[7]), u2f(rec[9])};
    const double inv = u2f(rec[10]);
    static const int kEdges[3][2] = {{1, 2}, {2, 0}, {0, 1}};
    double m[3], q[3];
    int kind[3];
    for (int e = 0; e < 3; ++e) {
        const int a = kEdges[e][0], b = kEdges[e][1];
        const double ca = (y[a] - y[b]) * inv, cb = (x[b] - x[a]) * inv, cc = (x[a] * y[b] - x[b] * y[a]) * inv;
        if (!std::isfinite(ca) || !std::isfinite(cb) || !std::isfinite(cc)) {
            whole();
            return;
        }
        kind[e] = ca > 0.0 ? 1 : ca < 0.0 ? -1 : 0;
        m[e] = kind[e] ? -cb / ca : cb;
        q[e] = kind[e] ? -cc / ca : cc;
    }
    const double span = static_cast<double>(g_tile) / static_cast<double>(sc);
    for (int ty = ty0; ty <= ty1; ++ty) {
        const double y0 = ty * span - 1.0, y1 = (ty + 1) * span + 1.0;
        double left = -1e30, right = 1e30;
        bool empty = false;
        for (int e = 0; e < 3; ++e) {
            const double a0 = m[e] * y0, a1 = m[e] * y1;
            if (kind[e] == 0) empty = empty || std::max(a0, a1) + q[e] < 0.0;
            else if (kind[e] > 0) left = std::max(left, std::min(a0, a1) + q[e]);
            else right = std::min(right, std::max(a0, a1) + q[e]);
        }
        left -= 1.0;
        right += 1.0;
        if (empty || left > right) continue;
        const int rx0 = std::max(tx0, static_cast<int>(std::floor(std::max(left, -1.0) / span)));
        const int rx1 = std::min(tx1, static_cast<int>(std::floor(std::min(right, 4096.0) / span)));
        if (rx0 <= rx1) g_spans.push_back({static_cast<std::int16_t>(ty), static_cast<std::int16_t>(rx0), static_cast<std::int16_t>(rx1)});
    }
}

bool reads_own_word(const std::uint32_t *rec, const std::uint32_t *setup) {
    const std::uint32_t fpsm = setup[3], tpsm = setup[14];
    if (rec[1] != 1 || (setup[0] & (16u | 16384u)) != 16u || setup[12] != setup[1] || setup[13] != setup[2]) return false;
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
    if (rec[1] != 1 || !(setup[0] & 16u)) return whole;
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

struct Decode {
    std::uint32_t setup, offset, words;
};
std::vector<Decode> g_decodes;

void plan_decodes(std::uint32_t prims) {
    Batch &b = g_batch;
    g_decodes.clear();
    if (!g_decode || prims <= kDecodePrims) return;
    const std::uint32_t setups = static_cast<std::uint32_t>(b.setups.size() / kSetupWords);
    static const int kKey[] = {12, 13, 14, 15, 16, 17, 20, 22};
    for (std::uint32_t i = 0; i < setups; ++i) {
        std::uint32_t *w = b.setups.data() + i * kSetupWords;
        if (!(w[0] & 2u)) continue;
        bool shared = false;
        for (const Decode &d : g_decodes) {
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
        const std::uint32_t tw = w[15] & 0xFFu, th = w[15] >> 8;
        const std::uint32_t mu = (w[7] >> 24) & 3u, mv = (w[7] >> 26) & 3u;
        const auto inside = [](std::uint32_t mode, std::uint32_t region, std::uint32_t size) { return mode < 2 || ((region & 0x3FFu) < (1u << size) && (region >> 16) < (1u << size)); };
        if (!inside(mu, w[18], tw) || !inside(mv, w[19], th)) continue;
        const std::uint32_t words = 1u << (tw + th);
        if (g_decoded_used + words > kDecodedWords) continue;
        if (g_scale > 1 && rendered(pages(w[12], w[13], w[14], 0, 0, (1 << tw) - 1, (1 << th) - 1))) continue;
        w[24] = g_decoded_used;
        g_decodes.push_back({i, g_decoded_used, words});
        g_decoded_used += words;
    }
}

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
        if (!(setup[0] & 2u) || reads_own_word(rec, setup)) continue;
        const int tw = std::max(1 << (setup[15] & 0xFFu), static_cast<int>(setup[18] >> 16) + 1), th = std::max(1 << (setup[15] >> 8), static_cast<int>(setup[19] >> 16) + 1);
        const Range t = read_range(rec, setup, region(setup[12], setup[13], setup[14], 0, 0, tw - 1, th - 1));
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
            info.area += rec[1] == 0 ? std::fabs(0.5 / static_cast<double>(u2f(rec[10]))) : bw * bh;
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

void flush_batch() {
    Batch &b = g_batch;
    if (b.prims.empty()) {
        b.key = ~0ull;
        return;
    }
    const std::uint32_t prims = static_cast<std::uint32_t>(b.prims.size() / kPrimWords);
    g_touched.clear();
    g_spans.clear();
    g_span_start.resize(prims + 1);
    for (std::uint32_t i = 0; i < prims; ++i) {
        g_span_start[i] = static_cast<std::uint32_t>(g_spans.size());
        add_spans(b.prims.data() + i * kPrimWords, b.boxes[i * 2], b.boxes[i * 2 + 1]);
        for (std::size_t k = g_span_start[i]; k < g_spans.size(); ++k) {
            const Span &sp = g_spans[k];
            std::uint32_t t = static_cast<std::uint32_t>(sp.ty * g_tiles_per_row + sp.tx0);
            for (int tx = sp.tx0; tx <= sp.tx1; ++tx, ++t)
                if (!g_tile_count[t]++) g_touched.push_back(t);
        }
    }
    g_span_start[prims] = static_cast<std::uint32_t>(g_spans.size());
    plan_decodes(prims);
    const std::uint32_t prim_offset = arena_append(b.prims.data(), b.prims.size() * 4);
    const std::uint32_t setup_offset = arena_append(b.setups.data(), b.setups.size() * 4);
    for (const Decode &d : g_decodes) dispatch_linear(kDecode, d.words, push_params({setup_offset + d.setup * kSetupWords, d.offset, d.words}));
    std::uint32_t total = 0;
    for (std::uint32_t t : g_touched) total += g_tile_count[t];
    const std::uint32_t tiles = static_cast<std::uint32_t>(g_touched.size());
    const std::uint32_t tile_offset = static_cast<std::uint32_t>(g_arena.size());
    const std::uint32_t list_offset = tile_offset + tiles * 2 + 2;
    const std::uint32_t list_words = (total + 1) / 2;
    std::uint32_t *grown = g_arena.grow(tiles * 2 + 2 + list_words);
    if (list_words) grown[tiles * 2 + 2 + list_words - 1] = 0;
    std::uint32_t cursor = 0;
    for (std::uint32_t i = 0; i < tiles; ++i) {
        const std::uint32_t t = g_touched[i];
        grown[i * 2] = (t % static_cast<std::uint32_t>(g_tiles_per_row)) | ((t / static_cast<std::uint32_t>(g_tiles_per_row)) << 16);
        grown[i * 2 + 1] = cursor;
        g_tile_fill[t] = cursor;
        cursor += g_tile_count[t];
    }
    grown[tiles * 2] = 0;
    grown[tiles * 2 + 1] = cursor;
    auto *halves = reinterpret_cast<unsigned char *>(grown + tiles * 2 + 2);
    for (std::uint32_t i = 0; i < prims; ++i) {
        const auto index = static_cast<std::uint16_t>(i);
        for (std::uint32_t k = g_span_start[i]; k < g_span_start[i + 1]; ++k) {
            const Span &sp = g_spans[k];
            std::uint32_t t = static_cast<std::uint32_t>(sp.ty * g_tiles_per_row + sp.tx0);
            for (int tx = sp.tx0; tx <= sp.tx1; ++tx, ++t) std::memcpy(halves + 2u * g_tile_fill[t]++, &index, 2);
        }
    }
    for (std::uint32_t t : g_touched) g_tile_count[t] = 0;
    std::uint32_t snapshot_lo, snapshot_hi;
    const std::uint32_t snapshot = snapshot_batch(prims, snapshot_lo, snapshot_hi);
    const std::uint32_t param = push_params({tile_offset, prim_offset, setup_offset, tiles, list_offset, snapshot_lo, snapshot_hi});
    note_raster(prims, tiles, total, snapshot);
    if (g_batch_log) {
        if (g_batch_log_frame > 1 && frame_count() == g_batch_log_frame) std::fprintf(stderr, "[break] %s %u\n", kWhyName[g_why], prims);
        ++g_why_runs[g_why];
        g_why_prims[g_why] += prims;
        g_why = kWhyFlush;
    }
    g_commands.push_back({kRaster, std::min(tiles, 1024u), (tiles + 1023) / 1024, param});
    mark_rendered(pages(b.fbp, b.fbw, b.fpsm, b.minx, b.miny, b.maxx, b.maxy));
    if (b.zwrite) mark_rendered(pages(b.zbp, b.fbw, b.zpsm, b.minx, b.miny, b.maxx, b.maxy));
    b.prims.clear();
    b.setups.clear();
    b.boxes.clear();
    g_batch_reads.clear();
    b.key = ~0ull;
    b.minx = b.miny = 1 << 30;
    b.maxx = b.maxy = -1;
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
        if (clut) g_commands.push_back({kLoadClut, 1, 1, push_params({offset, count})});
        else dispatch_linear(kUpload, total, push_params({offset, count, total}));
        i = j;
    }
    g_pre.clear();
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
    queue_pre(op);
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
    const std::uint32_t per_row = std::max<std::uint32_t>(1, im.dbw * 64 / pw), rows = im.rrw ? im.total / im.rrw : 0;
    for (std::uint32_t py = (im.dsay + ph - 1) / ph; py < (im.dsay + rows) / ph; ++py)
        for (std::uint32_t px = (im.dsax + pw - 1) / pw; px < (im.dsax + im.rrw) / pw; ++px)
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

void ensure_capacity() {
    bool changed = false;
    const std::uint64_t data_bytes = std::max<std::uint64_t>(g_arena.size() * 4, 16);
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
    wgpuQueueWriteBuffer(g_queue, g_data, 0, g_arena.data(), g_arena.size() * 4);
    wgpuQueueWriteBuffer(g_queue, g_params, 0, g_param_records.data(), g_param_records.size() * kParamStride);
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
        wgpuComputePassEncoderSetPipeline(pass, g_pipelines[c.pipeline]);
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
    g_decoded_used = 0;
    g_clut_loads = 0;
}

void bench_capture() {
    BenchSubmit &sub = g_bench.subs.emplace_back();
    sub.commands = g_commands;
    sub.params.resize(g_param_records.size() * (kParamStride / 4));
    if (!g_param_records.empty()) std::memcpy(sub.params.data(), g_param_records.data(), g_param_records.size() * kParamStride);
    sub.arena.assign(g_arena.data(), g_arena.data() + g_arena.size());
    sub.info = std::move(g_bench_info);
    g_bench_info.clear();
}

// Browsers copy writes above about 1 MiB through a slower path, so large uploads go in pieces.
void submit(WGPUTextureView present_view = nullptr) {
    if (g_commands.empty() && !present_view) return;
    if (g_bench.capturing) bench_capture();
    if (g_profile_out && !present_view && g_timestamps && frame_count() >= g_profile_from && frame_count() < g_profile_to) {
        profile_submit();
        return;
    }
    ensure_capacity();
    if (ps2::hitch::on()) {
        for (const Command &c : g_commands) {
            ++g_hitch_now.commands[c.pipeline];
            g_hitch_now.groups[c.pipeline] += static_cast<std::uint64_t>(c.x) * c.y;
        }
        g_hitch_now.arena_bytes += g_arena.size() * 4;
        ++g_hitch_now.submits;
    }
    wgpuQueueWriteBuffer(g_queue, g_data, 0, g_arena.data(), g_arena.size() * 4);
    wgpuQueueWriteBuffer(g_queue, g_params, 0, g_param_records.data(), g_param_records.size() * kParamStride);
    if (g_query_ready) {
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
    const bool measure = g_timestamps && !g_query_pending;
    WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(g_device, nullptr);
    WGPUPassTimestampWrites writes = WGPU_PASS_TIMESTAMP_WRITES_INIT;
    writes.querySet = g_queries;
    writes.beginningOfPassWriteIndex = 0;
    writes.endOfPassWriteIndex = 1;
    WGPUComputePassDescriptor cd = WGPU_COMPUTE_PASS_DESCRIPTOR_INIT;
    if (measure) cd.timestampWrites = &writes;
    WGPUComputePassEncoder pass = wgpuCommandEncoderBeginComputePass(enc, &cd);
    Pipeline current = kPipelines;
    ++g_submits;
    ++g_total_passes;
    static const unsigned skip = [] {  // PS2_GPU_SKIP=raster,snapshot: leave these kinds of work out (the picture is wrong; only to see what they cost)
        unsigned mask = 0;
        const std::string list = os::env("PS2_GPU_SKIP");
        for (int k = 0; k < kPipelines && !list.empty(); ++k)
            if (list.find(kEntryPoints[k]) != std::string::npos) mask |= 1u << k;
        return mask;
    }();
    for (const Command &c : g_commands) {
        ++g_dispatches[c.pipeline];
        if (skip & (1u << c.pipeline)) continue;
        if (c.pipeline != current) {
            wgpuComputePassEncoderSetPipeline(pass, g_pipelines[c.pipeline]);
            current = c.pipeline;
        }
        const std::uint32_t offset = c.param * kParamStride;
        wgpuComputePassEncoderSetBindGroup(pass, 0, g_group, 1, &offset);
        wgpuComputePassEncoderDispatchWorkgroups(pass, c.x, c.y, 1);
    }
    wgpuComputePassEncoderEnd(pass);
    wgpuComputePassEncoderRelease(pass);
    if (measure) {
        wgpuCommandEncoderResolveQuerySet(enc, g_queries, 0, 2, g_query_resolve, 0);
        wgpuCommandEncoderCopyBufferToBuffer(enc, g_query_resolve, 0, g_query_read, 0, 16);
    }
    if (present_view) {
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
        wgpuRenderPassEncoderEnd(rp);
        wgpuRenderPassEncoderRelease(rp);
    }
    WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(enc, nullptr);
    wgpuQueueSubmit(g_queue, 1, &cmd);
    wgpuCommandBufferRelease(cmd);
    wgpuCommandEncoderRelease(enc);
    if (measure) {
        g_query_pending = true;
        WGPUBufferMapCallbackInfo cb = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
        cb.mode = WGPUCallbackMode_AllowProcessEvents;
        cb.callback = [](WGPUMapAsyncStatus status, WGPUStringView, void *, void *) {
            if (status == WGPUMapAsyncStatus_Success) g_query_ready = true;
            else g_query_pending = false;
        };
        wgpuBufferMapAsync(g_query_read, WGPUMapMode_Read, 0, 16, cb);
    }
#ifdef __EMSCRIPTEN__
    wgpuInstanceProcessEvents(g_instance);
#else
    wgpuDevicePoll(g_device, false, nullptr);
#endif
    g_commands.clear();
    g_command_info.clear();
    ++g_serial;
    g_param_records.clear();
    g_arena.clear();
    g_decoded_used = 0;
    g_clut_loads = 0;
}

void read_back(WGPUBuffer source, std::uint64_t bytes, void *out, std::uint64_t offset = 0) {
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
    cb.userdata1 = &done;
    wgpuBufferMapAsync(g_readback, WGPUMapMode_Read, 0, bytes, cb);
    wait_until(done);
    const void *mapped = wgpuBufferGetConstMappedRange(g_readback, 0, bytes);
    if (mapped) std::memcpy(out, mapped, bytes);
    wgpuBufferUnmap(g_readback);
}

void wait_gpu() {
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
    g_bench.subs.clear();
    g_bench_info.clear();
    std::fprintf(stderr, "[bench] capturing the chip work of vblank %llu (game frame %llu)\n", static_cast<unsigned long long>(g_bench.vblank),
                 static_cast<unsigned long long>(frame_count()));
}

struct BenchBuffers {
    WGPUBuffer params = nullptr, data = nullptr;
    WGPUBindGroup group = nullptr;
};

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
            for (const Command &c : g_bench.subs[i].commands) {
                if (skip & (1u << c.pipeline)) continue;
                if (size_filter && c.pipeline == kRaster && ((c.x * c.y <= 8u) == (size_filter == 1))) continue;  // 1: leave out the small runs (8 tiles or fewer), 2: leave out the big ones
                if (c.pipeline != current) {
                    wgpuComputePassEncoderSetPipeline(pass, g_pipelines[c.pipeline]);
                    current = c.pipeline;
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
    wgpuComputePassEncoderSetPipeline(pass, g_pipelines[c.pipeline]);
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

void bench_run() {
    g_bench.capturing = false;
    g_bench.done = true;
    if (g_bench.subs.empty()) {
        std::fprintf(stderr, "[bench] no chip work in that frame\n");
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
                                {"copy and display only", all & ~((1u << kCopyRead) | (1u << kCopyWrite) | (1u << kDisplay)), true},
                                {"raster, big runs only (over 8 tiles)", all & ~(1u << kRaster), true, 1},
                                {"raster, small runs only (8 tiles or fewer)", all & ~(1u << kRaster), true, 2}};
    bench_time(bufs, 0, 3, true);  // warm up
    for (const Variant &v : variants) {
        double best = 1e30;
        for (int trial = 0; trial < 3; ++trial) best = std::min(best, bench_time(bufs, v.skip, reps, v.restore, v.size_filter));
        std::fprintf(stderr, "[bench] %-32s %8.3f ms per frame\n", v.name, best);
    }
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
    std::vector<std::uint32_t> vram(static_cast<std::size_t>(kMemorySize / 4) * g_scale * g_scale);
    read_back(g_vram, vram.size() * 4, vram.data());
    std::fprintf(stderr, "[bench] memory after the frame: hash %016llx\n", static_cast<unsigned long long>(fnv_hash(vram, 0, vram.size())));
    if (const std::string path = os::env("PS2_GPU_BENCH_DUMP"); !path.empty()) {
        if (std::FILE *f = os::open(path.c_str(), "wb")) {
            std::fwrite(vram.data(), 4, vram.size(), f);
            std::fclose(f);
        }
    }
    for (BenchBuffers &b : bufs) {
        wgpuBindGroupRelease(b.group);
        wgpuBufferRelease(b.params);
        wgpuBufferRelease(b.data);
    }
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

bool texel_rect(const std::uint32_t *setup, const Vertex *v, int count, std::uint32_t prim, bool fst, int rect[4]) {
    const int tw = 1 << (setup[15] & 0xFFu), th = 1 << (setup[15] >> 8);
    float u0 = 1e30f, v0 = 1e30f, u1 = -1e30f, v1 = -1e30f;
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
    const std::size_t prims = b.boxes.size() / 2;
    if (prims > 64) return hits(b.minx, b.miny, b.maxx, b.maxy);
    for (std::size_t i = 0; i < prims; ++i) {
        const std::uint32_t lo = b.boxes[i * 2], hi = b.boxes[i * 2 + 1];
        if (hits(static_cast<int>(lo & 0xFFFFu), static_cast<int>(lo >> 16), static_cast<int>(hi & 0xFFFFu), static_cast<int>(hi >> 16))) return true;
    }
    return false;
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

Mode mode() { return g_mode; }

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
    if (want == "dxc") std::fprintf(stderr, "[gpu] PS2_DX12_COMPILER=dxc: dxcompiler.dll and dxil.dll are not next to the program (scripts/fetch_dxc.sh): using FXC\n");
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
    WGPUAdapterInfo info = WGPU_ADAPTER_INFO_INIT;
    if (wgpuAdapterGetInfo(adapter, &info) == WGPUStatus_Success) {
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
    const WGPUFeatureName timestamp_feature = WGPUFeatureName_TimestampQuery;
    g_timestamps = wgpuAdapterHasFeature(adapter, WGPUFeatureName_TimestampQuery);
    if (g_timestamps) {
        dd.requiredFeatureCount = 1;
        dd.requiredFeatures = &timestamp_feature;
    }
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
    return true;
}

bool init(Mode m) {
    if (m == Mode::off) return false;
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
        order.push_back({WGPUInstanceBackend_DX12, "Direct3D 12"});
        order.push_back({WGPUInstanceBackend_Vulkan, "Vulkan"});
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
            if (opened) g_dx12_compiler = "DXC";
            else {
                if (dx12 && !dxc.empty()) std::fprintf(stderr, "[gpu] Direct3D 12 could not start with DXC: trying FXC\n");
                opened = open_backend(t.backends, t.name, round == 1 || order.size() == 1, std::string());
                if (opened && dx12) g_dx12_compiler = "FXC";
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
    else if (chosen == &order[0] && order.size() > 1) std::fprintf(stderr, "[gpu] graphics API: %s (the default on Windows; PS2_GPU_BACKEND=vulkan picks Vulkan)\n", chosen->name);
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
    if (const char *scale = std::getenv("PS2_SCALE")) g_scale = std::clamp<std::uint32_t>(static_cast<std::uint32_t>(std::atoi(scale)), 1u, 3u);
    g_copy_words = (1u << 20) * g_scale * g_scale;
    if (const int tile = std::atoi(os::env("PS2_TILE").c_str()); tile == 4 || tile == 8 || tile == 16) g_tile = tile;
    g_tiles_per_row = (2048 * static_cast<int>(g_scale) + g_tile - 1) / g_tile;
    g_tile_count.assign(static_cast<std::size_t>(g_tiles_per_row) * g_tiles_per_row, 0);
    g_tile_fill.assign(static_cast<std::size_t>(g_tiles_per_row) * g_tiles_per_row, 0);
    g_field_words = (1u << 19) * g_scale * g_scale;
    g_vram = make_buffer(static_cast<std::uint64_t>(kMemorySize) * g_scale * g_scale, WGPUBufferUsage_Storage | WGPUBufferUsage_CopySrc | WGPUBufferUsage_CopyDst);
    std::vector<std::uint32_t> tables;
    swizzle_tables(tables);
    g_tables = make_buffer(tables.size() * 4, WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst);
    wgpuQueueWriteBuffer(g_queue, g_tables, 0, tables.data(), tables.size() * 4);
    g_clut = make_buffer(static_cast<std::uint64_t>(kClutSlots) * kClutEntries * 4, WGPUBufferUsage_Storage | WGPUBufferUsage_CopySrc | WGPUBufferUsage_CopyDst);
    g_scratch = make_buffer((static_cast<std::uint64_t>(g_copy_words) + 2ull * g_field_words) * 4, WGPUBufferUsage_Storage | WGPUBufferUsage_CopySrc);
    g_cache = make_buffer(static_cast<std::uint64_t>(kCacheWords) * 4, WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst);
    g_decoded = make_buffer(static_cast<std::uint64_t>(kDecodedWords) * 4, WGPUBufferUsage_Storage);
    if (const char *decode = std::getenv("PS2_DECODE")) g_decode = std::atoi(decode) != 0;
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
            std::fclose(f);
            std::fprintf(stderr, "[gpu] shader from %s (%zu bytes)\n", path.c_str(), shader_text.size());
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
    WGPUShaderModule module = wgpuDeviceCreateShaderModule(g_device, &md);
    for (int i = 0; i < kPipelines; ++i) {
        WGPUComputePipelineDescriptor cd = WGPU_COMPUTE_PIPELINE_DESCRIPTOR_INIT;
        cd.layout = g_pipeline_layout;
        cd.compute.module = module;
        cd.compute.entryPoint = view(kEntryPoints[i]);
        WGPUConstantEntry constants[3] = {WGPU_CONSTANT_ENTRY_INIT, WGPU_CONSTANT_ENTRY_INIT, WGPU_CONSTANT_ENTRY_INIT};
        constants[0].key = view("SCALE");
        constants[0].value = static_cast<double>(g_scale);
        constants[1].key = view("TILE");
        constants[1].value = static_cast<double>(g_tile);
        constants[2].key = view("EXPLICIT_FMA");
        constants[2].value = explicit_fma ? 1.0 : 0.0;
        // a shader loaded with PS2_GS_SHADER may lack the later constants; TILE and EXPLICIT_FMA are listed only when it declares them
        const bool has_tile = shader_text.empty() || shader_text.find("override TILE") != std::string::npos;
        const bool has_fma = shader_text.empty() || shader_text.find("override EXPLICIT_FMA") != std::string::npos;
        cd.compute.constantCount = has_tile ? (has_fma ? 3 : 2) : 1;
        cd.compute.constants = constants;
        g_pipelines[i] = wgpuDeviceCreateComputePipeline(g_device, &cd);
        if (!g_pipelines[i]) return false;
    }
    std::fprintf(stderr, "[gpu] %d shaders compiled in %.0f ms\n", kPipelines, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - compile_start).count());
    wgpuShaderModuleRelease(module);
    ensure_capacity();
    g_mode = m;
    return true;
}

void draw(const State &st, const Vertex *v, int count) {
    if (g_image.active && !g_image.pending.empty()) flush_image();
    std::uint64_t attr = st.prim;
    if (!(st.prmodecont & 1u)) attr = (st.prim & 7u) | (st.prmode & ~7ull);
    const std::uint32_t prim = static_cast<std::uint32_t>(st.prim) & 7u;
    const Context &c = st.ctx[(attr >> 9) & 1u];
    SetupCache &cache = g_setup_cache;
    if (cache.generation != g_setup_generation || cache.attr != attr || cache.clut != g_clut_slot) {
        cache.generation = g_setup_generation;
        cache.attr = attr;
        cache.clut = g_clut_slot;
        const std::uint32_t *w = cache.setup;
        cache.flags = make_setup(st, attr, cache.setup);
        cache.key = static_cast<std::uint64_t>(w[1]) | (static_cast<std::uint64_t>(w[2]) << 14) | (static_cast<std::uint64_t>(w[3]) << 20) |
                    (static_cast<std::uint64_t>(w[5]) << 26) | (static_cast<std::uint64_t>(w[6]) << 40);
        if (cache.flags & 2u) {
            const std::uint32_t tw = w[15] & 0xFFu, th = w[15] >> 8;
            const int tex_w = std::max(1 << tw, static_cast<int>(w[18] >> 16) + 1), tex_h = std::max(1 << th, static_cast<int>(w[19] >> 16) + 1);
            cache.tex = region(w[12], w[13], w[14], 0, 0, tex_w - 1, tex_h - 1);
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
    const auto first = [&](float lo) { return static_cast<int>(std::floor(std::ceil(lo * samples) / samples)); };

    std::uint32_t rec[kPrimWords] = {};
    int minx, miny, maxx, maxy;
    switch (prim) {
    case 3: case 4: case 5: {
        if (count < 3) return;
        const float x0 = sx(v[0]), y0 = sy(v[0]), x1 = sx(v[1]), y1 = sy(v[1]), x2 = sx(v[2]), y2 = sy(v[2]);
        const float area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
        if (area == 0.0f) return;
        minx = std::max(scax0, first(std::min({x0, x1, x2})));
        maxx = std::min(scax1, static_cast<int>(std::ceil(std::max({x0, x1, x2}))) - 1);
        miny = std::max(scay0, first(std::min({y0, y1, y2})));
        maxy = std::min(scay1, static_cast<int>(std::ceil(std::max({y0, y1, y2}))) - 1);
        rec[1] = 0;
        const float xy[6] = {x0, y0, x1, y1, x2, y2};
        for (int i = 0; i < 6; ++i) rec[4 + i] = f2u(xy[i]);
        rec[10] = f2u(1.0f / area);
        const float sg = area > 0.0f ? 1.0f : -1.0f;
        const auto inclusive = [&](float ax, float ay, float bx, float by) {
            const float gx = (ay - by) * sg, gy = (bx - ax) * sg;
            return gx > 0.0f || (gx == 0.0f && gy > 0.0f);
        };
        rec[27] = (inclusive(x1, y1, x2, y2) ? 1u : 0u) | (inclusive(x2, y2, x0, y0) ? 2u : 0u) | (inclusive(x0, y0, x1, y1) ? 4u : 0u);
        for (int i = 0; i < 3; ++i) attrs(rec, i, v[i], fst, iip ? nullptr : &v[2]);
        rec[24] = v[0].z;
        rec[25] = f2u(static_cast<float>(static_cast<std::int64_t>(v[1].z) - static_cast<std::int64_t>(v[0].z)));
        rec[26] = f2u(static_cast<float>(static_cast<std::int64_t>(v[2].z) - static_cast<std::int64_t>(v[0].z)));
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
            if (written(tex)) {
                if (!texel_rect(setup, v, count, prim, fst, rect)) {
                    g_why = kWhyTexel;
                    emit_all();
                } else {
                    const Range used = region(setup[12], setup[13], setup[14], rect[0], rect[1], rect[2], rect[3]);
                    const bool small = rect[2] - rect[0] < 64 && rect[3] - rect[1] < 64;
                    const bool cached = b.texflushes == g_texflushes && small && shares_frame(setup, b);
                    const bool same = shares_frame(setup, b);
                    const bool frame = !cached && (same ? drawn_under(b, rect) : used.overlaps(b.fb_range.lo, b.fb_range.hi));
                    if (frame || (b.zwrite && !z_untouched && used.overlaps(b.z_range.lo, b.z_range.hi))) {
                        if (g_batch_log_frame > 1 && frame_count() == g_batch_log_frame) {
                            int hit = -1;
                            const std::size_t n = b.boxes.size() / 2;
                            for (std::size_t i = 0; i < n && hit < 0; ++i) {
                                const std::uint32_t lo = b.boxes[i * 2], hi = b.boxes[i * 2 + 1];
                                if (rect[0] <= static_cast<int>(hi & 0xFFFFu) && static_cast<int>(lo & 0xFFFFu) <= rect[2] && rect[1] <= static_cast<int>(hi >> 16) && static_cast<int>(lo >> 16) <= rect[3]) hit = static_cast<int>(i);
                            }
                            const std::uint32_t hl = hit >= 0 ? b.boxes[hit * 2] : 0, hh = hit >= 0 ? b.boxes[hit * 2 + 1] : 0;
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
        b.rx1 = -2;
    }
    b.zte = b.zte || zte;
    b.zwrite = b.zwrite || (zte && !(flags & 32u));
    const std::size_t setups = b.setups.size() / kSetupWords;
    std::uint32_t setup_index;
    if (setups && std::memcmp(b.setups.data() + (setups - 1) * kSetupWords, setup, kSetupWords * 4) == 0) {
        setup_index = static_cast<std::uint32_t>(setups - 1);
    } else {
        setup_index = static_cast<std::uint32_t>(setups);
        b.setups.insert(b.setups.end(), setup, setup + kSetupWords);
    }
    if (flags & 2u) {
        if (g_batch_reads.empty() || g_batch_reads.back().lo != tex.lo || g_batch_reads.back().hi != tex.hi) {
            bool known = false;
            for (const Range &t : g_batch_reads) known = known || (t.lo <= tex.lo && tex.hi <= t.hi);
            if (!known) g_batch_reads.push_back(tex);
        }
    }
    rec[0] = setup_index;
    rec[2] = static_cast<std::uint32_t>(minx) | (static_cast<std::uint32_t>(miny) << 16);
    rec[3] = static_cast<std::uint32_t>(maxx) | (static_cast<std::uint32_t>(maxy) << 16);
    b.prims.insert(b.prims.end(), rec, rec + kPrimWords);
    b.boxes.push_back(rec[2]);
    b.boxes.push_back(rec[3]);
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
    mark_rendered(pages(static_cast<std::uint32_t>(buf >> 32) & 0x3FFFu, static_cast<std::uint32_t>(buf >> 48) & 0x3Fu, static_cast<std::uint32_t>(buf >> 56) & 0x3Fu,
                        static_cast<int>(pos >> 32) & 0x7FF, static_cast<int>(pos >> 48) & 0x7FF, (static_cast<int>(pos >> 32) & 0x7FF) + static_cast<int>(w) - 1,
                        (static_cast<int>(pos >> 48) & 0x7FF) + static_cast<int>(h) - 1));
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
    PreOp op;
    op.clut = true;
    const std::uint32_t rec[10] = {g_clut_slot, next, cbp, cpsm, csm, static_cast<std::uint32_t>(tex0 >> 56) & 0x1Fu, (psm == PSMT8 || psm == PSMT8H) ? 1u : 0u, cbw, cou, cov};
    std::memcpy(op.rec, rec, sizeof rec);
    op.range = csm ? region(cbp, cbw, cpsm, static_cast<int>(cou * 16), static_cast<int>(cov), static_cast<int>(cou * 16 + 255), static_cast<int>(cov))
                   : region(cbp, 1, cpsm, 0, 0, 15, 15);
    queue_pre(op);
    g_clut_slot = next;
    ++g_clut_loads;
}

void texflush() { ++g_texflushes; }

void flush() {
    if (g_mode == Mode::off) return;
    flush_all();
    submit();
    if (g_batch_log) {
        std::uint32_t runs = 0, prims = 0;
        for (int w = 0; w < kWhys; ++w) runs += g_why_runs[w], prims += g_why_prims[w];
        if (runs) {
            std::fprintf(stderr, "[batch] frame %llu: %u runs, %u shapes, ended by:", static_cast<unsigned long long>(frame_count()), runs, prims);
            for (int w = 0; w < kWhys; ++w)
                if (g_why_runs[w]) std::fprintf(stderr, " %s %u (%u shapes)", kWhyName[w], g_why_runs[w], g_why_prims[w]);
            std::fprintf(stderr, "\n");
        }
        for (int w = 0; w < kWhys; ++w) g_why_runs[w] = g_why_prims[w] = 0;
    }
    if (!g_bench.configured || g_bench.done) return;
    ++g_vblanks;
    if (g_bench.capturing) bench_run();
    else if (g_vblanks + 1 == g_bench.vblank) bench_start();
}

void wait_idle() {
    flush_all();
    submit();
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
    if (g_scale != 1) dispatch_linear(kResolve, bytes / 4, push_params({offset / 4}));
    submit();
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
    while (g_pending.busy && !g_pending.ready) pump();
    const bool done = g_pending.ready;
    collect_download();
    return done;
}

void download(std::uint8_t *memory, std::uint32_t offset, std::uint32_t bytes) {
    flush_all();
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
    submit();
    scale = g_scale;
    vram.resize(static_cast<std::size_t>(kMemorySize / 4) * g_scale * g_scale);
    read_back(g_vram, vram.size() * 4, vram.data());
    clut.resize(kClutEntries);
    read_back(g_clut, kClutEntries * 4, clut.data(), static_cast<std::uint64_t>(g_clut_slot) * kClutEntries * 4);
}

void write_state(const std::vector<std::uint32_t> &vram, std::uint32_t scale, const std::vector<std::uint32_t> &clut) {
    wait_idle();
    const std::size_t words = kMemorySize / 4;
    const std::uint32_t from = scale * scale, to = g_scale * g_scale;
    std::fill(std::begin(g_rendered), std::end(g_rendered), from == to && to > 1);
    if (from == to && vram.size() == words * to) {
        wgpuQueueWriteBuffer(g_queue, g_vram, 0, vram.data(), vram.size() * 4);
    } else {
        std::vector<std::uint32_t> layers(words * to);
        for (std::size_t w = 0; w < words; ++w)
            for (std::uint32_t k = 0; k < to; ++k) layers[w * to + k] = vram[w * from];
        wgpuQueueWriteBuffer(g_queue, g_vram, 0, layers.data(), layers.size() * 4);
    }
    if (clut.size() >= kClutEntries) wgpuQueueWriteBuffer(g_queue, g_clut, static_cast<std::uint64_t>(g_clut_slot) * kClutEntries * 4, clut.data(), kClutEntries * 4);
    wait_idle();
}

void stats() {
    if (g_mode == Mode::off) return;
    std::fprintf(stderr, "[gpu] %llu submits, dispatches:", static_cast<unsigned long long>(g_submits));
    for (int i = 0; i < kPipelines; ++i) std::fprintf(stderr, " %s %llu", kEntryPoints[i], static_cast<unsigned long long>(g_dispatches[i]));
    std::fprintf(stderr, "\n");
}

void configure_surface() {
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
    const std::uint32_t param = push_params({src.fbp, src.fbw, src.psm, src.dbx, src.dby, src.width, src.height, src.halved ? 1u : 0u, current});
    g_commands.push_back({kDisplay, (src.width * g_scale + 7) / 8, (src.height * g_scale + 7) / 8, param});
    const float sw = static_cast<float>(g_surface_width), sh = static_cast<float>(g_surface_height);
    const float aspect = g_options.widescreen ? 16.0f / 9.0f : 4.0f / 3.0f;
    const float vw = std::min(sw, sh * aspect), vh = vw / aspect;
    const std::uint32_t blit[8] = {src.width * g_scale, src.height * g_scale, current, previous, blend ? 1u : 0u, static_cast<std::uint32_t>(vw), static_cast<std::uint32_t>(vh), ps2::video_options()};
    wgpuQueueWriteBuffer(g_queue, g_blit_params, 0, blit, sizeof blit);
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
    else wgpuSurfaceGetCurrentTexture(g_surface, &st);
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
    submit(target);
    if (log.on) log.lap(log.submit);
#ifndef __EMSCRIPTEN__
    ps2::hitch::enter_present_stage("presenting the picture (wgpuSurfacePresent)");
    wgpuSurfacePresent(g_surface);
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
        const std::uint32_t param = push_params({src.fbp, src.fbw, src.psm, src.dbx, src.dby, src.width, src.height, src.halved ? 1u : 0u});
        g_commands.push_back({kDisplay, (src.width * g_scale + 7) / 8, (src.height * g_scale + 7) / 8, param});
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
    const std::uint32_t param = push_params({src.fbp, src.fbw, src.psm, src.dbx, src.dby, src.width, src.height, src.halved ? 1u : 0u});
    g_commands.push_back({kDisplay, (src.width * g_scale + 7) / 8, (src.height * g_scale + 7) / 8, param});
    submit();
    width = src.width * g_scale;
    height = src.height * g_scale;
    rgba.resize(static_cast<std::size_t>(width) * height);
    read_back(g_scratch, rgba.size() * 4, rgba.data());
    return true;
}

}
