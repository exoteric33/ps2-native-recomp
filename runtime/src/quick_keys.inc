// The quick keys of a window run: F5 saves a state, F8 loads the newest one, F9 writes a problem report (docs/play-keys.md).
//
// This is not a header of its own. system.cpp includes it once, inside namespace ps2, after the machine state code it uses
// (machine_state, g_frames, g_options); it is kept in a file apart only because system.cpp is long. Everything here that is not
// in namespace quick is the small public interface declared at the end of ps2/system.hpp.
//
// Who does what. The window thread only sets a request (quick_request) and shows the answer (quick_notice). The emulation thread
// does the work at the first frame boundary after the request, the same safe point `--save-state` uses (a request makes vblank
// ask for it, run() serves it): it serializes the machine into memory, which is the only part that must stand still. The files
// are written by a worker thread, so a save costs the game the time of the serializing (about a tenth of a second for God of War
// II), not the time of the disk. The picture of a report is read back by the thread that draws, at its next present.

namespace quick {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

constexpr std::size_t kKeepQuick = 20;           // quick saves kept in the states folder
constexpr std::size_t kReportLines = 300;        // lines of the log in a report
constexpr unsigned kSave = 1, kLoad = 2, kReport = 4, kHang = 8;  // kHang: only for tests, see test_keys()
constexpr std::int64_t kGiveUpMs = 4000;         // a request the emulation has not answered in this time is dropped (the game hangs)

std::atomic<unsigned> g_requests{0};
std::atomic<std::int64_t> g_asked_at{0};         // when the oldest unanswered request was made
const Clock::time_point g_started = Clock::now();
std::size_t g_size_hint = 48u << 20;             // size of the last state, so the next one does not grow its buffer step by step

std::int64_t now_ms() { return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count(); }

// ---- messages for the player ----

std::mutex g_notice_lock;
QuickNotice g_notice;
bool g_notice_fresh = false;

void notice(int kind, const std::string &text) {
    {
        std::lock_guard<std::mutex> hold(g_notice_lock);
        g_notice = {text, kind};
        g_notice_fresh = true;
    }
    std::fprintf(stderr, "[keys] %s\n", text.c_str());
}

// ---- frame times, for the speed line of a report ----

constexpr std::size_t kRing = 1024;
std::atomic<std::int64_t> g_frame_ns[kRing];
std::atomic<std::uint64_t> g_frame_n{0};

void note_frame() {
    const std::uint64_t n = g_frame_n.load(std::memory_order_relaxed);
    g_frame_ns[n % kRing].store(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count(), std::memory_order_relaxed);
    g_frame_n.store(n + 1, std::memory_order_release);
}

// frames a second over the last `seconds` seconds up to now (less when the game has run for a shorter time); also how long ago the
// last frame was, which is long when the game hangs
double recent_fps(double seconds, double &last_frame_ago) {
    const std::int64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
    const std::uint64_t n = g_frame_n.load(std::memory_order_acquire);
    last_frame_ago = 0.0;
    if (!n) return 0.0;
    last_frame_ago = static_cast<double>(now - g_frame_ns[(n - 1) % kRing].load(std::memory_order_relaxed)) / 1e9;
    std::uint64_t count = 0;
    while (count < n && count < kRing && static_cast<double>(now - g_frame_ns[(n - 1 - count) % kRing].load(std::memory_order_relaxed)) < seconds * 1e9) ++count;
    const double span = std::min(seconds, std::chrono::duration<double>(Clock::now() - g_started).count());
    return span > 0.0 ? static_cast<double>(count) / span : 0.0;
}

// ---- where things go ----

// One folder holds the three kinds of files: states\, reports\ and logs\. Next to the memory card file, since every way of
// starting a game passes --memcard (the app's Play button, the desktop shortcut, play.py), so a card and its saves stay together
// and each game has its own. Without a card: next to the program. --quick-dir names it explicitly.
fs::path base_dir() {
    fs::path base;
    if (!g_options.quick_dir.empty()) base = os::path(g_options.quick_dir);
    else if (!g_options.memcard_path.empty()) base = os::path(g_options.memcard_path).parent_path();
    else if (!g_options.program_path.empty()) base = os::path(g_options.program_path).parent_path();
    if (base.empty()) base = ".";
    std::error_code error;
    const fs::path full = fs::absolute(base, error);
    return error ? base : full.lexically_normal();
}

fs::path states_dir() { return base_dir() / "states"; }
fs::path reports_dir() { return base_dir() / "reports"; }

std::string stamp() {
    const std::time_t t = std::time(nullptr);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &t);
#else
    localtime_r(&t, &local);
#endif
    char text[32];
    std::strftime(text, sizeof text, "%Y-%m-%d_%H-%M-%S", &local);
    return text;
}

std::string frame_text() {
    char text[32];
    std::snprintf(text, sizeof text, "f%06llu", static_cast<unsigned long long>(g_frames));
    return text;
}

fs::path with_suffix(fs::path p, const char *suffix) {
    p += suffix;
    return p;
}

// ---- files ----

// The finished file appears in one step (os::replace_file in ps2/os.hpp): the bytes go to "<file>.tmp", are forced to the disk, and
// the finished file then replaces the target, with a few retries when a virus scanner holds it for a moment. A reader (F8, another
// program) never sees half a file, and a crash in the middle leaves no broken state behind.
bool write_whole(const fs::path &target, const void *data, std::size_t size, std::string &why) {
    return os::replace_file(os::utf8(target), data, size, &why);
}

bool read_whole(const fs::path &file, std::vector<std::uint8_t> &out) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), {});
    return true;
}

std::string megabytes(std::size_t bytes) {
    char text[32];
    std::snprintf(text, sizeof text, "%.1f MB", static_cast<double>(bytes) / 1048576.0);
    return text;
}

