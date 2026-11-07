#include "live_build.hpp"

#ifndef __EMSCRIPTEN__

#include "ps2/os.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <thread>

#ifndef _WIN32
#include <dlfcn.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
extern char **environ;
#endif

#ifndef PS2_LIVE_DEFAULT_CXX
#define PS2_LIVE_DEFAULT_CXX ""
#endif

namespace ps2::live {

double seconds_since(Clock::time_point t) { return std::chrono::duration<double>(Clock::now() - t).count(); }

std::uint64_t fnv(std::uint64_t h, const void *data, std::size_t size) {
    const auto *p = static_cast<const unsigned char *>(data);
    for (std::size_t i = 0; i < size; ++i) h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}

std::string hex(std::uint64_t v, int digits) {
    char text[32];
    std::snprintf(text, sizeof text, "%0*llx", digits, static_cast<unsigned long long>(v));
    return text;
}

// ---------------------------------------------------------------------------------------------------------------- processes

namespace {

#ifdef _WIN32
std::wstring quote_argument(const std::wstring &a) {
    if (!a.empty() && a.find_first_of(L" \t\n\v\"") == std::wstring::npos) return a;
    std::wstring r = L"\"";
    for (std::size_t i = 0;; ++i) {
        std::size_t backslashes = 0;
        while (i < a.size() && a[i] == L'\\') {
            ++i;
            ++backslashes;
        }
        if (i == a.size()) {
            r.append(backslashes * 2, L'\\');
            break;
        }
        if (a[i] == L'"') {
            r.append(backslashes * 2 + 1, L'\\');
            r.push_back(L'"');
        } else {
            r.append(backslashes, L'\\');
            r.push_back(a[i]);
        }
    }
    r.push_back(L'"');
    return r;
}

// Every compiler process belongs to this job, and the job kills its processes when the runner ends (closed, crashed or killed).
HANDLE compiler_job() {
    static HANDLE job = [] {
        HANDLE h = CreateJobObjectW(nullptr, nullptr);
        if (h) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
            info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            SetInformationJobObject(h, JobObjectExtendedLimitInformation, &info, sizeof info);
        }
        return h;
    }();
    return job;
}
#endif

// Starts a program with its output (both streams) appended to log. Runs below normal priority, the game comes first, unless the
// machine is so busy that this left the compiler no time at all (normal_priority) or the caller asks for normal priority.
bool spawn(const std::vector<std::string> &args, const fs::path &log, Process &out, bool below_normal) {
    const bool low = below_normal && !toolchain().normal_priority.load();
#ifdef _WIN32
    std::wstring line;
    for (const std::string &a : args) {
        if (!line.empty()) line += L' ';
        line += quote_argument(ps2::os::path(a).wstring());
    }
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE out_file = CreateFileW(log.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE in_file = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out_file == INVALID_HANDLE_VALUE || in_file == INVALID_HANDLE_VALUE) {
        if (out_file != INVALID_HANDLE_VALUE) CloseHandle(out_file);
        if (in_file != INVALID_HANDLE_VALUE) CloseHandle(in_file);
        return false;
    }
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = in_file;
    si.hStdOutput = out_file;
    si.hStdError = out_file;
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | (low ? BELOW_NORMAL_PRIORITY_CLASS : NORMAL_PRIORITY_CLASS), nullptr, nullptr, &si, &pi);
    CloseHandle(out_file);
    CloseHandle(in_file);
    if (!ok) return false;
    if (HANDLE job = compiler_job()) AssignProcessToJobObject(job, pi.hProcess);
    CloseHandle(pi.hThread);
    out.handle = pi.hProcess;
    return true;
#else
    std::vector<std::string> storage = args;
    std::vector<char *> argv;
    for (std::string &a : storage) argv.push_back(a.data());
    argv.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    const std::string log_path = ps2::os::utf8(log);
    posix_spawn_file_actions_addopen(&actions, 1, log_path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    posix_spawn_file_actions_adddup2(&actions, 1, 2);
    posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
    pid_t pid = -1;
    const int status = posix_spawn(&pid, argv[0], &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    if (status != 0) return false;
    if (low) setpriority(PRIO_PROCESS, static_cast<id_t>(pid), 10);
    out.pid = pid;
    return true;
#endif
}

// The exit code, or -1 when the process was killed or could not be waited for. tick() is called about every quarter of a second
// while it runs (the caller watches for a compiler that gets no processor time).
template <class Tick>
int wait_for(Process &p, Tick tick) {
#ifdef _WIN32
    while (WaitForSingleObject(p.handle, 250) == WAIT_TIMEOUT) tick();
    DWORD code = 1;
    if (!GetExitCodeProcess(p.handle, &code)) code = 1;
    return static_cast<int>(code);
#else
    int status = 0;
    for (;;) {
        const pid_t done = waitpid(p.pid, &status, WNOHANG);
        if (done == p.pid) break;
        if (done < 0 && errno != EINTR) return -1;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        tick();
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

// Gives a running process normal priority. On Linux and macOS lowering a nice value again needs the superuser or a nice limit
// (RLIMIT_NICE) that allows it: many PCs have 0, so the call is refused and does nothing (checked on Linux, docs/linux.md); the
// processes started later are not made nice either way (normal_priority).
void raise_priority(Process &p) {
#ifdef _WIN32
    if (p.handle) SetPriorityClass(p.handle, NORMAL_PRIORITY_CLASS);
#else
    if (p.pid > 0) setpriority(PRIO_PROCESS, static_cast<id_t>(p.pid), 0);
#endif
}

void release(Process &p) {
#ifdef _WIN32
    if (p.handle) CloseHandle(p.handle);
    p.handle = nullptr;
#else
    p.pid = -1;
#endif
}

void kill_process(Process &p) {
#ifdef _WIN32
    if (p.handle) TerminateProcess(p.handle, 1);
#else
    if (p.pid > 0) kill(p.pid, SIGTERM);
#endif
}

bool same(const Process &a, const Process &b) {
#ifdef _WIN32
    return a.handle == b.handle;
#else
    return a.pid == b.pid;
#endif
}

// The compiler runs below normal priority so that the game comes first. On a machine whose processors are all busy (the game, a
// browser, other programs) that can mean no processor time at all: nothing is ever compiled and the interpreter stays. A step that
// takes this long, where a few seconds are normal, switches the compiler to normal priority for good (a few seconds of work now and
// then; the game itself stays faster with compiled programs than without).
void watch_priority(Clock::time_point since, const Job &job) {
    Toolchain &tc = toolchain();
    if (job.urgent && job.urgent->load()) {
        // The game waits for this job (a background translation that the game now needs, ee_live.cpp): the compilers it has running go
        // to normal priority now, the ones it starts later start there. Only this job is meant, and the toolchain's setting stays.
        if (job.below_normal && !tc.normal_priority.load()) {
            std::lock_guard lock(tc.mutex);
            for (Process &p : tc.alive) raise_priority(p);
        }
        return;
    }
    if (!job.starve || !job.below_normal || tc.normal_priority.load() || tc.starve_s <= 0 || seconds_since(since) < tc.starve_s) return;
    tc.normal_priority.store(true);
    {
        std::lock_guard lock(tc.mutex);
        for (Process &p : tc.alive) raise_priority(p);
    }
    if (job.say)
        job.say("a compile step has taken " + std::to_string(static_cast<int>(seconds_since(since) + 0.5)) +
                " s at below normal priority (the processors are busy): the compiler runs at normal priority from now on");
}

std::string log_tail(const fs::path &log, std::size_t lines) {
    std::ifstream in(log, std::ios::binary);
    std::vector<std::string> all;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) all.push_back(line);
    }
    std::string text;
    for (std::size_t i = all.size() > lines ? all.size() - lines : 0; i < all.size(); ++i) text += "\n    " + all[i].substr(0, 200);
    return text;
}

// A name that is a program on the PATH.
fs::path on_path(const std::string &name) {
    const std::string path = ps2::os::env("PATH");
#ifdef _WIN32
    const char separator = ';';
#else
    const char separator = ':';
#endif
    std::size_t from = 0;
    while (from <= path.size()) {
        std::size_t to = path.find(separator, from);
        if (to == std::string::npos) to = path.size();
        if (to > from) {
            const fs::path dir = ps2::os::path(path.substr(from, to - from));
#ifdef _WIN32
            if (is_file(dir / (name + ".exe"))) return dir / (name + ".exe");
#endif
            if (is_file(dir / name)) return dir / name;
        }
        from = to + 1;
    }
    return {};
}

void stop_at_exit() { stop_all(); }

}  // namespace

Toolchain &toolchain() {
    static Toolchain *tc = [] {
        auto *t = new Toolchain;  // never freed: a compiler process may still be running while the process ends
        std::atexit(stop_at_exit);
        return t;
    }();
    return *tc;
}

void stop_all() {
    Toolchain &tc = toolchain();
    std::lock_guard lock(tc.mutex);
    tc.stopping = true;
    for (Process &p : tc.alive) kill_process(p);
}

void use_environment() {
    static const bool once = [] {
        Toolchain &tc = toolchain();
        // PS2_VU_LIVE_PRIORITY=low never raises the compiler's priority, =normal starts at normal; PS2_VU_LIVE_STARVE_S=<seconds> is the wait before it does.
        if (const std::string v = ps2::os::env("PS2_VU_LIVE_PRIORITY"); v == "normal") tc.normal_priority.store(true);
        else if (v == "low") tc.starve_s = 0;
        if (const std::string v = ps2::os::env("PS2_VU_LIVE_STARVE_S"); !v.empty()) tc.starve_s = std::atof(v.c_str());
        return true;
    }();
    (void)once;
}

fs::path exe_directory() {
#ifdef _WIN32
    wchar_t buffer[4096];
    const DWORD n = GetModuleFileNameW(nullptr, buffer, 4096);
    if (n > 0 && n < 4096) return fs::path(buffer).parent_path();
#elif defined(__APPLE__)
    char buffer[4096];
    std::uint32_t size = sizeof buffer;
    if (_NSGetExecutablePath(buffer, &size) == 0) return fs::path(buffer).parent_path();
#else
    std::error_code error;
    const fs::path p = fs::read_symlink("/proc/self/exe", error);
    if (!error) return p.parent_path();
#endif
    return {};
}

bool is_file(const fs::path &p) {
    std::error_code error;
    return fs::is_regular_file(p, error);
}

fs::path find_compiler() {
    const auto usable = [](const std::string &name) -> fs::path {
        if (name.empty()) return {};
        fs::path p = ps2::os::path(name);
        if (p.has_parent_path()) {
            if (is_file(p)) return p;
#ifdef _WIN32
            if (is_file(p.string() + ".exe")) return fs::path(p.string() + ".exe");
#endif
            return {};
        }
        return on_path(name);
    };
    if (const fs::path p = usable(ps2::os::env("PS2_VU_CXX")); !p.empty()) return p;
    const fs::path exe = exe_directory();
    if (!exe.empty()) {
        for (const char *relative : {"compiler/bin/clang++", "compiler/bin/c++", "compiler/clang++", "../compiler/bin/clang++", "../compiler/bin/c++"}) {
            fs::path p = exe / relative;
#ifdef _WIN32
            p += ".exe";
#endif
            if (is_file(p)) return p.lexically_normal();
        }
    }
    if (const fs::path p = usable(PS2_LIVE_DEFAULT_CXX); !p.empty()) return p;
    if (const fs::path p = usable(ps2::os::env("CXX")); !p.empty()) return p;
    for (const char *name : {"clang++", "g++", "c++"})
        if (const fs::path p = on_path(name); !p.empty()) return p;
    return {};
}

// ---------------------------------------------------------------------------------------------------------------- libraries

#ifdef _WIN32
const char *const kLibraryExtension = ".dll";
void *open_library(const fs::path &path) { return reinterpret_cast<void *>(LoadLibraryW(path.c_str())); }
void *find_symbol(void *library, const char *name) { return reinterpret_cast<void *>(GetProcAddress(reinterpret_cast<HMODULE>(library), name)); }
#elif defined(__APPLE__)
const char *const kLibraryExtension = ".dylib";
void *open_library(const fs::path &path) { return dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL); }
void *find_symbol(void *library, const char *name) { return dlsym(library, name); }
#else
const char *const kLibraryExtension = ".so";
void *open_library(const fs::path &path) { return dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL); }
void *find_symbol(void *library, const char *name) { return dlsym(library, name); }
#endif

