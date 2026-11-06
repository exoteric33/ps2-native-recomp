#include "ps2/disc.hpp"
#include "ps2/os.hpp"
#include "ps2/system.hpp"
#include "ps2/ee_live.hpp"
#include "ps2/vu_live.hpp"

#include "big_stack_thread.hpp"
#ifdef PS2_WINDOW
#include "window.hpp"
#endif
#ifdef PS2_PROFILE
#include "profile.hpp"
#endif

#include <algorithm>
#include <cstdio>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <string>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <vector>
#include <atomic>
#include <chrono>
#include <thread>
#if defined(__x86_64__) || defined(__i386__)
#include <cpuid.h>
#endif
#ifdef PS2_WINDOW
#include <deque>
#include <mutex>
#endif

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#include <timeapi.h>
#include <shellapi.h>
#else
#include <cerrno>
#include <unistd.h>
#ifdef __GLIBC__
#include <execinfo.h>
#endif
#ifdef PS2_WINDOW
#include <fcntl.h>
#endif
#endif

void register_generated_code();
void register_generated_vu();

namespace {

// The game folder port.sh made (games/<name>): --game-dir when given, else the first of the program's folder, its parent and the
// disc image's folder that holds vu1/ or entries.txt. Empty when none does. The log starts before the arguments are read in full
// and needs the answer too, so this is one function used in both places.
std::filesystem::path find_game_dir(const std::string &program_location, const std::string &disc_path, const std::string &explicit_dir) {
    std::error_code error;
    if (!explicit_dir.empty()) return std::filesystem::absolute(ps2::os::path(explicit_dir), error);
    const std::filesystem::path program_file = std::filesystem::absolute(ps2::os::path(program_location), error);
    std::vector<std::filesystem::path> places{program_file.parent_path(), program_file.parent_path().parent_path()};
    if (!disc_path.empty()) {
        const std::filesystem::path disc = std::filesystem::absolute(ps2::os::path(disc_path), error);
        if (!error) places.push_back(disc.parent_path());
    }
    for (const auto &place : places)
        if (std::filesystem::is_directory(place / "vu1", error) || std::filesystem::is_regular_file(place / "entries.txt", error)) return place;
    return {};
}

int usage() {
    std::fprintf(stderr,
                 "usage: ps2run [elf] [--arg text]... [--frames n] [--ntsc] [--misses file]\n"
                 "              [--disc image.iso] [--memcard card.ps2] [--pal | --ntsc] [--trace-syscalls] [--trace-io] [--trace-threads]\n"
                 "              [--trace-iop] [--trace-iop-hle] [--frames-dir dir] [--frame-every n] [--frame-at n]...\n"
                 "              [--press frame:button[+button][:frames]]... [--window] [--fullscreen]\n"
                 "              [--save-state frame:file] [--load-state file|newest] [--quick-dir dir] [--widescreen] [--patches file]\n"
                 "              [--record file] [--no-record] [--replay file [--replay-continue]] [--rtc seconds] [--game-dir dir]\n");
    return 2;
}

#ifdef _WIN32
// On Windows ps2run is a window (GUI) program, so a double-click or a shortcut opens no console window. Such a program starts
// with no stdout or stderr unless its parent handed it a pipe or a file. This gives the output back where there is one:
//   - a stream the parent passed (a pipe, a file, NUL: port.sh, report.py, autotests.py, `> log.txt`) is used as it is;
//   - otherwise the program joins the console of its parent (cmd, PowerShell, Windows Terminal), so the log shows there;
//   - with no console either (double-click, shortcut, the app) the stream goes to NUL, so a write is harmless.
bool has_std_handle(DWORD which) {
    const HANDLE h = GetStdHandle(which);
    return h && h != INVALID_HANDLE_VALUE && GetFileType(h) != FILE_TYPE_UNKNOWN;
}

void connect_stream(std::FILE *stream, DWORD which, bool console) {
    // the C stream (printf, std::cerr) and the Windows handle (code that writes to it directly, like wgpu-native) both move
    const char *target = console ? "CONOUT$" : "NUL";
    if (!std::freopen(target, "wb", stream)) std::freopen("NUL", "wb", stream);
    const wchar_t *wide_target = console ? L"CONOUT$" : L"NUL";
    HANDLE h = CreateFileW(wide_target, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE) SetStdHandle(which, h);
}

void connect_output() {
    const bool need_out = !has_std_handle(STD_OUTPUT_HANDLE), need_err = !has_std_handle(STD_ERROR_HANDLE);
    if (need_out || need_err) {
        // joins the console of the parent; "access denied" means this process has a console already. No console at all
        // (double-click, shortcut, a program without a window) fails here, and the missing streams go to NUL.
        const bool console = AttachConsole(ATTACH_PARENT_PROCESS) || GetLastError() == ERROR_ACCESS_DENIED;
        if (need_out) connect_stream(stdout, STD_OUTPUT_HANDLE, console);
        if (need_err) connect_stream(stderr, STD_ERROR_HANDLE, console);
    }
    // binary, so a log line ends in LF like on Linux (a stream without a descriptor has none to set)
    for (std::FILE *stream : {stdout, stderr}) {
        const int fd = _fileno(stream);
        if (fd >= 0) _setmode(fd, _O_BINARY);
    }
}
#endif

// PS2_CRASH_TEST=<where>:<what> (a test aid, off unless set; docs/nolog.md): makes the program fail on purpose, to see that the log
// then ends with the crash line. where: main (the window thread, after the log copy started), emu (the emulation thread), tee (the thread that copies the log). what: access (a write to address 0), stack (endless
// recursion: a stack overflow), abort, throw (an exception nobody catches), param (a C runtime call with an argument it refuses).
__attribute__((noinline)) int crash_test_recurse(int depth) {
    // A frame this big is probed from the top down, so the overflow is found while most of a page is still left (under Wine the
    // handler has no room at all when the last bytes are used up one small frame after the other: its own limit, not a bug here).
    volatile char pad[1000000];  // not a power of two: the last level then ends well above the limit
    pad[0] = static_cast<char>(depth);
    return crash_test_recurse(depth + 1) + pad[0];
}

[[maybe_unused]] void crash_test(const char *where) {
    static const std::string setting = ps2::os::env("PS2_CRASH_TEST");
    const std::string want = std::string(where) + ":";
    if (setting.rfind(want, 0) != 0) return;
    const std::string what = setting.substr(want.size());
    std::fprintf(stderr, "[crash-test] %s at %s\n", what.c_str(), where);
    std::fflush(stderr);
    if (what == "access") *static_cast<volatile int *>(nullptr) = 1;
    else if (what == "stack") std::exit(crash_test_recurse(0));
    else if (what == "abort") std::abort();
    else if (what == "throw") throw std::runtime_error("PS2_CRASH_TEST");
    else if (what == "param") {
#ifdef _WIN32
        _close(12345);  // the C runtime calls its invalid-parameter handler for a file number that is not open
#endif
    }
}

#ifdef PS2_WINDOW
// While a window is open the runner's log (stderr) is kept in two more places: in memory (the last lines go into an F9 report) and
// in a file, logs\ps2run_<date>_<time>.log next to the memory card (the newest 10 are kept). A program started by a double-click or a
// shortcut has no console, so without the file nobody could read what it said before it crashed. The log still reaches the
// original stderr (the console, a pipe, a file the parent passed, or nothing) as before and in the same order: stderr is pointed at
// a pipe, and a thread passes what comes out of it on.
class LogTee {
public:
    ~LogTee() { stop(); }

