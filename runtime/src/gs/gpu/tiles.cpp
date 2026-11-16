#include "tiles.hpp"

#include "../queue.hpp"
#include "ps2/crash.hpp"
#include "ps2/os.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <string>
#include <thread>
#include <vector>

#ifdef __EMSCRIPTEN__
// the browser build builds the lists in place
#elif defined(__linux__)
#include <climits>
#include <linux/futex.h>
#include <pthread.h>
#include <sys/syscall.h>
#include <unistd.h>
#elif defined(_WIN32)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#endif

namespace ps2::gs::gpu::tiles {

namespace {

float u2f(std::uint32_t u) {
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}

struct Geometry {
    int scale = 1, tile = 8, per_row = 128;  // resolution scale, tile size in pixels, tiles along one row of the screen
};
Geometry g_geo;
bool g_configured = false;

struct Span {
    std::int16_t ty, tx0, tx1;
    std::uint16_t prim;  // index of the shape in the batch (a batch has at most 65536 shapes)
};

// The spans of a batch's shapes, in order (add_spans). Filled by hand: the capacity is made sure of once per shape, not checked at every push.
struct SpanBuf {
    std::unique_ptr<Span[]> spans;
    std::size_t used = 0, capacity = 0;

    void ensure(std::size_t extra) {
        if (used + extra <= capacity) return;
        std::size_t next = std::max<std::size_t>(capacity * 2, 1u << 14);
        while (next < used + extra) next *= 2;
        auto bigger = std::make_unique_for_overwrite<Span[]>(next);
        if (used) std::memcpy(bigger.get(), spans.get(), used * sizeof(Span));
        spans = std::move(bigger);
        capacity = next;
    }
    void push_back(Span s) { spans[used++] = s; }
    std::size_t size() const { return used; }
    void clear() { used = 0; }
};

// The scratch of one thread that builds lists: a count per tile, the place in the lists per tile, the touched tiles in the order they were first touched
// and the spans. Every thread that builds has its own.
struct Builder {
    std::vector<std::uint32_t> count, fill, touched;
    SpanBuf spans;
    std::uint32_t touched_n = 0;  // tiles in `touched`
    std::size_t counted = 0;      // spans already counted

