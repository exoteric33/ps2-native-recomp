// Sampling profiler for the emulation thread on Windows: the same PS2_PROFILE=<file> and PS2_PROFILE_FROM=<frame>
// switches as host/profile.cpp on Linux, but the file is a readable list of the busiest functions (DbgHelp reads the
// symbol table the linker left in ps2run.exe), so no other tool is needed.
//
// A helper thread stops the emulation thread about every millisecond, reads where it is (the instruction pointer and
// the word on top of the stack, which is the caller when the thread sits at the start of a library function) and lets
// it go on. Only the thread that called start_profile is sampled; the GPU and VU threads are not.

#include "profile.hpp"

#include "ps2/system.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <timeapi.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cxxabi.h>
#include <functional>
#include <iterator>
#include <map>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

struct Sample {
    std::uintptr_t pc, caller, caller2, caller3;
};

constexpr std::size_t kMaxSamples = 1'000'000;  // 16 MB; about 15 minutes of sampling, later samples are dropped
HANDLE g_target = nullptr;
std::atomic<bool> g_running{false};
std::thread g_thread;
std::vector<Sample> g_samples;
std::atomic<std::size_t> g_count{0};
std::uint64_t g_from = 0;
bool g_armed = false;
DWORD64 g_image_begin = 0, g_image_end = 0;  // ps2run.exe in memory

void sampler() {
    timeBeginPeriod(1);
    while (g_running.load(std::memory_order_relaxed)) {
        Sample s{};
        bool got = false;
        // nothing that can take a lock (malloc, stdio) may run between Suspend and Resume: the target may hold it
        if (SuspendThread(g_target) != static_cast<DWORD>(-1)) {
            CONTEXT c{};
            c.ContextFlags = CONTEXT_FULL;
            if (GetThreadContext(g_target, &c)) {
                s.pc = static_cast<std::uintptr_t>(c.Rip);
                // Outside ps2run.exe (a system library: a wait, a copy, a clock): walk the stack with the libraries' unwind tables up to
                // the first return address inside ps2run.exe, so the sample can be charged to the function of ours that called it.
                // Only reads memory; the stack cannot change while the thread is stopped.
                // The two callers above that (caller2, caller3) say who called a library helper of ours (std::filesystem, operator new).
                if (s.pc < g_image_begin || s.pc >= g_image_end) {
                    CONTEXT u = c;
                    const auto up = [&u]() {
                        DWORD64 base = 0;
                        PRUNTIME_FUNCTION entry = RtlLookupFunctionEntry(u.Rip, &base, nullptr);
                        if (entry) {
                            void *handler_data = nullptr;
                            DWORD64 frame = 0;
                            RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, u.Rip, entry, &u, &handler_data, &frame, nullptr);
                        } else {  // a leaf function: the return address is on top of the stack
                            DWORD64 top = 0;
                            SIZE_T read = 0;
                            if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(u.Rsp), &top, sizeof top, &read) || read != sizeof top) return false;
                            u.Rip = top;
                            u.Rsp += 8;
                        }
                        return u.Rip != 0;
                    };
                    for (int depth = 0; depth < 16 && up(); ++depth)
                        if (u.Rip >= g_image_begin && u.Rip < g_image_end) {
                            s.caller = static_cast<std::uintptr_t>(u.Rip);
                            if (up() && u.Rip >= g_image_begin && u.Rip < g_image_end) {
                                s.caller2 = static_cast<std::uintptr_t>(u.Rip);
                                if (up() && u.Rip >= g_image_begin && u.Rip < g_image_end) s.caller3 = static_cast<std::uintptr_t>(u.Rip);
                            }
                            break;
                        }
                }
                got = true;
            }
            ResumeThread(g_target);
        }
        const std::size_t n = g_count.load(std::memory_order_relaxed);
        if (got && n < g_samples.size()) {
            g_samples[n] = s;
            g_count.store(n + 1, std::memory_order_relaxed);
        }
        Sleep(1);
    }
    timeEndPeriod(1);
}

void arm() {
    if (g_armed) return;
    g_armed = true;
    // a real handle of the calling (emulation) thread: GetCurrentThread() is only a placeholder
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_target,
                         THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0)) {
        std::fprintf(stderr, "[profile] cannot open the emulation thread (error %lu)\n", GetLastError());
        return;
    }
    const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(GetModuleHandleA(nullptr));
    const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(reinterpret_cast<const char *>(dos) + dos->e_lfanew);
    g_image_begin = reinterpret_cast<DWORD64>(dos);
    g_image_end = g_image_begin + nt->OptionalHeader.SizeOfImage;
    g_samples.assign(kMaxSamples, Sample{});
    g_running.store(true);
    g_thread = std::thread(sampler);
}

std::string demangle(const char *name) {
    int status = 0;
    char *text = abi::__cxa_demangle(name, nullptr, nullptr, &status);
    std::string out = status == 0 && text ? text : name;
    std::free(text);
    return out;
}

struct Symbol {
    std::uint32_t rva;
    std::string name;
};