    // The log goes into `folder`; if no file can be made there (a folder that cannot be created or written), into a folder of the
    // system's temporary files instead, and the first lines say so. The pipe and the thread start either way.
    bool start(const std::filesystem::path &folder) {
        if (active_) return true;
        std::error_code error;
        std::filesystem::path place = folder;
        for (int attempt = 0; attempt < 2 && !file_; ++attempt) {
            if (attempt) {
                place = std::filesystem::temp_directory_path(error) / "ps2run-logs";
                if (error) break;
            }
            std::filesystem::create_directories(place, error);
            std::vector<std::filesystem::path> old;
            for (std::filesystem::directory_iterator it(place, error), end; !error && it != end; it.increment(error)) {
                const std::string name = ps2::os::utf8(it->path().filename());
                if (name.rfind("ps2run_", 0) == 0 && name.size() > 4 && name.compare(name.size() - 4, 4, ".log") == 0) old.push_back(it->path());
            }
            std::sort(old.begin(), old.end());
            for (std::size_t i = 0; i + 9 < old.size(); ++i) std::filesystem::remove(old[i], error);
            const std::time_t t = std::time(nullptr);
            std::tm local{};
#ifdef _WIN32
            localtime_s(&local, &t);
#else
            localtime_r(&t, &local);
#endif
            char stamp[32];
            std::strftime(stamp, sizeof stamp, "%Y-%m-%d_%H-%M-%S", &local);
            // two runs that start in the same second must not share a file
            std::filesystem::path file = place / (std::string("ps2run_") + stamp + ".log");
            for (int n = 2; std::filesystem::exists(file, error) && n < 100; ++n) file = place / (std::string("ps2run_") + stamp + "_" + std::to_string(n) + ".log");
            path_ = ps2::os::utf8(file);
            // "ab": every write goes to the end of the file, so a line written beside the copy thread (write_direct) is never written over
            file_ = ps2::os::open(path_, "ab");
        }
        int fds[2];
#ifdef _WIN32
        // The descriptor the stderr stream really writes to. It is 2 when the parent passed a stderr, but not when the program
        // started with no standard handles: connect_output() then reopened the stream onto NUL, and the C runtime gave it a new
        // descriptor (4, 5, ...) while 2 stayed unusable. Pointing only 2 at the pipe left fprintf(stderr) writing to NUL, so
        // the log file stayed empty for every shortcut, app and double-click start.
        err_fd_ = _fileno(stderr);
        if (err_fd_ < 0) {
            std::freopen("NUL", "wb", stderr);
            err_fd_ = _fileno(stderr);
        }
        if (err_fd_ < 0) err_fd_ = 2;
        if (_pipe(fds, 1 << 20, _O_BINARY) != 0) return false;
        saved_ = _dup(err_fd_);
        if (saved_ < 0) saved_ = _open("NUL", _O_WRONLY | _O_BINARY);
        if (_dup2(fds[1], err_fd_) != 0) return false;
        _setmode(err_fd_, _O_BINARY);
        SetStdHandle(STD_ERROR_HANDLE, reinterpret_cast<HANDLE>(_get_osfhandle(err_fd_)));  // code that writes to the handle (wgpu-native) too
#else
        if (pipe(fds) != 0) return false;
        saved_ = dup(2);
        if (saved_ < 0) saved_ = open("/dev/null", O_WRONLY);
        if (dup2(fds[1], 2) < 0) return false;
#endif
        read_fd_ = fds[0];
        write_fd_ = fds[1];
        active_ = true;
        thread_ = std::thread([this] { pump(); });
        if (!file_) std::fprintf(stderr, "[log] no log file could be made in %s or in the temporary folder: this log is not written to disk\n", ps2::os::utf8(folder).c_str());
        else if (place != folder) std::fprintf(stderr, "[log] %s cannot be written to; this log is written to %s instead\n", ps2::os::utf8(folder).c_str(), path_.c_str());
        else std::fprintf(stderr, "[log] this log is also written to %s\n", path_.c_str());
        return true;
    }

    // Like flush(), and when the copy thread has not got to the line in time (it is slow or stuck, for example while a virus scanner
    // holds the file), `last` is appended to the log file from here, so a crash line is in the file even then. Used by the crash handlers.
    void flush_or_write(int wait_ms, const char *last, std::size_t size) {
        if (!active_) return;
        if (flush(wait_ms)) return;
        if (path_.empty() || !size) return;
#ifdef _WIN32
        const HANDLE h = CreateFileW(ps2::os::path(path_).c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) return;
        DWORD written = 0;
        WriteFile(h, last, static_cast<DWORD>(size), &written, nullptr);
        CloseHandle(h);
#else
        const int fd = open(path_.c_str(), O_WRONLY | O_APPEND);
        if (fd < 0) return;
        const ssize_t ignored = write(fd, last, size);
        (void)ignored;
        close(fd);
#endif
    }

    // Everything written so far has been passed on and recorded when this returns (or wait_ms has passed). It takes no lock, so it
    // is safe in a crash handler even when the thread that crashed was holding one.
    bool flush(int wait_ms) {
        if (!active_) return true;
        const std::uint64_t want = ++sent_;
        put(write_fd_, kMarker, sizeof kMarker - 1);
        for (int waited = 0; seen_.load() < want && waited < wait_ms; ++waited) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return seen_.load() >= want;
    }