    void prepare() {
        const std::size_t tiles = static_cast<std::size_t>(g_geo.per_row) * static_cast<std::size_t>(g_geo.per_row) + 8;
        if (count.size() == tiles) return;
        count.assign(tiles, 0);  // 8 words to spare, as when the counting rounds of build were unrolled four wide (they wrote past a span's last tile)
        fill.assign(tiles, 0);
        touched.assign(tiles + 8, 0);
    }
    void add_spans(const std::uint32_t *rec, std::uint32_t lo, std::uint32_t hi, std::uint16_t prim);
    // A batch in steps: begin, add its shapes in order (all at once, or a part whenever more are drawn: the spans and counts come out the same,
    // since they are made shape by shape and span by span in the same order either way), finish for the table and the lists.
    void begin() {
        prepare();
        spans.clear();
        touched_n = 0;
        counted = 0;
    }
    void add(const std::uint32_t *prims, std::uint32_t from, std::uint32_t to);
    Lists finish(Room room, void *context);
    void abandon() {  // a batch given up half way: the counts back to zero for the next one
        for (std::uint32_t n = 0; n < touched_n; ++n) count[touched[n]] = 0;
        begin();
    }
    Lists build(const std::uint32_t *prims, std::uint32_t shapes, Room room, void *context) {
        begin();
        add(prims, 0, shapes);
        return finish(room, context);
    }
};

void Builder::add_spans(const std::uint32_t *rec, std::uint32_t lo, std::uint32_t hi, std::uint16_t prim) {
    const int sc = g_geo.scale, tile = g_geo.tile;
    const int tx0 = static_cast<int>(lo & 0xFFFFu) * sc / tile, ty0 = static_cast<int>(lo >> 16) * sc / tile;
    const int tx1 = (static_cast<int>(hi & 0xFFFFu) * sc + sc - 1) / tile, ty1 = (static_cast<int>(hi >> 16) * sc + sc - 1) / tile;
    spans.ensure(static_cast<std::size_t>(std::max(ty1 - ty0 + 1, 0)));  // at most one span per tile row
    const auto whole = [&] {
        for (int ty = ty0; ty <= ty1; ++ty) spans.push_back({static_cast<std::int16_t>(ty), static_cast<std::int16_t>(tx0), static_cast<std::int16_t>(tx1), prim});
    };
    if ((rec[1] & kKindMask) != 0 || tx1 - tx0 < 2) {
        whole();
        return;
    }
    // The three edges of the triangle are the lanes of a vector (lane 3 repeats lane 0 and is not used). Every lane does the same double
    // operations, in the same order and without fused multiply-add, as the scalar loop of the earlier versions did, so the spans are the same.
    typedef double V4 __attribute__((vector_size(32)));
    typedef long long M4 __attribute__((vector_size(32)));
    const double px0 = u2f(rec[4]), px1 = u2f(rec[6]), px2 = u2f(rec[8]), py0 = u2f(rec[5]), py1 = u2f(rec[7]), py2 = u2f(rec[9]);
    const double inv = u2f(rec[10]);
    const V4 xa = {px1, px2, px0, px0}, xb = {px2, px0, px1, px0}, ya = {py1, py2, py0, py0}, yb = {py2, py0, py1, py0};  // edge e runs from point a to point b: {1, 2}, {2, 0}, {0, 1}
    const V4 ca = (ya - yb) * inv, cb = (xb - xa) * inv, cc = (xa * yb - xb * ya) * inv;
    const V4 limit = {1.7976931348623157e308, 1.7976931348623157e308, 1.7976931348623157e308, 1.7976931348623157e308};  // DBL_MAX: a value is finite when |v| <= it
    const M4 finite = (__builtin_elementwise_abs(ca) <= limit) & (__builtin_elementwise_abs(cb) <= limit) & (__builtin_elementwise_abs(cc) <= limit);
    if (!(finite[0] & finite[1] & finite[2])) {
        whole();
        return;
    }
    const V4 zero = {0.0, 0.0, 0.0, 0.0};
    const M4 positive = ca > zero, negative = ca < zero, slanted = positive | negative;
    const V4 divided_m = (-cb) / ca, divided_q = (-cc) / ca;  // the lanes of a vertical edge (ca == 0) are not used
    const V4 vm = (V4)(((M4)divided_m & slanted) | ((M4)cb & ~slanted)), vq = (V4)(((M4)divided_q & slanted) | ((M4)cc & ~slanted));
    const M4 kind_v = negative - positive;  // +1 for an edge that bounds the triangle on the left, -1 on the right, 0 for a vertical one
    const int kind[3] = {static_cast<int>(kind_v[0]), static_cast<int>(kind_v[1]), static_cast<int>(kind_v[2])};
    const double span = static_cast<double>(tile) / static_cast<double>(sc);
    // x / span is exactly x * (1 / span) when span is a power of two (4, 8, 2, 16 ...: every scale but 3), and a multiplication is cheaper than a division
    const bool span_pow2 = tile % sc == 0 && ((tile / sc) & (tile / sc - 1)) == 0;
    const double span_inverse = 1.0 / span;
    for (int ty = ty0; ty <= ty1; ++ty) {
        const double y0 = ty * span - 1.0, y1 = (ty + 1) * span + 1.0;
        const V4 a0 = vm * y0, a1 = vm * y1;
        // std::min(a0, a1) is (a1 < a0 ? a1 : a0), std::max(a0, a1) is (a0 < a1 ? a1 : a0)
        const M4 smaller = a1 < a0, larger = a0 < a1;
        const V4 low = (V4)(((M4)a1 & smaller) | ((M4)a0 & ~smaller)) + vq, high = (V4)(((M4)a1 & larger) | ((M4)a0 & ~larger)) + vq;
        double left = -1e30, right = 1e30;
        bool empty = false;
        for (int e = 0; e < 3; ++e) {
            if (kind[e] == 0) empty = empty || high[e] < 0.0;
            else if (kind[e] > 0) left = std::max(left, low[e]);
            else right = std::min(right, high[e]);
        }
        left -= 1.0;
        right += 1.0;
        if (empty || left > right) continue;
        const double lo_x = std::max(left, -1.0), hi_x = std::min(right, 4096.0);
        const int rx0 = std::max(tx0, static_cast<int>(std::floor(span_pow2 ? lo_x * span_inverse : lo_x / span)));
        const int rx1 = std::min(tx1, static_cast<int>(std::floor(span_pow2 ? hi_x * span_inverse : hi_x / span)));
        if (rx0 <= rx1) spans.push_back({static_cast<std::int16_t>(ty), static_cast<std::int16_t>(rx0), static_cast<std::int16_t>(rx1), prim});
    }
}

void Builder::add(const std::uint32_t *prims, std::uint32_t from, std::uint32_t to) {
    // Count how many shapes touch each tile. `touched` lists the tiles in the order they are first touched. A plain loop with a branch on the first
    // touch: on the God of War ship fight (1.6 million shapes, 4.2 million spans, 41 % of them one tile long) it took 13 ms where the form unrolled
    // four wide with a running touched count (no branch per tile) took 20: that count chained every tile to the one before it. The same for the
    // lists below (10 ms against 11).
    std::uint32_t *const count = this->count.data();
    std::uint32_t *const touched = this->touched.data();
    std::uint32_t touched_n = this->touched_n;
    const std::int32_t tiles_per_row = g_geo.per_row;
    for (std::uint32_t i = from; i < to; ++i) add_spans(prims + i * kPrimWords, prims[i * kPrimWords + 2], prims[i * kPrimWords + 3], static_cast<std::uint16_t>(i));
    // One loop over all the new spans (not one per shape): its exits are easy to predict.
    const Span *const span_list = spans.spans.get();
    const std::size_t span_count = spans.size();
    for (std::size_t k = counted; k < span_count; ++k) {
        const Span sp = span_list[k];
        const std::uint32_t t = static_cast<std::uint32_t>(sp.ty * tiles_per_row + sp.tx0);
        const std::int32_t len = sp.tx1 - sp.tx0 + 1;
        for (std::int32_t j = 0; j < len; ++j) {
            const std::uint32_t c = count[t + j];
            if (c == 0) touched[touched_n++] = t + j;
            count[t + j] = c + 1;
        }
    }
    counted = span_count;
    this->touched_n = touched_n;
}

Lists Builder::finish(Room room, void *context) {
    std::uint32_t *const count = this->count.data();
    const std::uint32_t *const touched = this->touched.data();
    const std::int32_t tiles_per_row = g_geo.per_row;
    const Span *const span_list = spans.spans.get();
    const std::size_t span_count = spans.size();
    std::uint32_t total = 0;
    for (std::uint32_t n = 0; n < touched_n; ++n) total += count[touched[n]];
    const std::uint32_t tiles = touched_n;
    const std::uint32_t list_words = (total + 1) / 2;
    const std::uint32_t words = tiles * 2 + 2 + list_words;
    std::uint32_t *const grown = room(context, words);
    if (list_words) grown[words - 1] = 0;
    std::uint32_t cursor = 0;
    std::uint32_t *const fill = this->fill.data();
    const std::uint32_t row_width = static_cast<std::uint32_t>(tiles_per_row);
    const bool row_pow2 = (row_width & (row_width - 1)) == 0;
    const std::uint32_t row_mask = row_width - 1, row_shift = static_cast<std::uint32_t>(__builtin_ctz(row_width));
    for (std::uint32_t i = 0; i < tiles; ++i) {
        const std::uint32_t t = touched[i];
        const std::uint32_t tx = row_pow2 ? (t & row_mask) : (t % row_width), ty = row_pow2 ? (t >> row_shift) : (t / row_width);
        grown[i * 2] = tx | (ty << 16);
        grown[i * 2 + 1] = cursor;
        fill[t] = cursor;
        cursor += count[t];
    }
    grown[tiles * 2] = 0;
    grown[tiles * 2 + 1] = cursor;
    auto *halves = reinterpret_cast<unsigned char *>(grown + tiles * 2 + 2);
    for (std::size_t k = 0; k < span_count; ++k) {
        const Span sp = span_list[k];
        const std::uint16_t index = sp.prim;
        const std::uint32_t t = static_cast<std::uint32_t>(sp.ty * tiles_per_row + sp.tx0);
        const std::int32_t len = sp.tx1 - sp.tx0 + 1;
        for (std::int32_t j = 0; j < len; ++j) {
            const std::uint32_t f = fill[t + j]++;
            std::memcpy(halves + 2u * f, &index, 2);
        }
    }
    for (std::uint32_t n = 0; n < touched_n; ++n) count[touched[n]] = 0;
    spans.clear();
    this->touched_n = 0;
    counted = 0;
    Lists lists;
    lists.tiles = tiles;
    lists.total = total;
    lists.words = words;
    return lists;
}

Builder g_own;  // the calling thread's (the window thread's) scratch for build_now

// ---- PS2_TILE_LOG ----------------------------------------------------------------------------------------------------------------------------

const bool g_log = !os::env("PS2_TILE_LOG").empty();
const std::uint64_t g_log_batches = g_log ? std::strtoull(os::env("PS2_TILE_LOG").c_str(), nullptr, 10) : 0;  // PS2_TILE_LOG=<n>, n > 1: also print the fingerprint after the first n batches
std::uint64_t g_hash = 0xCBF29CE484222325ull, g_batches = 0, g_entries = 0, g_words = 0;  // the window thread's (note_lists)
std::uint64_t g_waits = 0, g_wait_ns = 0;                                                  // finish waits (window thread)
constexpr std::uint64_t kTaken = ~0ull - 1;
std::uint64_t g_stolen = 0;                                                                   // jobs the window thread built itself (build_waiting)
std::atomic<std::uint64_t> g_busy_ns{0}, g_jobs{0};                                         // the helper's

std::atomic<std::size_t> g_peak_backlog{0};  // the most jobs started and not yet placed (window thread)
std::atomic<std::size_t> g_sets{0};           // job buffer sets made (window thread)
std::atomic<std::uint64_t> g_held{0}, g_peak_held{0};  // words the jobs' buffers held when they were last released, now and at most (window thread)
void memory_note(const char *when);  // PS2_TILE_LOG: what the helper's buffers and the process hold (defined below)

[[maybe_unused]] std::uint64_t now_ns() { return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()); }

void print_log() {
    std::fprintf(stderr, "[tiles] %llu batches, %llu list entries, %llu words, fingerprint %016llx\n", static_cast<unsigned long long>(g_batches), static_cast<unsigned long long>(g_entries),
                 static_cast<unsigned long long>(g_words), static_cast<unsigned long long>(g_hash));
    if (const std::uint64_t jobs = g_jobs.load())
        std::fprintf(stderr, "[tiles] helper: %llu jobs, %.1f ms busy in all; the window thread built %llu itself and waited %llu times for %.1f ms in all\n",
                     static_cast<unsigned long long>(jobs), static_cast<double>(g_busy_ns.load()) / 1e6, static_cast<unsigned long long>(g_stolen), static_cast<unsigned long long>(g_waits),
                     static_cast<double>(g_wait_ns) / 1e6);
    memory_note("at the end");
}

// ---- the helper thread ----------------------------------------------------------------------------------------------------------------------

#ifndef __EMSCRIPTEN__

#if defined(__linux__)
void futex_wait(std::atomic<std::uint32_t> &word, std::uint32_t seen) { syscall(SYS_futex, reinterpret_cast<std::uint32_t *>(&word), FUTEX_WAIT_PRIVATE, seen, nullptr, nullptr, 0); }
void futex_wake(std::atomic<std::uint32_t> &word) { syscall(SYS_futex, reinterpret_cast<std::uint32_t *>(&word), FUTEX_WAKE_PRIVATE, INT_MAX, nullptr, nullptr, 0); }
#elif defined(_WIN32)
void futex_wait(std::atomic<std::uint32_t> &word, std::uint32_t seen) { WaitOnAddress(&word, &seen, sizeof seen, INFINITE); }
void futex_wake(std::atomic<std::uint32_t> &word) { WakeByAddressAll(&word); }
#else
void futex_wait(std::atomic<std::uint32_t> &word, std::uint32_t seen) { word.wait(seen); }
void futex_wake(std::atomic<std::uint32_t> &word) { word.notify_all(); }
#endif

constexpr std::size_t kRing = 1024;      // jobs the helper's queue holds (a power of two)

struct Shared {
    std::vector<std::unique_ptr<Job>> jobs;  // every job ever made (never freed)
    std::vector<Job *> idle;                  // jobs free for use, the last one released first (its buffers are the warmest)
    std::deque<Job *> pending;                // started and not yet released, oldest first (the window thread's)
    Builder builder;                          // the helper's scratch
    Job *ring[kRing] = {};
    alignas(64) std::atomic<std::uint64_t> head{0};  // jobs queued (window thread writes)
    alignas(64) std::atomic<std::uint64_t> tail{0};  // jobs taken (helper writes)
    alignas(64) std::atomic<std::uint32_t> sleeping{0}, bell{0};
    std::thread *thread = nullptr;  // never joined or destroyed: a running thread's destructor at exit would end the program
    std::thread::id owner;          // the thread that starts and places the jobs (the window thread)
    int stream_spin_us = 0;  // PS2_TILE_STREAM_SPIN: microseconds the helper looks for an open batch's next shapes before it sleeps (wait_for_shapes). On the God of War ship 0 drew as fast as 50 or 200 (6.05 against 6.0 ms) and spent 2.6 instead of 4.5 ms of the helper's processor a frame
    int spin_us = 0;  // PS2_TILE_SPIN: microseconds the idle helper spins before it sleeps. 0, 50 and 300 drew at the same speed on the Ryzen PC (2026-11-11); 300 cost 1.3 ms of processor a frame
    bool cores = false;
};
std::atomic<Shared *> g_shared{nullptr};  // made on the first job and never freed: the helper may still be running while the program exits

void memory_note(const char *when) {
    // Counters only: this runs on the emulation thread at the end of a run, while the window thread may still be making jobs.
    const std::size_t sets = g_sets.load();
    const double held_mb = static_cast<double>(g_held.load()) * 4.0 / 1048576.0, peak_held_mb = static_cast<double>(g_peak_held.load()) * 4.0 / 1048576.0;
    double peak = 0, commit = 0, now = 0;
#ifdef _WIN32
    using MemoryInfo = BOOL(WINAPI *)(HANDLE, PROCESS_MEMORY_COUNTERS *, DWORD);
    if (const HMODULE kernel = GetModuleHandleW(L"kernel32.dll"))
        if (const auto get = reinterpret_cast<MemoryInfo>(reinterpret_cast<void *>(GetProcAddress(kernel, "K32GetProcessMemoryInfo")))) {
            PROCESS_MEMORY_COUNTERS pmc{};
            pmc.cb = sizeof pmc;
            if (get(GetCurrentProcess(), &pmc, sizeof pmc)) {
                peak = static_cast<double>(pmc.PeakWorkingSetSize) / 1048576.0;
                commit = static_cast<double>(pmc.PeakPagefileUsage) / 1048576.0;
                now = static_cast<double>(pmc.WorkingSetSize) / 1048576.0;
            }
        }
#elif defined(__linux__)
    if (std::FILE *f = std::fopen("/proc/self/status", "r")) {  // resident now and its peak (VmRSS, VmHWM), the virtual peak (VmPeak) in place of the commit
        char line[256];
        while (std::fgets(line, sizeof line, f)) {
            double *to = !std::strncmp(line, "VmRSS:", 6) ? &now : !std::strncmp(line, "VmHWM:", 6) ? &peak : !std::strncmp(line, "VmPeak:", 7) ? &commit : nullptr;
            if (to) *to = std::strtod(std::strchr(line, ':') + 1, nullptr) / 1024.0;
        }
        std::fclose(f);
    }
#endif
    std::fprintf(stderr, "[tiles] memory %s: %zu job buffer sets hold %.1f MB (at most %.1f), peak backlog %zu, process working set %.0f MB now, %.0f MB peak, peak commit %.0f MB (code at %p)\n", when, sets, held_mb, peak_held_mb, g_peak_backlog.load(), now, peak, commit, reinterpret_cast<void *>(&memory_note));
}

std::uint32_t *job_room(void *context, std::size_t words) { return static_cast<Job *>(context)->lists.discard_and_reserve(words); }

void name_thread() {
#ifdef _WIN32
    using SetDescription = HRESULT(WINAPI *)(HANDLE, PCWSTR);
    if (const HMODULE kernel = GetModuleHandleW(L"kernel32.dll"))
        if (const auto set = reinterpret_cast<SetDescription>(reinterpret_cast<void *>(GetProcAddress(kernel, "SetThreadDescription")))) set(GetCurrentThread(), L"ps2 tiles");
#elif defined(__linux__)
    pthread_setname_np(pthread_self(), "ps2 tiles");  // shows in /proc/<pid>/task/<tid>/comm
#endif
}

constexpr std::uint32_t kClosed = 0, kOpen = 1, kCancelled = 2;  // Job::state

void ring_bell(Shared &s) {
    if (s.sleeping.load()) {  // sequentially consistent, after the store that announced the work: see helper_main
        s.bell.fetch_add(1);
        futex_wake(s.bell);
    }
}

// The helper with an open job and all its shapes so far done: waits until more come or the job is closed or cancelled (the next shapes are usually
// about 100 microseconds away: 1024 shapes drawn). It sleeps at once unless PS2_TILE_STREAM_SPIN says to look for them a while first.
void wait_for_shapes(Shared *s, const Job *job, std::uint32_t at) {
    const auto more = [&] { return job->count.load() != at || job->state.load() != kOpen; };
    const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(s->stream_spin_us);
    do {
        for (int i = 0; i < 16; ++i) {
#if defined(__x86_64__) || defined(_M_X64)
            __builtin_ia32_pause();
#endif
        }
        if (more()) return;
    } while (std::chrono::steady_clock::now() < until);
    const std::uint32_t seen = s->bell.load();
    s->sleeping.store(1);  // sequentially consistent, like the loads in more() and feed's and close's stores and ring_bell's check
    if (!more()) futex_wait(s->bell, seen);
    s->sleeping.store(0);
}

// The helper builds a job it claimed, its shapes as they come (an open job's, see open/feed/close). False when the job was cancelled.
bool run_job(Shared *s, Job *job) {
    Builder &b = s->builder;
    b.begin();
    std::uint32_t at = 0;
    for (;;) {
        const std::uint32_t state = job->state.load(std::memory_order_acquire);  // before the count: once closed, the count read after it is the last
        if (state == kCancelled) {
            b.abandon();
            return false;
        }
        const std::uint32_t n = job->count.load(std::memory_order_acquire);
        if (n > at) {
            const std::uint64_t t0 = g_log ? now_ns() : 0;
            b.add(job->prims.load(std::memory_order_acquire), at, n);  // read after the count: where the records are now, which holds the first n
            if (g_log) g_busy_ns.fetch_add(now_ns() - t0, std::memory_order_relaxed);
            at = n;
            continue;
        }
        if (state == kClosed) break;
        wait_for_shapes(s, job, at);
    }
    const std::uint64_t t0 = g_log ? now_ns() : 0;
    job->result = b.finish(job_room, job);
    if (g_log) {
        g_busy_ns.fetch_add(now_ns() - t0, std::memory_order_relaxed);
        g_jobs.fetch_add(1, std::memory_order_relaxed);
    }
    return true;
}

void helper_main(Shared *s) {
    name_thread();
    ps2::crash::enter_thread(ps2::crash::Thread::Tiles);  // a crash report names this thread
    std::uint32_t cpu_seen[64] = {};
    std::uint64_t last_print = 0, rounds = 0;
    for (;;) {
        const std::uint64_t at = s->tail.load(std::memory_order_relaxed);
        if (s->head.load(std::memory_order_acquire) == at) {
            // Nothing queued: wait a little for the next batch (they come every 50 microseconds or so while the window thread draws), then sleep.
            const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(s->spin_us);
            bool found = false;
            do {
                for (int i = 0; i < 16; ++i) {
#if defined(__x86_64__) || defined(_M_X64)
                    __builtin_ia32_pause();
#endif
                }
                if (s->head.load(std::memory_order_acquire) != at) {
                    found = true;
                    break;
                }
            } while (std::chrono::steady_clock::now() < until);
            if (!found) {
                const std::uint32_t seen = s->bell.load();
                s->sleeping.store(1);  // sequentially consistent, like the head check after it and the window thread's publish and sleeping check: one of the two sees the other
                if (s->head.load() == at) futex_wait(s->bell, seen);
                s->sleeping.store(0);
            }
            continue;
        }
        Job *job = s->ring[at & (kRing - 1)];
        std::uint64_t expect = at;  // fails when the window thread took the job (build_waiting); then it may already be in use again with another ticket
        if (job->claim.compare_exchange_strong(expect, kTaken, std::memory_order_acq_rel)) {
            run_job(s, job);
            s->tail.store(at + 1, std::memory_order_release);
            job->done.store(1, std::memory_order_release);
        } else {
            s->tail.store(at + 1, std::memory_order_release);
        }
#ifdef _WIN32
        if (s->cores) {  // PS2_CORES=1: where this thread ran, like the three threads of system.cpp
            ++cpu_seen[GetCurrentProcessorNumber() & 63u];
            if ((++rounds & 63u) == 0) {
                const std::uint64_t t = now_ns() / 1000000u;
                if (t - last_print >= 3000) {
                    if (last_print) {
                        std::uint32_t total = 0;
                        for (std::uint32_t n : cpu_seen) total += n;
                        std::string line;
                        for (int i = 0; i < 64; ++i)
                            if (cpu_seen[i] * 50 >= total) line += " #" + std::to_string(i) + ":" + std::to_string(cpu_seen[i] * 100 / total) + "%";
                        std::fprintf(stderr, "[cores] tile helper %s\n", line.c_str());
                    }
                    std::memset(cpu_seen, 0, sizeof cpu_seen);
                    last_print = t;
                }
            }
        }
#else
        (void)cpu_seen;
        (void)last_print;
        (void)rounds;
#endif
    }
}

Shared &shared() {
    if (!g_shared.load(std::memory_order_relaxed)) {
        Shared *s = new Shared;
        s->owner = std::this_thread::get_id();
        if (const std::string spin = os::env("PS2_TILE_SPIN"); !spin.empty()) s->spin_us = std::clamp(std::atoi(spin.c_str()), 0, 100000);
        s->cores = !os::env("PS2_CORES").empty();
        if (const std::string spin = os::env("PS2_TILE_STREAM_SPIN"); !spin.empty()) s->stream_spin_us = std::clamp(std::atoi(spin.c_str()), 0, 100000);
        s->thread = new std::thread(helper_main, s);
        g_shared.store(s, std::memory_order_release);  // owner() reads it from other threads
    }
    return *g_shared.load();
}

#else
void memory_note(const char *) {}
#endif  // !__EMSCRIPTEN__

}  // namespace

