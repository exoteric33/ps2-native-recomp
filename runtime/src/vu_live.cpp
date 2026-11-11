#include "ps2/vu_live.hpp"

#include "ps2/crash.hpp"
#include "ps2/os.hpp"

#ifdef __EMSCRIPTEN__

// The browser build has its own compiler for vector programs (vu_jit_web.cpp): nothing to do here.
namespace ps2::vu::live {
std::atomic<bool> g_ready{false};
void start(const std::string &) {}
bool active() { return false; }
void unknown(std::uint64_t, std::uint32_t, const std::uint8_t *) {}
void note_call(std::uint64_t) {}
std::vector<Program> take() { return {}; }
}  // namespace ps2::vu::live

#else

#include "live_build.hpp"
#include "vurecomp_gen.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// The headers the generated code includes, embedded in the runner (cmake/embed.cmake) so the library is compiled against exactly the
// layouts this runner has, whatever sits next to it.
extern const char kLiveHdrCpu[], kLiveHdrFpu[], kLiveHdrVu[], kLiveHdrVuOps[];

namespace fs = std::filesystem;
namespace lb = ::ps2::live;  // what vector programs and main processor code share: compiler, processes, libraries (live_build.cpp)

namespace ps2::vu::live {

std::atomic<bool> g_ready{false};

namespace {

using Clock = lb::Clock;

constexpr std::size_t kCodeSize = 16384;

struct Pending {
    std::uint64_t key = 0;
    std::uint32_t entry = 0;
    std::vector<std::uint8_t> code;
};

// ---------------------------------------------------------------------------------------------------------------- state

struct State {
    std::mutex mutex;
    std::condition_variable wake;
    std::vector<Pending> pending;
    std::vector<Program> done;
    std::unordered_set<std::uint64_t> seen;
    std::unordered_map<std::uint64_t, std::uint32_t> calls;  // interpreted calls of programs waiting to be built
    std::size_t chunk = 24;      // programs in one library at most: the busiest go first, so a scene gets fast soon
    bool with_targets = false;
    bool stopping = false;
    bool worker = false;
    bool busy = false;
    fs::path cache;
    fs::path compiler;
    std::uint64_t stamp = 0;
    std::vector<std::string> flags;
    std::size_t jobs = 2;
    int delay_ms = 300;
    int failures = 0;
    Clock::time_point began = Clock::now();
    bool keep_work = false;
};

State *g = nullptr;  // never freed: the worker may still run while the process ends

double seconds_since(Clock::time_point t) { return lb::seconds_since(t); }

void say(const char *format, auto... args) {
    std::fprintf(stderr, "[vu-live +%.1fs] ", seconds_since(g->began));
    if constexpr (sizeof...(args) == 0) std::fputs(format, stderr);
    else std::fprintf(stderr, format, args...);
    std::fprintf(stderr, "\n");
}

std::uint64_t fnv(std::uint64_t h, const void *data, std::size_t size) { return lb::fnv(h, data, size); }
std::string hex(std::uint64_t v, int digits) { return lb::hex(v, digits); }

std::vector<std::string> compile_flags() {
    std::string opt = "-O3";
    if (const std::string v = ps2::os::env("PS2_VU_LIVE_OPT"); !v.empty()) opt = v;
    return lb::compile_flags(opt);
}

// ---------------------------------------------------------------------------------------------------------------- loading

// Loads one library of the cache and queues its programs for the vector unit. False when it is not usable.
bool load_library(const fs::path &path, std::size_t &count) {
    void *library = lb::open_library(path);
    if (!library) return false;
    using StampFn = std::uint64_t (*)();
    using InitFn = const Program *(*)(const LiveHost *, std::uint32_t *);
    const auto stamp = reinterpret_cast<StampFn>(lb::find_symbol(library, "ps2_vu_live_stamp"));
    const auto init = reinterpret_cast<InitFn>(lb::find_symbol(library, "ps2_vu_live_init"));
    if (!stamp || !init || stamp() != g->stamp) return false;  // stays loaded: it cannot be unloaded safely, and it is rare
    crash::note_live_library(crash::Thread::LiveVu, reinterpret_cast<const void *>(init));  // a crash in it is named as live VU code
    static const LiveHost host = {&ps2::vu::interpret, &ps2::vu::kick, &ps2::vu::top, &ps2::vu::itop};
    std::uint32_t n = 0;
    const Program *table = init(&host, &n);
    std::lock_guard lock(g->mutex);
    for (std::uint32_t i = 0; i < n; ++i) {
        if (!table[i].function) continue;
        g->seen.insert(table[i].key);
        g->done.push_back(table[i]);
        ++count;
    }
    g_ready.store(true, std::memory_order_release);
    return true;
}

bool is_cache_library(const fs::path &p, std::string &prefix) {
    const std::string name = ps2::os::utf8(p.filename());
    const std::size_t dash = name.find('-');
    if (dash != 8 || !name.ends_with(lb::kLibraryExtension)) return false;
    for (std::size_t i = 0; i < 8; ++i)
        if (!std::isxdigit(static_cast<unsigned char>(name[i]))) return false;
    prefix = name.substr(0, 8);
    return true;
}


// ---------------------------------------------------------------------------------------------------------------- building

using lb::Timing;

// Makes the library for a batch and loads it. False when it could not be built (the reason is in the log).
bool build(const std::vector<Pending> &batch, const std::string &id, Timing &timing, std::size_t &programs) {
    const fs::path work = g->cache / ("work-" + id);
    const fs::path src = work / "src";
    std::error_code error;
    fs::create_directories(src, error);
    auto t = Clock::now();

    std::vector<vurecomp::Input> inputs;
    for (const Pending &p : batch) inputs.push_back({p.key, p.entry, p.code});
    // A few programs per file, so that the files can be compiled side by side: a file costs about the same however many programs it holds
    // once it is large, but every file has to read the headers once.
    const std::size_t per_file = std::clamp<std::size_t>((batch.size() * 3 / 2 + g->jobs - 1) / g->jobs, 1, 8);
    const vurecomp::Result generated = vurecomp::generate_files(std::move(inputs), src, g->stamp, per_file, g->with_targets);
    programs = generated.programs;
    timing.generate = seconds_since(t);
    if (generated.programs == 0) {
        fs::remove_all(work, error);
        return true;  // nothing the generator can compile: they stay with the interpreter
    }

    lb::Job job;
    for (const auto &e : fs::directory_iterator(src, error))
        if (e.path().extension() == ".cpp") job.sources.push_back(e.path());
    std::sort(job.sources.begin(), job.sources.end());
    job.include_dir = g->cache / "include";
    job.work = work;
    job.library = work / ("library" + std::string(lb::kLibraryExtension));
    job.compiler = g->compiler;
    job.flags = g->flags;
    job.jobs = g->jobs;
    job.below_normal = true;
    job.say = [](const std::string &text) { say("%s", text.c_str()); };
    {
        std::lock_guard lock(g->mutex);
        if (g->stopping) return false;
    }
    if (!lb::compile_and_link(job, timing)) return false;

    // Into the cache under its final name (the stamp of this runner in front, so a runner of another version leaves it alone).
    const fs::path final_path = g->cache / (hex(g->stamp >> 32, 8) + "-" + id + lb::kLibraryExtension);
    fs::rename(job.library, final_path, error);
    if (error) {
        say("cannot move the library into %s: %s", ps2::os::utf8(g->cache).c_str(), error.message().c_str());
        return false;
    }
    if (!g->keep_work) fs::remove_all(work, error);
    std::size_t loaded = 0;
    if (!load_library(final_path, loaded)) {
        say("the library made just now does not load (%s)", ps2::os::utf8(final_path.filename()).c_str());
        return false;
    }
    return true;
}

void worker() {
    crash::enter_thread(crash::Thread::LiveVu);
    for (;;) {
        crash::test_poll(crash::Thread::LiveVu);
        std::vector<Pending> batch;
        bool delay = true;
        {
            std::unique_lock lock(g->mutex);
            g->wake.wait(lock, [] { return g->stopping || !g->pending.empty(); });
            if (g->stopping) return;
            delay = !g->busy;
        }
        // Wait a moment after a quiet time: a new scene brings its programs within a second or two, and one library for them is cheaper than many.
        if (delay) std::this_thread::sleep_for(std::chrono::milliseconds(g->delay_ms));
        {
            std::lock_guard lock(g->mutex);
            if (g->stopping) return;
            // The busiest programs first (calls counted since they were met), a chunk at a time; the rest waits for the next round.
            std::sort(g->pending.begin(), g->pending.end(), [](const Pending &a, const Pending &b) {
                const auto ca = g->calls.find(a.key), cb = g->calls.find(b.key);
                return (ca == g->calls.end() ? 0u : ca->second) > (cb == g->calls.end() ? 0u : cb->second);
            });
            const std::size_t take = std::min(g->pending.size(), g->chunk);
            batch.assign(std::make_move_iterator(g->pending.begin()), std::make_move_iterator(g->pending.begin() + static_cast<std::ptrdiff_t>(take)));
            g->pending.erase(g->pending.begin(), g->pending.begin() + static_cast<std::ptrdiff_t>(take));
            for (const Pending &p : batch) g->calls.erase(p.key);
        }
        const auto began = Clock::now();
        {
            std::lock_guard lock(g->mutex);
            g->busy = !g->pending.empty();  // more are waiting: the next chunk starts at once
        }
        const std::string id = hex(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count()), 11);
        Timing timing;
        std::size_t programs = 0;
        const bool ok = build(batch, id, timing, programs);
        {
            std::lock_guard lock(g->mutex);
            if (g->stopping) return;
        }
        if (ok) {
            g->failures = 0;
            say("%zu new vector programs compiled in %.1f s (%zu with jump targets; generate %.1f, compile %.1f, link %.1f), switched in", batch.size(), seconds_since(began), programs, timing.generate, timing.compile, timing.link);
        } else if (++g->failures >= 2) {
            say("building failed twice: no more vector programs are built; the interpreter runs the rest (a log is in %s)", ps2::os::utf8(g->cache).c_str());
            std::lock_guard lock(g->mutex);
            g->worker = false;
            g->pending.clear();
            return;
        }
    }
}

void stop() {
    if (!g) return;
    lb::stop_all();
    std::lock_guard lock(g->mutex);
    g->stopping = true;
    g->wake.notify_all();
}

}  // namespace