    // stderr goes straight to where it went before; the file is closed. Safe to call twice.
    void stop() {
        if (!active_) return;
        flush(500);
        active_ = false;
#ifdef _WIN32
        _dup2(saved_, err_fd_);
        _setmode(err_fd_, _O_BINARY);
        SetStdHandle(STD_ERROR_HANDLE, reinterpret_cast<HANDLE>(_get_osfhandle(err_fd_)));
        _close(write_fd_);
#else
        dup2(saved_, 2);
        close(write_fd_);
#endif
        if (thread_.joinable()) thread_.join();
#ifdef _WIN32
        _close(read_fd_);
        _close(saved_);
#else
        close(read_fd_);
        close(saved_);
#endif
        if (file_) std::fclose(file_);
        file_ = nullptr;
    }

    std::string tail(std::size_t lines) {
        std::lock_guard<std::mutex> hold(m_);
        std::string out;
        for (std::size_t i = ring_.size() > lines ? ring_.size() - lines : 0; i < ring_.size(); ++i) out += ring_[i] + "\n";
        return out + partial_;
    }

    std::string head(std::size_t lines) {
        std::lock_guard<std::mutex> hold(m_);
        std::string out;
        for (std::size_t i = 0; i < head_.size() && i < lines; ++i) out += head_[i] + "\n";
        return out;
    }

private:
    // a line of its own that the reader takes out of the stream: it tells flush() that everything before it was handled
    static constexpr char kMarker[] = "\x1e<ps2run-log-flush>\x1e\n";
    static constexpr std::size_t kRingLines = 600, kHeadLines = 100;
    static constexpr std::size_t kFileLimit = 64u << 20;

    static void put(int fd, const char *data, std::size_t size) {
        while (size) {
#ifdef _WIN32
            const int n = _write(fd, data, static_cast<unsigned>(std::min<std::size_t>(size, 1u << 20)));
#else
            const ssize_t n = write(fd, data, size);
#endif
            if (n <= 0) return;
            data += n;
            size -= static_cast<std::size_t>(n);
        }
    }

    void pump() {
        char buffer[8192];
        crash_test("tee");
        for (;;) {
#ifdef _WIN32
            const int n = _read(read_fd_, buffer, sizeof buffer);
#else
            const ssize_t n = read(read_fd_, buffer, sizeof buffer);
#endif
            if (n <= 0) return;
            std::string_view chunk(buffer, static_cast<std::size_t>(n));
            for (;;) {
                const std::size_t at = chunk.find(kMarker);
                const std::string_view part = at == std::string_view::npos ? chunk : chunk.substr(0, at);
                if (!part.empty()) record(part);
                if (at == std::string_view::npos) break;
                chunk.remove_prefix(at + sizeof kMarker - 1);
                ++seen_;
            }
        }
    }

    void record(std::string_view part) {
        put(saved_, part.data(), part.size());
        std::lock_guard<std::mutex> hold(m_);
        if (file_ && written_ < kFileLimit) {
            std::fwrite(part.data(), 1, part.size(), file_);
            std::fflush(file_);
            written_ += part.size();
            if (written_ >= kFileLimit) std::fputs("\n[log] this file reached 64 MB and stops here\n", file_);
        }
        for (const char c : part) {
            if (c != '\n') {
                if (partial_.size() < 4000) partial_ += c;
                continue;
            }
            if (!partial_.empty() && partial_.back() == '\r') partial_.pop_back();
            if (head_.size() < kHeadLines) head_.push_back(partial_);
            ring_.push_back(std::move(partial_));
            partial_.clear();
            if (ring_.size() > kRingLines) ring_.pop_front();
        }
    }