void configure(int scale, int tile) {
    g_geo.scale = scale;
    g_geo.tile = tile;
    g_geo.per_row = (2048 * scale + tile - 1) / tile;
    g_own = Builder();
    g_configured = true;
}

Lists build_now(const std::uint32_t *prims, std::uint32_t count, Room room, void *context) { return g_own.build(prims, count, room, context); }

bool logging() { return g_log; }

void report() {
    if (g_log) print_log();
}

void note_lists(const std::uint32_t *words, const Lists &lists) {
    if (!g_log) return;
    std::uint64_t h = g_hash;
    const auto mix = [&h](std::uint32_t v) { h = (h ^ v) * 0x100000001B3ull; };
    mix(lists.tiles);
    mix(lists.total);
    for (std::uint32_t i = 0; i < lists.words; ++i) mix(words[i]);
    g_hash = h;
    ++g_batches;
    g_entries += lists.total;
    g_words += lists.words;
    if (g_batches == g_log_batches && g_batches > 1)
        std::fprintf(stderr, "[tiles] after %llu batches: %llu list entries, %llu words, fingerprint %016llx\n", static_cast<unsigned long long>(g_batches), static_cast<unsigned long long>(g_entries),
                     static_cast<unsigned long long>(g_words), static_cast<unsigned long long>(g_hash));
    if (g_batches == g_log_batches && g_batches > 1) memory_note("after that");
}