// One thread that writes the files, in the order they were handed over.
class Worker {
public:
    ~Worker() { stop(); }
    void post(std::function<void()> job) {
        std::lock_guard<std::mutex> hold(m_);
        if (!thread_.joinable()) thread_ = std::thread([this] { loop(); });
        jobs_.push_back(std::move(job));
        wake_.notify_one();
    }
    // waits until everything handed over so far is written
    bool drain(int ms) {
        std::unique_lock<std::mutex> hold(m_);
        return idle_.wait_for(hold, std::chrono::milliseconds(ms), [this] { return jobs_.empty() && !busy_; });
    }
    void stop() {
        {
            std::lock_guard<std::mutex> hold(m_);
            stop_ = true;
            wake_.notify_all();
        }
        if (thread_.joinable()) thread_.join();
    }

private:
    void loop() {
        std::unique_lock<std::mutex> hold(m_);
        for (;;) {
            wake_.wait(hold, [this] { return stop_ || !jobs_.empty(); });
            if (jobs_.empty()) return;
            std::function<void()> job = std::move(jobs_.front());
            jobs_.pop_front();
            busy_ = true;
            hold.unlock();
            try {
                job();
            } catch (const std::exception &e) {
                std::fprintf(stderr, "[keys] a file could not be written: %s\n", e.what());
            }
            hold.lock();
            busy_ = false;
            idle_.notify_all();
        }
    }
    std::mutex m_;
    std::condition_variable wake_, idle_;
    std::deque<std::function<void()>> jobs_;
    std::thread thread_;
    bool busy_ = false, stop_ = false;
};

Worker &worker() {
    static Worker w;
    return w;
}

// The machine can only be written out between two interrupt handlers of the game (state.cpp in kernel.cpp says so). A frame boundary
// is that almost always; when it is not, the key is tried again at the next frame, for a second.
constexpr int kTries = 60;
int g_tries[3] = {};

enum class Serialized { done, busy, failed };

Serialized serialize(state::Writer &w, std::string &why) {
    w.out.clear();
    w.out.reserve(g_size_hint);
    try {
        machine_state(w);
    } catch (const std::exception &e) {
        why = e.what();
        return why.find("interrupt handler") != std::string::npos ? Serialized::busy : Serialized::failed;
    }
    g_size_hint = w.out.size() + (1u << 20);
    return Serialized::done;
}

// true when the key has to be tried again at the next frame
bool retry_later(unsigned bit, int slot) {
    if (++g_tries[slot] >= kTries) return false;
    g_requests.fetch_or(bit);
    return true;
}

// ---- recording and replaying the controller ports (docs/replay.md) ----
//
// A window run writes down, for every frame in which the pad state the game gets changes, the state of the port (buttons, the four
// stick values, the pressure of L2 and R2, plugged in or not), and every kCheckEvery frames a checkpoint: a hash of what the game has
// done so far. The file is text, one line each, next to the quick save it began from (<state>.inputs). A replay (--replay) gives the
// game the same pad states at the same frames, and compares each checkpoint; the first one that differs says where the game went
// another way. Only the emulation thread touches this; the host's keys reach it through latch_pads in system.cpp, which is what makes
// a window run the same as a headless one (the game gets the host's state at the frame boundary, not at some moment inside a frame).
//
// What a checkpoint covers: kSlices slices of the EE memory, one slice per frame in turn (the whole memory within kSlices frames, at
// about 0.1 ms a frame: measured 90 us for a slice of 512 KB), the scratchpad, the main registers of the CPU, the frame number and the emulated time. Everything chains: a
// checkpoint also holds all the ones before it.

constexpr unsigned kCheckEvery = 15;
constexpr unsigned kSlices = 64;
unsigned g_slices = kSlices;  // a replay uses the recording's own number
constexpr int kFlushEvery = 60;        // frames; the file is written this often, so a crash loses at most a second
constexpr std::size_t kKeepRuns = 10;  // run_*.inputs files kept

constexpr std::uint64_t kMul = 0x9E3779B97F4A7C15ull;

std::uint64_t mix(std::uint64_t h, std::uint64_t v) {
    h = (h ^ v) * kMul;
    return h ^ (h >> 29);
}

// four independent lanes (a multiply each) so that a slice of memory costs a fraction of a millisecond
std::uint64_t hash_bytes(const void *data, std::size_t bytes) {
    const std::uint8_t *p = static_cast<const std::uint8_t *>(data);
    std::uint64_t h[4] = {1, 2, 3, 4};
    std::size_t i = 0;
    for (; i + 32 <= bytes; i += 32) {
        std::uint64_t w[4];
        std::memcpy(w, p + i, 32);
        for (int k = 0; k < 4; ++k) {
            h[k] = (h[k] ^ w[k]) * kMul;
            h[k] ^= h[k] >> 31;
        }
    }
    std::uint64_t r = bytes;
    for (; i < bytes; ++i) r = mix(r, p[i]);
    return mix(mix(mix(mix(r, h[0]), h[1]), h[2]), h[3]);
}

std::uint64_t frame_digest() {
    const std::size_t slice = kRamSize / g_slices;
    std::uint64_t d = hash_bytes(g_ram + (g_frames % g_slices) * slice, slice);
    d = mix(d, hash_bytes(g_spr, kScratchpadSize));
    d = mix(d, hash_bytes(g_cpu.r, sizeof g_cpu.r));
    d = mix(d, hash_bytes(&g_cpu.hi, sizeof g_cpu.hi));
    d = mix(d, hash_bytes(&g_cpu.lo, sizeof g_cpu.lo));
    d = mix(d, hash_bytes(g_cpu.f, sizeof g_cpu.f));
    d = mix(d, g_cpu.sa);
    d = mix(d, g_cpu.pc & ~kUnwind);
    d = mix(d, now());
    return mix(d, g_frames);
}

struct Recorder {
    std::FILE *file = nullptr;
    std::string buffer;
    fs::path path;
    fs::path state;  // the quick save it began from, empty for the power-on
    PadSample last[2];
    bool first = true;
    std::uint64_t chain = 0;
    std::uint64_t began = 0;
    int unflushed = 0;
    std::uint64_t last_frame = 0;  // the newest frame in the file (a quick load changes g_frames before the recording ends)
};
Recorder g_rec;

void rec_flush() {
    if (!g_rec.file) return;
    if (!g_rec.buffer.empty()) std::fwrite(g_rec.buffer.data(), 1, g_rec.buffer.size(), g_rec.file);
    g_rec.buffer.clear();
    std::fflush(g_rec.file);
    g_rec.unflushed = 0;
}

void record_stop() {
    if (!g_rec.file) return;
    g_rec.buffer += "e " + std::to_string(g_rec.last_frame) + "\n";
    rec_flush();
    std::fclose(g_rec.file);
    g_rec.file = nullptr;
}

// Where the memory card of a recording is. The card is part of what the game sees (it reads it at the start and at every save), so a
// replay needs the card as it was when the recording began: `state` is the copy beside the quick save that the recording began with
// (F5 writes it, <state>.ps2); `own` is a copy of the card at this moment, written beside the recording (<recording>.ps2) - the start of a
// run, and a quick load, which leaves the card as it is and so does not go back to the copy beside the state.
enum class CardAt { own, state };

