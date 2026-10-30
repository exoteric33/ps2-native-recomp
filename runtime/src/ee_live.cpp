#include "ps2/ee_live.hpp"

#ifdef __EMSCRIPTEN__

// The browser build has no compiler to call: code the build does not know still stops the game there.
namespace ps2::ee::live {
void start(const std::string &, const std::string &, const std::string &) {}
bool active() { return false; }
bool translate_missing(std::uint32_t) { return false; }
void frame_tick() {}
}  // namespace ps2::ee::live

#else

#include "live_build.hpp"

#include "eerecomp_gen.hpp"
#include "ps2/ee_live_abi.hpp"
#include "ps2/generated.hpp"
#include "ps2/memory.hpp"
#include "ps2/os.hpp"
#include "ps2/system.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

// The headers the generated code includes, embedded in the runner (cmake/embed.cmake) so the library is compiled against exactly the
// layouts this runner has, whatever sits next to it.
extern const char kLiveHdrCpu[], kLiveHdrFpu[], kLiveHdrVu[], kLiveHdrMemory[], kLiveHdrMmi[], kLiveHdrGenerated[], kLiveHdrEeLiveAbi[];

namespace lb = ::ps2::live;
namespace fs = std::filesystem;

namespace ps2::ee::live {

namespace {

// Bump when recomp/codegen.cpp or recomp/analysis.cpp change what they write for the same code in a way the headers do not show: an
// old library would otherwise still be loaded (the cache also drops libraries of another runner version by the stamp).
constexpr const char *kGeneratorVersion = "ee-live 3";

using Clock = lb::Clock;

// A window translated in the background (frame_tick): made by the worker thread from a copy of the window, switched in by the emulation
// thread.
enum JobState { kQueued, kRunning, kBuilt, kFailed, kCancelled };

struct BgJob {
    std::uint32_t base = 0;
    std::string name;                     // window_name() of the image
    std::vector<std::uint8_t> image;      // the window as it was when its content settled
    std::uint64_t frame = 0;              // the frame it was queued in
    Clock::time_point queued = Clock::now();
    std::atomic<int> state{kQueued};
    std::atomic<bool> urgent{false};      // the game waits for it: the compilers go to normal priority
    std::atomic<bool> cancel{false};      // the window changed under it: end the build, nobody wants the result
    bool quick = false;                   // built with little optimisation (State::quick_flags): the game may be about to wait for it
    bool upgrade = false;                 // the window has a quick library already: this one is the same code with the normal flags
    bool sparse = false;                  // a loaded program (not a window): checked by the first words of its functions
    std::vector<std::uint32_t> candidates;   // pointers into the image that were found in memory when the quick library was made
    std::vector<std::uint32_t> entries;      // the entry points the quick library was made from
    // Filled in by the worker before the state turns kBuilt or kFailed.
    fs::path library;
    std::string error;
    bool no_code = false;                 // the window holds no code (data): nothing to translate, not a failure
    lb::Timing timing;
    double seconds = 0;                   // the worker's time on it
    std::size_t code = 0, files = 0, entry_points = 0, static_calls = 0, known_entries = 0;
};

// What frame_tick knows of one code window.
struct Watch {
    std::uint64_t hash = 0;
    std::uint32_t extent = 0;
    int stable = 0;                       // how many checks in a row saw the same content
    bool handled = false;                 // this content was looked at already (a job was made, or none was needed)
    int jobs = 0;                         // background jobs made for this window so far
};

struct State {
    fs::path program;       // the game's executable
    fs::path cache;         // ee-live: the libraries
    fs::path game_dir;      // the folder port.sh made, or empty (entries.txt, overlays\)
    fs::path compiler;
    std::vector<std::uint32_t> windows;   // bases of the code windows of the program
    std::vector<std::pair<std::uint32_t, std::uint32_t>> segments;   // the code the program itself has: {address, size}
    std::vector<std::pair<std::uint32_t, std::uint32_t>> file_segments;   // everything the executable's file fills, code or data: {address, size in memory}
    std::uint64_t stamp = 0;
    std::vector<std::string> flags;
    std::vector<std::string> quick_flags; // for the first translation of a window, which the game waits for: fast to compile, slower code
    bool two_step = false;                // quick_flags differ from flags: such a library is followed by one made with flags, in the background
    std::set<std::string> fast;           // window images whose whole library is a quick one, still to be made again (marker files)
    std::atomic<int> priority_work{0};    // stops and background builds the game may wait for: the builds that only improve code hold back meanwhile
    std::size_t jobs = 4;                 // compiler processes at a time for a background build
    std::size_t jobs_now = 4;             // for a build the game waits for: every processor thread there is
    bool keep_work = false;
    bool ready = false;     // a compiler was found and the cache folder is usable: misses can be translated
    bool headers = false;   // under prep_mutex
    int failures = 0;
    std::set<std::uint32_t> failed;       // addresses that were tried and did not work: not again
    std::map<std::string, std::string> overlay_kept;   // image name -> the image in overlays\ that stands for it (itself, or a near copy that was there before)
    bool overlay_limit_said = false;      // the log line about the overlay limits was written
    std::set<std::string> whole;          // window images that were translated as a whole (marker files in the cache): later misses translate only what they reach
    std::set<std::uint32_t> flat_started; // bases of loaded-code runs (memory the program never mentions) that were translated as a whole in this run
    std::unique_ptr<recomp::StaticProgram> program_analysis;   // built at the first translation, for the calls into a window (under prep_mutex)
    std::vector<std::uint32_t> main_entries;
    bool analysed = false;
    std::size_t libraries = 0;            // loaded from the cache or made in this run
    Clock::time_point began = Clock::now();