#ifdef __EMSCRIPTEN__

bool threaded() { return false; }
Job *start(const std::uint32_t *, std::uint32_t) { return nullptr; }
Job *open(const std::uint32_t *, std::uint32_t) { return nullptr; }
void feed(Job *, const std::uint32_t *, std::uint32_t) {}
void close(Job *, const std::uint32_t *, std::uint32_t) {}
void cancel(Job *) {}
bool owner() { return true; }
std::size_t backlog() { return 0; }
Job *next_finished(bool) { return nullptr; }
void build_waiting() {}
void release(Job *) {}

#else

bool threaded() {
    // PS2_TILE_THREAD: 0 off, 1 on (when there is a render ring), 2 on even without a render ring (a single-thread run builds its lists on the helper: how the lists are
    // compared with the old code's in a run that ends cleanly); not set: on on Linux (measured faster on big batches, no loss elsewhere), off elsewhere.
    static const int mode = [] {
        const std::string text = os::env("PS2_TILE_THREAD");
        if (!text.empty()) return std::atoi(text.c_str());
#ifdef __linux__
        std::fprintf(stderr, "[tiles] helper thread on by default on Linux; PS2_TILE_THREAD=0 turns it off\n");
        return 1;
#else
        return 0;  // off by default: the helper costs memory (hundreds of MB) and is not measured on this platform
#endif
    }();
    return mode != 0 && g_configured && (mode == 2 || queue::active());
}