    bool active_ = false;
    int read_fd_ = -1, write_fd_ = -1, saved_ = -1, err_fd_ = 2;
    std::FILE *file_ = nullptr;
    std::string path_;
    std::thread thread_;
    std::mutex m_;
    std::atomic<std::uint64_t> sent_{0}, seen_{0};
    std::uint64_t written_ = 0;
    std::deque<std::string> ring_;
    std::vector<std::string> head_;
    std::string partial_;
};

LogTee g_log;
#endif

// After a crash the card is never written over the card file (the card in memory may be half of a save, or damaged): the newest
// card, if it changed since the file was last written, goes to "<card>.crash.ps2" beside it. This saves it and puts the line about
// it into `line` (snprintf only: no allocation, no stream lock), returning the line's length, 0 for none.
int crash_card_line(char *line, std::size_t size) {
    const char *where = nullptr;
    int length = 0;
    switch (ps2::save_memory_card_after_crash(&where)) {
    case 1:
        length = std::snprintf(line, size, "[mc] the card changed less than a second before the crash; that newest card is kept in %s (the card file keeps the last card written normally)\n", where);
        break;
    case 2: length = std::snprintf(line, size, "[mc] the card was being changed when the program crashed; the card file keeps the last card written normally\n"); break;
    case 3: length = std::snprintf(line, size, "[mc] the card changed before the crash, but no copy could be written; the card file keeps the last card written normally\n"); break;
    default: break;
    }
    return length < 0 ? 0 : std::min<int>(length, static_cast<int>(size) - 1);
}

#ifdef _WIN32
// The memory card file is written when the game saves (50 frames after its last write) and again whenever the game ends. These
// two cover the ways a Windows program can be ended from outside while it is running:
//   - Ctrl+C, Ctrl+Break or closing the console window the program was started from (it is told, then given a few seconds):
//     the card file is written, as when the window is closed (unless a card command is in the middle of a page right then);
//   - a crash (an unhandled exception in the program itself): the newest card goes beside the card file, never over it
//     (crash_card_line).
// Both then let the system do what it would have done anyway: the program ends.
BOOL WINAPI on_console_event(DWORD) {
    ps2::flush_memory_card(true);
    return FALSE;
}

// What a Windows exception code means, for the crash line (a log is read by people who do not know the codes).
const char *describe(DWORD code) {
    switch (code) {
    case 0xC0000005: return " (access violation: a wrong memory address)";
    case 0xC00000FD: return " (stack overflow)";
    case 0xC0000094: return " (integer division by zero)";
    case 0xC000001D: return " (illegal instruction)";
    case 0xC0000409: return " (the program stopped itself after a failed check)";
    case 0xC0000374: return " (damaged heap)";
    case 0x20474343:
    case 0xE06D7363: return " (a C++ exception nobody caught)";
    default: return "";
    }
}

// The crash line and the log. A crash is reported by a helper thread that was started with the handlers and has a stack of its own:
// the crashing thread may have no stack left (a stack overflow leaves about one page, and formatting a line needs more), and a
// second fault inside the handler used to end the process before the log had anything. The crashing thread only wakes the helper
// and waits (a few bytes of stack). Four ways end a program without the line, and all four come here:
//   - an unhandled exception (an access violation, a stack overflow, ...): on_crash;
//   - abort(), which a failed check or std::terminate ends in: on_abort (UCRT's own abort writes nothing to a window program);
//   - an exception nobody caught: on_terminate, which also says what it was;
//   - a refused argument of the C runtime (a bad file number, say), which the runtime answers with a fast fail that no handler
//     sees: on_invalid_parameter.
namespace crash {
HANDLE g_ask = nullptr, g_done = nullptr;
volatile LONG g_running = 0;
volatile DWORD g_reporter = 0;  // the thread that is writing the report
DWORD g_code = 0;
void *g_address = nullptr;
char g_text[400];

void report(DWORD code, void *address, const char *text) {
    // written to the handle directly: another thread may hold the lock of the C stream
    char line[640];
    DWORD written = 0;
    int length = 0;
#ifdef PS2_WINDOW
    // the last thing in the log file, so a player who sends it shows what happened
    if (text) length = std::snprintf(line, sizeof line, "[crash] %s\n", text);
    else length = std::snprintf(line, sizeof line, "[crash] the program crashed: exception %08lX%s at address %p\n", static_cast<unsigned long>(code), describe(code), address);
    if (length > 0) WriteFile(GetStdHandle(STD_ERROR_HANDLE), line, static_cast<DWORD>(std::min<std::size_t>(length, sizeof line - 1)), &written, nullptr);
#else
    (void)code, (void)address, (void)text;
#endif
    char card[640];
    const int card_length = crash_card_line(card, sizeof card);
    if (card_length > 0) WriteFile(GetStdHandle(STD_ERROR_HANDLE), card, static_cast<DWORD>(card_length), &written, nullptr);
#ifdef PS2_WINDOW
    // if the copy thread is stuck, the lines go into the file from here
    if (length > 0) {
        char both[1300];
        int size = std::min<int>(length, static_cast<int>(sizeof line) - 1);
        std::memcpy(both, line, static_cast<std::size_t>(size));
        if (card_length > 0) {
            std::memcpy(both + size, card, static_cast<std::size_t>(card_length));
            size += card_length;
        }
        g_log.flush_or_write(1500, both, static_cast<std::size_t>(size));
    }
#endif
}

DWORD WINAPI helper(void *) {
    for (;;) {
        if (WaitForSingleObject(g_ask, INFINITE) != WAIT_OBJECT_0) return 0;
        g_reporter = GetCurrentThreadId();
        report(g_code, g_address, g_text[0] ? g_text : nullptr);
        SetEvent(g_done);
    }
}

// The first crash writes the report (on the helper thread, or on the spot when there is none); a second thread that crashes at the
// same time waits for it, so the process is not ended in the middle of the report. A crash inside the report itself (the reporting
// thread comes back here) is not reported again.
void run(DWORD code, void *address, const char *text) {
    const DWORD me = GetCurrentThreadId();
    if (InterlockedCompareExchange(&g_running, 1, 0) != 0) {
        if (g_reporter != me && g_done) WaitForSingleObject(g_done, 5000);
        return;
    }
    g_code = code;
    g_address = address;
    if (text) {
        std::snprintf(g_text, sizeof g_text, "%s", text);
    } else {
        g_text[0] = 0;
    }
    if (g_ask && g_done) {
        SetEvent(g_ask);
        WaitForSingleObject(g_done, 8000);
    } else {
        g_reporter = me;
        report(code, address, text);
    }
}

LONG WINAPI on_crash(EXCEPTION_POINTERS *info) {
    run(info->ExceptionRecord->ExceptionCode, info->ExceptionRecord->ExceptionAddress, nullptr);
    return EXCEPTION_CONTINUE_SEARCH;
}

void on_abort(int) {
    run(0, nullptr, "the program called abort() (a check of the program failed, or an exception nobody caught)");
    std::_Exit(3);
}

void on_terminate() {
    char what[300] = "unknown exception";
    try {
        if (const auto current = std::current_exception()) std::rethrow_exception(current);
    } catch (const std::exception &e) {
        std::snprintf(what, sizeof what, "%s", e.what());
    } catch (...) {
    }
    char text[400];
    std::snprintf(text, sizeof text, "the program ended by an exception nobody caught: %s", what);
    run(0, nullptr, text);
    std::_Exit(3);
}

void on_invalid_parameter(const wchar_t *, const wchar_t *function, const wchar_t *, unsigned int, uintptr_t) {
    char name[120] = "?";
    if (function) WideCharToMultiByte(CP_UTF8, 0, function, -1, name, sizeof name - 1, nullptr, nullptr);
    char text[300];
    std::snprintf(text, sizeof text, "the C runtime refused an argument (in %s)", name);
    run(0, nullptr, text);
    std::_Exit(3);
}

void install() {
    g_ask = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (g_ask && g_done) {
        if (const HANDLE h = CreateThread(nullptr, 256 * 1024, helper, nullptr, 0, nullptr)) CloseHandle(h);
        else g_ask = nullptr;
    }
    SetUnhandledExceptionFilter(on_crash);
    std::signal(SIGABRT, on_abort);
    std::set_terminate(on_terminate);
    _set_invalid_parameter_handler(on_invalid_parameter);
}
}  // namespace crash
#endif

#ifndef _WIN32
// The same for Linux. A run that ended without its closing lines ("[ee] stopped after ..." and the audio summary) used to leave no trace
// of why: a crash (SIGSEGV), an abort (a failed allocation or an exception nobody caught: SIGABRT) and the terminal closing (SIGHUP) each
// ended the process at once, and a core dump says nothing in the log. This writes one line and the stack of the thread that got the
// signal, gives the log copy and the memory card a moment to reach the disk, and then lets the signal do what it would have done (a core
// dump, if the system makes one). SIGINT and SIGTERM are SDL's: it turns them into "window closed" (window/poll). SIGKILL, which the
// compositor's "not responding" window and `hyprctl kill` send, cannot be caught; a run that ends with neither line has had one.
//
// The memory card is not written inside the handler: almost nothing is safe there (an abort from a damaged heap comes while the
// allocator's lock is held, and the first allocation would hang the program instead of ending it). A helper thread, started with
// the handlers, waits on a pipe; the handler wakes it with one byte (write() is safe in a handler) and waits up to two seconds for
// it. After SIGHUP (the terminal was closed, as Ctrl+C) the helper writes the card file as usual; after a crash or an abort it puts
// the newest card beside the card file, never over it (crash_card_line).
int g_card_wake[2] = {-1, -1};
std::atomic<int> g_card_done{0};  // lock-free, so the handler may read it

void card_helper() {
    char what = 0;
    for (;;) {
        const ssize_t got = read(g_card_wake[0], &what, 1);
        if (got == 1) break;
        if (got < 0 && errno == EINTR) continue;
        return;
    }
    if (what == 'h') ps2::flush_memory_card(true);
    else {
        char line[640];
        const int length = crash_card_line(line, sizeof line);
        if (length > 0) {
            ssize_t ignored = write(2, line, static_cast<std::size_t>(length));
            (void)ignored;
        }
    }
    g_card_done.store(1);
}

void on_fatal_signal(int sig) {
    static volatile std::sig_atomic_t running = 0;
    if (!running) {
        running = 1;
        const char *name = sig == SIGSEGV ? "SIGSEGV (a crash)" : sig == SIGBUS ? "SIGBUS (a crash)" : sig == SIGILL ? "SIGILL (a crash)" : sig == SIGFPE ? "SIGFPE (a crash)"
                           : sig == SIGABRT ? "SIGABRT (an abort)" : sig == SIGHUP ? "SIGHUP (the terminal was closed)" : "a signal";
        const char *head = "\n[crash] the program was ended by ";
        ssize_t ignored = write(2, head, std::strlen(head));
        ignored = write(2, name, std::strlen(name));
        ignored = write(2, "; its stack:\n", 14);
        (void)ignored;
#ifdef __GLIBC__
        void *frames[64];
        const int count = backtrace(frames, 64);
        backtrace_symbols_fd(frames, count, 2);
#endif
        const char what = sig == SIGHUP ? 'h' : 'c';
        if (g_card_wake[1] >= 0 && write(g_card_wake[1], &what, 1) == 1)
            for (int i = 0; i < 200 && !g_card_done.load(); ++i) {
                struct timespec pause = {0, 10 * 1000 * 1000};
                nanosleep(&pause, nullptr);
            }
#ifdef PS2_WINDOW
        g_log.flush(300);
#endif
    }
    std::signal(sig, SIG_DFL);
    std::raise(sig);
}

void install_signal_handlers() {
#ifdef __GLIBC__
    void *warm[2];
    backtrace(warm, 2);  // the first call loads the unwinder (and may allocate): not in the handler
#endif
    if (pipe(g_card_wake) == 0) std::thread(card_helper).detach();
    else g_card_wake[0] = g_card_wake[1] = -1;
    for (const int sig : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGHUP}) std::signal(sig, on_fatal_signal);
    std::signal(SIGPIPE, SIG_IGN);  // a log pipe whose reader went away makes a write fail instead of ending the run without a word
}
#endif

