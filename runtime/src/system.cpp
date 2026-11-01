#include "dmac.hpp"
#include "gs/gif.hpp"
#include "vif.hpp"
#include "vu_micro.hpp"
#include "gs/gs.hpp"
#include "gs/queue.hpp"
#include "hw.hpp"
#include "iop/iop.hpp"
#include "iop/sif.hpp"
#include "iop/sio2.hpp"
#include "iop/spu2.hpp"
#include "ipu/ipu.hpp"
#include "ps2/disc.hpp"
#include "ps2/ee_live.hpp"
#include "kernel.hpp"
#include "load_stats.hpp"
#include "ps2/generated.hpp"
#include "ps2/memory.hpp"
#include "ps2/os.hpp"
#include "ps2/state.hpp"
#include "ps2/state_request.hpp"
#include "ps2/system.hpp"
#include "ps2/video.hpp"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <cstdlib>
#include <filesystem>
#include <iterator>
#include <mutex>
#include <stdexcept>
#ifndef __EMSCRIPTEN__
#include <condition_variable>
#include <ctime>
#include <deque>
#include <initializer_list>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#endif

namespace ps2 {

Cpu g_cpu;
Options g_options;

// What the game's controller port gets: the pad state of one port as the host tells it. The host (window thread) writes g_pad_in at any
// time; the emulation takes it over once per frame (latch_pads), so a run is the same however the host's timing falls. The recording
// (quick_keys.inc) notes what was taken over.
struct PadSample {
    std::uint16_t buttons = 0;
    std::uint8_t axes[4] = {0x80, 0x80, 0x80, 0x80};  // right x, right y, left x, left y (as set_pad_axis numbers them)
    std::uint8_t l2 = 0xFF, r2 = 0xFF;                // pressure of L2 and R2
    bool connected = false;
    bool operator==(const PadSample &o) const {
        return buttons == o.buttons && std::memcmp(axes, o.axes, 4) == 0 && l2 == o.l2 && r2 == o.r2 && connected == o.connected;
    }
};

// the quick keys F5, F8 and F9 (quick_keys.inc, further down in this file)
namespace quick {
// Called by vblank with the pad state the host asked for; a recording notes it, a replay replaces it by the recorded one. `apply` is the
// set of ports (bit 0 = port 1) that must be given to the game: the ones the host changed, and all of them at the first frame.
void pads_hook(PadSample (&pads)[2], unsigned &apply);
// every frame: the checkpoints of a recording or a replay
void frame_check();
bool vblank_tick();
void service();
void finish();
void on_present(const gs::DisplayRegs &regs);
std::string card_note(const std::filesystem::path &state_file);
}

namespace {

constexpr std::int64_t kMaxSlice = 20000;

struct Event {
    int id;
    std::uint64_t at;
    std::function<void()> action;
};

std::uint64_t g_base = 0;
std::int64_t g_given = 0;
std::vector<Event> g_events;
std::atomic<bool> g_halted{false};  // set by the window thread when it is closed, read by the emulation thread (and the vector unit loops)
std::string g_halt_reason;
std::uint64_t g_frames = 0;
std::uint64_t g_iop_synced = 0;
bool g_state_request = false;
bool g_file_state_request = false;  // the request is for --save-state, not (only) for a key
bool g_resumed = false;
std::atomic<int> g_state_operation{0};
std::mutex g_state_mutex;
std::string g_state_path;
void (*g_state_complete)(bool, bool) = nullptr;

std::atomic<std::uint32_t> g_video_options{0};

std::uint64_t next_event_time() {
    std::uint64_t t = UINT64_MAX;
    for (const Event &e : g_events) t = std::min(t, e.at);
    return t;
}

void run_due_events() {
    sync_iop();
    for (;;) {
        const std::uint64_t t = now();
        auto it = std::min_element(g_events.begin(), g_events.end(), [](const Event &a, const Event &b) { return a.at < b.at; });
        if (it == g_events.end() || it->at > t) return;
        auto action = std::move(it->action);
        g_events.erase(it);
        action();
    }
}

void refill(Cpu &c) {
    const std::uint64_t t = now(), next = next_event_time();
    std::int64_t slice = kMaxSlice;
    if (next != UINT64_MAX) slice = std::clamp<std::int64_t>(static_cast<std::int64_t>(next - std::min(next, t)), 1, kMaxSlice);
    set_budget(c, slice);
}

std::uint64_t frame_cycles() { return kEeClock / (g_options.pal ? 50u : 60u); }

void dump_frame() {
    std::vector<std::uint32_t> rgba;
    std::uint32_t w = 0, h = 0;
    if (!gs::display(rgba, w, h)) return;
    char name[32];
    std::snprintf(name, sizeof name, "/frame_%05llu.ppm", static_cast<unsigned long long>(g_frames));
    std::FILE *f = os::open(g_options.frames_dir + name, "wb");
    if (!f) return;
    std::fprintf(f, "P6\n%u %u\n255\n", w, h);
    for (std::uint32_t p : rgba) {
        const std::uint8_t px[3] = {static_cast<std::uint8_t>(p), static_cast<std::uint8_t>(p >> 8), static_cast<std::uint8_t>(p >> 16)};
        std::fwrite(px, 1, 3, f);
    }
    std::fclose(f);
}

void apply_presses() {
    // Once the last press is over the pad is left alone, so the web page's live input works after a scripted start.
    std::uint64_t last = 0;
    for (const Press &p : g_options.presses) last = std::max<std::uint64_t>(last, p.frame + p.frames);
    if (!g_options.presses.empty() && g_frames <= last) {
        std::uint16_t mask = 0;
        std::uint8_t stick = 0;
        for (const Press &p : g_options.presses)
            if (g_frames >= p.frame && g_frames < p.frame + p.frames) {
                mask |= p.buttons;
                stick |= p.stick;
            }
        set_pad_buttons(mask, 0);
        set_pad_axis(2, (stick & 4u) ? 0x00 : (stick & 8u) ? 0xFF : 0x80, 0);
        set_pad_axis(3, (stick & 1u) ? 0x00 : (stick & 2u) ? 0xFF : 0x80, 0);
    }
    if (!g_options.axis_presses.empty()) {
        // Outside a hold the stick rests; within one it takes the given tilt.
        std::uint8_t axes[4] = {0x80, 0x80, 0x80, 0x80};
        for (const AxisPress &p : g_options.axis_presses)
            if (g_frames >= p.frame && g_frames < p.frame + p.frames) axes[p.axis] = p.value;
        for (int a = 0; a < 4; ++a) set_pad_axis(a, axes[a], 0);
    }
}

std::mutex g_pad_lock;
PadSample g_pad_in[2];
unsigned g_pad_dirty = 0;  // ports whose state the host changed since the last frame

void pad_init() {
    static const bool once = [] {
        g_pad_in[0].connected = true;  // port 1 is plugged in from the start (g_plugged in sio2.cpp), port 2 is not
        return true;
    }();
    (void)once;
}

void latch_pads() {
    PadSample pads[2];
    unsigned apply;
    {
        std::lock_guard<std::mutex> hold(g_pad_lock);
        pad_init();
        pads[0] = g_pad_in[0];
        pads[1] = g_pad_in[1];
        apply = g_pad_dirty;
        g_pad_dirty = 0;
    }
    quick::pads_hook(pads, apply);
    for (int p = 0; p < 2; ++p) {
        if (!(apply & (1u << p))) continue;
        iop::sio2::set_buttons(p, pads[p].buttons);
        for (int a = 0; a < 4; ++a) iop::sio2::set_axis(p, a, pads[p].axes[a]);
        iop::sio2::set_pressure(p, 8, pads[p].l2);
        iop::sio2::set_pressure(p, 9, pads[p].r2);
        iop::sio2::set_connected(p, pads[p].connected);
    }
}

void time_frames() {
    static const char *spec = std::getenv("PS2_TIME");
    if (!spec) return;
    static const std::uint64_t from = std::strtoull(spec, nullptr, 10);
    static const std::uint64_t count = std::strchr(spec, ',') ? std::strtoull(std::strchr(spec, ',') + 1, nullptr, 10) : 500;
    static std::chrono::steady_clock::time_point start;
    if (g_frames == from) {
        gs::flush_and_wait();
        start = std::chrono::steady_clock::now();
    } else if (g_frames == from + count) {
        gs::flush_and_wait();
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        std::fprintf(stderr, "[time] frames %llu-%llu: %.2f ms per frame\n", static_cast<unsigned long long>(from), static_cast<unsigned long long>(from + count), ms / static_cast<double>(count));
    }
}

void apply_patches(bool boot) {
    for (const Patch &p : g_options.patches) {
        if (boot ? p.place == 1 : p.place == 0) continue;
        switch (p.bytes) {
        case 1: st8(p.address, static_cast<std::uint8_t>(p.value)); break;
        case 2: st16(p.address, static_cast<std::uint16_t>(p.value)); break;
        case 4: st32(p.address, static_cast<std::uint32_t>(p.value)); break;
        default: st64(p.address, p.value); break;
        }
    }
}

// PS2_TRACK=address,length (hex, bytes of main memory): at every frame print the words of that range that changed since the
// last frame ("[track] frame N addr value"), the first frame prints them all. For finding what a game keeps in a state variable.
void track_words() {
    static const char *spec = std::getenv("PS2_TRACK");
    static const bool small_only = std::getenv("PS2_TRACK_SMALL") != nullptr;  // only changes between two values below 256 (state numbers, flags)
    static std::uint32_t base = 0, len = 0;
    static std::vector<std::uint32_t> last;
    static bool first = true;
    if (!spec) return;
    if (first) {
        first = false;
        if (std::sscanf(spec, "%x,%x", &base, &len) != 2) len = 0;
        len &= ~3u;
        last.assign(len / 4, 0);
    }
    for (std::uint32_t i = 0; i < len / 4; ++i) {
        const std::uint32_t v = ld32(base + i * 4);
        if (v != last[i] || g_frames == 0) {
            if (small_only && (v >= 256 || last[i] >= 256)) { last[i] = v; continue; }
            std::fprintf(stderr, "[track] frame %llu %08X %08X\n", static_cast<unsigned long long>(g_frames), base + i * 4, v);
            last[i] = v;
        }
    }
}

// PS2_IOP_LOAD=1: every 100 frames, the real time per frame the emulation thread spent on the IOP side (see load_stats.hpp)
void report_load() {
    if (!cost::g_on || g_frames % 100 != 0) return;
    static std::uint64_t last_ns[cost::kParts] = {}, last_count[cost::kParts] = {}, last_cycles = 0, last_bytes = 0, last_frame = 0;
    const double frames = static_cast<double>(g_frames - last_frame);
    if (frames <= 0) return;
    auto ms = [&](int p) { return static_cast<double>(cost::g_ns[p] - last_ns[p]) / 1e6 / frames; };
    auto calls = [&](int p) { return static_cast<double>(cost::g_count[p] - last_count[p]) / frames; };
    std::fprintf(stderr,
                 "[load] frames %llu-%llu, ms per frame: sync_iop %.2f (%.0f calls) = iop run %.2f [interpreter %.2f for %.0f k cycles, jobs %.2f of which sound chip %.2f (%.0f ticks)] + sif pump %.2f; disc %.3f (%.2f reads, %.1f KB)\n",
                 static_cast<unsigned long long>(last_frame), static_cast<unsigned long long>(g_frames), ms(cost::kSync), calls(cost::kSync), ms(cost::kIopRun), ms(cost::kIopExec),
                 static_cast<double>(cost::g_exec_cycles - last_cycles) / 1000.0 / frames, ms(cost::kIopJobs), ms(cost::kSpuTick), calls(cost::kSpuTick), ms(cost::kSifPump), ms(cost::kDisc),
                 calls(cost::kDisc), static_cast<double>(cost::g_disc_bytes - last_bytes) / 1024.0 / frames);
    for (int p = 0; p < cost::kParts; ++p) {
        last_ns[p] = cost::g_ns[p];
        last_count[p] = cost::g_count[p];
    }
    last_cycles = cost::g_exec_cycles;
    last_bytes = cost::g_disc_bytes;
    last_frame = g_frames;
}

void vblank() {
    ++g_frames;
    report_load();
    time_frames();
    apply_presses();
    latch_pads();
    quick::frame_check();
    apply_patches(false);
    track_words();
    ee::live::frame_tick();   // code windows that settled are translated in the background, finished libraries come in here (docs/gvz.md)
    if (g_options.on_vblank) g_options.on_vblank();
    iop::sio2::flush_cards();
    if (g_options.trace_iop_modules && g_frames % 25 == 0) std::fprintf(stderr, "[frame] %llu prims %llu\n", static_cast<unsigned long long>(g_frames), static_cast<unsigned long long>(gs::primitives_drawn()));
    if (!g_options.frames_dir.empty() && g_options.frame_every && g_frames % g_options.frame_every == 0) dump_frame();
    hw::vblank_start();
    iop::vblank(true);
    schedule(hw::kEventVblankEnd + 100, now() + hblank_cycles() * 22u, [] { iop::vblank(false); });
    kernel::on_vblank_start();
    if (g_options.max_frames && g_frames >= g_options.max_frames) halt("frame limit reached");
    if (g_options.save_state_frame && g_frames == g_options.save_state_frame) g_state_request = g_file_state_request = true;
    if (quick::vblank_tick()) g_state_request = true;  // F5, F8 or F9 was pressed: served at the same safe point
    schedule(hw::kEventVblank, now() + frame_cycles(), vblank);
}

void idle(Cpu &c) {
    std::uint64_t next = next_event_time();
    if (next == UINT64_MAX && iop::idle()) {
        halt("deadlock: every thread waits and no event is pending");
        return;
    }
    next = std::min(next, now() + kEeClock / 2000);
    g_base = std::max(now(), next);
    g_given = 0;
    c.budget = 0;
    run_due_events();
}

}

void save_context(const Cpu &c, ThreadContext &t) {
    std::memcpy(t.r, c.r, sizeof t.r);
    t.hi = c.hi;
    t.lo = c.lo;
    t.sa = c.sa;
    t.pc = c.pc & ~kUnwind;
    std::memcpy(t.f, c.f, sizeof t.f);
    t.facc = c.facc;
    t.fcr31 = c.fcr31;
}

void load_context(Cpu &c, const ThreadContext &t) {
    std::memcpy(c.r, t.r, sizeof t.r);
    c.hi = t.hi;
    c.lo = t.lo;
    c.sa = t.sa;
    c.pc = t.pc;
    std::memcpy(c.f, t.f, sizeof t.f);
    c.facc = t.facc;
    c.fcr31 = t.fcr31;
}

std::uint64_t now() { return g_base + static_cast<std::uint64_t>(g_given - g_cpu.budget); }

std::uint64_t rtc_seconds() {
    if (g_options.rtc_epoch) return g_options.rtc_epoch + now() / kEeClock;
    return static_cast<std::uint64_t>(std::time(nullptr));
}

std::uint64_t hblank_cycles() { return frame_cycles() * 2u / (g_options.pal ? 625u : 525u); }

void set_budget(Cpu &c, std::int64_t cycles) {
    g_base = now();
    g_given = cycles;
    c.budget = static_cast<std::int32_t>(cycles);
}

void schedule(int event, std::uint64_t at, std::function<void()> action) {
    g_events.erase(std::remove_if(g_events.begin(), g_events.end(), [&](const Event &e) { return e.id == event; }), g_events.end());
    g_events.push_back({event, at, std::move(action)});
}

std::uint64_t scheduled_at(int event) {
    for (const Event &e : g_events)
        if (e.id == event) return e.at;
    return UINT64_MAX;
}

bool preempt(Cpu &c) {
    run_due_events();
    refill(c);
    return g_halted || g_state_request || g_state_operation.load(std::memory_order_relaxed) || kernel::must_unwind();
}

void halt(const std::string &reason) {
    if (!g_halted) std::fprintf(stderr, "[ee] halted: %s\n", reason.c_str());
    g_halted = true;
    g_halt_reason = reason;
}

bool halted() { return g_halted; }
const std::string &halt_reason() { return g_halt_reason; }

bool request_state_operation(bool loading, const char *path, void (*complete)(bool, bool)) {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    if (g_state_operation.load(std::memory_order_acquire)) return false;
    g_state_path = path;
    g_state_complete = complete;
    g_state_operation.store(loading ? 2 : 1, std::memory_order_release);
    return true;
}

void set_video_options(std::uint32_t options) { g_video_options.store(options, std::memory_order_relaxed); }
std::uint32_t video_options() { return g_video_options.load(std::memory_order_relaxed); }

namespace {

constexpr char kStateMagic[8] = {'P', 'S', '2', 'S', 'T', 'A', 'T', 'E'};
constexpr std::uint32_t kStateVersion = 7;

std::function<void()> event_action(int id, std::uint64_t at) {
    if (id == hw::kEventVblank) return vblank;
    if (id == hw::kEventVblankEnd + 100) return [] { iop::vblank(false); };
    if (id >= hw::kEventCount) return kernel::alarm_action(id - hw::kEventCount);
    return hw::event_action(id, at);
}

std::atomic<std::uint32_t> g_vu_fence{0};

void sync_vu_thread() {
    if (!gs::queue::vu_active()) return;
    const std::uint32_t seen = g_vu_fence.load();
    gs::queue::vu().post(gs::queue::kVuFence, 0, 0, nullptr, 0);
    while (g_vu_fence.load() == seen) gs::queue::wait(g_vu_fence, seen);
}

void machine_state(state::Stream &s) {
    char magic[8];
    std::memcpy(magic, kStateMagic, sizeof magic);
    s.bytes(magic, sizeof magic);
    std::uint32_t version = kStateVersion;
    s.pod(version);
    if (std::memcmp(magic, kStateMagic, sizeof magic) != 0) throw std::runtime_error("not a state file");
    // Our version 4 differs from upstream's 4 (Cpu and thread layouts); 5 adds upstream's waiting HLE RPC replies; 6 adds the disc stream
    // of the IOP (sceCdSt*, where it is and whether it runs), so a state saved while a film or a music stream plays goes on streaming;
    // 7 adds the SIF RPC endpoints kept per server (guest servers, their order) and the IOP-to-EE request packet ring (SIF RPC rework, PR #27).
    if (version != kStateVersion && !(s.loading() && (version >= 4 && version <= 6))) throw std::runtime_error("state file from another version");
    s.set_version(version);
    s.tag("ee");
    s.pod(g_cpu);
    s.bytes(g_ram, kRamSize);
    s.bytes(g_spr, kScratchpadSize);
    s.pod(g_base);
    s.pod(g_given);
    s.pod(g_frames);
    s.pod(g_iop_synced);
    const bool forced_rate = g_options.rate_forced, forced_pal = g_options.pal;
    s.pod(g_options.pal);
    if (forced_rate) g_options.pal = forced_pal;  // --pal / --ntsc win over the state (a 50 Hz disc can be run at 60 Hz to see what a 60 Hz one asks)
    s.vec(g_events, [&s](Event &e) {
        s.pod(e.id);
        s.pod(e.at);
        if (s.loading()) e.action = event_action(e.id, e.at);
    });
    kernel::state(s);
    hw::state(s);
    dmac::state(s);
    sync_vu_thread();
    vif::state(s);
    vu::state(s);
    ipu::state(s);
    gs::state(s);
    gif::state(s);
    iop::state(s);
    sif::state(s);
    s.tag("end");
}

}

bool save_state(const std::string &path) {
    try {
        state::Writer w;
        machine_state(w);
        // temp file + rename: a crash, a kill or a full disk during the write keeps the state that was there before
        std::string why;
        if (!os::replace_file(path, w.out.data(), w.out.size(), &why)) throw std::runtime_error(why);
        std::fprintf(stderr, "[state] saved frame %llu to %s (%zu KB)\n", static_cast<unsigned long long>(g_frames), path.c_str(), w.out.size() >> 10);
        return true;
    } catch (const std::exception &e) {
        std::fprintf(stderr, "[state] cannot save: %s\n", e.what());
        return false;
    }
}

bool load_state(const std::uint8_t *data, std::size_t size) {
    try {
        state::Reader r(data, size);
        machine_state(r);
        g_resumed = true;
        std::fprintf(stderr, "[state] loaded frame %llu\n", static_cast<unsigned long long>(g_frames));
        return true;
    } catch (const std::exception &e) {
        std::fprintf(stderr, "[state] cannot load: %s\n", e.what());
        return false;
    }
}

bool load_state(const std::string &path) {
    std::ifstream in(os::path(path), std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "[state] cannot open %s\n", path.c_str());
        return false;
    }
    const std::vector<std::uint8_t> data{std::istreambuf_iterator<char>(in), {}};
    if (!load_state(data.data(), data.size())) return false;
    quick::card_note(os::path(path));  // a state made by a quick key has a copy of its memory card beside it; says so if the card differs
    return true;
}