namespace {
Job *queue_job(const std::uint32_t *prims, std::uint32_t count, std::uint32_t state) {
    Shared &s = shared();
    Job *job;
    if (s.idle.empty()) {
        s.jobs.push_back(std::make_unique<Job>());
        job = s.jobs.back().get();
        g_sets.store(s.jobs.size(), std::memory_order_relaxed);
    } else {
        job = s.idle.back();
        s.idle.pop_back();
    }
    job->prims.store(prims, std::memory_order_relaxed);
    job->count.store(count, std::memory_order_relaxed);
    job->state.store(state, std::memory_order_relaxed);
    job->done.store(0, std::memory_order_relaxed);
    const std::uint64_t h = s.head.load(std::memory_order_relaxed);
    job->ticket = h;
    job->claim.store(h, std::memory_order_release);  // after prims, count and done: whoever claims it sees them
    s.pending.push_back(job);
    if (s.pending.size() > g_peak_backlog.load(std::memory_order_relaxed)) g_peak_backlog.store(s.pending.size(), std::memory_order_relaxed);
    s.ring[h & (kRing - 1)] = job;
    s.head.store(h + 1);  // sequentially consistent, see helper_main
    ring_bell(s);
    return job;
}
}  // namespace

Job *start(const std::uint32_t *prims, std::uint32_t count) { return queue_job(prims, count, kClosed); }