void record_start(const fs::path &file, const fs::path &state, CardAt card_at) {
    record_stop();
    std::error_code error;
    fs::create_directories(file.parent_path(), error);
    g_rec.file = os::open(os::utf8(file), "wb");
    if (!g_rec.file) {
        std::fprintf(stderr, "[replay] cannot write the recording %s\n", os::utf8(file).c_str());
        return;
    }
    g_rec.path = file;
    g_rec.state = state;
    g_rec.first = true;
    g_rec.chain = 0;
    g_rec.began = g_frames;
    g_rec.last_frame = g_frames;
    g_rec.buffer = "ps2-inputs 1\ncheck-every " + std::to_string(kCheckEvery) + " slices " + std::to_string(kSlices) + "\n";
    g_rec.buffer += std::string("pal ") + (g_options.pal ? "1" : "0") + "\n";
    if (!g_options.program_path.empty()) g_rec.buffer += "program " + g_options.program_path + "\n";
    if (!g_options.disc_path.empty()) g_rec.buffer += "disc " + g_options.disc_path + "\n";
    if (state.empty()) g_rec.buffer += "start power-on frame 0\n";
    else g_rec.buffer += "start state " + os::utf8(state.filename()) + " frame " + std::to_string(g_frames) + "\n";
    g_rec.buffer += "recorded " + stamp() + "\n";
    if (g_options.rtc_epoch) g_rec.buffer += "rtc " + std::to_string(g_options.rtc_epoch) + "\n";
    const char *card_line = "card none\n";
    if (!g_options.memcard_path.empty()) {
        if (card_at == CardAt::state) {
            card_line = "card state\n";
        } else {
            std::vector<std::uint8_t> card;
            if (memory_card_image(card, true)) {
                card_line = "card file\n";
                worker().post([copy = with_suffix(file, ".ps2"), card = std::move(card)]() {
                    std::string why;
                    if (!write_whole(copy, card.data(), card.size(), why)) std::fprintf(stderr, "[replay] the card copy of the recording was not written: %s\n", why.c_str());
                });
            }
        }
    }
    g_rec.buffer += card_line;
    rec_flush();
    std::fprintf(stderr, "[replay] recording the pad from frame %llu into %s\n", static_cast<unsigned long long>(g_frames), os::utf8(file).c_str());
}

// ---- the replay ----

struct Replay {
    bool active = false;
    bool first = true;
    bool finished = false;
    int status = 4;  // see replay_status()
    std::uint64_t began = 0;
    std::uint64_t end = 0;
    std::uint64_t checked = 0;
    std::uint64_t last_good = 0;
    std::uint64_t first_bad = 0;  // --replay-continue: the checkpoint where the game first went another way, 0 until then
    std::uint64_t chain = 0;
    std::map<std::uint64_t, std::vector<std::pair<int, PadSample>>> pads;
    std::map<std::uint64_t, std::uint32_t> checks;
};
Replay g_rep;

void replay_end(int status, const std::string &why) {
    g_rep.status = status;
    g_rep.finished = true;
    halt(why);
}

// the pad states of this frame (system.cpp, latch_pads)
void pads_hook(PadSample (&pads)[2], unsigned &apply) {
    if (g_rep.active) {
        if (g_rep.finished) return;
        if (g_rep.first) {
            g_rep.first = false;
            if (g_rep.began + 1 != g_frames) {
                std::fprintf(stderr, "[replay] the recording began at frame %llu, but this run is at frame %llu: wrong state for this recording?\n",
                             static_cast<unsigned long long>(g_rep.began), static_cast<unsigned long long>(g_frames - 1));
                replay_end(5, "wrong start for the recording");
                return;
            }
        }
        apply = 0;
        const auto it = g_rep.pads.find(g_frames);
        if (it != g_rep.pads.end())
            for (const auto &entry : it->second) {
                pads[entry.first] = entry.second;
                apply |= 1u << entry.first;
            }
        return;
    }
    if (!g_rec.file) return;
    if (g_rec.first) apply = 3;  // the first frame gives the game the whole state, whatever the state it was loaded from held
    char line[96];
    for (int p = 0; p < 2; ++p) {
        if (!g_rec.first && pads[p] == g_rec.last[p]) continue;
        g_rec.last[p] = pads[p];
        const PadSample &s = pads[p];
        std::snprintf(line, sizeof line, "p%d %llu %04X %u %u %u %u %u %u %d\n", p + 1, static_cast<unsigned long long>(g_frames), s.buttons, s.axes[0], s.axes[1],
                      s.axes[2], s.axes[3], s.l2, s.r2, s.connected ? 1 : 0);
        g_rec.buffer += line;
    }
    g_rec.first = false;
}

void frame_check() {
    if (g_rep.active) {
        if (g_rep.finished) return;
        g_rep.chain = mix(g_rep.chain, frame_digest());
        if (g_frames % kCheckEvery == 0 && !g_rep.first_bad) {
            const auto it = g_rep.checks.find(g_frames);
            if (it != g_rep.checks.end()) {
                ++g_rep.checked;
                if (static_cast<std::uint32_t>(g_rep.chain ^ (g_rep.chain >> 32)) != it->second) {
                    std::fprintf(stderr, "[replay] DIFFERENT: the game went another way between frame %llu and frame %llu (the checkpoint at %llu differs)\n",
                                 static_cast<unsigned long long>(g_rep.last_good), static_cast<unsigned long long>(g_frames), static_cast<unsigned long long>(g_frames));
                    if (!g_options.replay_continue) {
                        replay_end(3, "the replay went another way");
                        return;
                    }
                    g_rep.first_bad = g_frames;  // every later checkpoint differs too (they are chained): play on without comparing
                }
                g_rep.last_good = g_frames;
            }
        }
        if (g_frames >= g_rep.end && g_rep.first_bad) {
            std::fprintf(stderr, "[replay] played on to frame %llu as asked (--replay-continue); the game went another way at the checkpoint of frame %llu\n",
                         static_cast<unsigned long long>(g_frames), static_cast<unsigned long long>(g_rep.first_bad));
            replay_end(3, "the replay is over, another way");
        } else if (g_frames >= g_rep.end) {
            std::fprintf(stderr, "[replay] same to the end: frames %llu to %llu, %llu checkpoints all equal\n", static_cast<unsigned long long>(g_rep.began),
                         static_cast<unsigned long long>(g_frames), static_cast<unsigned long long>(g_rep.checked));
            replay_end(0, "the replay is over");
        }
        return;
    }
    if (!g_rec.file) return;
    g_rec.last_frame = g_frames;
    g_rec.chain = mix(g_rec.chain, frame_digest());
    if (g_frames % kCheckEvery == 0) {
        char line[48];
        std::snprintf(line, sizeof line, "h %llu %08x\n", static_cast<unsigned long long>(g_frames), static_cast<unsigned>(g_rec.chain ^ (g_rec.chain >> 32)));
        g_rec.buffer += line;
    }
    if (++g_rec.unflushed >= kFlushEvery) rec_flush();
}