#ifndef __EMSCRIPTEN__
#include "quick_keys.inc"
#else
namespace quick {
void pads_hook(PadSample (&)[2], unsigned &) {}
void frame_check() {}
bool vblank_tick() { return false; }
void service() {}
void finish() {}
void on_present(const gs::DisplayRegs &) {}
std::string card_note(const std::filesystem::path &) { return {}; }
}
bool replay_begin() { return false; }
bool replay_active() { return false; }
int replay_status() { return 0; }
void record_begin_run() {}
void quick_request(QuickKey) {}
bool quick_notice(QuickNotice &) { return false; }
void quick_watchdog() {}
std::string quick_folder(const char *) { return {}; }
std::string quick_newest_state() { return {}; }
#endif

std::uint64_t frame_count() { return g_frames; }

bool load_elf(const std::string &path, std::uint32_t &entry) {
    std::ifstream file(os::path(path), std::ios::binary);
    if (!file) return false;
    const std::vector<std::uint8_t> data{std::istreambuf_iterator<char>(file), {}};
    if (data.size() < 52 || std::memcmp(data.data(), "\x7f" "ELF", 4) != 0) return false;
    const auto u32 = [&](std::size_t o) { std::uint32_t v; std::memcpy(&v, data.data() + o, 4); return v; };
    const auto u16 = [&](std::size_t o) { std::uint16_t v; std::memcpy(&v, data.data() + o, 2); return v; };
    entry = u32(24);
    const std::uint32_t phoff = u32(28);
    // The zero fill of every segment comes first and the file contents after it, so a later segment's zero fill (Tekken 5's
    // overlay area is a 24 MiB segment with no file contents that lies over its own loader code) never wipes the contents of an
    // earlier one. Memory is zero at power-on, so this is what a loader that only copies the file contents gives.
    for (const bool copy : {false, true}) {
        for (std::uint32_t i = 0; i < u16(44); ++i) {
            const std::uint64_t ph = phoff + std::uint64_t{i} * u16(42);
            // The ELF comes from the disc: a program header past the end of the file, or a segment whose sums wrap round 4 GiB,
            // would read past the file or write past the 32 MiB of memory. ELF spec: p_offset + p_filesz lies inside the file and
            // the segment (p_vaddr up to p_memsz, and the p_filesz bytes copied) inside memory.
            if (ph > data.size() || data.size() - ph < 24) {
                std::fprintf(stderr, "[elf] %s is damaged: program header %u lies past the end of the file\n", path.c_str(), i);
                return false;
            }
            if (u32(ph) != 1) continue;
            const std::uint32_t offset = u32(ph + 4), vaddr = physical(u32(ph + 8)), filesz = u32(ph + 16), memsz = u32(ph + 20);
            if (std::uint64_t{vaddr} + std::max(memsz, filesz) > kRamSize || std::uint64_t{offset} + filesz > data.size()) {
                std::fprintf(stderr, "[elf] %s is damaged: segment %u does not fit in the file or in memory\n", path.c_str(), i);
                return false;
            }
            if (copy) {
                std::memcpy(g_ram + vaddr, data.data() + offset, filesz);
                watch_span(vaddr, filesz, "boot:load_exec");
            } else if (memsz > filesz) {
                std::memset(g_ram + vaddr + filesz, 0, memsz - filesz);
            }
        }
    }
    return true;
}