    // Background translation of settled windows. Everything here but what is marked is used by the emulation thread only.
    bool background = true;               // PS2_EE_LIVE_BG=0 switches it off
    bool verbose = false;                 // PS2_EE_LIVE_VERBOSE=1: one line for every window that settles
    std::vector<Watch> watch;             // one per window
    std::uint32_t tick = 0;
    std::vector<std::shared_ptr<BgJob>> active;   // queued, running, or finished and not yet taken
    std::set<std::string> bg_seen;        // image names a job was made for (not again)
    struct Started {                      // a window a job was made for: its 16 KiB units, so that a window inside it is not translated again
        std::uint32_t base = 0, size = 0;
        std::vector<std::uint64_t> units;
    };
    std::vector<Started> started;
    int bg_failures = 0;
    std::uint64_t looks = 0, look_us = 0, look_max_us = 0;   // the cost of frame_tick's look at the windows
    std::mutex mutex;                     // guards queue, done, worker_started
    std::condition_variable wake;
    std::deque<std::shared_ptr<BgJob>> queue;
    std::vector<std::shared_ptr<BgJob>> done;
    std::atomic<std::size_t> done_count{0};
    bool worker_started = false;
    std::atomic<bool> worker_busy{false};
    std::atomic<bool> quitting{false};
    std::mutex prep_mutex;                // the headers in the cache and the program analysis: made once, by whoever needs them first
    std::mutex generate_mutex;            // the translator is used by one thread at a time
    std::thread prepare;                  // reads the program ahead of the first window translation (prepare_in_background)
    bool prepare_started = false;         // emulation thread only
};

State *g = nullptr;  // never freed: a compile may still be running while the process ends

// One write per line, so that lines of two threads do not mix.
void say(const char *format, auto... args) {
    char text[4096];
    const int n = std::snprintf(text, sizeof text, "[ee-live +%.1fs f%llu] ", lb::seconds_since(g->began), static_cast<unsigned long long>(ps2::frame_count()));
    if constexpr (sizeof...(args) == 0) std::snprintf(text + n, sizeof text - static_cast<std::size_t>(n), "%s", format);
    else std::snprintf(text + n, sizeof text - static_cast<std::size_t>(n), format, args...);
    std::string line = text;
    line += '\n';
    std::fputs(line.c_str(), stderr);
}

EeLiveHost &host() {
    static EeLiveHost h = [] {
        EeLiveHost x{};
        x.layout = static_cast<std::uint64_t>(sizeof(Cpu)) | (static_cast<std::uint64_t>(sizeof(EeLiveHost)) << 32);
        x.ram = g_ram;
        x.spr = g_spr;
        x.watch_on = g_watch_on;
        x.watch_base = g_watch_base;
        x.watch_end = g_watch_end;
        x.dispatch = &ps2::dispatch;
#define X(R, name, params, args) x.name = &ps2::name;
        PS2_EE_LIVE_FUNCTIONS(X)
#undef X
        return x;
    }();
    return h;
}

// Loads one library of the cache and adds its modules to the ones the game has. False when it is not usable.
bool load_library(const fs::path &path, std::size_t &modules, int rank) {
    void *library = lb::open_library(path);
    if (!library) return false;
    using StampFn = std::uint64_t (*)();
    using InitFn = const ModuleInfo *(*)(const EeLiveHost *, std::uint32_t *);
    const auto stamp = reinterpret_cast<StampFn>(lb::find_symbol(library, "ps2_ee_live_stamp"));
    const auto init = reinterpret_cast<InitFn>(lb::find_symbol(library, "ps2_ee_live_init"));
    if (!stamp || !init || stamp() != g->stamp) return false;  // stays loaded: it cannot be unloaded safely, and it is rare
    std::uint32_t n = 0;
    const ModuleInfo *table = init(&host(), &n);
    if (!table || !n) return false;
    // Checks of the entries that only a guess found (recomp::HeadChecks): a library from before them has no such function.
    using SoftFn = const std::uint32_t *(*)(std::uint32_t, std::uint32_t *);
    const auto soft_of = reinterpret_cast<SoftFn>(lb::find_symbol(library, "ps2_ee_live_soft"));
    std::vector<ps2::SoftChecks> soft(n);
    if (soft_of)
        for (std::uint32_t k = 0; k < n; ++k) soft[k].words = soft_of(k, &soft[k].count);
    ps2::add_modules_ranked(table, n, rank, soft.data());
    modules += n;
    ++g->libraries;
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

std::vector<std::uint8_t> read_file(const fs::path &p) {
    std::ifstream in(p, std::ios::binary);
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// The captured windows in overlays\ are bounded, because the next build of the game translates every one of them. A window whose
// code shares its memory with data that keeps changing (Tekken 5 at 0x00488000: the loaded game's own tables) gets a new name, the
// CRC-32 of its content, at nearly every miss, and 1400 images of 256 KiB (374 MB) were written in a few runs. The rules:
//  - an image that differs from one already there in at most 1/16 of its words is the same code with other data: that image stands
//    for it (and is marked as just used), nothing is written, and the entry points go under the name of the one that is kept;
//  - at most kOverlaysPerBase images per window address, kOverlaysTotal in all, kOverlayBytes together. Past a limit the images
//    used least recently (by the time of the file) go: the folder keeps what the latest plays needed.
// 8 per address is enough for a game that loads one image per area into the same window (Silent Hill 2: a handful of areas a
// session), 32 in all and 16 MiB hold several windows at 256 KiB each, and the build analyses every image once: 32 stay quick.
// scripts/windows.py (capture and trim) uses the same numbers and the same rule for a folder that is too full from before.
// PS2_EE_LIVE_OVERLAYS=<total> lowers the total (a test of the limit).
constexpr std::size_t kOverlaysPerBase = 8, kOverlaysTotal = 32;
constexpr std::uint64_t kOverlayBytes = 16u << 20;

struct SavedOverlay {
    fs::path file;
    std::string name;
    std::uint32_t base = 0;
    std::uint64_t size = 0;
    fs::file_time_type time;
};

// <8 hex digits>_<8 hex digits>.bin: the names the runner and windows.py make (eerecomp reads other names too; those are left alone).
bool overlay_file(const fs::path &p, std::string &name, std::uint32_t &base) {
    name = ps2::os::utf8(p.stem());
    if (p.extension() != ".bin" || name.size() != 17 || name[8] != '_') return false;
    for (std::size_t i = 0; i < 17; ++i)
        if (i != 8 && !std::isxdigit(static_cast<unsigned char>(name[i]))) return false;
    base = static_cast<std::uint32_t>(std::stoul(name.substr(0, 8), nullptr, 16));
    return true;
}

std::vector<SavedOverlay> list_overlays(const fs::path &dir) {
    std::vector<SavedOverlay> all;
    std::error_code error;
    for (fs::directory_iterator it(dir, error), end; !error && it != end; it.increment(error)) {
        SavedOverlay o;
        if (!it->is_regular_file(error) || !overlay_file(it->path(), o.name, o.base)) continue;
        o.file = it->path();
        o.size = it->file_size(error);
        o.time = it->last_write_time(error);
        all.push_back(std::move(o));
    }
    return all;
}

// The same code with other data: at most 1/16 of the words differ (a different length counts as different words).
bool near_duplicate(const std::vector<std::uint8_t> &a, const std::vector<std::uint8_t> &b) {
    const std::size_t common = std::min(a.size(), b.size()) / 4, longest = std::max(a.size(), b.size()) / 4;
    std::size_t different = longest - common;
    for (std::size_t i = 0; i < common && different * 16 <= longest; ++i)
        if (std::memcmp(a.data() + i * 4, b.data() + i * 4, 4) != 0) ++different;
    return different * 16 <= longest;
}

// Removes the images past the limits, least recently used first; `keep` is never removed.
void trim_overlays(std::vector<SavedOverlay> all, const std::string &keep) {
    static const std::size_t total_limit = [] {
        const std::string v = ps2::os::env("PS2_EE_LIVE_OVERLAYS");
        const long n = v.empty() ? 0 : std::strtol(v.c_str(), nullptr, 10);
        return n > 0 ? std::min<std::size_t>(static_cast<std::size_t>(n), kOverlaysTotal) : kOverlaysTotal;
    }();
    std::stable_sort(all.begin(), all.end(), [&](const SavedOverlay &a, const SavedOverlay &b) {
        if ((a.name == keep) != (b.name == keep)) return a.name == keep;
        return a.time > b.time;
    });
    std::map<std::uint32_t, std::size_t> per_base;
    std::size_t kept = 0, removed = 0;
    std::uint64_t bytes = 0;
    for (const SavedOverlay &o : all) {
        const bool fits = per_base[o.base] < std::min(kOverlaysPerBase, total_limit) && kept < total_limit && bytes + o.size <= kOverlayBytes;
        if (fits || o.name == keep) {
            ++per_base[o.base];
            ++kept;
            bytes += o.size;
            continue;
        }
        std::error_code error;
        if (fs::remove(o.file, error)) ++removed;
    }
    if (removed && !g->overlay_limit_said) {
        g->overlay_limit_said = true;
        say("the folder overlays\\ is at its limit (%zu images per window address, %zu in all, %u MiB): removed %zu of the images used least recently; this is said once", std::min(kOverlaysPerBase, total_limit), total_limit,
            static_cast<unsigned>(kOverlayBytes >> 20), removed);
    }
}

// Saves a captured window image, or finds the saved one that stands for it; returns the name it is saved under.
std::string save_overlay(const fs::path &dir, const std::string &name, std::uint32_t base, const std::vector<std::uint8_t> &image) {
    std::error_code error;
    const auto touch = [&](const fs::path &f) { fs::last_write_time(f, fs::file_time_type::clock::now(), error); };
    const fs::path file = dir / (name + ".bin");
    if (fs::exists(file, error)) {
        touch(file);
        return name;
    }
    std::vector<SavedOverlay> all = list_overlays(dir);
    std::vector<const SavedOverlay *> same_base;
    for (const SavedOverlay &o : all)
        if (o.base == base) same_base.push_back(&o);
    std::sort(same_base.begin(), same_base.end(), [](const SavedOverlay *a, const SavedOverlay *b) { return a->time > b->time; });
    for (const SavedOverlay *o : same_base)
        if (o->size >= image.size() / 2 && o->size <= image.size() * 2 && near_duplicate(read_file(o->file), image)) {
            touch(o->file);
            return o->name;
        }
    {
        std::ofstream out(file, std::ios::binary);
        out.write(reinterpret_cast<const char *>(image.data()), static_cast<std::streamsize>(image.size()));
    }
    SavedOverlay added;
    added.file = file;
    added.name = name;
    added.base = base;
    added.size = image.size();
    added.time = fs::file_time_type::clock::now();
    all.push_back(std::move(added));
    trim_overlays(std::move(all), name);
    return name;
}

// Puts what was found where port.sh keeps it, when the game folder has them: the entry point in entries.txt and the captured window
// in overlays\, so that the next build of the game translates it ahead of time (scripts/windows.py capture does the same).
void remember(const std::string &name, std::uint32_t base, const std::vector<std::uint8_t> &image, const std::vector<std::uint32_t> &entries, bool program) {
    if (g->game_dir.empty()) return;
    std::error_code error;
    // The image that stands for this one in overlays\ (itself unless a near copy was saved before): entries go under its name.
    std::string kept = name;
    const fs::path overlays = g->game_dir / "overlays";
    if (!program && fs::is_directory(overlays, error)) {
        const auto it = g->overlay_kept.find(name);
        if (it != g->overlay_kept.end()) kept = it->second;
        else g->overlay_kept[name] = kept = save_overlay(overlays, name, base, image);
    }
    const fs::path entries_file = g->game_dir / "entries.txt";
    if (fs::is_regular_file(entries_file, error)) {
        std::set<std::uint32_t> known;
        const recomp::Entries have = recomp::read_entries(ps2::os::utf8(entries_file));
        if (program) known.insert(have.main.begin(), have.main.end());
        else if (const auto it = have.modules.find(kept); it != have.modules.end()) known.insert(it->second.begin(), it->second.end());
        std::string text;
        for (const std::uint32_t entry : entries) {
            if (!known.insert(program ? entry : entry - base).second) continue;
            char token[96];
            if (program) std::snprintf(token, sizeof token, "%x\n", entry);   // an entry point of the program itself
            else std::snprintf(token, sizeof token, "%s:%x\n", kept.c_str(), entry - base);
            text += token;
        }
        if (!text.empty()) {
            // The file ends with a newline in the way port.sh writes it; if some other tool left it without, start a new line.
            bool needs_newline = false;
            if (std::ifstream in(entries_file, std::ios::binary | std::ios::ate); in && in.tellg() > 0) {
                in.seekg(-1, std::ios::end);
                needs_newline = in.get() != '\n';
            }
            std::ofstream out(entries_file, std::ios::binary | std::ios::app);
            if (needs_newline) out << '\n';
            out << text;
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------- building

// What building one library gave. `why` is empty when the reason was already said (the compiler's own messages).
struct Built {
    bool ok = false;
    fs::path library;
    fs::path work;
    lb::Timing timing;
    recomp::LiveResult generated;
    std::size_t files = 0;
    std::string why;
};

// The analysis of the program itself (about half a second for a big game), made once: by whoever needs it first, or ahead of time by
// the thread prepare_in_background() starts. Everything that needs it waits for the lock.
void ensure_analysis() {
    std::error_code error;
    std::lock_guard lock(g->prep_mutex);
    if (g->analysed) return;
    g->analysed = true;
    if (!g->game_dir.empty() && fs::is_regular_file(g->game_dir / "entries.txt", error))
        g->main_entries = recomp::read_entries(ps2::os::utf8(g->game_dir / "entries.txt")).main;
    try {
        g->program_analysis = std::make_unique<recomp::StaticProgram>(ps2::os::utf8(g->program), g->main_entries);
    } catch (const std::exception &e) {
        say("cannot read the program for its calls into the window: %s", e.what());
    }
}

// The part of the work that needs the program analysis: the calls the program itself makes into the window and what entries.txt knows
// of this image from before, as entry points of the first translation of the window.
void add_window_entries(recomp::LiveRequest &request, std::size_t &static_calls, std::size_t &known_entries) {
    std::error_code error;
    ensure_analysis();
    if (g->program_analysis)
        for (const std::uint32_t t : g->program_analysis->calls_into(request.base, static_cast<std::uint32_t>(request.image.size()))) {
            request.entries.push_back(t);
            ++static_calls;
        }
    if (!g->game_dir.empty() && fs::is_regular_file(g->game_dir / "entries.txt", error)) {
        const recomp::Entries have = recomp::read_entries(ps2::os::utf8(g->game_dir / "entries.txt"));
        if (const auto it = have.modules.find(request.name); it != have.modules.end())
            for (const std::uint32_t off : it->second) {
                request.entries.push_back(request.base + off);
                ++known_entries;
            }
    }
    std::sort(request.entries.begin(), request.entries.end());
    request.entries.erase(std::unique(request.entries.begin(), request.entries.end()), request.entries.end());
}

// Translates, compiles and links one request into a library file of the cache (not loaded yet). `what` names it in messages. A
// background build compiles below normal priority, never starves into normal on its own, and goes to normal when `urgent` is set.
Built build_library(recomp::LiveRequest request, const std::string &what, bool background, const std::atomic<bool> *urgent, const std::atomic<bool> *cancel, bool quick = false, bool upgrade = false) {
    Built b;
    const std::string id = lb::hex(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count()), 11) +
                           (background ? "b" : "") + (quick ? "q" : "");
    request.prefix = "m_" + request.name + "_" + id + "_";
    request.stamp = g->stamp;
    b.work = g->cache / ("work-" + id);
    const fs::path src = b.work / "src";
    std::error_code error;
    fs::create_directories(src, error);
    {
        std::lock_guard lock(g->prep_mutex);
        if (error || !(g->headers = g->headers || lb::write_headers(g->cache / "include", {{"cpu.hpp", kLiveHdrCpu}, {"fpu.hpp", kLiveHdrFpu}, {"vu.hpp", kLiveHdrVu}, {"memory.hpp", kLiveHdrMemory}, {"mmi.hpp", kLiveHdrMmi}, {"generated.hpp", kLiveHdrGenerated}, {"ee_live_abi.hpp", kLiveHdrEeLiveAbi}}))) {
            b.why = "cannot write into " + ps2::os::utf8(g->cache) + ": the game stops";
            return b;
        }
    }
    auto t = Clock::now();
    {
        std::lock_guard lock(g->generate_mutex);
        b.generated = recomp::generate_live(request, src);
    }
    b.timing.generate = lb::seconds_since(t);
    if (!b.generated.ok) {
        b.why = "the translator could not use " + what + ": " + b.generated.error;
        return b;
    }

    lb::Job job;
    for (const auto &e : fs::directory_iterator(src, error))
        if (e.path().extension() == ".cpp") job.sources.push_back(e.path());
    // The biggest first: the compile ends when the last file does, so the long ones should not start last.
    std::sort(job.sources.begin(), job.sources.end(), [&](const fs::path &a, const fs::path &b) {
        std::error_code e1, e2;
        const auto sa = fs::file_size(a, e1), sb = fs::file_size(b, e2);
        return sa != sb ? sa > sb : a < b;
    });
    job.include_dir = g->cache / "include";
    job.work = b.work;
    job.library = b.work / ("library" + std::string(lb::kLibraryExtension));
    job.compiler = g->compiler;
    job.flags = quick ? g->quick_flags : g->flags;
    job.jobs = std::min(background ? g->jobs : g->jobs_now, job.sources.size());
    job.jobs_urgent = std::min(g->jobs_now, job.sources.size());
    job.below_normal = background;   // a waiting game needs this now; a background build leaves the game its processors
    job.starve = !background;
    job.urgent = urgent;
    job.cancel = cancel;
    job.hold = upgrade ? &g->priority_work : nullptr;   // a build that only improves code waits while a stop or a build the game may need is under way
    job.say = [](const std::string &text) { say("%s", text.c_str()); };
    b.files = job.sources.size();
    if (!lb::compile_and_link(job, b.timing)) return b;   // the compiler's messages were said

    b.library = g->cache / (lb::hex(g->stamp >> 32, 8) + "-" + id + lb::kLibraryExtension);
    fs::rename(job.library, b.library, error);
    if (error) {
        b.why = "cannot move the library into " + ps2::os::utf8(g->cache) + ": " + error.message();
        return b;
    }
    if (!g->keep_work) fs::remove_all(b.work, error);
    b.ok = true;
    return b;
}

// ---------------------------------------------------------------------------------------------------------------- background

// The worker thread: one background build at a time, in the order the windows settled.
void worker_main() {
    for (;;) {
        std::shared_ptr<BgJob> job;
        {
            std::unique_lock lock(g->mutex);
            g->wake.wait(lock, [] { return !g->queue.empty() || g->quitting.load(); });
            if (g->quitting.load()) return;
            job = g->queue.front();
            g->queue.pop_front();
            job->state = kRunning;
            g->worker_busy = true;
        }
        const auto began = Clock::now();
        recomp::LiveRequest request;
        request.base = job->base;
        request.image = job->image;
        request.name = job->name;
        request.focused = false;   // a first translation: all of the window
        request.sparse_checks = job->sparse;
        request.candidates = job->candidates;
        request.entries = job->entries;
        add_window_entries(request, job->static_calls, job->known_entries);
        const Built built = g->quitting.load() || job->cancel.load() ? Built{} : build_library(request, "the window " + job->name, true, &job->urgent, &job->cancel, job->quick, job->upgrade);
        job->timing = built.timing;
        job->files = built.files;
        job->code = built.generated.code;
        job->entry_points = built.generated.accepted.size();
        job->library = built.library;
        job->error = built.why;
        job->no_code = !built.ok && built.generated.error.rfind("none of the entry points is code", 0) == 0;
        job->seconds = lb::seconds_since(began);
        if (!job->upgrade) --g->priority_work;
        {
            std::lock_guard lock(g->mutex);
            job->state = built.ok ? kBuilt : kFailed;
            g->done.push_back(job);
            g->done_count = g->done.size();
            g->worker_busy = false;
        }
    }
}

void quit_worker() {
    if (g->looks) std::fprintf(stderr, "[ee-live] background watch: %llu looks at %zu windows, %.0f us each on average, %.0f us at most; %zu windows translated in the background\n", static_cast<unsigned long long>(g->looks), g->windows.size(),
                               static_cast<double>(g->look_us) / static_cast<double>(g->looks), static_cast<double>(g->look_max_us), g->started.size());
    if (g->prepare.joinable()) g->prepare.join();
    if (!g->worker_started) return;
    g->quitting = true;
    g->wake.notify_all();
    lb::stop_all();   // a compile in progress ends, so that the worker does too
    for (int i = 0; i < 300 && g->worker_busy.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
}

// The text of a marker file in the cache: which windows were translated as a whole, and whether by a quick build that is still to be
// followed by one with the normal flags.
void write_marker(const std::string &name, bool fast) {
    std::ofstream(g->cache / (lb::hex(g->stamp >> 32, 8) + "-whole-" + name + ".mark")) << (fast ? "fast\n" : "whole image translated\n");
}

void enqueue(const std::shared_ptr<BgJob> &job) {
    {
        std::lock_guard lock(g->mutex);
        // A build the game may wait for goes in front of the ones that only improve code.
        if (job->upgrade) g->queue.push_back(job);
        else {
            auto at = g->queue.begin();
            while (at != g->queue.end() && !(*at)->upgrade) ++at;
            g->queue.insert(at, job);
        }
        if (!g->worker_started) {
            g->worker_started = true;
            std::thread(worker_main).detach();
        }
    }
    if (!job->upgrade) ++g->priority_work;
    g->active.push_back(job);
    g->wake.notify_one();
}

// A window with a quick library: the same code again with the normal flags, in the background. The game goes on meanwhile; at a frame
// boundary the better library is switched in, and it runs from then on (dispatch.cpp, ranks).
void queue_upgrade(std::uint32_t base, const std::string &name, std::vector<std::uint8_t> image, bool sparse = false, std::vector<std::uint32_t> candidates = {}, std::vector<std::uint32_t> entries = {}) {
    if (!g->two_step || !g->background || !g->fast.count(name) || g->bg_failures >= 2) return;
    for (const std::shared_ptr<BgJob> &j : g->active)
        if (j->upgrade && j->name == name) return;
    auto job = std::make_shared<BgJob>();
    job->base = base;
    job->image = std::move(image);
    job->name = name;
    job->frame = ps2::frame_count();
    job->upgrade = true;
    job->sparse = sparse;
    job->candidates = std::move(candidates);
    job->entries = std::move(entries);
    if (g->verbose) say("window 0x%08X (image %s): the quick library is in use, making it again with the normal flags in the background", base, name.c_str());
    enqueue(job);
}

// The emulation thread: takes one finished build into use, if the window still holds what it was made from.
void finish_background(const std::shared_ptr<BgJob> &job) {
    std::error_code error;
    const std::uint32_t size = static_cast<std::uint32_t>(job->image.size());
    if (job->cancel.load()) {
        if (g->verbose) say("the background translation of window 0x%08X (image %s) was ended: the window changed", job->base, job->name.c_str());
        return;
    }
    if (job->no_code) {
        if (g->verbose) say("window 0x%08X (image %s) holds no code: nothing to translate", job->base, job->name.c_str());
        return;
    }
    if (job->state != kBuilt) {
        say("the background translation of window 0x%08X (image %s) did not work%s%s", job->base, job->name.c_str(), job->error.empty() ? "" : ": ", job->error.c_str());
        if (++g->bg_failures >= 2) {
            g->background = false;
            say("no more windows are translated in the background (the game still translates what it needs when it needs it)");
        }
        return;
    }
    if (job->upgrade) {
        // The window need not be the same any more: the library is of this content, serves it whenever it is in place (the check words say)
        // and is taken in like the others. It runs instead of the quick one from now on.
        std::size_t modules = 0;
        if (!load_library(job->library, modules, 1)) {
            say("the library made in the background does not load (%s)", ps2::os::utf8(job->library.filename()).c_str());
            if (++g->bg_failures >= 2) g->background = false;
            return;
        }
        g->fast.erase(job->name);
        write_marker(job->name, false);
        say("switched in the library of window 0x%08X (image %s, %u KiB) made with the normal flags: %.1f s after the quick one (translate %.1f, compile %.1f, link %.1f), %zu instructions, %zu files", job->base, job->name.c_str(), size / 1024,
            lb::seconds_since(job->queued), job->timing.generate, job->timing.compile, job->timing.link, job->code, job->files);
        return;
    }
    // A result of a guess that went stale is dropped, never used: the window has to hold exactly the image that was translated, in size
    // and in every byte. (A library also checks every translated word of the code against the window each time it is entered, so a
    // wrong one could not run anyway; this keeps it from being loaded at all.)
    const bool same = recomp::window_extent(g_ram, kRamSize, job->base) == size && std::memcmp(g_ram + job->base, job->image.data(), size) == 0;
    if (!same) {
        say("dropped the background translation of window 0x%08X (image %s): the window changed while it was built", job->base, job->name.c_str());
        fs::remove(job->library, error);
        return;
    }
    if (ps2::window_covered(job->base, size)) {
        say("the background translation of window 0x%08X (image %s) is not needed any more: another library serves it", job->base, job->name.c_str());
        return;
    }
    std::size_t modules = 0;
    if (!load_library(job->library, modules, job->quick ? 0 : 1)) {
        say("the library made in the background does not load (%s)", ps2::os::utf8(job->library.filename()).c_str());
        if (++g->bg_failures >= 2) g->background = false;
        return;
    }
    g->whole.insert(job->name);
    if (job->quick) g->fast.insert(job->name);
    write_marker(job->name, job->quick);
    remember(job->name, job->base, job->image, {}, false);
    say("switched in the background translation of window 0x%08X (image %s, %u KiB%s): settled at frame %llu, ready %.1f s later (translate %.1f, compile %.1f, link %.1f), %zu entry points%s%s, %zu instructions, %zu files",
        job->base, job->name.c_str(), size / 1024, job->quick ? ", quick" : "", static_cast<unsigned long long>(job->frame), lb::seconds_since(job->queued), job->timing.generate, job->timing.compile, job->timing.link, job->entry_points,
        job->static_calls ? " (calls of the program included)" : "", job->known_entries ? " (entries.txt included)" : "", job->code, job->files);
    if (job->quick) queue_upgrade(job->base, job->name, job->image);
}

// The emulation thread: every finished background build, in the order they finished.
void take_finished() {
    std::vector<std::shared_ptr<BgJob>> finished;
    {
        std::lock_guard lock(g->mutex);
        finished.swap(g->done);
        g->done_count = 0;
    }
    for (const std::shared_ptr<BgJob> &job : finished) {
        g->active.erase(std::remove(g->active.begin(), g->active.end(), job), g->active.end());
        finish_background(job);
    }
}

// The emulation thread: the game needs code at pc, in the window at `base` that holds the image `name`. When a background build of
// exactly this image is under way the game waits for it (it goes to normal priority meanwhile); one that has not started is not worth
// waiting for. True when code for pc is there afterwards.
bool use_background(std::uint32_t pc, std::uint32_t base, const std::string &name) {
    std::shared_ptr<BgJob> job;
    for (const std::shared_ptr<BgJob> &candidate : g->active)
        if (candidate->base == base && candidate->name == name && !candidate->upgrade) job = candidate;   // an upgrade is not waited for: the window has a library
    if (!job) return false;
    if (job->state == kQueued) {
        std::lock_guard lock(g->mutex);
        const auto at = std::find(g->queue.begin(), g->queue.end(), job);
        if (at != g->queue.end()) {   // not started: the translation the game would wait for is the one it can start itself
            g->queue.erase(at);
            job->state = kCancelled;
            --g->priority_work;
            g->active.erase(std::remove(g->active.begin(), g->active.end(), job), g->active.end());
            return false;
        }
    }
    if (job->state == kRunning) {
        job->urgent = true;
        say("no translated code at 0x%08X: the background translation of window 0x%08X (image %s) is under way for %.1f s, the game waits for it", pc, base, name.c_str(), lb::seconds_since(job->queued));
        while (job->state.load() == kRunning) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    take_finished();
    return ps2::has_code(pc);
}

std::uint64_t hash_words(const std::uint8_t *p, std::size_t n) {
    std::uint64_t h = 0xCBF29CE484222325ull;
    for (std::size_t i = 0; i + 8 <= n; i += 8) {
        std::uint64_t w;
        std::memcpy(&w, p + i, 8);
        h = (h ^ w) * 0x100000001B3ull;
        h ^= h >> 29;
    }
    return h;
}

// Code has "jr ra" in it every few hundred words; a window of data (tables, pictures, sound) hardly ever holds that word.
bool has_return(const std::uint8_t *p, std::size_t n) {
    for (std::size_t i = 0; i + 4 <= n; i += 4) {
        std::uint32_t w;
        std::memcpy(&w, p + i, 4);
        if (w == 0x03E00008u) return true;
    }
    return false;
}

bool all_zero(const std::uint8_t *p, std::size_t n) {
    for (std::size_t i = 0; i + 8 <= n; i += 8) {
        std::uint64_t w;
        std::memcpy(&w, p + i, 8);
        if (w) return false;
    }
    return true;
}

constexpr std::uint32_t kWatchEvery = 8;      // frames between two looks at the windows (about 0.13 s)
constexpr int kSettledChecks = 3;             // looks in a row that saw the same content: about 0.4 s of no change
constexpr int kJobsPerWindow = 4;             // background builds for one window in a run (a window that keeps changing is not chased)
constexpr std::size_t kMaxActive = 4;         // builds waiting for the worker

std::vector<std::uint64_t> unit_hashes(const std::uint8_t *p, std::uint32_t size) {
    std::vector<std::uint64_t> out;
    for (std::uint32_t at = 0; at + recomp::kWindowUnit <= size; at += recomp::kWindowUnit) out.push_back(hash_words(p + at, recomp::kWindowUnit));
    return out;
}

// Windows are 16 KiB steps in one area of memory, each reaching to the end of what the game wrote: a window sits inside another one that
// holds the same bytes. The translation of the bigger one has the code of the smaller one.
bool inside_started(std::uint32_t base, std::uint32_t extent, const std::vector<std::uint64_t> &units) {
    for (const State::Started &s : g->started) {
        if (base < s.base || base + extent > s.base + s.size || (base - s.base) % recomp::kWindowUnit) continue;
        const std::size_t first = (base - s.base) / recomp::kWindowUnit;
        if (std::equal(units.begin(), units.end(), s.units.begin() + static_cast<std::ptrdiff_t>(first))) return true;
    }
    return false;
}

// The content of window `base` (extent bytes) has stopped changing. Starts translating it in the background unless something serves it.
// False when it has to wait for the worker (the window is looked at again at the next look).
bool start_background(Watch &w, std::uint32_t base, std::uint32_t extent) {
    if (all_zero(g_ram + base, extent) || !has_return(g_ram + base, extent)) return true;   // empty, or data
    if (ps2::window_covered(base, extent)) {   // the build or an earlier library has this code
        // A quick library from an earlier run, or one made while the window was loading: the better one is made now that it is back.
        if (g->fast.empty() || w.jobs >= kJobsPerWindow) return true;
        const std::string name = recomp::window_name(base, g_ram + base, extent);
        if (!g->fast.count(name)) return true;
        if (g->active.size() >= kMaxActive) return false;
        ++w.jobs;
        queue_upgrade(base, name, std::vector<std::uint8_t>(g_ram + base, g_ram + base + extent));
        return true;
    }
    if (w.jobs >= kJobsPerWindow) return true;
    const std::vector<std::uint64_t> units = unit_hashes(g_ram + base, extent);
    if (inside_started(base, extent, units)) return true;
    if (g->active.size() >= kMaxActive) return false;
    auto job = std::make_shared<BgJob>();
    job->base = base;
    job->image.assign(g_ram + base, g_ram + base + extent);
    job->name = recomp::window_name(base, job->image.data(), job->image.size());
    if (g->whole.count(job->name) || !g->bg_seen.insert(job->name).second) return true;   // done before (a library of the cache, or an earlier job)
    job->frame = ps2::frame_count();
    job->quick = g->two_step;
    ++w.jobs;
    g->started.push_back({base, extent, units});
    if (g->verbose) say("window 0x%08X settled (%u KiB, image %s): translating it in the background while the game goes on", base, extent / 1024, job->name.c_str());
    enqueue(job);
    return true;
}

// A build whose window has changed (it is still being loaded, or the game put other code there) is of no use: end it, so that the
// build of what the window holds now does not wait behind it.
void drop_stale_jobs() {
    for (std::size_t i = 0; i < g->active.size();) {
        const std::shared_ptr<BgJob> job = g->active[i];
        const std::uint32_t size = static_cast<std::uint32_t>(job->image.size());
        const bool state_ok = job->state == kQueued || job->state == kRunning;
        if (state_ok && !job->cancel.load() && (recomp::window_extent(g_ram, kRamSize, job->base) != size || std::memcmp(g_ram + job->base, job->image.data(), size) != 0)) {
            job->cancel = true;
            if (job->state == kQueued) {
                std::lock_guard lock(g->mutex);
                const auto at = std::find(g->queue.begin(), g->queue.end(), job);
                if (at != g->queue.end()) {
                    g->queue.erase(at);
                    job->state = kCancelled;
                    if (!job->upgrade) --g->priority_work;
                    g->active.erase(g->active.begin() + static_cast<std::ptrdiff_t>(i));
                    continue;
                }
            }
        }
        ++i;
    }
}

// A window with code nobody serves has appeared: the stop that may follow it needs the analysis of the program, so it is made now, on a
// thread of its own, while the game still streams the window in or plays on.
void prepare_in_background() {
    if (g->prepare_started) return;
    g->prepare_started = true;
    g->prepare = std::thread([] { ensure_analysis(); });
}

void watch_windows() {
    const auto began = Clock::now();
    drop_stale_jobs();
    std::vector<std::pair<std::uint32_t, std::size_t>> settled;   // {base, window}
    for (std::size_t i = 0; i < g->windows.size(); ++i) {
        const std::uint32_t base = g->windows[i];
        if (base >= kRamSize || kRamSize - base < recomp::kWindowUnit) continue;
        Watch &w = g->watch[i];
        const std::uint32_t extent = recomp::window_extent(g_ram, kRamSize, base);
        const std::uint64_t hash = hash_words(g_ram + base, extent);
        if (hash != w.hash || extent != w.extent) {
            static const std::string trace = ps2::os::env("PS2_EE_LIVE_WATCH");   // debugging: hex bases of windows whose changes are said
            if (!trace.empty() && trace.find(lb::hex(base, 8)) != std::string::npos) say("window 0x%08X changed: %u KiB, hash %016llx", base, extent / 1024, static_cast<unsigned long long>(hash));
            w.hash = hash;
            w.extent = extent;
            w.stable = 0;
            w.handled = false;
            if (!g->prepare_started && !all_zero(g_ram + base, extent) && has_return(g_ram + base, extent) && !ps2::window_covered(base, extent)) prepare_in_background();
            continue;
        }
        if (w.handled || ++w.stable < kSettledChecks) continue;
        w.handled = true;
        settled.emplace_back(base, i);
    }
    // The lowest and the biggest first, so that a window inside another one finds the other one started.
    std::sort(settled.begin(), settled.end(), [](const auto &a, const auto &b) { return a.first != b.first ? a.first < b.first : g->watch[a.second].extent > g->watch[b.second].extent; });
    for (const auto &[base, i] : settled)
        if (!start_background(g->watch[i], base, g->watch[i].extent)) g->watch[i].handled = false;
    const std::uint64_t us = static_cast<std::uint64_t>(lb::seconds_since(began) * 1e6);
    ++g->looks;
    g->look_us += us;
    g->look_max_us = std::max(g->look_max_us, us);
    if (g->verbose && g->looks % 100 == 0)
        say("background watch: %llu looks at %zu windows, %.0f us each on average, %.0f us at most", static_cast<unsigned long long>(g->looks), g->windows.size(), static_cast<double>(g->look_us) / static_cast<double>(g->looks), static_cast<double>(g->look_max_us));
}

}  // namespace

void frame_tick() {
    if (!g || !g->ready || !g->background) return;
    if (g->done_count.load()) take_finished();
    if (++g->tick < kWatchEvery) return;
    g->tick = 0;
    watch_windows();
}

void start(const std::string &program, const std::string &cache, const std::string &game_dir) {
    if (g || cache.empty()) return;
    g = new State;
    g->program = ps2::os::path(program);
    g->cache = ps2::os::path(cache);
    g->game_dir = ps2::os::path(game_dir);
    const std::vector<std::uint8_t> elf = read_file(g->program);
    g->windows = recomp::code_windows(elf);
    g->segments = recomp::code_segments(elf);
    g->file_segments = recomp::file_segments(elf);
    if (elf.empty()) return;  // the program cannot be read (the runner will say so): nothing to translate for

    // PS2_EE_LIVE_OPT=-O0 .. -O3 chooses the optimisation level of the libraries; the game waits while one is made, so a quick compile
    // beats fast code (docs/ee-live.md has the measurements).
    std::string opt = "-O1";
    if (const std::string v = ps2::os::env("PS2_EE_LIVE_OPT"); !v.empty()) opt = v;
    g->flags = lb::compile_flags(opt);
    // PS2_EE_LIVE_QUICK=<level> is the level of the first translation of a window, the one the game waits for (default -O0: about half the
    // compile time, code a few percent slower); it is followed by the same translation at the PS2_EE_LIVE_OPT level in the background.
    // PS2_EE_LIVE_QUICK=off waits for a build at the normal level.
    std::string quick = "-O0";
    if (const std::string v = ps2::os::env("PS2_EE_LIVE_QUICK"); !v.empty()) quick = v;
    g->quick_flags = lb::compile_flags(quick == "off" ? opt : quick);
    g->two_step = quick != "off" && g->quick_flags != g->flags;
    g->keep_work = !ps2::os::env("PS2_EE_LIVE_KEEP").empty();
    g->jobs = std::clamp<std::size_t>(std::thread::hardware_concurrency() * 3 / 4, 2, 9);   // a build in the background: most of the machine, below normal priority
    g->jobs_now = std::clamp<std::size_t>(std::thread::hardware_concurrency(), 2, 16);   // the game is paused while this runs: all of it
    if (const std::string v = ps2::os::env("PS2_EE_LIVE_JOBS"); !v.empty()) g->jobs = g->jobs_now = std::clamp<std::size_t>(std::strtoul(v.c_str(), nullptr, 10), 1, 16);

    // The stamp says which libraries this runner may load: the translator, the headers, the flags and the program they were made for.
    std::uint64_t h = 0xCBF29CE484222325ull;
    h = lb::fnv(h, kGeneratorVersion, std::strlen(kGeneratorVersion));
    for (const char *text : {kLiveHdrCpu, kLiveHdrFpu, kLiveHdrVu, kLiveHdrMemory, kLiveHdrMmi, kLiveHdrGenerated, kLiveHdrEeLiveAbi})
        h = lb::fnv(h, text, std::strlen(text));
    for (const std::string &f : g->flags)
        if (f.rfind("-O", 0) != 0) h = lb::fnv(h, f.data(), f.size() + 1);   // the level of optimisation does not change what a library computes
    h = lb::fnv(h, elf.data(), elf.size());
    const std::size_t layout = sizeof(Cpu);
    h = lb::fnv(h, &layout, sizeof layout);
    g->stamp = h;

    // What earlier runs left: load it now, and clear out what another version made.
    std::size_t modules = 0, files = 0;
    std::error_code error;
    if (fs::is_directory(g->cache, error)) {
        const std::string mine = lb::hex(g->stamp >> 32, 8);
        std::vector<fs::path> libraries;
        for (const auto &e : fs::directory_iterator(g->cache, error)) {
            std::string prefix;
            const std::string file = ps2::os::utf8(e.path().filename());
            if (e.is_regular_file() && file.size() > 18 && file.ends_with(".mark") && file.compare(8, 7, "-whole-") == 0) {
                if (file.compare(0, 8, mine) == 0) {
                    const std::string name = file.substr(15, file.size() - 20);
                    g->whole.insert(name);
                    if (std::ifstream in(e.path()); in && in.get() == 'f') g->fast.insert(name);   // "fast": only a quick library so far
                } else {
                    fs::remove(e.path(), error);
                }
            } else if (e.is_regular_file() && is_cache_library(e.path(), prefix)) {
                if (prefix == mine) libraries.push_back(e.path());
                else fs::remove(e.path(), error);
            } else if (e.is_directory() && ps2::os::utf8(e.path().filename()).rfind("work-", 0) == 0) {
                fs::remove_all(e.path(), error);  // a build the last run did not finish
            }
        }
        std::sort(libraries.begin(), libraries.end());
        for (const fs::path &p : libraries) {
            // A quick library has a q after the dash; it runs only where no better one of the same content has the function.
            const std::string name = ps2::os::utf8(p.filename());
            if (load_library(p, modules, name.find('q', 9) == std::string::npos ? 1 : 0)) ++files;
            else fs::remove(p, error);
        }
    }
    if (files) say("loaded %zu translated code windows from %zu files in %s", modules, files, cache.c_str());

    g->compiler = lb::find_compiler();
    if (g->compiler.empty()) {
        say("no C++ compiler found (PS2_VU_CXX names one, or put one in a compiler folder next to the runner): code the build does not have stops the game");
        return;
    }
    lb::use_environment();
    // PS2_EE_LIVE_BG=0 switches off the translation of settled windows in the background (frame_tick): only misses are translated then.
    const std::string bg = ps2::os::env("PS2_EE_LIVE_BG");
    g->background = bg != "0" && bg != "off";
    g->verbose = !ps2::os::env("PS2_EE_LIVE_VERBOSE").empty();
    g->watch.assign(g->windows.size(), Watch{});
    if (g->background) std::atexit([] { quit_worker(); });   // a build in progress ends with the program; the line of numbers is said
    g->ready = true;
    say("translating code the build does not have while the game runs%s, with %s, into %s", g->background ? " (windows that settle are translated in the background)" : "", ps2::os::utf8(g->compiler).c_str(), cache.c_str());
}

bool active() { return g && g->ready; }

constexpr std::uint32_t kLoadedCap = 0x400000;   // the longest run of memory taken as a loaded program (loaded_run) or as the image of a segment

// Code the game read from the disc into memory that its executable never mentions (Tekken 5: the boot program is a small loader, the game
// proper is read to 0x001EA000 and called from there). There is no window and no segment to go by, so the run of non-empty 16 KiB units
// around the address is the image, as for a window, only longer: such a program is 1 to 2 MiB with data between its code.
bool loaded_run(std::uint32_t pc, std::uint32_t &base, std::uint32_t &size) {
    constexpr std::uint32_t kUnit = recomp::kWindowUnit, kLowest = 0x00100000u;
    if (pc < kLowest || pc >= kRamSize || (pc & 3u)) return false;
    const auto filled = [](std::uint32_t at) {
        for (std::uint32_t i = 0; i < kUnit; i += 8) {
            std::uint64_t word;
            std::memcpy(&word, g_ram + at + i, 8);
            if (word) return true;
        }
        return false;
    };
    std::uint32_t first = pc & ~(kUnit - 1), last = first;
    if (!filled(first)) return false;
    while (first > kLowest && last - first + kUnit < kLoadedCap / 2 && filled(first - kUnit)) first -= kUnit;
    while (last + 2 * kUnit <= kRamSize && last + kUnit - first < kLoadedCap && filled(last + kUnit)) last += kUnit;
    std::uint32_t begin = first, end = last + kUnit;
    // What the executable's own file fills (its code and its data, which the game writes all the time) is not part of the image.
    for (const auto &[address, bytes] : g->file_segments) {
        if (pc - address < bytes) return false;
        if (address >= end || address + bytes <= begin) continue;
        if (address > pc) end = std::min(end, address);
        else begin = std::max(begin, (address + bytes + 3u) & ~3u);
    }
    if (end - begin < 0x400u) return false;
    base = begin;
    size = end - begin;
    return true;
}

// Words of memory that point into the image [base, base + size) at something that is not zero: possible function starts. A jump table
// a data file brought in, a table of callbacks the game filled in at start-up, a function pointer in an object: none of them is in the
// image, so the search of the image does not see them, and each was one stop of its own (Tekken 5: 918 of 1350). Only those that nothing
// translated serves yet are of use.
std::vector<std::uint32_t> pointers_into(std::uint32_t base, std::uint32_t size) {
    std::vector<std::uint32_t> found;
    for (std::uint32_t at = 0; at + 4 <= kRamSize; at += 4) {
        std::uint32_t word;
        std::memcpy(&word, g_ram + at, 4);
        if (word - base < size && !(word & 3u)) found.push_back(word);
    }
    std::sort(found.begin(), found.end());
    found.erase(std::unique(found.begin(), found.end()), found.end());
    found.erase(std::remove_if(found.begin(), found.end(), [&](std::uint32_t target) {
        std::uint32_t word;
        std::memcpy(&word, g_ram + target, 4);
        return word == 0 || ps2::has_code(target);
    }), found.end());
    return found;
}

// The image for code of the executable's segment [address, address + bytes) around `at`: all of the segment, or the longest stretch kLoadedCap
// allows around `at` when it is bigger than that. At least two units, so that a short segment still has room for a function's tail.
void segment_image(std::uint32_t address, std::uint32_t bytes, std::uint32_t at, std::uint32_t &base, std::uint32_t &size) {
    const std::uint32_t first = address & ~3u, last = first + (bytes & ~3u);
    base = first;
    if (last - first > kLoadedCap) base = std::max(first, std::min((at & ~3u) > kLoadedCap / 2 ? (at & ~3u) - kLoadedCap / 2 : first, last - kLoadedCap));
    size = std::min<std::uint32_t>(std::min(kLoadedCap, last - base), kRamSize - base);
    if (size < 2 * recomp::kWindowUnit) size = std::min<std::uint32_t>(2 * recomp::kWindowUnit, kRamSize - base);
}

// Functions of the program itself that a translated image calls (jal, j) and nothing translated serves. The build never reached them,
// because only this code calls them (Tekken 5: the loader program's library, called by the game it loads: 47 stops of 0.2 to 1 s).
// They are translated right after the image, in one more library, instead of one stop each; their addresses go to entries.txt.
void translate_program_calls(const std::vector<std::uint8_t> &image, std::uint32_t base) {
    std::map<std::size_t, std::vector<std::uint32_t>> by_segment;
    for (std::size_t at = 0; at + 4 <= image.size(); at += 4) {
        std::uint32_t word;
        std::memcpy(&word, image.data() + at, 4);
        if ((word >> 26) != 2u && (word >> 26) != 3u) continue;
        const std::uint32_t target = ((base + static_cast<std::uint32_t>(at) + 4u) & 0xF0000000u) | ((word & 0x03FFFFFFu) << 2);
        if (target - base < image.size()) continue;
        for (std::size_t k = 0; k < g->segments.size(); ++k)
            if (target - g->segments[k].first < g->segments[k].second) {
                by_segment[k].push_back(target);
                break;
            }
    }
    for (auto &[k, targets] : by_segment) {
        std::sort(targets.begin(), targets.end());
        targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
        targets.erase(std::remove_if(targets.begin(), targets.end(), [](std::uint32_t t) { return ps2::has_code(t); }), targets.end());
        while (!targets.empty()) {
        std::uint32_t seg_base = 0, seg_size = 0;
        segment_image(g->segments[k].first, g->segments[k].second, targets.front(), seg_base, seg_size);
        std::vector<std::uint32_t> mine;
        for (const std::uint32_t t : targets)
            if (t - seg_base < seg_size) mine.push_back(t);
        if (mine.empty()) break;   // cannot happen: the first target is inside the image taken around it
        targets.erase(std::remove_if(targets.begin(), targets.end(), [&](std::uint32_t t) { return t - seg_base < seg_size; }), targets.end());
        recomp::LiveRequest request;
        request.base = seg_base;
        request.image.assign(g_ram + seg_base, g_ram + seg_base + seg_size);
        request.name = "main_" + recomp::window_name(request.base, request.image.data(), request.image.size());
        request.focused = true;
        request.entries = mine;
        request.candidates = pointers_into(request.base, static_cast<std::uint32_t>(request.image.size()));
        const auto began = Clock::now();
        const Built built = build_library(request, "functions of the program that " + std::to_string(mine.size()) + " calls reach", false, nullptr, nullptr);
        std::size_t modules = 0;
        if (!built.ok || !load_library(built.library, modules, 1)) {
            if (!built.why.empty()) say("%s", built.why.c_str());
            continue;
        }
        say("switched in %zu functions of the program that the image calls: %.1f s (translate %.1f, compile %.1f, link %.1f), %zu instructions, %zu files", mine.size(), lb::seconds_since(began), built.timing.generate, built.timing.compile,
            built.timing.link, built.generated.code, built.files);
        remember(request.name, request.base, request.image, mine, true);
        }
    }
}

bool translate_missing(std::uint32_t pc) {
    if (!g || !g->ready || g->failed.count(pc)) return false;
    const auto began = Clock::now();
    std::error_code error;
    // The game waits from here on: builds that only improve code start no more compilers until it goes on.
    struct Waiting {
        Waiting() { ++g->priority_work; }
        ~Waiting() { --g->priority_work; }
    } waiting;

    // A window that settled earlier may have been built in the background by now: take it in, and the game goes on without a pause.
    if (g->done_count.load()) {
        take_finished();
        if (ps2::has_code(pc)) return true;
    }

    // Which window is it in: the same rule as scripts/windows.py capture (a window reaches as far as the game wrote to it).
    std::uint32_t base = 0, size = 0;
    bool found = false;
    for (const std::uint32_t b : g->windows) {
        const std::uint32_t extent = recomp::window_extent(g_ram, kRamSize, b);
        if (pc - b < extent && std::any_of(g_ram + b, g_ram + b + extent, [](std::uint8_t v) { return v != 0; })) {
            base = b;
            size = extent;
            found = true;
            break;
        }
    }
    // Not in a window: code of the program itself that its translation did not reach (a function of the program that only window code
    // calls, say; port.sh finds those by running the game and adds them to entries.txt). The program does not change, so two units
    // of memory around the address are the image.
    bool region = false;
    if (!found)
        for (const auto &[address, bytes] : g->segments)
            if (pc - address < bytes) {
                // The whole segment is the image: pointers found in memory (pointers_into) can name any function of it, and one stop then
                // translates all that the game has pointed at so far, not just the missed one.
                segment_image(address, bytes, pc, base, size);
                found = region = true;
                break;
            }
    bool flat = false;
    if (!found && loaded_run(pc, base, size)) found = flat = true;
    if (!found) {
        g->failed.insert(pc);
        say("no translated code at 0x%08X, and it is in none of the %zu code windows, not in the code of the program and not in memory the game loaded: the game stops", pc, g->windows.size());
        return false;
    }
    std::vector<std::uint8_t> image(g_ram + base, g_ram + base + size);
    const std::string name = (region ? "main_" : "") + recomp::window_name(base, image.data(), image.size());

    // The window settled a while ago and its translation is under way in the background: wait for the rest of it.
    if (!region && !flat && use_background(pc, base, name)) {
        say("switched in after %.1f s of waiting for the background translation", lb::seconds_since(began));
        return true;
    }

    // A window image that was translated as a whole before (by this run or an earlier one) only lacks the entry point that was missed:
    // translating all of it again for that costs seconds. One that was not (the build has a module of the same code, made from the
    // entry points it knew) is looked through completely this time.
    const bool whole_done = g->whole.count(name) != 0 || (flat && g->flat_started.count(base) != 0);
    if (g->verbose) say("miss 0x%08X: %s", pc, ps2::module_report(pc).c_str());
    if (region) say("no translated code at 0x%08X (the program's own code, not reached by the build): translating it now, the game waits", pc);
    else say("no translated code at 0x%08X (%s 0x%08X, image %s, %u KiB): translating it now, the game waits", pc, flat ? "loaded code at" : "window", base, name.c_str(), size / 1024);

    recomp::LiveRequest request;
    request.base = base;
    request.image = image;
    request.name = name;
    static const bool force_focus = !ps2::os::env("PS2_EE_LIVE_FOCUS").empty();   // debugging: every miss translates only what it reaches
    request.focused = region || whole_done || force_focus;
    request.sparse_checks = flat;
    const auto pointers_began = Clock::now();
    request.candidates = pointers_into(base, size);
    const double pointers_s = lb::seconds_since(pointers_began);
    // The first translation of a whole window is the long one: it is compiled with little optimisation (the game waits for it), and the
    // same code is made again with the normal flags in the background afterwards. Single functions (the focused ones) are small: normal flags.
    const bool quick = !request.focused && g->two_step;
    request.entries.push_back(pc);
    std::size_t static_calls = 0, known_entries = 0;
    // An image that is translated as a whole now: the calls the program itself makes into the window, and what entries.txt knows of this
    // image from before, are translated together with the entry point that was missed, so that this is not repeated for each of them.
    const auto entries_began = Clock::now();
    if (!request.focused) add_window_entries(request, static_calls, known_entries);
    const double entries_s = lb::seconds_since(entries_began);

    const auto fail = [&](const char *why) {
        g->failed.insert(pc);
        if (++g->failures >= 2) {
            g->ready = false;
            say("translating failed twice: no more code is translated while playing (a log is in the work-* folder of %s)", ps2::os::utf8(g->cache).c_str());
        } else {
            say("%s", why);
        }
        return false;
    };

    const Built built = build_library(request, "0x" + lb::hex(pc, 8), false, nullptr, nullptr, quick);
    if (!built.ok) {
        if (!built.why.empty()) say("%s", built.why.c_str());
        return fail("the game stops");
    }
    std::size_t modules = 0;
    const auto load_began = Clock::now();
    if (!load_library(built.library, modules, quick ? 0 : 1)) {
        say("the library made just now does not load (%s)", ps2::os::utf8(built.library.filename()).c_str());
        return fail("the game stops");
    }
    g->failures = 0;
    if (g->verbose)
        say("the stop in parts: program analysis %.2f s, analysis of the window %.2f s, writing C++ %.2f s, compile %.2f s (processor time %.1f s, the longest compiler %.2f s, %zu files), link %.2f s, load %.2f s, the rest %.2f s",
            entries_s, built.generated.analyse_s, built.generated.codegen_s, built.timing.compile, built.timing.cpu, built.timing.longest, built.files, built.timing.link, lb::seconds_since(load_began),
            lb::seconds_since(began) - entries_s - built.timing.generate - built.timing.compile - built.timing.link - lb::seconds_since(load_began));
    say("switched in after %.1f s%s (translate %.1f, compile %.1f, link %.1f): %zu entry points%s%s, %zu pointers found in memory, %zu instructions, %zu files", lb::seconds_since(began), quick ? " (quick)" : "", built.timing.generate, built.timing.compile, built.timing.link,
        built.generated.accepted.size(), static_calls ? " (calls of the program included)" : "", known_entries ? " (entries.txt included)" : "", request.candidates.size(), built.generated.code, built.files);
    if (g->verbose) say("looking for pointers in memory took %.3f s", pointers_s);
    if (!region) translate_program_calls(image, base);
    if (flat) g->flat_started.insert(base);
    if (!request.focused) {
        g->whole.insert(name);
        if (quick) g->fast.insert(name);
        write_marker(name, quick);
    }
    if (!flat) remember(name, base, image, {pc}, region);   // a loaded program is not an overlay of the build: eerecomp checks those word for word
    if (quick) queue_upgrade(base, name, std::move(image), flat, std::move(request.candidates), std::move(request.entries));
    return true;
}

}  // namespace ps2::ee::live

#endif