std::vector<std::string> compile_flags(const std::string &optimisation) {
    std::vector<std::string> f = {"-std=c++20", optimisation, "-DNDEBUG", "-ffp-contract=off", "-w"};
#if defined(__AVX2__)
    f.push_back("-mavx2");
#endif
#if defined(__FMA__)
    f.push_back("-mfma");
#endif
#ifndef _WIN32
    f.push_back("-fPIC");
    f.push_back("-fvisibility=hidden");
#endif
    return f;
}

bool write_headers(const fs::path &include_dir, const std::vector<HeaderFile> &files) {
    std::error_code error;
    fs::create_directories(include_dir / "ps2", error);
    for (const HeaderFile &f : files) {
        const fs::path path = include_dir / "ps2" / f.name;
        const std::size_t size = std::strlen(f.text);
        bool same = false;
        if (std::ifstream in(path, std::ios::binary); in) {
            std::string old((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            same = old.size() == size && std::memcmp(old.data(), f.text, size) == 0;
        }
        if (!same) {
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            out.write(f.text, static_cast<std::streamsize>(size));
            if (!out) return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------------------------------------------- building

bool compile_and_link(const Job &job, Timing &timing) {
    Toolchain &tc = toolchain();
    const fs::path log = job.work / "log.txt";
    const std::string compiler = ps2::os::utf8(job.compiler);
    const std::string include = "-I" + ps2::os::utf8(job.include_dir);
    const auto say = [&](const std::string &text) {
        if (job.say) job.say(text);
    };
    std::vector<fs::path> objects;
    std::vector<std::pair<Process, std::size_t>> running;
    std::size_t next = 0;
    bool ok = true;
    auto t = Clock::now();
    const auto reap = [&] {
        auto [process, index] = running.front();
        running.erase(running.begin());
        const int code = wait_for(process, [&] {
            watch_priority(t, job);
            if (job.cancel && job.cancel->load()) kill_process(process);
        });
        {
            std::lock_guard lock(tc.mutex);
            for (std::size_t i = 0; i < tc.alive.size(); ++i)
                if (same(tc.alive[i], process)) {
                    tc.alive.erase(tc.alive.begin() + static_cast<std::ptrdiff_t>(i));
                    break;
                }
        }
#ifdef _WIN32
        if (FILETIME created, exited, kernel, user; process.handle && GetProcessTimes(process.handle, &created, &exited, &kernel, &user)) {
            const auto ticks = [](const FILETIME &f) { return static_cast<double>((static_cast<std::uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime) * 1e-7; };
            timing.cpu += ticks(kernel) + ticks(user);
            timing.longest = std::max(timing.longest, ticks(exited) - ticks(created));
        }
#endif
        release(process);
        if (code != 0) {
            ok = false;
            if (job.cancel && job.cancel->load()) return;
            say("the compiler stopped with code " + std::to_string(code) + " on " + ps2::os::utf8(job.sources[index].filename()) + ":" + log_tail(log, 6));
        }
    };
    while ((next < job.sources.size() && ok) || !running.empty()) {
        if (job.cancel && job.cancel->load()) ok = false;
        while (ok && next < job.sources.size() && running.size() < ((job.urgent && job.urgent->load()) ? std::max(job.jobs, job.jobs_urgent) : job.jobs) &&
               !(job.hold && job.hold->load() > 0 && !(job.urgent && job.urgent->load()))) {
            std::vector<std::string> args = {compiler};
            args.insert(args.end(), job.flags.begin(), job.flags.end());
            args.push_back(include);
            args.push_back("-c");
            args.push_back(ps2::os::utf8(job.sources[next]));
            fs::path object = job.sources[next];
            object.replace_extension(".o");
            args.push_back("-o");
            args.push_back(ps2::os::utf8(object));
            Process process;
            if (!spawn(args, log, process, job.below_normal && !(job.urgent && job.urgent->load()))) {
                say("cannot start " + compiler);
                ok = false;
                break;
            }
            {
                std::lock_guard lock(tc.mutex);
                tc.alive.push_back(process);
                if (tc.stopping) kill_process(process);
            }
            objects.push_back(object);
            running.emplace_back(process, next);
            ++next;
        }
        if (!running.empty()) reap();
        else if (ok && next < job.sources.size()) std::this_thread::sleep_for(std::chrono::milliseconds(20));   // held back: the game waits for another build
    }
    timing.compile = seconds_since(t);
    if (!ok) return false;

    t = Clock::now();
    {
        std::vector<std::string> args = {compiler, "-shared"};
        for (const std::string &f : job.flags)
            if (f.rfind("-O", 0) != 0 && f.rfind("-std", 0) != 0) args.push_back(f);
#ifdef _WIN32
        args.push_back("-static-libgcc");
        args.push_back("-static-libstdc++");
#endif
        // Windows refuses a command line over 32767 characters (CreateProcess fails with "filename or extension too long", the log of
        // the game then says "cannot start <compiler>"). A big library has 200 objects or more, each named by its full path, so a game
        // folder in a long path is enough to cross the limit (Final Fantasy X, 213 objects: 26 600 characters in a path of 71, over the
        // limit at about 100). Past 12 000 characters the objects go into a response file, `@file`, which the compiler reads for itself.
        // Quotes and forward slashes: both ways a compiler can split such a file (Windows or GNU rules) read that the same.
        std::size_t object_chars = 0;
        for (const fs::path &o : objects) object_chars += ps2::os::utf8(o).size() + 3;
        if (object_chars > 12000) {
            const fs::path list = job.work / "link.rsp";
            std::ofstream out(list, std::ios::binary | std::ios::trunc);
            for (const fs::path &o : objects) {
                std::string name = ps2::os::utf8(o);
                std::replace(name.begin(), name.end(), '\\', '/');
                out << '"' << name << "\"\n";
            }
            out.close();
            if (!out) {
                say("cannot write " + ps2::os::utf8(list));
                return false;
            }
            args.push_back("@" + ps2::os::utf8(list));
        } else {
            for (const fs::path &o : objects) args.push_back(ps2::os::utf8(o));
        }
        args.push_back("-o");
        args.push_back(ps2::os::utf8(job.library));
        Process process;
        if (!spawn(args, log, process, job.below_normal && !(job.urgent && job.urgent->load()))) {
            say("cannot start " + compiler);
            return false;
        }
        {
            std::lock_guard lock(tc.mutex);
            tc.alive.push_back(process);
        }
        const int code = wait_for(process, [&] { watch_priority(t, job); });
        {
            std::lock_guard lock(tc.mutex);
            for (std::size_t i = 0; i < tc.alive.size(); ++i)
                if (same(tc.alive[i], process)) {
                    tc.alive.erase(tc.alive.begin() + static_cast<std::ptrdiff_t>(i));
                    break;
                }
        }
        release(process);
        if (code != 0) {
            say("the linker stopped with code " + std::to_string(code) + ":" + log_tail(log, 6));
            return false;
        }
    }
    timing.link = seconds_since(t);
    return true;
}

}  // namespace ps2::live

#endif