Job *open(const std::uint32_t *prims, std::uint32_t count) { return queue_job(prims, count, kOpen); }

void feed(Job *job, const std::uint32_t *prims, std::uint32_t count) {
    job->prims.store(prims, std::memory_order_release);
    job->count.store(count);  // sequentially consistent: the helper's sleep check sees it or ring_bell sees the helper asleep
    ring_bell(*g_shared.load());
}

void close(Job *job, const std::uint32_t *prims, std::uint32_t count) {
    job->prims.store(prims, std::memory_order_release);
    job->count.store(count, std::memory_order_release);
    job->state.store(kClosed);  // after the count (run_job reads the state first)
    ring_bell(*g_shared.load());
}

void cancel(Job *job) {
    Shared &s = *g_shared.load();
    job->state.store(kCancelled);
    std::uint64_t expect = job->ticket;
    if (job->claim.compare_exchange_strong(expect, kTaken, std::memory_order_acq_rel)) {
        job->done.store(1, std::memory_order_release);  // the helper had not reached it
    } else {
        ring_bell(s);
        while (!job->done.load(std::memory_order_acquire)) std::this_thread::yield();  // the helper gives it up at its next look (at most kStreamStep shapes)
    }
    if (s.pending.empty() || s.pending.back() != job) {  // cannot happen: an open job is the newest
        std::fprintf(stderr, "[tiles] BAD cancel: the job is not the newest\n");
        std::abort();
    }
    s.pending.pop_back();
    release(job);
}