// A name for the calling thread that Windows tools show (Process Explorer, a debugger, a profiler), so the CPU time of the
// emulation thread can be told from the window thread. SetThreadDescription exists from Windows 10 1607 on,
// so it is looked up instead of linked. Elsewhere this does nothing.
[[maybe_unused]] void name_thread([[maybe_unused]] const wchar_t *name) {
#ifdef _WIN32
    using SetDescription = HRESULT(WINAPI *)(HANDLE, PCWSTR);
    if (const HMODULE kernel = GetModuleHandleW(L"kernel32.dll"))
        if (const auto set = reinterpret_cast<SetDescription>(reinterpret_cast<void *>(GetProcAddress(kernel, "SetThreadDescription")))) set(GetCurrentThread(), name);
#endif
}

}

// One line about the machine for the log, next to the [gpu] lines: a tester's log then says which processor (and how many cores) the speed
// was seen on. The name comes from CPUID on x86 (any system); the core count and the split into fast and small cores (an "efficiency
// class" of Windows) from Windows' own list; elsewhere only the number of logical processors is known.
static void log_machine() {
    std::string name = "unknown processor";
#if defined(__x86_64__) || defined(__i386__)
    unsigned int regs[12] = {};
    if (__get_cpuid_max(0x80000000u, nullptr) >= 0x80000004u) {
        for (unsigned int i = 0; i < 3; ++i) __cpuid(0x80000002u + i, regs[i * 4], regs[i * 4 + 1], regs[i * 4 + 2], regs[i * 4 + 3]);
        name.assign(reinterpret_cast<const char *>(regs), 48);
        name.resize(std::strlen(name.c_str()));
        const auto first = name.find_first_not_of(' ');
        name = first == std::string::npos ? std::string("unknown processor") : name.substr(first);
    }
#endif
    char detail[160] = "";
    const unsigned int logical = std::thread::hardware_concurrency();
#ifdef _WIN32
    DWORD bytes = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &bytes);
    std::vector<unsigned char> buffer(bytes);
    int cores = 0, best = -1, fast = 0;
    std::vector<int> classes;
    if (bytes && GetLogicalProcessorInformationEx(RelationProcessorCore, reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data()), &bytes)) {
        for (DWORD at = 0; at < bytes;) {
            const auto *info = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *>(buffer.data() + at);
            if (info->Size == 0) break;
            ++cores;
            classes.push_back(info->Processor.EfficiencyClass);
            best = std::max<int>(best, info->Processor.EfficiencyClass);
            at += info->Size;
        }
        for (const int c : classes) fast += c == best;
    }
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof memory;
    const double gigabytes = GlobalMemoryStatusEx(&memory) ? static_cast<double>(memory.ullTotalPhys) / (1024.0 * 1024.0 * 1024.0) : 0.0;
    if (cores > 0 && fast < cores) std::snprintf(detail, sizeof detail, "%d cores (%d fast, %d small), %u logical processors", cores, fast, cores - fast, logical);
    else if (cores > 0) std::snprintf(detail, sizeof detail, "%d cores, %u logical processors", cores, logical);
    else std::snprintf(detail, sizeof detail, "%u logical processors", logical);
    if (gigabytes > 0.0) std::snprintf(detail + std::strlen(detail), sizeof detail - std::strlen(detail), ", %.0f GB RAM", gigabytes);