// The function symbols the linker left in ps2run.exe (COFF symbol table), sorted by address. DbgHelp does not read them.
std::vector<Symbol> load_symbols() {
    std::vector<Symbol> symbols;
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::FILE *f = _wfopen(path, L"rb");
    if (!f) return symbols;
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<unsigned char> file(static_cast<std::size_t>(size));
    const bool ok = size > 0 && std::fread(file.data(), 1, file.size(), f) == file.size();
    std::fclose(f);
    if (!ok) return symbols;
    const auto u32 = [&](std::size_t at) { std::uint32_t v = 0; if (at + 4 <= file.size()) std::memcpy(&v, &file[at], 4); return v; };
    const auto u16 = [&](std::size_t at) { std::uint16_t v = 0; if (at + 2 <= file.size()) std::memcpy(&v, &file[at], 2); return v; };
    const std::size_t pe = u32(0x3C);
    const std::size_t sections = u16(pe + 6), optional = u16(pe + 20);
    const std::size_t table = u32(pe + 12), count = u32(pe + 16);
    if (!table || !count || table + count * 18 > file.size()) return symbols;
    const std::size_t first_section = pe + 24 + optional, strings = table + count * 18;
    for (std::size_t i = 0; i < count;) {
        const std::size_t at = table + i * 18;
        const std::size_t aux = file[at + 17];
        const int section = static_cast<std::int16_t>(u16(at + 12));
        const unsigned storage = file[at + 16];
        i += 1 + aux;
        if (section <= 0 || static_cast<std::size_t>(section) > sections || (storage != 2 && storage != 3)) continue;
        const std::size_t header = first_section + static_cast<std::size_t>(section - 1) * 40;
        if (!(u32(header + 36) & 0x20000000u)) continue;  // not in a section with code
        std::string name;
        if (u32(at) == 0) {
            const std::size_t offset = strings + u32(at + 4);
            if (offset < file.size()) name = reinterpret_cast<const char *>(&file[offset]);
        } else {
            name.assign(reinterpret_cast<const char *>(&file[at]), strnlen(reinterpret_cast<const char *>(&file[at]), 8));
        }
        symbols.push_back({u32(header + 12) + u32(at + 8), std::move(name)});
    }
    std::sort(symbols.begin(), symbols.end(), [](const Symbol &a, const Symbol &b) { return a.rva < b.rva; });
    return symbols;
}

// The exported function of a loaded DLL at or below an address (its nearest export: a guess for unexported code), "?" when none
std::string export_below(HMODULE module, std::uintptr_t address) {
    const auto *base = reinterpret_cast<const unsigned char *>(module);
    const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
    const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY &dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!dir.VirtualAddress || !dir.Size) return "?";
    const auto *exports = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY *>(base + dir.VirtualAddress);
    const auto *functions = reinterpret_cast<const DWORD *>(base + exports->AddressOfFunctions);
    const auto *names = reinterpret_cast<const DWORD *>(base + exports->AddressOfNames);
    const auto *ordinals = reinterpret_cast<const WORD *>(base + exports->AddressOfNameOrdinals);
    const auto rva = static_cast<DWORD>(address - reinterpret_cast<std::uintptr_t>(base));
    DWORD best = 0;
    const char *best_name = nullptr;
    for (DWORD i = 0; i < exports->NumberOfNames; ++i) {
        const DWORD f = functions[ordinals[i]];
        if (f <= rva && f >= best && !(f >= dir.VirtualAddress && f < dir.VirtualAddress + dir.Size)) {  // not a forwarder string
            best = f;
            best_name = reinterpret_cast<const char *>(base + names[i]);
        }
    }
    return best_name ? best_name : "?";
}

// "ps2::vu::run_fast(int, unsigned int)" for an address of ps2run.exe, "SomeLibrary.dll!Export" for one of a DLL
std::string describe(std::uintptr_t address, std::uintptr_t image_begin, std::uintptr_t image_end, const std::vector<Symbol> &symbols,
                     std::unordered_map<std::uintptr_t, std::string> &cache) {
    const auto found = cache.find(address);
    if (found != cache.end()) return found->second;
    std::string text;
    if (address >= image_begin && address < image_end) {
        const auto rva = static_cast<std::uint32_t>(address - image_begin);
        const auto it = std::upper_bound(symbols.begin(), symbols.end(), rva, [](std::uint32_t v, const Symbol &s) { return v < s.rva; });
        if (it != symbols.begin()) {
            text = demangle(std::prev(it)->name.c_str());
        } else {
            char hex[32];
            std::snprintf(hex, sizeof hex, "ps2run+0x%x", rva);
            text = hex;
        }
    } else {
        HMODULE module = nullptr;
        char path[MAX_PATH] = {};
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(address), &module) &&
            GetModuleFileNameA(module, path, MAX_PATH)) {
            const std::string full = path;
            text = full.substr(full.find_last_of(R"(\/)") + 1) + "!" + export_below(module, address);
        } else {
            text = "?";
        }
    }
    cache[address] = text;
    return text;
}

}

