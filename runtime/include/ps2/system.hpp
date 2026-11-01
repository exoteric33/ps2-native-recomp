#pragma once

#include "cpu.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ps2 {

inline constexpr std::uint64_t kEeClock = 294912000ull;
inline constexpr std::uint32_t kHandlerReturn = 0x00001000u;
inline constexpr std::uint32_t kIdle = 0x00001010u;

extern Cpu g_cpu;

struct Press {
    std::uint64_t frame;
    std::uint32_t frames;
    std::uint16_t buttons;
    std::uint8_t stick = 0;
};

struct Patch {
    std::uint8_t place;
    std::uint8_t bytes;
    std::uint32_t address;
    std::uint64_t value;
};

struct AxisPress {
    std::uint64_t frame;
    std::uint32_t frames;
    int axis;
    std::uint8_t value;
};

struct Options {
    bool trace_syscalls = false;
    bool trace_io = false;
    bool trace_threads = false;
    bool pal = true;
    bool widescreen = false;
    bool rate_forced = false;
    std::uint64_t max_frames = 0;
    std::string misses_path;
    std::string disc_path;
    bool trace_iop_modules = false;
    bool trace_iop_hle = false;
    std::string frames_dir;
    std::uint32_t frame_every = 60;
    std::vector<std::uint64_t> frame_capture_frames; // additional exact frames requested by --frame-at
    bool sync_pictures = false;  // render_pending reads each picture back at once instead of a picture or two later (headless runs with threads)
    std::vector<Press> presses;
    std::vector<AxisPress> axis_presses;
    std::vector<Patch> patches;
    std::function<void()> on_vblank;
    std::function<void(const std::string &name, const std::uint8_t *code, std::size_t size)> on_vu_program;
    std::string memcard_path;
    std::function<void()> on_memcard_saved;
    std::string save_state_path;
    std::uint64_t save_state_frame = 0;
    // for the keys of a window run (F5, F8, F9; docs/play-keys.md)
    std::string program_path;                                  // the game's program, set by the host
    std::string command_line;                                  // what the runner was started with, for reports
    std::string quick_dir;                                     // --quick-dir; default: the folder of the memory card, else of the program
    std::function<std::string(std::size_t lines)> recent_log;  // the host's copy of the log: its last lines ...
    std::function<std::string(std::size_t lines)> first_log;   // ... and its first lines
    std::function<void()> on_exit;                             // the window run is about to end the process
    // recording and replaying of the controller ports (docs/replay.md)
    bool auto_record = false;                                  // a window run records by itself (the host sets it; PS2_RECORD=0 or --no-record turns it off)
    std::string record_path;                                   // --record <file>: record from the start of this run into that file
    std::string replay_path;                                   // --replay <file>: play a recording headless and check it
    bool replay_continue = false;                              // --replay-continue: after a checkpoint that differs, play on to the end of the recording
    std::string load_state_path;                               // the state this run was started from (--load-state), for the recording's header
    // The console's clock as the game reads it (sceCdReadClock). 0: the host's clock. Otherwise: this many seconds (UTC) at emulated
    // time 0, and the clock then runs with the emulated time, so a recording and its replay show the game the same time
    std::uint64_t rtc_epoch = 0;                               // --rtc <seconds>; a recording writes it into its header and a replay takes it from there
};

extern Options g_options;

struct ThreadContext {
    Reg128 r[32];
    Reg128 hi, lo;
    std::uint32_t sa, pc;
    std::uint32_t f[32];
    std::uint32_t facc, fcr31;
};

void save_context(const Cpu &c, ThreadContext &t);
void load_context(Cpu &c, const ThreadContext &t);

std::uint64_t now();
bool frame_capture_requested(std::uint64_t frame);
std::uint64_t rtc_seconds();  // the clock the game reads, in seconds since 1970 (UTC), see Options::rtc_epoch
std::uint64_t hblank_cycles();
void set_budget(Cpu &c, std::int64_t cycles);
void schedule(int event, std::uint64_t at, std::function<void()> action);
std::uint64_t scheduled_at(int event);
bool preempt(Cpu &c);

bool has_code(std::uint32_t pc);
void raise_intc(unsigned cause);
void request_reschedule();
void request_iop_sync();
void sync_iop();

bool load_elf(const std::string &path, std::uint32_t &entry);
void boot(std::uint32_t entry, const std::vector<std::string> &args);
int run();
void halt(const std::string &reason);
bool save_state(const std::string &path);
bool load_state(const std::string &path);
bool load_state(const std::uint8_t *data, std::size_t size);
bool halted();
const std::string &halt_reason();
std::uint64_t frame_count();

bool display_frame(std::vector<std::uint32_t> &rgba, std::uint32_t &width, std::uint32_t &height);