void sync_iop() {
    const cost::Timer whole(cost::kSync);
    const std::uint64_t t = now();
    if (t > g_iop_synced) {
        iop::run(t - g_iop_synced);
        g_iop_synced = t;
    }
    const cost::Timer pump_time(cost::kSifPump);
    sif::pump();
}

void request_iop_sync() {
    if (g_cpu.budget > 64) set_budget(g_cpu, 64);
}

void boot(std::uint32_t entry, const std::vector<std::string> &args) {
    init_watch();
    hw::reset();
    kernel::reset();
    if (!g_options.disc_path.empty() && !disc::open(g_options.disc_path))
        std::fprintf(stderr, "[ee] cannot open disc image %s\n", g_options.disc_path.c_str());
    iop::set_trace(g_options.trace_iop_modules, g_options.trace_iop_hle);
    iop::reset();
    std::vector<std::string> guest_args = args;
    std::size_t device = guest_args.empty() ? 0 : guest_args[0].find(':');
#ifdef _WIN32
    if (device == 1) device = std::string::npos;
#endif
    if (!guest_args.empty() && device == std::string::npos) {
        std::string name = guest_args[0].substr(guest_args[0].find_last_of("/\\") + 1);
        for (char &ch : name) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        guest_args[0] = "cdrom0:\\" + name + ";1";
    }
    std::uint32_t argv = 0x00000800u, payload = argv + 4u * static_cast<std::uint32_t>(guest_args.size() + 1);
    for (std::size_t i = 0; i < guest_args.size(); ++i) {
        store<std::uint32_t>(g_ram + argv + 4 * i, payload);
        std::memcpy(g_ram + payload, guest_args[i].c_str(), guest_args[i].size() + 1);
        payload += static_cast<std::uint32_t>(guest_args[i].size() + 1);
    }
    store<std::uint32_t>(g_ram + argv + 4 * guest_args.size(), 0);
    kernel::start_main_thread(entry, 0, static_cast<std::uint32_t>(guest_args.size()), argv);
    apply_patches(true);
    schedule(hw::kEventVblank, frame_cycles(), vblank);
}