// run_*.inputs: only the newest few are kept
void prune_runs() {
    std::vector<std::pair<fs::file_time_type, fs::path>> found;
    std::error_code error;
    for (fs::directory_iterator it(states_dir(), error), end; !error && it != end; it.increment(error)) {
        const std::string name = os::utf8(it->path().filename());
        if (name.size() > 14 && name.compare(0, 4, "run_") == 0 && name.compare(name.size() - 7, 7, ".inputs") == 0) {
            std::error_code time_error;
            found.emplace_back(fs::last_write_time(it->path(), time_error), it->path());
        }
    }
    std::sort(found.begin(), found.end());
    for (std::size_t i = 0; i + kKeepRuns < found.size(); ++i) {
        fs::remove(found[i].second, error);
        fs::remove(with_suffix(found[i].second, ".ps2"), error);
    }
}

// The recording of a run begins when the run begins (power-on, or the state --load-state gave); with every quick save and quick load a
// new one begins (record_for_state). --record names one file and keeps to it.
// The game reads the console's clock (sceCdReadClock) and keeps what it reads in memory (Shadow of the Colossus does it in every frame
// of the shrine), so two runs that read the host's clock go another way within 15 frames, and a recording would never replay. A run
// that records therefore lets the clock run with the emulated time from here on, and the recording says where it started (`rtc`).
void pin_rtc() {
    if (g_options.rtc_epoch) return;
    const std::uint64_t wall = static_cast<std::uint64_t>(std::time(nullptr)), played = now() / kEeClock;
    g_options.rtc_epoch = wall > played ? wall - played : wall;
}

void record_run_start() {
    if (!g_options.record_path.empty() || g_options.auto_record) pin_rtc();
    if (!g_options.record_path.empty()) {
        record_start(os::path(g_options.record_path), os::path(g_options.load_state_path), CardAt::own);
        return;
    }
    if (!g_options.auto_record) return;
    prune_runs();
    record_start(states_dir() / ("run_" + stamp() + "_" + frame_text() + ".inputs"), os::path(g_options.load_state_path), CardAt::own);
}

// A recording belongs to a state: <state>.inputs. The one before it (the attempt that ended with a quick load) is kept as <state>.prev.inputs.
void record_for_state(const fs::path &state_file, CardAt card_at) {
    if (!g_options.auto_record || !g_options.record_path.empty()) return;
    record_stop();
    std::error_code error;
    const fs::path file = with_suffix(state_file, ".inputs");
    if (fs::exists(file, error)) {
        fs::rename(file, with_suffix(state_file, ".prev.inputs"), error);
        worker().drain(10000);  // the card copy of that recording may still be on its way
        fs::rename(with_suffix(file, ".ps2"), with_suffix(state_file, ".prev.inputs.ps2"), error);
    }
    record_start(file, state_file, card_at);
}

// ---- the quick saves ----

// quick_<date>_<time>_f<frame>.state, oldest first. By the time the file was written, not by its name: the clock of the PC goes back
// an hour once a year, and the names of that hour would sort before the ones of the hour before.
std::vector<fs::path> quick_states() {
    std::vector<std::pair<fs::file_time_type, fs::path>> found;
    std::error_code error;
    for (fs::directory_iterator it(states_dir(), error), end; !error && it != end; it.increment(error)) {
        const std::string name = os::utf8(it->path().filename());
        if (name.size() <= 12 || name.compare(0, 6, "quick_") != 0 || name.compare(name.size() - 6, 6, ".state") != 0) continue;
        std::error_code time_error;
        found.emplace_back(fs::last_write_time(it->path(), time_error), it->path());
    }
    std::sort(found.begin(), found.end());
    std::vector<fs::path> paths;
    for (auto &entry : found) paths.push_back(std::move(entry.second));
    return paths;
}

// keeps the newest kKeepQuick quick saves; each one's card copy goes with it. Also clears the half-written files that a run which
// was ended in the middle of a write left behind (this runs on the writing thread, so none of them is being written now).
void prune() {
    std::vector<fs::path> all = quick_states();
    std::error_code error;
    for (std::size_t i = 0; i + kKeepQuick < all.size(); ++i) {
        fs::remove(all[i], error);
        fs::remove(with_suffix(all[i], ".ps2"), error);
        fs::remove(with_suffix(all[i], ".inputs"), error);
        fs::remove(with_suffix(all[i], ".prev.inputs"), error);
        fs::remove(with_suffix(all[i], ".inputs.ps2"), error);
        fs::remove(with_suffix(all[i], ".prev.inputs.ps2"), error);
    }
    for (fs::directory_iterator it(states_dir(), error), end; !error && it != end; it.increment(error)) {
        const std::string name = os::utf8(it->path().filename());
        std::error_code remove_error;
        if (name.size() > 4 && name.compare(0, 6, "quick_") == 0 && name.compare(name.size() - 4, 4, ".tmp") == 0) fs::remove(it->path(), remove_error);
    }
}

std::string unique_name(const fs::path &dir, const std::string &name) {
    std::error_code error;
    std::string candidate = name;
    for (int n = 2; fs::exists(dir / (candidate + ".state"), error) && n < 100; ++n) candidate = name + "_" + std::to_string(n);
    return candidate;
}

// A state file that machine_state can read: right magic and version at the start, the "end" tag at the end. Checked before a load
// that replaces a running machine, since a file that fails half way would leave nothing to go on with.
bool looks_like_state(const std::vector<std::uint8_t> &data) {
    if (data.size() < 12 + 8 + kRamSize || std::memcmp(data.data(), kStateMagic, sizeof kStateMagic) != 0) return false;
    std::uint32_t version = 0;
    std::memcpy(&version, data.data() + sizeof kStateMagic, sizeof version);
    static const char kEnd[8] = {'e', 'n', 'd', 0, 0, 0, 0, 0};
    // 4: before the HLE replies; every version machine_state() can load is accepted, so quick saves made before an update still load
    return version >= 4 && version <= kStateVersion && std::memcmp(data.data() + data.size() - 8, kEnd, 8) == 0;
}