void start(const std::string &cache) {
    if (g || cache.empty()) return;
    g = new State;
    g->cache = ps2::os::path(cache);
    g->flags = compile_flags();
    g->keep_work = !ps2::os::env("PS2_VU_LIVE_KEEP").empty();
    if (const std::string v = ps2::os::env("PS2_VU_LIVE_JOBS"); !v.empty()) g->jobs = std::clamp<std::size_t>(std::strtoul(v.c_str(), nullptr, 10), 1, 16);
    else g->jobs = std::clamp<std::size_t>(std::thread::hardware_concurrency() / 4, 1, 3);
    if (const std::string v = ps2::os::env("PS2_VU_LIVE_DELAY_MS"); !v.empty()) g->delay_ms = std::atoi(v.c_str());
    if (const std::string v = ps2::os::env("PS2_VU_LIVE_CHUNK"); !v.empty()) g->chunk = std::clamp<std::size_t>(std::strtoul(v.c_str(), nullptr, 10), 1, 4096);
    g->with_targets = ps2::os::env("PS2_VU_LIVE_TARGETS") == "1";
    lb::use_environment();  // PS2_VU_LIVE_PRIORITY and PS2_VU_LIVE_STARVE_S, see docs/vu-live.md

    // The stamp says which libraries this runner may load: the generator, the headers and the flags that went into them.
    std::uint64_t h = 0xCBF29CE484222325ull;
    h = fnv(h, vurecomp::kGeneratorVersion, std::strlen(vurecomp::kGeneratorVersion));
    for (const char *text : {kLiveHdrCpu, kLiveHdrFpu, kLiveHdrVu, kLiveHdrVuOps}) h = fnv(h, text, std::strlen(text));
    for (const std::string &f : g->flags) h = fnv(h, f.data(), f.size() + 1);
    const std::size_t regs = sizeof(Vu0Regs);
    h = fnv(h, &regs, sizeof regs);
    g->stamp = h;

    // What earlier runs left: load it now, and clear out what another version made.
    std::size_t loaded = 0, files = 0;
    std::error_code error;
    if (fs::is_directory(g->cache, error)) {
        const std::string mine = hex(g->stamp >> 32, 8);
        std::vector<fs::path> libraries;
        for (const auto &e : fs::directory_iterator(g->cache, error)) {
            std::string prefix;
            if (e.is_regular_file() && is_cache_library(e.path(), prefix)) {
                if (prefix == mine) libraries.push_back(e.path());
                else fs::remove(e.path(), error);
            } else if (e.is_directory() && ps2::os::utf8(e.path().filename()).rfind("work-", 0) == 0) {
                fs::remove_all(e.path(), error);  // a build the last run did not finish
            }
        }
        std::sort(libraries.begin(), libraries.end());
        for (const fs::path &p : libraries) {
            if (load_library(p, loaded)) ++files;
            else fs::remove(p, error);
        }
    }
    if (files) say("loaded %zu compiled vector programs from %zu files in %s", loaded, files, cache.c_str());

    g->compiler = lb::find_compiler();
    if (g->compiler.empty()) {
        say("no C++ compiler found (PS2_VU_CXX names one, or put one in a compiler folder next to the runner): vector programs the build does not know run in the interpreter");
        return;
    }
    g->worker = true;
    say("compiling vector programs the build does not know with %s, into %s", ps2::os::utf8(g->compiler).c_str(), cache.c_str());
    std::atexit(stop);  // (lb::stop_all() is registered by the toolchain itself; this also wakes the worker)
    std::thread([] {
        if (!lb::write_headers(g->cache / "include", {{"cpu.hpp", kLiveHdrCpu}, {"fpu.hpp", kLiveHdrFpu}, {"vu.hpp", kLiveHdrVu}, {"vu_ops.hpp", kLiveHdrVuOps}})) {
            say("cannot write into %s: no vector programs are built", ps2::os::utf8(g->cache).c_str());
            std::lock_guard lock(g->mutex);
            g->worker = false;
            return;
        }
        worker();
    }).detach();
}

bool active() { return g && g->worker; }

void note_call(std::uint64_t key) {
    std::lock_guard lock(g->mutex);
    ++g->calls[key];
}

void unknown(std::uint64_t key, std::uint32_t entry, const std::uint8_t *code) {
    if (!g || !g->worker) return;
    std::lock_guard lock(g->mutex);
    if (!g->seen.insert(key).second) return;
    Pending p;
    p.key = key;
    p.entry = entry;
    p.code.assign(code, code + kCodeSize);
    g->pending.push_back(std::move(p));
    g->wake.notify_one();
}

std::vector<Program> take() {
    std::vector<Program> out;
    if (!g) return out;
    std::lock_guard lock(g->mutex);
    out.swap(g->done);
    g_ready.store(false, std::memory_order_release);
    return out;
}

}  // namespace ps2::vu::live

#endif