// ---------------------------------------------------------------------------------------------------------------------
// Where the three threads run (Windows). The laptop has 2 fast cores (4 logical processors, 0 to 3) and 8 small ones; the
// switches below are for trying placements without a new build, and PS2_CORES=1 shows where each thread really ran.
//   PS2_QOS=1          the runner opts out of Windows' power throttling (EcoQoS) for the process and for each of its three threads
//   PS2_IDEAL=a,b,c    ideal logical processor of the emulation, vector-unit and window threads ('-' leaves one alone)
//   PS2_AFFINITY=a,b,c hex masks of the logical processors each of the same three threads may use ('-' leaves one alone)
//   PS2_PRIO=a,b,c     thread priority (-2 to 2) of the same three
//   PS2_CORES=1        every 3 s: on which logical processors each thread was, from samples taken once per frame or work item
#ifdef _WIN32
extern "C" {
void *__stdcall GetCurrentThread(void);
void *__stdcall GetCurrentProcess(void);
unsigned long __stdcall SetThreadIdealProcessor(void *, unsigned long);
unsigned long long __stdcall SetThreadAffinityMask(void *, unsigned long long);
int __stdcall SetThreadPriority(void *, int);
int __stdcall SetThreadInformation(void *, int, void *, unsigned long);
int __stdcall SetProcessInformation(void *, int, void *, unsigned long);
unsigned long __stdcall GetCurrentProcessorNumber(void);
}
#endif