// A state does not hold the memory card, so a quick save has a copy of the card beside it (<state>.ps2). Loading a state never
// changes the card: a player who goes back to an older state must keep the saves made since. This says so in the log when the
// card in use differs from the copy, and returns a short text for the window (empty when they are the same or there is no copy).
std::string card_note(const fs::path &state_file) {
    if (g_options.memcard_path.empty()) return {};
    const fs::path copy = with_suffix(state_file, ".ps2");
    std::vector<std::uint8_t> saved, current;
    if (!read_whole(copy, saved) || !memory_card_image(current, true)) return {};
    if (saved == current) return {};
    std::fprintf(stderr, "[state] note: the memory card is not the one that was in use when %s was saved.\n", os::utf8(state_file.filename()).c_str());
    std::fprintf(stderr, "[state]       The card was not changed, so saves made since then stay on it. To go back to the card of that\n");
    std::fprintf(stderr, "[state]       moment, close the game and copy %s over %s.\n", os::utf8(copy).c_str(), g_options.memcard_path.c_str());
    return "memory card left as it is";
}

void save() {
    const fs::path dir = states_dir();
    std::error_code error;
    fs::create_directories(dir, error);
    if (error) {
        notice(4, "Could not save: no folder " + os::utf8(dir));
        return;
    }
    state::Writer w;
    std::string why;
    switch (serialize(w, why)) {
    case Serialized::busy:
        if (retry_later(kSave, 0)) return;
        [[fallthrough]];
    case Serialized::failed:
        g_tries[0] = 0;
        notice(4, "Could not save (F5): " + why);
        return;
    case Serialized::done: break;
    }
    g_tries[0] = 0;
    const std::string name = unique_name(dir, "quick_" + stamp() + "_" + frame_text());
    std::vector<std::uint8_t> card;
    const bool has_card = memory_card_image(card, true);
    const fs::path file = dir / (name + ".state");
    record_for_state(file, CardAt::state);  // the pad recording of the time after this save; its card is the copy beside the save
    worker().post([file, bytes = std::move(w.out), card = std::move(card), has_card]() {
        std::string why;
        if (!write_whole(file, bytes.data(), bytes.size(), why)) {
            notice(4, "Could not save (F5): " + why);
            return;
        }
        if (has_card && !write_whole(with_suffix(file, ".ps2"), card.data(), card.size(), why)) std::fprintf(stderr, "[keys] the card copy was not written: %s\n", why.c_str());
        prune();
        std::fprintf(stderr, "[state] quick save %s (%s)\n", os::utf8(file).c_str(), megabytes(bytes.size()).c_str());
        notice(1, "Saved (F5)");
    });
}

// The window thread plays the audio queue, which still holds the sound from before the load; it clears it when it sees the notice.
void load() {
    worker().drain(10000);  // a save made a moment ago must be in the folder
    const std::vector<fs::path> all = quick_states();
    if (all.empty()) {
        notice(4, "No quick save yet. Press F5 to make one.");
        return;
    }
    const fs::path file = all.back();
    std::vector<std::uint8_t> data;
    if (!read_whole(file, data) || !looks_like_state(data)) {
        notice(4, "The newest quick save cannot be used: " + os::utf8(file.filename()));
        return;
    }
    // What the machine is now, so a load that fails half way (a bug, not a damaged file: that was checked above) can be undone.
    state::Writer before;
    std::string why;
    if (serialize(before, why) != Serialized::done) before.out.clear();
    try {
        state::Reader r(data.data(), data.size());
        machine_state(r);
    } catch (const std::exception &e) {
        std::fprintf(stderr, "[state] quick load failed: %s\n", e.what());
        try {
            if (before.out.empty()) throw std::runtime_error("nothing to go back to");
            state::Reader back(before.out.data(), before.out.size());
            machine_state(back);
            notice(4, "Could not load the quick save; the game goes on where it was");
        } catch (const std::exception &) {
            halt("a quick load failed and could not be undone");
            notice(4, "The quick load failed and the game cannot go on");
        }
        return;
    }
    g_resumed = true;
    record_for_state(file, CardAt::own);  // a new recording from this state; the card is the one in use now, not the copy beside the state
    std::fprintf(stderr, "[state] quick load %s (frame %llu)\n", os::utf8(file).c_str(), static_cast<unsigned long long>(g_frames));
    const std::string note = card_note(file);
    notice(2, note.empty() ? "Loaded (F8)" : "Loaded (F8) - " + note);
}

// ---- the picture of a report ----

std::atomic<bool> g_shot_wanted{false};
std::mutex g_shot_lock;
std::condition_variable g_shot_cv;
std::vector<std::uint32_t> g_shot;
std::uint32_t g_shot_w = 0, g_shot_h = 0;
bool g_shot_done = false, g_shot_ok = false;
gs::DisplayRegs g_last_regs{};  // the last picture the drawing thread presented
bool g_have_regs = false;

// Called by the thread that draws, for every picture it presents. It is the one thread that may read the GPU back.
void on_present(const gs::DisplayRegs &regs) {
    g_last_regs = regs;
    g_have_regs = true;
    if (!g_shot_wanted.load(std::memory_order_relaxed) || !g_shot_wanted.exchange(false)) return;
    std::vector<std::uint32_t> pixels;
    std::uint32_t w = 0, h = 0;
    const bool ok = gs::display(regs, pixels, w, h);
    {
        std::lock_guard<std::mutex> hold(g_shot_lock);
        g_shot.swap(pixels);
        g_shot_w = w;
        g_shot_h = h;
        g_shot_ok = ok;
        g_shot_done = true;
    }
    g_shot_cv.notify_all();
}

// The picture on the screen now. With a drawing thread that thread takes it at its next present (a frame away); without one the
// calling thread draws, so it takes the picture itself. `drawing_thread` is the emergency path: the window thread is the drawing
// thread, and the emulation does not answer.
bool take_picture(std::vector<std::uint32_t> &pixels, std::uint32_t &w, std::uint32_t &h, bool drawing_thread) {
    if (drawing_thread) return g_have_regs && gs::display(g_last_regs, pixels, w, h);
    if (!gs::queue::active()) {
        g_shot_wanted = false;
        return gs::display(pixels, w, h);
    }
    std::unique_lock<std::mutex> hold(g_shot_lock);
    if (!g_shot_cv.wait_for(hold, std::chrono::milliseconds(500), [] { return g_shot_done; }) || !g_shot_ok) return false;
    pixels = g_shot;
    w = g_shot_w;
    h = g_shot_h;
    return true;
}