#else
    std::snprintf(detail, sizeof detail, "%u logical processors", logical);
#endif
    std::fprintf(stderr, "[cpu] %s: %s\n", name.c_str(), detail);
}

// The window run's threads without a window (PS2_THREADS=1): this thread draws, one runs the vector units, one is the emulation.
// The emulation waits for the drawing as in a window (post_present lets it run two pictures ahead), nothing is shown.
// --frames-dir works too: the pictures are read back here, on the drawing side, at the same frames as in a run with one thread, under the
// same names (a picture that cannot be shown, such as one while the display is off, is skipped).
static int run_threads_headless(const std::function<int()> &emulate) {
    ps2::init_graphics();
    ps2::g_options.on_vblank = [] { ps2::post_present(); };
    const std::string pictures = ps2::g_options.frames_dir;
    ps2::g_options.frames_dir.clear();  // the emulation thread must not read the picture back itself
    ps2::g_options.sync_pictures = true;
    ps2::start_render_thread();
    if (!std::getenv("PS2_NO_VU_THREAD")) {
        ps2::start_vu_thread();
        std::thread(ps2::run_vu_thread).detach();
    }
    std::atomic<bool> done{false};
    int result = 0;
    ps2::host::BigStackThread emulation([&] {
        result = emulate();
        done.store(true);
        ps2::wake_render_thread();
    });
    const std::function<void(const std::uint32_t *, std::uint32_t, std::uint32_t)> none, save = [&](const std::uint32_t *rgba, std::uint32_t width, std::uint32_t height) {
        if (!ps2::frame_capture_requested(ps2::picture_frame())) return;
        char name[48];
        std::snprintf(name, sizeof name, "/frame_%05u.ppm", ps2::picture_frame());
        if (std::FILE *f = ps2::os::open(pictures + name, "wb")) {
            std::fprintf(f, "P6\n%u %u\n255\n", width, height);
            for (std::uint32_t i = 0; i < width * height; ++i) {
                const std::uint8_t px[3] = {static_cast<std::uint8_t>(rgba[i]), static_cast<std::uint8_t>(rgba[i] >> 8), static_cast<std::uint8_t>(rgba[i] >> 16)};
                std::fwrite(px, 1, 3, f);
            }
            std::fclose(f);
        }
    };
    while (!done.load())
        if (ps2::render_pending(false, pictures.empty() ? none : save, nullptr, 8.0) == ps2::Rendered::idle) ps2::wait_render_work(4);
    emulation.join();
    std::fflush(nullptr);
    std::_Exit(result);
}