namespace {

enum class Who { Emulation = 0, Vu = 1, Render = 2 };

struct CoreLog {
    std::atomic<std::uint32_t> seen[3][64];
    std::uint64_t last_print = 0;
};
CoreLog g_core_log;

#ifdef _WIN32
std::string tune_item(const char *name, int index) {
    std::string text = os::env(name);
    for (int i = 0; i < index && !text.empty(); ++i) {
        const auto comma = text.find(',');
        text = comma == std::string::npos ? std::string() : text.substr(comma + 1);
    }
    const auto comma = text.find(',');
    return comma == std::string::npos ? text : text.substr(0, comma);
}
#endif

void thread_tune(Who who) {
#ifdef _WIN32
    static const bool cores = !os::env("PS2_CORES").empty();
    (void)cores;
    struct Throttling {
        unsigned long version, control, state;
    };
    if (!os::env("PS2_QOS").empty()) {
        Throttling t{1, 1, 0};  // control EXECUTION_SPEED, state 0: never throttle
        static const bool process = SetProcessInformation(GetCurrentProcess(), 4, &t, sizeof t) != 0;  // ProcessPowerThrottling
        (void)process;
        SetThreadInformation(GetCurrentThread(), 4, &t, sizeof t);  // ThreadPowerThrottling
    }
    const std::string ideal = tune_item("PS2_IDEAL", static_cast<int>(who));
    if (!ideal.empty() && ideal != "-") SetThreadIdealProcessor(GetCurrentThread(), static_cast<unsigned long>(std::atoi(ideal.c_str())));
    const std::string mask = tune_item("PS2_AFFINITY", static_cast<int>(who));
    if (!mask.empty() && mask != "-") SetThreadAffinityMask(GetCurrentThread(), std::strtoull(mask.c_str(), nullptr, 16));
    const std::string prio = tune_item("PS2_PRIO", static_cast<int>(who));
    if (!prio.empty() && prio != "-") SetThreadPriority(GetCurrentThread(), std::atoi(prio.c_str()));
#else
    (void)who;
#endif
}

// Called once per frame (emulation thread), per work item (vector-unit thread) or per loop (window thread); cheap.
void core_sample(Who who) {
#ifdef _WIN32
    static const bool on = !os::env("PS2_CORES").empty();
    if (!on) return;
    const unsigned long cpu = GetCurrentProcessorNumber();
    g_core_log.seen[static_cast<int>(who)][cpu & 63u].fetch_add(1, std::memory_order_relaxed);
    if (who != Who::Emulation) return;
    const std::uint64_t t = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    if (t - g_core_log.last_print < 3000) return;
    if (g_core_log.last_print) {
        static const char *const kNames[3] = {"emulation", "vector units", "window"};
        for (int w = 0; w < 3; ++w) {
            std::uint32_t total = 0, n[64];
            for (int i = 0; i < 64; ++i) total += n[i] = g_core_log.seen[w][i].exchange(0);
            if (!total) continue;
            std::string line;
            for (int i = 0; i < 64; ++i)
                if (n[i] * 50 >= total) line += " #" + std::to_string(i) + ":" + std::to_string(n[i] * 100 / total) + "%";
            std::fprintf(stderr, "[cores] %-12s%s\n", kNames[w], line.c_str());
        }
    }
    g_core_log.last_print = t;
#else
    (void)who;
#endif
}

}