// A PNG with the pixels as they are: the data is stored in the zlib stream without compression, which needs no library. About
// 0.9 MB for a normal picture.
std::vector<std::uint8_t> make_png(const std::uint32_t *rgba, std::uint32_t w, std::uint32_t h) {
    static std::uint32_t crc_table[256];
    static bool have_table = false;
    if (!have_table) {
        for (std::uint32_t n = 0; n < 256; ++n) {
            std::uint32_t c = n;
            for (int k = 0; k < 8; ++k) c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            crc_table[n] = c;
        }
        have_table = true;
    }
    std::vector<std::uint8_t> raw;
    raw.reserve(static_cast<std::size_t>(h) * (1 + 3u * w));
    for (std::uint32_t y = 0; y < h; ++y) {
        raw.push_back(0);  // filter: none
        for (std::uint32_t x = 0; x < w; ++x) {
            const std::uint32_t p = rgba[static_cast<std::size_t>(y) * w + x];
            raw.push_back(static_cast<std::uint8_t>(p));
            raw.push_back(static_cast<std::uint8_t>(p >> 8));
            raw.push_back(static_cast<std::uint8_t>(p >> 16));
        }
    }
    std::vector<std::uint8_t> z{0x78, 0x01};
    std::uint32_t a = 1, b = 0;
    for (std::uint8_t byte : raw) {
        a = (a + byte) % 65521u;
        b = (b + a) % 65521u;
    }
    for (std::size_t at = 0; at < raw.size() || at == 0;) {
        const std::size_t n = std::min<std::size_t>(65535, raw.size() - at);
        const bool last = at + n >= raw.size();
        z.push_back(last ? 1 : 0);
        z.push_back(static_cast<std::uint8_t>(n));
        z.push_back(static_cast<std::uint8_t>(n >> 8));
        z.push_back(static_cast<std::uint8_t>(~n));
        z.push_back(static_cast<std::uint8_t>(~n >> 8));
        z.insert(z.end(), raw.begin() + static_cast<std::ptrdiff_t>(at), raw.begin() + static_cast<std::ptrdiff_t>(at + n));
        at += n;
        if (last) break;
    }
    const std::uint32_t adler = (b << 16) | a;
    for (int shift = 24; shift >= 0; shift -= 8) z.push_back(static_cast<std::uint8_t>(adler >> shift));
    std::vector<std::uint8_t> png{0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    const auto chunk = [&](const char *type, const std::vector<std::uint8_t> &body) {
        const auto be32 = [&](std::uint32_t v) {
            for (int shift = 24; shift >= 0; shift -= 8) png.push_back(static_cast<std::uint8_t>(v >> shift));
        };
        be32(static_cast<std::uint32_t>(body.size()));
        const std::size_t from = png.size();
        png.insert(png.end(), type, type + 4);
        png.insert(png.end(), body.begin(), body.end());
        std::uint32_t c = 0xFFFFFFFFu;
        for (std::size_t i = from; i < png.size(); ++i) c = crc_table[(c ^ png[i]) & 0xFFu] ^ (c >> 8);
        be32(c ^ 0xFFFFFFFFu);
    };
    std::vector<std::uint8_t> header;
    for (std::uint32_t v : {w, h})
        for (int shift = 24; shift >= 0; shift -= 8) header.push_back(static_cast<std::uint8_t>(v >> shift));
    header.insert(header.end(), {8, 2, 0, 0, 0});  // 8 bits, RGB, the usual compression, filter and no interlace
    chunk("IHDR", header);
    chunk("IDAT", z);
    chunk("IEND", {});
    return png;
}

// ---- the problem report (F9) ----

std::string minutes_text(double seconds) {
    char text[48];
    const long total = static_cast<long>(seconds);
    if (total >= 3600) std::snprintf(text, sizeof text, "%ld h %ld min %ld s", total / 3600, (total / 60) % 60, total % 60);
    else std::snprintf(text, sizeof text, "%ld min %ld s", total / 60, total % 60);
    return text;
}

// lines of `text` that start with one of the prefixes, one per line
std::string pick_lines(const std::string &text, std::initializer_list<const char *> prefixes) {
    std::string out;
    std::size_t at = 0;
    while (at < text.size()) {
        const std::size_t end = std::min(text.find('\n', at), text.size());
        const std::string line = text.substr(at, end - at);
        for (const char *p : prefixes)
            if (line.rfind(p, 0) == 0) {
                out += "    " + line + "\n";
                break;
            }
        at = end + 1;
    }
    return out;
}

std::string report_info(bool emergency, const fs::path &dir, bool has_state, bool has_card, bool has_picture, std::size_t state_bytes) {
    std::string t;
    const auto add = [&](const std::string &label, const std::string &value) {
        t += label;
        t.append(label.size() < 18 ? 18 - label.size() : 1, ' ');
        t += value + "\n";
    };
    t += "Problem report of ps2run (pressed F9 in the game window)\n\n";
    add("written:", stamp() + " (local time)");
    const double fps_target = g_options.pal ? 50.0 : 60.0;
    add("frame:", std::to_string(g_frames) + " (about " + minutes_text(static_cast<double>(g_frames) / fps_target) + " of game time)");
    add("time since start:", minutes_text(std::chrono::duration<double>(Clock::now() - g_started).count()));
    char speed[200];
    double ago = 0.0;
    const double fps = recent_fps(5.0, ago);
    std::snprintf(speed, sizeof speed, "%.1f frames a second over the last 5 seconds (the game runs at %.0f); the last frame was %.1f s ago", fps, fps_target, ago);
    add("speed:", speed);
    add("runner built:", std::string(__DATE__) + " " + __TIME__);
    add("program:", g_options.program_path);
    add("command line:", g_options.command_line);
    add("memory card:", g_options.memcard_path.empty() ? "none" : g_options.memcard_path);
    add("report folder:", os::utf8(dir));
    t += "\nfiles:\n";
    t += has_state ? "    report.state       the machine at that moment (ps2run <program> --load-state report.state)\n" : "    (no save state)\n";
    t += has_card ? "    report.state.ps2   the memory card at that moment\n" : "    (no memory card copy)\n";
    t += has_picture ? "    picture.png        the picture on the screen\n" : "    (no picture)\n";
    t += "    log.txt            the last lines of the runner's log\n";
    if (has_state) t += "\nsave state size: " + megabytes(state_bytes) + "\n";
    if (emergency) t += "\nThe emulation did not answer F9 within " + std::to_string(kGiveUpMs / 1000) + " seconds, so this report was written by the window.\nThe game is probably hanging inside the runner.\n";
    const std::string first = g_options.first_log ? g_options.first_log(80) : std::string();
    const std::string startup = pick_lines(first, {"[gpu]", "[window]", "[audio] sound device", "[pad]"});
    t += "\nfrom the start of the log:\n" + (startup.empty() ? std::string("    (not captured)\n") : startup);
    return t;
}

void report(bool emergency) {
    std::vector<std::uint8_t> state;
    bool has_state = false;
    if (!emergency) {
        state::Writer w;
        std::string why;
        const Serialized result = serialize(w, why);
        if (result == Serialized::busy && retry_later(kReport, 2)) return;
        g_tries[2] = 0;
        if (result == Serialized::done) {
            state = std::move(w.out);
            has_state = true;
        } else {
            std::fprintf(stderr, "[keys] the report has no save state: %s\n", why.c_str());
        }
    }
    const fs::path dir = reports_dir() / (stamp() + "_" + frame_text() + (emergency ? "_noanswer" : ""));
    std::error_code error;
    fs::create_directories(dir, error);
    if (error) {
        notice(4, "Could not write the report: no folder " + os::utf8(dir));
        return;
    }
    std::vector<std::uint8_t> card;
    const bool has_card = memory_card_image(card, !emergency);
    std::vector<std::uint32_t> pixels;
    std::uint32_t w = 0, h = 0;
    const bool has_picture = take_picture(pixels, w, h, emergency) && w && h;
    // the pad recording since the last quick save or load (or the start of the run), and the state it began from: with those the
    // report can be replayed (docs/replay.md)
    std::string inputs;
    fs::path inputs_state, inputs_card;
    if (!emergency && g_rec.file) {
        rec_flush();
        std::vector<std::uint8_t> bytes;
        if (read_whole(g_rec.path, bytes)) {
            inputs.assign(bytes.begin(), bytes.end());
            inputs += "e " + std::to_string(g_rec.last_frame) + "\n";
            inputs_state = g_rec.state;
            inputs_card = with_suffix(g_rec.path, ".ps2");  // the recording's own card copy, if it has one (header line "card file")
        }
    }
    const std::string log = g_options.recent_log ? g_options.recent_log(kReportLines) : std::string("(the log was not captured in this run)\n");
    const std::string info = report_info(emergency, dir, has_state, has_card, has_picture, state.size());
    worker().post([dir, state = std::move(state), card = std::move(card), pixels = std::move(pixels), w, h, log, info, has_state, has_card, has_picture, inputs = std::move(inputs), inputs_state, inputs_card]() {
        std::string why;
        bool ok = write_whole(dir / "info.txt", info.data(), info.size(), why);
        ok = write_whole(dir / "log.txt", log.data(), log.size(), why) && ok;
        if (has_picture) {
            const std::vector<std::uint8_t> png = make_png(pixels.data(), w, h);
            ok = write_whole(dir / "picture.png", png.data(), png.size(), why) && ok;
        }
        if (has_card) ok = write_whole(dir / "report.state.ps2", card.data(), card.size(), why) && ok;
        if (has_state) ok = write_whole(dir / "report.state", state.data(), state.size(), why) && ok;
        if (!inputs.empty()) {
            // recording.state.inputs goes with recording.state (the quick save it began from); a recording from the power-on has no state
            std::error_code copy_error;
            const bool with_state = !inputs_state.empty() && fs::exists(inputs_state, copy_error);
            if (with_state) {
                fs::copy_file(inputs_state, dir / "recording.state", fs::copy_options::overwrite_existing, copy_error);
                if (copy_error) std::fprintf(stderr, "[keys] the state of the recording was not copied: %s\n", copy_error.message().c_str());
                fs::copy_file(with_suffix(inputs_state, ".ps2"), dir / "recording.state.ps2", fs::copy_options::overwrite_existing, copy_error);
            }
            const std::string recording_name = with_state ? "recording.state.inputs" : "recording.inputs";
            ok = write_whole(dir / recording_name, inputs.data(), inputs.size(), why) && ok;
            if (!inputs_card.empty() && fs::exists(inputs_card, copy_error)) fs::copy_file(inputs_card, dir / (recording_name + ".ps2"), fs::copy_options::overwrite_existing, copy_error);
        }
        std::fprintf(stderr, "[keys] report written: %s%s\n", os::utf8(dir).c_str(), ok ? "" : (" (a file failed: " + why + ")").c_str());
        notice(ok ? 3 : 4, ok ? "Problem noted (F9), thank you" : "The report could not be written completely: " + why);
    });
}

// ---- called by the rest of system.cpp ----

struct TestKey {
    std::uint64_t frame;
    unsigned bits;
    bool done = false;  // pressed once only: a load goes back in time and would press it again
};

// PS2_TEST_KEYS=<frame>:<save|load|report|hang>[,<frame>:<key>...] presses the keys at those frames, for tests without a window. "hang"
// makes the emulation thread stand still for 12 seconds, to see what the window does when the game hangs. Not for players.
std::vector<TestKey> &test_keys() {
    static std::vector<TestKey> keys = [] {
        std::vector<TestKey> list;
        const std::string spec = os::env("PS2_TEST_KEYS");
        std::size_t at = 0;
        while (at < spec.size()) {
            std::size_t end = spec.find(',', at);
            if (end == std::string::npos) end = spec.size();
            const std::string item = spec.substr(at, end - at);
            const std::size_t colon = item.find(':');
            if (colon != std::string::npos) {
                const std::string what = item.substr(colon + 1);
                const unsigned bits = what == "save" ? kSave : what == "load" ? kLoad : what == "report" ? kReport : what == "hang" ? kHang : 0u;
                if (bits) list.push_back({std::strtoull(item.c_str(), nullptr, 10), bits});
            }
            at = end + 1;
        }
        return list;
    }();
    return keys;
}

// every frame, on the emulation thread: true when a key is waiting to be served
bool vblank_tick() {
    note_frame();
    for (TestKey &k : test_keys())
        if (!k.done && k.frame == g_frames) {
            k.done = true;
            if (k.bits == kHang) {
                std::fprintf(stderr, "[keys] test: the emulation stands still for 12 seconds\n");
                std::this_thread::sleep_for(std::chrono::seconds(12));
                continue;
            }
            if (k.bits == kReport) {
                {
                    std::lock_guard<std::mutex> hold(g_shot_lock);
                    g_shot_done = false;
                }
                g_shot_wanted = true;
            }
            if (g_requests.fetch_or(k.bits) == 0) g_asked_at.store(now_ms());
        }
    return g_requests.load(std::memory_order_relaxed) != 0;
}

// on the emulation thread, at the safe point
void service() {
    const unsigned want = g_requests.exchange(0);
    if (!want) return;
    const auto start = Clock::now();
    if (want & kReport) report(false);
    if (want & kSave) save();
    if (want & kLoad) load();
    std::fprintf(stderr, "[keys] the game stood still for %.0f ms\n", std::chrono::duration<double, std::milli>(Clock::now() - start).count());
}

// the emulation is over: the files that are still being written are finished
void finish() {
    record_stop();
    worker().drain(20000);
    worker().stop();
}

}  // namespace quick