int main(int argc, char **argv) {
#ifdef _WIN32
    // first: what can go wrong from here on is reported (crash::install), and this thread keeps stack for the report
    ps2::host::reserve_crash_stack();
    crash::install();
    // The runtime uses short SDL/condition-variable waits; request 1 ms timer granularity for Windows pacing.
    timeBeginPeriod(1);
    connect_output();
    // argv is in the ANSI code page and loses every character outside it: take the UTF-16 command line and use it as UTF-8
    std::vector<std::string> utf8_args;
    std::vector<char *> utf8_argv;
    int wide_argc = 0;
    if (wchar_t **wide = CommandLineToArgvW(GetCommandLineW(), &wide_argc)) {
        for (int i = 0; i < wide_argc; ++i) utf8_args.push_back(ps2::os::utf8(wide[i]));
        LocalFree(wide);
        for (std::string &a : utf8_args) utf8_argv.push_back(a.data());
        utf8_argv.push_back(nullptr);
        argc = wide_argc;
        argv = utf8_argv.data();
    }
#endif
    if (argc < 2) return usage();
    const bool from_disc = argv[1][0] == '-';
    std::string program = from_disc ? std::string() : argv[1];
    std::vector<std::uint8_t> disc_elf;
#ifdef PS2_WINDOW
    // The log copy starts before anything else that can fail (the arguments, the window, the translation set-up, the disc), so
    // whatever ends the run later leaves a log file behind: the app points a player at it ("Logs are not being created", public
    // issue 42). Only the arguments that say where it goes are read here; main reads them all again below.
    {
        // PS2_LOG_COPY=1 (a test aid): a headless run keeps the log copy too, so the log file can be tested without a window
        bool window_run = ps2::os::env("PS2_LOG_COPY") == "1";
        std::string early_disc, early_game_dir;
        for (int i = from_disc ? 1 : 2; i < argc; ++i) {
            const std::string a = argv[i];
            if (a == "--window" || a == "--fullscreen") window_run = true;
            else if (a == "--memcard" && i + 1 < argc) ps2::g_options.memcard_path = argv[++i];
            else if (a == "--quick-dir" && i + 1 < argc) ps2::g_options.quick_dir = argv[++i];
            else if (a == "--disc" && i + 1 < argc) early_disc = argv[++i];
            else if (a == "--game-dir" && i + 1 < argc) early_game_dir = argv[++i];
        }
        if (!from_disc) ps2::g_options.program_path = argv[1];
        // The log goes where the quick saves go (the memory card's folder, else the game folder), so the game folder must be known
        // by now, as it is later. An image-only start has no program path yet: its folder stands in for it, as below.
        ps2::g_options.game_dir = ps2::os::utf8(find_game_dir(from_disc ? early_disc : argv[1], early_disc, early_game_dir).lexically_normal());
        // PS2_NO_LOG_COPY=1 leaves the log alone (no memory copy, no file): the way back if the pipe is ever in the way
        if (window_run && ps2::os::env("PS2_NO_LOG_COPY").empty() && g_log.start(ps2::os::path(ps2::quick_folder("logs")))) {
            ps2::g_options.recent_log = [](std::size_t lines) { return g_log.tail(lines); };
            ps2::g_options.first_log = [](std::size_t lines) { return g_log.head(lines); };
            ps2::g_options.on_exit = [] { g_log.stop(); };
            std::atexit([] { g_log.stop(); });  // the window run that returns from main
        }
    }
#endif
    crash_test("main");
#ifdef _WIN32
    SetConsoleCtrlHandler(on_console_event, TRUE);
#else
    install_signal_handlers();
#endif
#ifdef _WIN32
    std::atexit([] { timeEndPeriod(1); ps2::flush_memory_card(); });
#else
    std::atexit([] { ps2::flush_memory_card(); });
#endif
    std::vector<std::string> args{argv[1]};
    [[maybe_unused]] bool window = false, fullscreen = false;
    std::string load_path, given_game_dir;
    bool no_record = false;
    ps2::g_options.disc_program = from_disc;
    for (int i = 0; i < argc; ++i) {
        const std::string a = argv[i];
        ps2::g_options.command_line += (i ? " " : "") + (a.find_first_of(" \t") == std::string::npos ? a : "\"" + a + "\"");
    }
    for (int i = from_disc ? 1 : 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--arg" && i + 1 < argc) args.push_back(argv[++i]);
        else if (a == "--frames" && i + 1 < argc) ps2::g_options.max_frames = std::strtoull(argv[++i], nullptr, 10);
        else if (a == "--ntsc" || a == "--pal") {
            ps2::g_options.pal = a == "--pal";
            ps2::g_options.rate_forced = true;
        }
        else if (a == "--misses" && i + 1 < argc) ps2::g_options.misses_path = argv[++i];
        else if (a == "--trace-syscalls") ps2::g_options.trace_syscalls = true;
        else if (a == "--widescreen") ps2::g_options.widescreen = true;
        else if (a == "--patches" && i + 1 < argc) {
            if (!ps2::load_patches(argv[++i])) return usage();
        }
        else if (a == "--trace-io") ps2::g_options.trace_io = true;
        else if (a == "--trace-threads") ps2::g_options.trace_threads = true;
        else if (a == "--disc" && i + 1 < argc) ps2::g_options.disc_path = argv[++i];
        else if (a == "--game-dir" && i + 1 < argc) given_game_dir = argv[++i];
        else if (a == "--memcard" && i + 1 < argc) ps2::g_options.memcard_path = argv[++i];
        else if (a == "--trace-iop") ps2::g_options.trace_iop_modules = true;
        else if (a == "--trace-iop-hle") ps2::g_options.trace_iop_hle = true;
        else if (a == "--frames-dir" && i + 1 < argc) ps2::g_options.frames_dir = argv[++i];
        else if (a == "--frame-at" && i + 1 < argc) ps2::g_options.frame_capture_frames.push_back(std::strtoull(argv[++i], nullptr, 10));
        else if (a == "--press" && i + 1 < argc) {
            ps2::Press press;
            if (!ps2::parse_press(argv[++i], press)) return usage();
            ps2::g_options.presses.push_back(press);
        }
        else if (a == "--axis" && i + 1 < argc) {
            ps2::AxisPress press;
            if (!ps2::parse_axis(argv[++i], press)) return usage();
            ps2::g_options.axis_presses.push_back(press);
        }
#ifdef PS2_WINDOW
        else if (a == "--window") window = true;
        else if (a == "--fullscreen") window = fullscreen = true;
#endif
        else if (a == "--frame-every" && i + 1 < argc) ps2::g_options.frame_every = static_cast<std::uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        else if (a == "--save-state" && i + 1 < argc) {
            const std::string spec = argv[++i];
            const auto colon = spec.find(':');
            if (colon == std::string::npos) return usage();
            ps2::g_options.save_state_frame = std::strtoull(spec.substr(0, colon).c_str(), nullptr, 10);
            ps2::g_options.save_state_path = spec.substr(colon + 1);
        }
        else if (a == "--load-state" && i + 1 < argc) load_path = argv[++i];
        else if (a == "--record" && i + 1 < argc) ps2::g_options.record_path = argv[++i];
        else if (a == "--no-record") no_record = true;
        else if (a == "--replay" && i + 1 < argc) ps2::g_options.replay_path = argv[++i];
        else if (a == "--replay-continue") ps2::g_options.replay_continue = true;
        else if (a == "--rtc" && i + 1 < argc) ps2::g_options.rtc_epoch = std::strtoull(argv[++i], nullptr, 10);
        else if (a == "--quick-dir" && i + 1 < argc) ps2::g_options.quick_dir = argv[++i];
        else return usage();
    }
    std::sort(ps2::g_options.frame_capture_frames.begin(), ps2::g_options.frame_capture_frames.end());
    ps2::g_options.frame_capture_frames.erase(
        std::unique(ps2::g_options.frame_capture_frames.begin(), ps2::g_options.frame_capture_frames.end()),
        ps2::g_options.frame_capture_frames.end());

    std::string program_location = program;
    if (from_disc) {
        if (ps2::g_options.disc_path.empty()) return usage();
        if (!ps2::disc::open(ps2::g_options.disc_path)) {
            std::fprintf(stderr, "[elf] cannot open disc image %s\n", ps2::g_options.disc_path.c_str());
            return 1;
        }
        std::string why;
        if (!ps2::disc::read_boot_elf(program, disc_elf, why)) {
            std::fprintf(stderr, "[elf] %s\n", why.c_str());
            return 1;
        }
        args[0] = program;
        // A location anchor for caches/reports; this path is never opened for disc boot.
        std::string name = program;
        std::replace(name.begin(), name.end(), '\\', '/');
        name = name.substr(name.find_last_of("/:") + 1);
        name = name.substr(0, name.find(';'));
        program_location = ps2::os::utf8(ps2::os::path(ps2::g_options.disc_path).parent_path() / ps2::os::path(name));
        std::fprintf(stderr, "[elf] read %s from disc (%zu bytes)\n", program.c_str(), disc_elf.size());
    }
    ps2::g_options.program_path = program_location;

    // The image can be on another drive, and a portable runner need not be in the build tree. --game-dir names the folder
    // port.sh made independently of both paths. Otherwise keep looking around the ELF, then beside the image, as before.
    std::error_code error;
    const std::filesystem::path program_file = std::filesystem::absolute(ps2::os::path(program_location), error);
    std::filesystem::path game_dir;
    if (!given_game_dir.empty()) {
        game_dir = std::filesystem::absolute(ps2::os::path(given_game_dir), error);
        if (error || !std::filesystem::is_directory(game_dir, error)) {
            std::fprintf(stderr, "[game] not a directory: %s\n", given_game_dir.c_str());
            return 1;
        }
    } else {
        game_dir = find_game_dir(program_location, ps2::g_options.disc_path, {});
    }
    game_dir = game_dir.lexically_normal();
    ps2::g_options.game_dir = ps2::os::utf8(game_dir);
    const std::filesystem::path data_dir = game_dir.empty() ? program_file.parent_path() : game_dir;

#ifdef PS2_WINDOW
    // Playing records the vector programs that are not compiled into this build into the vu1 and vu0 folders of the game that
    // port.sh made, games/<name>/{vu1,vu0}, so `port.sh <name> <image> --no-explore` can compile them afterwards (docs/gow2.md,
    // docs/vu0-native.md). Without that, a game is slow wherever the exploring runs did not go: unknown programs run in the
    // interpreter. A PS2_VU_RECORD or PS2_VU0_RECORD the user set wins, each on its own. This runs before anything reads the
    // variables (the vector unit code reads them on its first program, long after main).
    if (window) {
        for (int unit : {1, 0}) {
            const char *setting = unit ? "PS2_VU_RECORD" : "PS2_VU0_RECORD";
            if (!ps2::os::env(setting).empty()) continue;
            const std::filesystem::path directory = (data_dir / (unit ? "vu1" : "vu0")).lexically_normal();
            if (std::filesystem::is_directory(directory, error) && ps2::os::set_env(setting, ps2::os::utf8(directory)))
                std::fprintf(stderr, "[vu%d] recording vector programs that are not compiled yet into %s\n", unit, ps2::os::env(setting).c_str());
        }
    }
    if (window && !open_window(fullscreen)) return 1;
    if (window) name_thread(L"ps2 window");
#endif
    // Vector programs the build does not know are compiled in the background while the game runs (docs/vu-live.md). On in a window,
    // off in headless runs unless PS2_VU_LIVE asks: PS2_VU_LIVE=1 (or a folder for the compiled libraries), PS2_VU_LIVE=0 switches it off.
    // The libraries go next to the game: the game folder that port.sh made (the one with the vu1 folder), else the program's own folder.
    {
        const std::string setting = ps2::os::env("PS2_VU_LIVE");
        if (setting != "0" && setting != "off" && (window || !setting.empty())) {
            std::string cache = setting == "1" || setting == "on" ? std::string() : setting;
            if (cache.empty()) cache = ps2::os::utf8(data_dir / "vu1-live");
            ps2::vu::live::start(cache);
        }
    }
    register_generated_code();
    register_generated_vu();
    // Main processor code the build does not know (code a game copies into a window while it runs) is translated and compiled on the
    // spot, the game waiting (docs/ee-live.md). On in a window, off in headless runs unless PS2_EE_LIVE asks (a headless run is how
    // port.sh explores, and it wants the stop): PS2_EE_LIVE=1 (or a folder for the compiled libraries), PS2_EE_LIVE=0 switches it off.
    // The libraries go next to the game like the vector programs'; entries.txt and overlays\ of the game folder get what was found.
    {
        const std::string setting = ps2::os::env("PS2_EE_LIVE");
        if (setting != "0" && setting != "off" && (window || !setting.empty())) {
            const std::string cache = setting.empty() || setting == "1" || setting == "on" ? ps2::os::utf8(data_dir / "ee-live") : setting;
            if (from_disc) ps2::ee::live::start(disc_elf, cache, ps2::os::utf8(game_dir));
            else ps2::ee::live::start(program_location, cache, ps2::os::utf8(game_dir));
        }
    }
    const auto emulate = [&]() -> int {
        name_thread(L"ps2 emulation");
        crash_test("emu");
        std::uint32_t entry = 0;
        const bool loaded = from_disc ? ps2::load_elf(disc_elf, entry) : ps2::load_elf(program, entry);
        if (!loaded) {
            std::fprintf(stderr, "cannot load %s\n", program.c_str());
            return 1;
        }
        // Segments are in EE RAM and live analysis owns its ELF bytes; release the host boot buffer.
        std::vector<std::uint8_t>().swap(disc_elf);
        ps2::boot(entry, args);
        if (load_path == "newest") {  // the newest quick save (F5) of this game
            load_path = ps2::quick_newest_state();
            if (load_path.empty()) std::fprintf(stderr, "[state] there is no quick save yet: starting from the beginning\n");
        }
        if (!load_path.empty() && !ps2::load_state(load_path)) return 1;
        ps2::g_options.load_state_path = load_path;
        // A window run records the pad (docs/replay.md) unless PS2_RECORD=0 or --no-record; a replay never records.
        const bool replaying = !ps2::g_options.replay_path.empty();
        ps2::g_options.auto_record = window && !no_record && !replaying && ps2::os::env("PS2_RECORD") != "0";
        if (replaying) {
            if (!ps2::replay_begin()) return 5;
        } else {
            ps2::record_begin_run();
        }
#ifdef PS2_PROFILE
        const char *profile = std::getenv("PS2_PROFILE");
        const char *profile_from = std::getenv("PS2_PROFILE_FROM");
        if (profile) start_profile(profile_from ? std::strtoull(profile_from, nullptr, 10) : 0);
        const int result = ps2::run();
        if (profile) write_profile(profile);
        ps2::flush_memory_card();
        if (ps2::replay_active()) return ps2::replay_status();
        return result;
#else
        // run() ends however the game ends: the window was closed, --frames was reached, the program quit or stopped (halt)
        const int result = ps2::run();
        ps2::flush_memory_card();  // a save in the last 50 frames is not written by the usual timer yet
        if (ps2::replay_active()) return ps2::replay_status();  // 0 same to the end, 3 another way, 4 the game ended early, 5 wrong start
        return result;
#endif
    };
#ifdef PS2_WINDOW
    if (window) {
        log_machine();
        return run_window(emulate);
    }
#endif
    // PS2_THREADS=1: a headless run with the three threads of a window run (emulation, vector units, drawing) so that replays and
    // tests can cover the path a player's window takes (docs/sh2.md).
    if (ps2::os::env("PS2_THREADS") == "1") {
        log_machine();
        return run_threads_headless(emulate);
    }
    return emulate();
}