int run() {
    thread_tune(Who::Emulation);
    Cpu &c = g_cpu;
    const std::uint64_t started_at = now();  // for the share of waiting loops skipped (hw.cpp)
    if (!g_resumed) set_budget(c, kMaxSlice);
    while (!g_halted) {
        core_sample(Who::Emulation);
        if (const int operation = g_state_operation.load(std::memory_order_acquire)) {
            // Request fields stay immutable until completion; never serialize
            // from the browser thread while the emulated CPUs are running.
            const bool loading = operation == 2;
            const bool success = loading ? load_state(g_state_path) : save_state(g_state_path);
            const auto complete = g_state_complete;
            g_state_operation.store(0, std::memory_order_release);
            if (complete) complete(loading, success);
            // A malformed state may have partially replaced machine memory.
            if (loading && !success) halt("Quick load failed. Reload the player to continue safely.");
            if (g_halted) break;
        }
        if (g_state_request) {
            g_state_request = false;
            if (g_file_state_request) {
                g_file_state_request = false;
                save_state(g_options.save_state_path);
            }
            quick::service();
        }
        c.pc &= ~kUnwind;
        run_due_events();
        kernel::service(c);
        if (g_halted) break;
        if (c.pc == kIdle) {
            idle(c);
            continue;
        }
        if (c.budget <= 0) refill(c);
        dispatch(c, 0, 0);
    }
    quick::finish();  // a quick save or a report that is still being written to the disk
    kernel::dump();
    iop::dump();
    sif::dump();
    if (const char *peek = std::getenv("PS2_PEEK")) {
        std::uint32_t at = 0, len = 0;
        if (std::sscanf(peek, "%x,%x", &at, &len) == 2)
            for (std::uint32_t i = 0; i < len; i += 16) {
                std::fprintf(stderr, "[peek] %08X:", at + i);
                for (std::uint32_t j = 0; j < 16 && i + j < len; j += 4) std::fprintf(stderr, " %08X", ld32(at + i + j));
                std::fprintf(stderr, "\n");
            }
    }
    if (const std::string path = os::env("PS2_DUMP"); !path.empty()) {
        if (std::FILE *f = os::open(path, "wb")) {
            std::fwrite(g_ram, 1, kRamSize, f);
            std::fwrite(iop::ram_ptr(0), 1, iop::kRamSize, f);
            std::fwrite(iop::spu2::ram(), 2, 1u << 20, f);
            std::fclose(f);
        }
    }
    gs::stats();
    ipu::stats();
    if (const std::string path = os::env("PS2_DUMP_AUDIO"); !path.empty()) iop::spu2::dump_audio(path.c_str());
    if (std::getenv("PS2_TRACE_SPU2")) iop::spu2::dump_voices();
    dmac::stats();
    gif::stats();
    vif::stats();
    if (const std::uint64_t skipped = hw::spin_skipped_cycles())
        std::fprintf(stderr, "[spin] %llu cycles of waiting loops skipped (%.1f %% of the time since this run started)\n", static_cast<unsigned long long>(skipped),
                     100.0 * static_cast<double>(skipped) / static_cast<double>(std::max<std::uint64_t>(now() - started_at, 1)));
    std::fprintf(stderr, "[ee] stopped after %llu frames, %llu cycles: %s\n",
                 static_cast<unsigned long long>(g_frames), static_cast<unsigned long long>(now()), g_halt_reason.c_str());
    return 0;
}