bool owner() {
    const Shared *s = g_shared.load(std::memory_order_acquire);
    return !s || std::this_thread::get_id() == s->owner;
}

void build_waiting() {
    Shared *s = g_shared.load(std::memory_order_relaxed);
    if (!s) return;
    for (auto it = s->pending.rbegin(); it != s->pending.rend(); ++it) {
        Job *job = *it;
        if (job->done.load(std::memory_order_acquire)) break;  // the helper got this far
        std::uint64_t expect = job->ticket;
        if (!job->claim.compare_exchange_strong(expect, kTaken, std::memory_order_acq_rel)) break;  // the helper is on it, and done with or on every older one
        job->result = g_own.build(job->prims.load(std::memory_order_relaxed), job->count.load(std::memory_order_relaxed), job_room, job);  // closed: this thread wrote them
        job->done.store(1, std::memory_order_release);
        ++g_stolen;
    }
}

std::size_t backlog() { return g_shared ? g_shared.load()->pending.size() : 0; }

Job *next_finished(bool wait) {
    if (!g_shared || g_shared.load()->pending.empty()) return nullptr;
    Shared &s = *g_shared.load();
    Job *job = s.pending.front();
    if (!job->done.load(std::memory_order_acquire)) {
        if (!wait) return nullptr;
        const std::uint64_t t0 = g_log ? now_ns() : 0;
        for (unsigned spins = 0; !job->done.load(std::memory_order_acquire); ++spins) {
            if (spins < 4000) {
#if defined(__x86_64__) || defined(_M_X64)
                __builtin_ia32_pause();
#endif
            } else {
                if (s.sleeping.load()) {  // the helper is asleep with this job waiting: a wake-up that was lost would be a hang
                    s.bell.fetch_add(1);
                    futex_wake(s.bell);
                }
                std::this_thread::yield();
            }
        }
        if (g_log) {
            ++g_waits;
            g_wait_ns += now_ns() - t0;
        }
    }
    s.pending.pop_front();
    return job;
}

void release(Job *job) {
    job->prims.store(nullptr, std::memory_order_relaxed);
    if (job->lists.capacity > kKeepWords) job->lists.free_memory();  // a rare big batch does not keep its buffer for good
    const std::uint64_t held = job->lists.capacity;
    const std::uint64_t total = g_held.load(std::memory_order_relaxed) + held - job->held;
    job->held = held;
    g_held.store(total, std::memory_order_relaxed);
    if (total > g_peak_held.load(std::memory_order_relaxed)) g_peak_held.store(total, std::memory_order_relaxed);
    Shared &s = *g_shared.load();
    s.idle.push_back(job);
}

#endif

}