struct WindowTarget {
    enum Kind { X11, Wayland, Canvas, Win32, Metal } kind = X11;
    void *display = nullptr;
    std::uint64_t window = 0;
    void *surface = nullptr;
    const char *selector = nullptr;
    std::uint32_t width = 0, height = 0;
};

// Lets the WebGPU renderer present straight to a window or canvas instead of handing frames back.
bool attach_window(const WindowTarget &target);
void resize_window(std::uint32_t width, std::uint32_t height);
bool present_frame();
void init_graphics();
void start_render_thread();
void start_vu_thread();
// The emulation thread is about to read or write the memory of VU1 itself: wait until the vector unit thread has done what was posted
// to it (VIF1 packets, programs), as the console would have by then. Nothing happens without that thread or when it has nothing to do.
void drain_vu_thread();
void run_vu_thread();
double vu_thread_idle_ms();
std::uint64_t vu1_interpreted_calls();
void post_present();
std::uint32_t picture_frame();  // the emulation frame of the picture render_pending is presenting now
enum class Rendered { idle, presented, busy };
Rendered render_pending(bool surface, const std::function<void(const std::uint32_t *rgba, std::uint32_t width, std::uint32_t height)> &copy,
                        const std::function<void()> &before_present, double budget_ms);
void wait_render_work(int timeout_ms);
void wake_render_thread();
bool copy_frame(std::vector<std::uint32_t> &rgba, std::uint32_t &width, std::uint32_t &height);
bool parse_press(const std::string &spec, Press &press);
bool parse_axis(const std::string &spec, AxisPress &press);
bool load_patches(const std::string &path);
double gpu_busy_ms();
void browser_yield();
std::uint64_t frames_run();
std::uint64_t primitives_drawn();
// pad 0 is the first controller port, pad 1 the second, which starts unplugged
void set_pad_buttons(std::uint16_t pressed, int pad = 0);
void set_pad_axis(int axis, std::uint8_t value, int pad = 0);
void set_pad_pressure(int button, std::uint8_t value, int pad = 0);
void set_pad_connected(int pad, bool connected);
std::uint16_t pad_rumble(int pad = 0);
void set_audio_sink(std::function<void(const std::int16_t *samples, std::size_t frames)> sink);
// Writes the memory card file if the game changed the card since the last write; call it wherever the game can end. hurry: do not wait
// long for the card (Ctrl+C, a crash).
void flush_memory_card(bool hurry = false);
// A copy of the memory card as the game sees it now (the file may be up to 50 frames behind). false when no card is in use. With
// wait false the card is given up after a moment if a card command holds it.
bool memory_card_image(std::vector<std::uint8_t> &out, bool wait);
// For a crash handler (runtime/src/iop/sio2.cpp): the newest card goes to "<card>.crash.ps2" beside the card file, never over it.
// Returns 0 when the card had not changed since it was last written, 1 when the copy was written (`where`, if not null, names it),
// 2 when the card was busy, 3 when the copy could not be written. It prints nothing.
int save_memory_card_after_crash(const char **where);

// --- Keys of a window run (docs/play-keys.md): F5 quick save, F8 quick load, F9 problem report ---
enum class QuickKey { save, load, report };
// Asks for it; any thread may call this. The emulation does the work at the next frame boundary.
void quick_request(QuickKey key);
// What the player should be told. kind: 0 working on it, 1 saved, 2 loaded, 3 report written, 4 trouble. Returns the newest message
// that was not taken yet.
struct QuickNotice {
    std::string text;
    int kind = 0;
};
bool quick_notice(QuickNotice &notice);
// The window thread calls this in its loop: a request the emulation does not answer (the game hangs inside the runner) is given up
// after a few seconds, and an F9 is then answered from the window thread, without a save state.
void quick_watchdog();
// The folder for "states", "reports" or "logs": next to the memory card file (else next to the program), or --quick-dir.
std::string quick_folder(const char *kind);
// The newest quick save, "" when there is none (--load-state newest).
std::string quick_newest_state();

// --- Recording and replaying of the controller ports (docs/replay.md) ---
// Pads: the host (window, web, tests) tells the pad state through the set_pad_* functions above; the emulation takes it over at the
// next frame boundary, the same for every run. The recording notes what was taken over, so a replay can give the game the same.
// replay_begin reads the file named by --replay (false and a message when it cannot); replay_active is true during such a run.
bool replay_begin();
bool replay_active();
// 0 = same to the end, 3 = the game went another way, 4 = the game ended before the recording did, 5 = the file could not be used
int replay_status();
// A recording of this run from its start (after --load-state, else from power-on); main calls it once before run().
void record_begin_run();

}