// PS2_PROFILE_TO=<frame> stops sampling at that frame (the file is still written when the run ends), so one stretch of a scene can be
// profiled alone (the God of War II film: its in-engine part, the full-motion video and what follows have different limits).
void start_profile(std::uint64_t from_frame) {
    g_from = from_frame;
    const char *to = std::getenv("PS2_PROFILE_TO");
    const std::uint64_t until = to ? std::strtoull(to, nullptr, 10) : 0;
    if (!g_from) arm();
    if (!g_from && !until) return;
    auto previous = ps2::g_options.on_vblank;
    ps2::g_options.on_vblank = [previous, until]() {
        if (g_from && ps2::frame_count() == g_from) arm();
        if (until && ps2::frame_count() == until) g_running.store(false, std::memory_order_relaxed);
        if (previous) previous();
    };
}

void write_profile(const char *path) {
    g_running.store(false);
    if (g_thread.joinable()) g_thread.join();
    if (g_target) {
        CloseHandle(g_target);
        g_target = nullptr;
    }
    const std::size_t total = g_count.load();
    std::FILE *f = std::fopen(path, "w");
    if (!f) return;
    const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(GetModuleHandleA(nullptr));
    const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(reinterpret_cast<const char *>(dos) + dos->e_lfanew);
    const std::uintptr_t begin = reinterpret_cast<std::uintptr_t>(dos), end = begin + nt->OptionalHeader.SizeOfImage;
    const std::vector<Symbol> symbols = load_symbols();
    std::unordered_map<std::uintptr_t, std::string> names;
    std::map<std::string, std::uint64_t> self, outside, callers, pairs, inclusive, chains;
    std::uint64_t in_program = 0;
    for (std::size_t i = 0; i < total; ++i) {
        const Sample &s = g_samples[i];
        if (s.pc >= begin && s.pc < end) {
            const std::string name = describe(s.pc, begin, end, symbols, names);
            ++self[name];
            ++inclusive[name];
            ++in_program;
        } else {
            const std::string name = describe(s.pc, begin, end, symbols, names);
            ++outside[name];
            const std::string caller = s.caller >= begin && s.caller < end ? describe(s.caller, begin, end, symbols, names) : std::string("?");
            ++callers[caller];
            ++inclusive[caller];
            ++pairs[name + "  <-  " + caller];
            std::string chain = name + "  <-  " + caller;
            for (const std::uintptr_t a : {s.caller2, s.caller3})
                if (a) chain += "  <-  " + describe(a, begin, end, symbols, names);
            ++chains[chain];
        }
    }
    const auto sorted = [](const std::map<std::string, std::uint64_t> &m) {
        std::vector<std::pair<std::string, std::uint64_t>> v(m.begin(), m.end());
        std::sort(v.begin(), v.end(), [](const auto &a, const auto &b) { return a.second > b.second; });
        return v;
    };
    const auto percent = [&](std::uint64_t n) { return total ? 100.0 * static_cast<double>(n) / static_cast<double>(total) : 0.0; };
    std::fprintf(f, "# %zu samples of the emulation thread (about one per millisecond of wall time), %llu of them inside ps2run.exe\n", total,
                 static_cast<unsigned long long>(in_program));
    std::fprintf(f, "# percent of all samples, samples, function\n\n# where the thread was, in ps2run.exe\n");
    std::size_t shown = 0;
    for (const auto &[name, hits] : sorted(self))
        if (shown++ < 60) std::fprintf(f, "%5.1f%% %7llu  %s\n", percent(hits), static_cast<unsigned long long>(hits), name.c_str());
    std::fprintf(f, "\n# where the thread was, outside ps2run.exe (system libraries)\n");
    shown = 0;
    for (const auto &[name, hits] : sorted(outside))
        if (shown++ < 20) std::fprintf(f, "%5.1f%% %7llu  %s\n", percent(hits), static_cast<unsigned long long>(hits), name.c_str());
    std::fprintf(f, "\n# the function of ps2run.exe that called into the system libraries at those moments (found by unwinding the stack)\n");
    shown = 0;
    for (const auto &[name, hits] : sorted(callers))
        if (shown++ < 20) std::fprintf(f, "%5.1f%% %7llu  %s\n", percent(hits), static_cast<unsigned long long>(hits), name.c_str());
    std::fprintf(f, "\n# system library function  <-  its caller in ps2run.exe\n");
    shown = 0;
    for (const auto &[name, hits] : sorted(pairs))
        if (shown++ < 30) std::fprintf(f, "%5.1f%% %7llu  %s\n", percent(hits), static_cast<unsigned long long>(hits), name.c_str());
    std::fprintf(f, "\n# the same with two more callers in ps2run.exe\n");
    shown = 0;
    for (const auto &[name, hits] : sorted(chains))
        if (shown++ < 30) std::fprintf(f, "%5.1f%% %7llu  %s\n", percent(hits), static_cast<unsigned long long>(hits), name.c_str());
    std::fprintf(f, "\n# functions of ps2run.exe with the system library time they caused added in\n");
    shown = 0;
    for (const auto &[name, hits] : sorted(inclusive))
        if (shown++ < 60) std::fprintf(f, "%5.1f%% %7llu  %s\n", percent(hits), static_cast<unsigned long long>(hits), name.c_str());
    std::fclose(f);
}