bool replay_active() { return quick::g_rep.active; }
int replay_status() { return quick::g_rep.status; }
void record_begin_run() { quick::record_run_start(); }

// Reads the file named by --replay: lines "p1|p2 <frame> <buttons hex> <rx> <ry> <lx> <ly> <l2> <r2> <connected>", "h <frame> <hash>",
// "start state <name> frame <n>" / "start power-on frame 0", "e <frame>"; other lines are the header or comments.
bool replay_begin() {
    std::FILE *f = os::open(g_options.replay_path, "rb");
    if (!f) {
        std::fprintf(stderr, "[replay] cannot open %s\n", g_options.replay_path.c_str());
        quick::g_rep.status = 5;
        return false;
    }
    quick::Replay &r = quick::g_rep;
    char line[256];
    bool magic = false;
    while (std::fgets(line, sizeof line, f)) {
        unsigned long long frame = 0, begin = 0;
        unsigned buttons, a0, a1, a2, a3, l2, r2, conn;
        char name[200];
        int port = 0;
        if (std::strncmp(line, "ps2-inputs 1", 12) == 0) magic = true;
        else if (std::sscanf(line, "p%d %llu %x %u %u %u %u %u %u %u", &port, &frame, &buttons, &a0, &a1, &a2, &a3, &l2, &r2, &conn) == 10 && (port == 1 || port == 2)) {
            PadSample s;
            s.buttons = static_cast<std::uint16_t>(buttons);
            s.axes[0] = static_cast<std::uint8_t>(a0);
            s.axes[1] = static_cast<std::uint8_t>(a1);
            s.axes[2] = static_cast<std::uint8_t>(a2);
            s.axes[3] = static_cast<std::uint8_t>(a3);
            s.l2 = static_cast<std::uint8_t>(l2);
            s.r2 = static_cast<std::uint8_t>(r2);
            s.connected = conn != 0;
            r.pads[frame].emplace_back(port - 1, s);
            r.end = std::max<std::uint64_t>(r.end, frame);
        } else if (std::sscanf(line, "h %llu %x", &frame, &a0) == 2) {
            r.checks[frame] = a0;
            r.end = std::max<std::uint64_t>(r.end, frame);
        } else if (std::sscanf(line, "e %llu", &frame) == 1) {
            r.end = std::max<std::uint64_t>(r.end, frame);
        } else if (std::sscanf(line, "start state %199s frame %llu", name, &begin) == 2 || std::sscanf(line, "start power-on frame %llu", &begin) == 1) {
            r.began = begin;
        } else if (std::sscanf(line, "rtc %llu", &begin) == 1) {
            if (!g_options.rtc_epoch) g_options.rtc_epoch = begin;  // the clock the game read when this was recorded (--rtc wins)
        } else if (std::sscanf(line, "check-every %u slices %u", &a0, &a1) == 2 && a0 == quick::kCheckEvery && a1 >= 1 && a1 <= 4096 && kRamSize % a1 == 0) {
            quick::g_slices = a1;
        }
    }
    std::fclose(f);
    if (!magic || r.pads.empty()) {
        std::fprintf(stderr, "[replay] %s is not a recording of this program\n", g_options.replay_path.c_str());
        r.status = 5;
        return false;
    }
    r.active = true;
    std::fprintf(stderr, "[replay] %s: frames %llu to %llu, %zu checkpoints\n", g_options.replay_path.c_str(), static_cast<unsigned long long>(r.began),
                 static_cast<unsigned long long>(r.end), r.checks.size());
    return true;
}