bool display_frame(std::vector<std::uint32_t> &rgba, std::uint32_t &width, std::uint32_t &height) { return gs::display(rgba, width, height); }
bool attach_window(const WindowTarget &t) { return gs::attach_window(static_cast<int>(t.kind), t.display, t.window, t.surface, t.selector, t.width, t.height); }
void resize_window(std::uint32_t width, std::uint32_t height) { gs::resize_window(width, height); }
bool present_frame() { return gs::present(); }
void init_graphics() { gs::init(); }
void start_render_thread() { gs::start_thread(); }
void start_vu_thread() { gs::queue::vu().start(32u << 20); }

void drain_vu_thread() {
    if (gs::queue::role() != gs::queue::Role::Emulation || !gs::queue::vu_active() || gs::queue::vu().empty()) return;
    sync_vu_thread();
}

std::atomic<double> g_vu_idle_ms{0.0};

double vu_thread_idle_ms() { return g_vu_idle_ms.load(); }
std::uint64_t vu1_interpreted_calls() { return vu::interpreted_calls(); }

void run_vu_thread() {
    gs::queue::role() = gs::queue::Role::Vu;
    gs::queue::Ring &in = gs::queue::vu();
    thread_tune(Who::Vu);
    // PS2_VU_SPIN=<microseconds>: look for new work this long before going to sleep (a sleep and a wake-up cost a system call each)
    static const long spin_us = std::atol(os::env("PS2_VU_SPIN").c_str());
    for (;;) {
        gs::queue::Header h;
        const std::uint8_t *payload = nullptr;
        core_sample(Who::Vu);
        while (in.peek(h, payload)) {
            switch (h.type) {
            case gs::queue::kVif1: vif::transfer_words(1, reinterpret_cast<const std::uint32_t *>(payload), h.arg0); break;
            case gs::queue::kVif1Write: vif::write(1, h.arg0, h.arg1); break;
            case gs::queue::kGif: gif::transfer(static_cast<int>(h.arg0), payload, h.arg1); break;
            case gs::queue::kPath3Idle: gif::vif1_idle(); break;
            case gs::queue::kGifReset:
                gif::drop_held();
                gs::queue::render().post(gs::queue::kGifReset, h.arg0, h.arg1, payload, h.bytes);
                break;
            case gs::queue::kVuReset:
                vif::reset_vu_side();
                vu::reset();
                break;
            case gs::queue::kVuFence:
                g_vu_fence.fetch_add(1);
                gs::queue::notify(g_vu_fence);
                break;
            default: gs::queue::render().post(static_cast<gs::queue::Type>(h.type), h.arg0, h.arg1, payload, h.bytes); break;
            }
            in.pop(h);
        }
        const auto start = std::chrono::steady_clock::now();
        bool found = false;
        if (spin_us > 0) {
            const auto until = start + std::chrono::microseconds(spin_us);
            while (!(found = in.peek(h, payload)) && std::chrono::steady_clock::now() < until) {
#if defined(__x86_64__) || defined(_M_X64)
                __builtin_ia32_pause();
#endif
            }
        }
        if (!found) in.wait_for_work(50);
        g_vu_idle_ms.store(g_vu_idle_ms.load() + std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
    }
}
void post_present() { gs::post_present(); }
std::uint32_t picture_frame() { return gs::present_frame(); }

Rendered render_pending(bool surface, const std::function<void(const std::uint32_t *, std::uint32_t, std::uint32_t)> &copy,
                        const std::function<void()> &before_present, double budget_ms) {
    static std::vector<std::uint32_t> frame;
    static thread_local bool tuned = false;
    if (!tuned) {
        tuned = true;
        thread_tune(Who::Render);
    }
    core_sample(Who::Render);
    return gs::render_pending([&](const gs::DisplayRegs &regs) {
        quick::on_present(regs);  // the picture for an F9 report is taken here, on the thread that may read the GPU
        if (before_present) before_present();
        if (surface) {
            gs::present(regs);
            if (!copy) return;
        }
        std::uint32_t width = 0, height = 0;
        if (copy && (g_options.sync_pictures ? gs::display(regs, frame, width, height) : gs::copy_frame(regs, frame, width, height))) copy(frame.data(), width, height);
    }, budget_ms);
}

void wait_render_work(int timeout_ms) { gs::queue::render().wait_for_work(timeout_ms); }
void wake_render_thread() { gs::queue::render().wake(); }
bool copy_frame(std::vector<std::uint32_t> &rgba, std::uint32_t &width, std::uint32_t &height) { return gs::copy_frame(rgba, width, height); }

bool parse_press(const std::string &spec, Press &press) {
    static const char *const kNames[16] = {"select", "l3", "r3", "start", "up", "right", "down", "left",
                                           "l2", "r2", "l1", "r1", "triangle", "circle", "cross", "square"};
    const auto colon = spec.find(':');
    if (colon == std::string::npos) return false;
    press.frame = std::strtoull(spec.c_str(), nullptr, 10);
    std::string rest = spec.substr(colon + 1);
    press.frames = 6;
    const auto second = rest.find(':');
    if (second != std::string::npos) {
        press.frames = static_cast<std::uint32_t>(std::strtoul(rest.c_str() + second + 1, nullptr, 10));
        rest = rest.substr(0, second);
    }
    press.buttons = 0;
    press.stick = 0;
    static const char *const kStick[4] = {"lup", "ldown", "lleft", "lright"};
    std::size_t start = 0;
    while (start <= rest.size()) {
        const auto plus = rest.find('+', start);
        const std::string name = rest.substr(start, plus == std::string::npos ? std::string::npos : plus - start);
        int bit = -1;
        for (int i = 0; i < 16; ++i)
            if (name == kNames[i]) bit = i;
        int axis = -1;
        for (int i = 0; i < 4; ++i)
            if (name == kStick[i]) axis = i;
        if (bit < 0 && axis < 0) return false;
        if (bit >= 0) press.buttons |= static_cast<std::uint16_t>(1u << bit);
        else press.stick |= static_cast<std::uint8_t>(1u << axis);
        if (plus == std::string::npos) break;
        start = plus + 1;
    }
    return true;
}

bool load_patches(const std::string &path) {
    std::FILE *f = os::open(path, "r");
    if (!f) return false;
    unsigned place = 0, bytes = 0;
    unsigned long address = 0;
    unsigned long long value = 0;
    while (std::fscanf(f, "%u %lx %u %llx", &place, &address, &bytes, &value) == 4)
        g_options.patches.push_back({static_cast<std::uint8_t>(place), static_cast<std::uint8_t>(bytes), static_cast<std::uint32_t>(address), value});
    std::fclose(f);
    return true;
}

bool parse_axis(const std::string &spec, AxisPress &press) {
    static const char *const kNames[4] = {"rx", "ry", "lx", "ly"};
    std::size_t at = 0;
    const auto field = [&](std::string &out) {
        const auto colon = spec.find(':', at);
        if (colon == std::string::npos) return false;
        out = spec.substr(at, colon - at);
        at = colon + 1;
        return true;
    };
    std::string frame, axis, value;
    if (!field(frame) || !field(axis)) return false;
    const auto tail = spec.find(':', at);
    value = spec.substr(at, tail == std::string::npos ? std::string::npos : tail - at);
    press.frame = std::strtoull(frame.c_str(), nullptr, 10);
    press.axis = -1;
    for (int i = 0; i < 4; ++i)
        if (axis == kNames[i]) press.axis = i;
    if (press.axis < 0) return false;
    press.value = static_cast<std::uint8_t>(std::strtoul(value.c_str(), nullptr, 0));
    press.frames = 6;
    if (tail != std::string::npos) press.frames = static_cast<std::uint32_t>(std::strtoul(spec.c_str() + tail + 1, nullptr, 10));
    return press.frame || press.frames;
}

double gpu_busy_ms() { return gs::gpu_busy_ms(); }

#ifdef __EMSCRIPTEN__
EM_ASYNC_JS(void, ps2_browser_yield, (), {
    if (!Module.yieldPort) {
        const channel = new MessageChannel();
        Module.yieldWaiters = [];
        channel.port1.onmessage = () => Module.yieldWaiters.shift()();
        Module.yieldPort = channel.port2;
    }
    await new Promise((resolve) => { Module.yieldWaiters.push(resolve); Module.yieldPort.postMessage(0); });
});
void browser_yield() { ps2_browser_yield(); }
#else
void browser_yield() {}
#endif

std::uint64_t frames_run() { return g_frames; }
std::uint64_t primitives_drawn() { return gs::primitives_drawn(); }
// The host's pad state waits in g_pad_in until the next frame (latch_pads); only the values that change mark a port as changed.
void set_pad_buttons(std::uint16_t pressed, int pad) {
    if (pad < 0 || pad > 1) return;
    std::lock_guard<std::mutex> hold(g_pad_lock);
    pad_init();
    if (g_pad_in[pad].buttons != pressed) g_pad_dirty |= 1u << pad;
    g_pad_in[pad].buttons = pressed;
}
void set_pad_axis(int axis, std::uint8_t value, int pad) {
    if (pad < 0 || pad > 1 || axis < 0 || axis > 3) return;
    std::lock_guard<std::mutex> hold(g_pad_lock);
    pad_init();
    if (g_pad_in[pad].axes[axis] != value) g_pad_dirty |= 1u << pad;
    g_pad_in[pad].axes[axis] = value;
}
void set_pad_pressure(int button, std::uint8_t value, int pad) {
    if (pad < 0 || pad > 1 || (button != 8 && button != 9)) return;
    std::lock_guard<std::mutex> hold(g_pad_lock);
    pad_init();
    std::uint8_t &slot = button == 8 ? g_pad_in[pad].l2 : g_pad_in[pad].r2;
    if (slot != value) g_pad_dirty |= 1u << pad;
    slot = value;
}
void set_pad_connected(int pad, bool connected) {
    if (pad < 0 || pad > 1) return;
    std::lock_guard<std::mutex> hold(g_pad_lock);
    pad_init();
    if (g_pad_in[pad].connected != connected) g_pad_dirty |= 1u << pad;
    g_pad_in[pad].connected = connected;
}
std::uint16_t pad_rumble(int pad) { return iop::sio2::rumble(pad); }
void set_audio_sink(std::function<void(const std::int16_t *, std::size_t)> sink) { iop::spu2::set_sink(std::move(sink)); }

}