void quick_request(QuickKey key) {
    unsigned bit = quick::kSave;
    const char *text = "Saving (F5)...";
    if (key == QuickKey::load) {
        bit = quick::kLoad;
        text = "Loading (F8)...";
    } else if (key == QuickKey::report) {
        bit = quick::kReport;
        text = "Writing a report (F9)...";
        {
            std::lock_guard<std::mutex> hold(quick::g_shot_lock);
            quick::g_shot_done = false;
        }
        quick::g_shot_wanted = true;
    }
    if (quick::g_requests.fetch_or(bit) == 0) quick::g_asked_at.store(quick::now_ms());
    quick::notice(0, text);
}

bool quick_notice(QuickNotice &out) {
    std::lock_guard<std::mutex> hold(quick::g_notice_lock);
    if (!quick::g_notice_fresh) return false;
    out = quick::g_notice;
    quick::g_notice_fresh = false;
    return true;
}

// The window thread calls this in its loop. A request that the emulation does not answer for a few seconds means the game hangs
// inside the runner: the report is then written from here (without a save state), which is the case F9 matters most in.
void quick_watchdog() {
    static std::int64_t last_call = 0;
    const std::int64_t now = quick::now_ms();
    // a window that was dragged or blocked stops the game as well; that is not a hang, so the time starts again
    const bool stalled = last_call && now - last_call > 1000;
    last_call = now;
    if (!quick::g_requests.load(std::memory_order_relaxed)) return;
    if (stalled) {
        quick::g_asked_at.store(now);
        return;
    }
    if (now - quick::g_asked_at.load() < quick::kGiveUpMs) return;
    const unsigned dropped = quick::g_requests.exchange(0);
    if (dropped & quick::kReport) quick::report(true);
    if (dropped & (quick::kSave | quick::kLoad)) quick::notice(4, "The game does not answer; nothing was saved or loaded");
}

std::string quick_newest_state() {
    const std::vector<std::filesystem::path> all = quick::quick_states();
    return all.empty() ? std::string() : os::utf8(all.back());
}

std::string quick_folder(const char *kind) { return os::utf8(quick::base_dir() / kind); }
