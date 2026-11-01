#include "kernel.hpp"

#include "hw.hpp"
#include "io.hpp"
#include "sio2.hpp"
#include "spu2.hpp"
#include "../load_stats.hpp"

#include "ps2/state.hpp"

#include <algorithm>
#include <cstdio>
#include <deque>
#include <map>
#include <memory>
#include <queue>
#include <set>
#include <stdexcept>

namespace ps2::iop {

void execute(Cpu &c, std::int64_t &budget);

namespace kernel {

bool g_trace_hle = false;
bool g_trace_modules = false;

namespace {

constexpr std::uint32_t kKernelBase = 0x00001000u;
constexpr std::uint32_t kUserBase = 0x00020000u;
constexpr std::uint32_t kInterruptStack = 0x0001F000u;
constexpr int kDefaultModulePriority = 8;
constexpr std::int64_t kSlice = 512;

struct HleEntry {
    std::string library;
    std::uint32_t index;
    HleFunction function;
};

struct Event {
    std::uint64_t at;
    std::uint64_t seq;
    Job job;
    std::function<void()> action;
    bool operator>(const Event &o) const { return at != o.at ? at > o.at : seq > o.seq; }
};

struct Interrupt {
    std::uint32_t handler{};
    std::uint32_t arg{};
    std::uint32_t mode{};
};

std::vector<HleEntry> g_hle;
std::map<std::pair<std::string, std::uint32_t>, std::uint32_t> g_hle_ids;
std::vector<std::pair<std::uint32_t, std::uint32_t>> g_hle_stubs;
std::map<std::string, std::uint32_t> g_hle_versions;
std::map<std::string, std::uint32_t> g_libraries;
std::map<std::string, HleModuleStart> g_hle_modules;
std::map<int, Module> g_modules;
int g_next_module = 1;

std::map<int, Thread> g_threads;
int g_current = 0;
int g_next_thread = 1;
std::uint64_t g_order = 0;
bool g_switch = false;
std::uint64_t g_switch_count = 0;  // thread switches so far (statistics for ReferSystemStatus; not part of the save state)

std::map<std::uint32_t, std::uint32_t> g_used;
std::uint32_t g_kernel_top = kKernelBase;

std::priority_queue<Event, std::vector<Event>, std::greater<Event>> g_events;
std::uint64_t g_event_seq = 0;
std::uint64_t g_now = 0;
std::uint64_t g_target = 0;
std::uint64_t g_ee_remainder = 0;

std::map<unsigned, Interrupt> g_interrupts;
bool g_suspended = false;
int g_nesting = 0;
bool g_nested_done = false;
std::uint32_t g_return_stub = 0;
std::uint32_t g_exit_stub = 0;
std::set<std::pair<std::string, std::uint32_t>> g_missing_reported;

Thread *best_ready() {
    Thread *best = nullptr;
    for (auto &[id, t] : g_threads) {
        if (t.status != kReady && t.status != kRun) continue;
        if (!best || t.priority < best->priority || (t.priority == best->priority && t.order < best->order)) best = &t;
    }
    return best;
}

void do_switch() {
    g_switch = false;
    Thread *cur = current();
    Thread *next = best_ready();
    if (next == cur && cur && cur->status == kRun) return;
    ++g_switch_count;
    if (cur && cur->status != kDormant) {
        cur->context = g_cpu;
        cur->suspended = g_suspended;
        if (cur->status == kRun) cur->status = kReady;
    }
    if (next) {
        next->status = kRun;
        g_current = next->id;
        g_cpu = next->context;
        g_suspended = next->suspended;
    } else {
        g_current = 0;
        g_suspended = false;
    }
}

std::uint32_t hle_id_for(const std::string &library, std::uint32_t index) {
    auto it = g_hle_ids.find({library, index});
    if (it != g_hle_ids.end()) return it->second;
    return register_hle(library, index, [library, index](Cpu &c) {
        if (g_missing_reported.insert({library, index}).second)
            std::fprintf(stderr, "[iop] missing %s #%u called from 0x%08X\n", library.c_str(), index, c.r[31]);
        c.r[2] = 0;
    });
}

void link_imports(std::uint32_t base, std::uint32_t size) {
    for (std::uint32_t a = base; a + 20 <= base + size; a += 4) {
        if (ram32(a) != 0x41E00000u) continue;
        char raw[9] = {};
        std::memcpy(raw, ram_ptr(a + 12), 8);
        const std::string library(raw);
        const auto guest = g_libraries.find(library);
        for (std::uint32_t stub = a + 20; stub + 8 <= base + size; stub += 8) {
            const std::uint32_t w0 = ram32(stub), w1 = ram32(stub + 4);
            if (w0 != 0x03E00008u || (w1 & 0xFFFF0000u) != 0x24000000u) break;
            const std::uint32_t index = w1 & 0xFFFFu;
            if (guest != g_libraries.end()) {
                const std::uint32_t target = ram32(guest->second + 20 + index * 4);
                set_ram32(stub, 0x08000000u | ((target >> 2) & 0x03FFFFFFu));
                set_ram32(stub + 4, 0);
            } else {
                if (!g_hle_versions.count(library) && g_trace_modules)
                    std::fprintf(stderr, "[iop] import from unknown library %s #%u\n", library.c_str(), index);
                set_ram32(stub, (kHleOpcode << 26) | hle_id_for(library, index));
            }
        }
    }
}

void relink_hle_imports() {
    for (const auto &[id, m] : g_modules) {
        for (std::uint32_t a = m.base; a + 20 <= m.base + m.size; a += 4) {
            if (ram32(a) != 0x41E00000u) continue;
            char raw[9] = {};
            std::memcpy(raw, ram_ptr(a + 12), 8);
            const std::string library(raw);
            for (std::uint32_t stub = a + 20; stub + 8 <= m.base + m.size; stub += 8) {
                const std::uint32_t w0 = ram32(stub), w1 = ram32(stub + 4);
                if ((w0 >> 26) == 2u && w1 == 0) continue;
                if ((w0 >> 26) != kHleOpcode || (w1 & 0xFFFF0000u) != 0x24000000u) break;
                set_ram32(stub, (kHleOpcode << 26) | hle_id_for(library, w1 & 0xFFFFu));
            }
        }
    }
    for (const auto &[at, id] : g_hle_stubs) set_ram32(at, (kHleOpcode << 26) | id);
}

// A module comes from the disc or from the game's memory, so it is untrusted: every read is checked against the file, and a read
// past its end gives 0 and marks the module bad (the loader then refuses it).
struct Elf {
    const std::vector<std::uint8_t> &data;
    mutable bool bad = false;
    bool has(std::uint64_t o, std::uint64_t n) const { return o <= data.size() && n <= data.size() - o; }
    std::uint32_t u32(std::uint64_t o) const {
        std::uint32_t v = 0;
        if (has(o, 4)) std::memcpy(&v, data.data() + o, 4); else bad = true;
        return v;
    }
    std::uint16_t u16(std::uint64_t o) const {
        std::uint16_t v = 0;
        if (has(o, 2)) std::memcpy(&v, data.data() + o, 2); else bad = true;
        return v;
    }
};

void relocate(const Elf &elf, std::uint32_t base, std::uint32_t size, bool v2) {
    const std::uint32_t shoff = elf.u32(32);
    const std::uint16_t shentsize = elf.u16(46), shnum = elf.u16(48);
    // The section headers and each relocation table (ELF spec, SHT_REL = 9: 8-byte entries) must lie inside the file; the loader
    // refuses the module otherwise (elf.bad), before anything was relocated.
    for (std::uint32_t i = 0; i < shnum; ++i) {
        const std::uint64_t sh = shoff + std::uint64_t{i} * shentsize;
        if (!elf.has(sh, 24)) { elf.bad = true; return; }
        if (elf.u32(sh + 4) == 9 && !elf.has(elf.u32(sh + 16), elf.u32(sh + 20) / 8 * 8ull)) { elf.bad = true; return; }
    }
    auto word = [&](std::int64_t offset) -> std::uint8_t * {
        static std::uint32_t dummy;
        if (offset < 0 || offset + 4 > size) return reinterpret_cast<std::uint8_t *>(&dummy);
        return ram_ptr(base + static_cast<std::uint32_t>(offset));
    };
    auto get = [&](std::int64_t o) { std::uint32_t v; std::memcpy(&v, word(o), 4); return v; };
    auto put = [&](std::int64_t o, std::uint32_t v) { std::memcpy(word(o), &v, 4); };
    for (std::uint32_t i = 0; i < shnum; ++i) {
        const std::size_t sh = shoff + i * shentsize;
        if (elf.u32(sh + 4) != 9) continue;
        const std::uint32_t off = elf.u32(sh + 16), count = elf.u32(sh + 20) / 8;
        std::int64_t hi_at = -1;
        std::uint32_t hi_word = 0;
        for (std::uint32_t k = 0; k < count; ++k) {
            const auto at = static_cast<std::int64_t>(static_cast<std::int32_t>(elf.u32(off + k * 8)));
            const std::uint32_t type = elf.u32(off + k * 8 + 4) & 0xFFu;
            std::uint32_t insn = get(at);
            switch (type) {
            case 2: put(at, insn + base); break;
            case 4: put(at, (insn & ~0x03FFFFFFu) | (((insn & 0x03FFFFFFu) + (base >> 2)) & 0x03FFFFFFu)); break;
            case 5:
                if (v2 && k + 1 < count) {
                    const auto next = static_cast<std::int64_t>(static_cast<std::int32_t>(elf.u32(off + (k + 1) * 8)));
                    std::uint32_t value = static_cast<std::uint32_t>(static_cast<std::int16_t>(get(next))) + (insn << 16) + base;
                    if (value & 0x8000u) value += 0x10000u;
                    put(at, (insn & ~0xFFFFu) | (value >> 16));
                } else {
                    hi_at = at;
                    hi_word = insn;
                }
                break;
            case 6:
                if (v2) {
                    put(at, (insn & ~0xFFFFu) | ((static_cast<std::uint32_t>(static_cast<std::int16_t>(insn)) + base) & 0xFFFFu));
                } else if (hi_at >= 0) {
                    std::uint32_t value = static_cast<std::uint32_t>(static_cast<std::int16_t>(insn)) + (hi_word << 16) + base;
                    put(at, (insn & ~0xFFFFu) | (value & 0xFFFFu));
                    if (value & 0x8000u) value += 0x10000u;
                    put(hi_at, (hi_word & ~0xFFFFu) | (value >> 16));
                    hi_at = -1;
                } else {
                    put(at, (insn & ~0xFFFFu) | ((static_cast<std::uint32_t>(static_cast<std::int16_t>(insn)) + base) & 0xFFFFu));
                }
                break;
            default:
                if (g_trace_modules) std::fprintf(stderr, "[iop] relocation type %u ignored\n", type);
                break;
            }
        }
    }
}

std::string module_name_of(const std::string &path) {
    std::string name = path;
    const auto slash = name.find_last_of("/\\:");
    if (slash != std::string::npos) name = name.substr(slash + 1);
    const auto semi = name.find(';');
    if (semi != std::string::npos) name = name.substr(0, semi);
    const auto dot = name.find('.');
    if (dot != std::string::npos) name = name.substr(0, dot);
    for (char &ch : name) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    return name;
}

}

void fault(const char *reason) {
    std::fprintf(stderr, "[iop] fault: %s (pc 0x%08X thread %d)\n", reason, g_cpu.pc, g_current);
    if (Thread *t = current()) {
        t->status = kDormant;
        g_current = 0;
    }
    g_switch = true;
}

std::uint32_t fetch_special(std::uint32_t pc) {
    std::fprintf(stderr, "[iop] fetch from 0x%08X\n", pc);
    fault("fetch outside RAM");
    return 0;
}

void syscall(Cpu &c, std::uint32_t pc) {
    std::fprintf(stderr, "[iop] syscall at 0x%08X (v0 0x%X)\n", pc, c.r[2]);
}

void set_status(Cpu &c, std::uint32_t value) { c.status = value; }

bool switch_pending() { return g_switch || (g_nesting && g_nested_done); }

std::uint64_t now() { return g_now; }
std::uint64_t usec_to_cycles(std::uint64_t usec) { return usec * 36864u / 1000u; }

void schedule(std::uint64_t at, std::function<void()> action) {
    g_events.push(Event{at, ++g_event_seq, Job{}, std::move(action)});
}

namespace {
JobHandler g_jobs[kJobCount] = {};
}

void register_job(std::uint32_t kind, JobHandler handler) { g_jobs[kind] = handler; }

void schedule(std::uint64_t at, const Job &job) {
    g_events.push(Event{at, ++g_event_seq, job, nullptr});
}

Thread *current() {
    auto it = g_threads.find(g_current);
    return it == g_threads.end() ? nullptr : &it->second;
}

Thread *thread(int id) {
    if (id == 0) return current();
    auto it = g_threads.find(id);
    return it == g_threads.end() ? nullptr : &it->second;
}

std::vector<int> thread_ids() {
    std::vector<int> ids;
    ids.reserve(g_threads.size());
    for (const auto &[id, t] : g_threads) ids.push_back(id);
    return ids;
}

std::uint64_t switch_count() { return g_switch_count; }

int create_thread(std::uint32_t entry, std::uint32_t stack_size, int priority, std::uint32_t gp, std::uint32_t attr, std::uint32_t option) {
    Thread t;
    t.id = g_next_thread++;
    t.entry = entry;
    t.stack_size = (stack_size + 0xFFu) & ~0xFFu;
    t.stack = alloc(t.stack_size, 1);
    t.priority = t.init_priority = priority;
    t.gp = gp;
    t.attr = attr;
    t.option = option;
    g_threads[t.id] = t;
    if (g_trace_modules) std::fprintf(stderr, "[iop] thread %d entry 0x%08X prio %d\n", t.id, entry, priority);
    return t.id;
}

bool delete_thread(int id) {
    auto it = g_threads.find(id);
    if (it == g_threads.end() || it->second.status != kDormant || id == g_current) return false;
    if (it->second.stack) free(it->second.stack);
    g_threads.erase(it);
    return true;
}

void start_thread(int id, std::uint32_t a0, std::uint32_t a1) {
    Thread *t = thread(id);
    if (!t) return;
    std::memset(&t->context, 0, sizeof t->context);
    t->context.pc = t->entry;
    t->context.npc = t->entry + 4;
    t->context.r[4] = a0;
    t->context.r[5] = a1;
    t->context.r[28] = t->gp;
    t->context.r[29] = t->stack + t->stack_size - 0x10u;
    t->context.r[31] = g_exit_stub;
    t->context.status = 0x401u;
    t->priority = t->init_priority;
    t->wakeups = 0;
    t->wait = kWaitNone;
    ready(*t);
}

void ready(Thread &t) {
    t.status = kReady;
    t.order = ++g_order;
    g_switch = true;
}

void block(Cpu &c, WaitKind kind, int id) {
    Thread *t = current();
    if (!t || g_nesting) {
        std::fprintf(stderr, "[iop] blocking call outside a thread (kind %d)\n", kind);
        return;
    }
    t->status = kWait;
    t->wait = kind;
    t->wait_id = id;
    t->context = c;
    g_switch = true;
}

void wake(Thread &t, std::uint32_t result) {
    if (t.status != kWait && t.status != kWaitSuspend) return;
    t.wait = kWaitNone;
    t.context.r[2] = result;
    if (t.status == kWaitSuspend) t.status = kSuspend;
    else ready(t);
}

void reschedule() { g_switch = true; }

bool in_interrupt() { return g_nesting > 0; }

std::uint32_t call_guest(std::uint32_t address, std::uint32_t a0, std::uint32_t a1, std::uint32_t a2, std::uint32_t a3) {
    const Cpu saved = g_cpu;
    const bool saved_done = g_nested_done;
    ++g_nesting;
    g_nested_done = false;
    g_cpu.r[4] = a0;
    g_cpu.r[5] = a1;
    g_cpu.r[6] = a2;
    g_cpu.r[7] = a3;
    g_cpu.r[29] = kInterruptStack - static_cast<std::uint32_t>(g_nesting - 1) * 0x1000u;
    g_cpu.r[31] = g_return_stub;
    g_cpu.pc = address;
    g_cpu.npc = address + 4;
    std::int64_t guard = 50'000'000;
    bool switch_wanted = g_switch;
    g_switch = false;
    while (!g_nested_done && guard > 0) {
        std::int64_t budget = kSlice;
        execute(g_cpu, budget);
        guard -= kSlice - budget;
        switch_wanted |= g_switch;
        g_switch = false;
    }
    g_switch = switch_wanted;
    if (guard <= 0) std::fprintf(stderr, "[iop] guest call to 0x%08X did not return\n", address);
    const std::uint32_t result = g_cpu.r[2];
    g_cpu = saved;
    g_nested_done = saved_done;
    --g_nesting;
    return result;
}

std::uint32_t alloc(std::uint32_t size, int mode, std::uint32_t at) {
    size = (size + 0xFFu) & ~0xFFu;
    if (!size) size = 0x100u;
    auto fits = [&](std::uint32_t start) {
        if (start < kUserBase || start + size > kRamSize) return false;
        auto it = g_used.lower_bound(start);
        if (it != g_used.end() && it->first < start + size) return false;
        if (it != g_used.begin()) {
            auto prev = std::prev(it);
            if (prev->first + prev->second > start) return false;
        }
        return true;
    };
    std::uint32_t chosen = 0;
    if (mode == 2) {
        if (fits(at & ~0xFFu)) chosen = at & ~0xFFu;
    } else if (mode == 1) {
        std::uint32_t top = kRamSize;
        for (auto it = g_used.rbegin(); ; ++it) {
            const std::uint32_t bottom = it == g_used.rend() ? kUserBase : it->first + it->second;
            if (top >= bottom + size && top - size >= kUserBase) { chosen = top - size; break; }
            if (it == g_used.rend()) break;
            top = it->first;
        }
    } else {
        std::uint32_t start = kUserBase;
        for (const auto &[a, s] : g_used) {
            if (a >= start + size) break;
            start = std::max(start, a + s);
        }
        if (start + size <= kRamSize) chosen = start;
    }
    if (!chosen) {
        std::fprintf(stderr, "[iop] out of memory allocating 0x%X\n", size);
        return 0;
    }
    g_used[chosen] = size;
    return chosen;
}

void free(std::uint32_t address) { g_used.erase(phys(address)); }

std::uint32_t free_total() {
    std::uint32_t used = 0;
    for (const auto &[a, s] : g_used) used += s;
    return kRamSize - kUserBase - used;
}

std::uint32_t free_max() {
    std::uint32_t best = 0, start = kUserBase;
    for (const auto &[a, s] : g_used) {
        if (a > start) best = std::max(best, a - start);
        start = std::max(start, a + s);
    }
    return std::max(best, kRamSize - start);
}

std::uint32_t kernel_alloc(std::uint32_t size) {
    const std::uint32_t at = g_kernel_top;
    g_kernel_top += (size + 15u) & ~15u;
    if (g_kernel_top > kInterruptStack - 0x4000u) std::fprintf(stderr, "[iop] kernel area exhausted\n");
    std::memset(ram_ptr(at), 0, size);
    return at;
}

std::uint32_t hle_stub(const HleFunction &function) {
    const std::uint32_t id = register_hle("kernel", static_cast<std::uint32_t>(g_hle.size()) + 0x1000u, function);
    const std::uint32_t at = kernel_alloc(8);
    set_ram32(at, (kHleOpcode << 26) | id);
    g_hle_stubs.emplace_back(at, id);
    return at;
}

void register_library(const std::string &name, std::uint32_t table) {
    g_libraries[name] = table;
    if (g_trace_modules) std::fprintf(stderr, "[iop] library %s at 0x%08X\n", name.c_str(), table);
}

void register_hle_library(const std::string &name, std::uint32_t version) { g_hle_versions[name] = version; }

bool library_exists(const std::string &name) { return g_libraries.count(name) || g_hle_versions.count(name); }

const std::map<int, Module> &modules() { return g_modules; }

void register_hle_module(const std::string &name, HleModuleStart start) { g_hle_modules[name] = std::move(start); }

void register_interrupt(unsigned line, std::uint32_t handler, std::uint32_t arg, std::uint32_t mode) {
    if (g_trace_modules) std::fprintf(stderr, "[iop] interrupt %u -> 0x%08X (arg 0x%X)\n", line, handler, arg);
    g_interrupts[line] = Interrupt{handler, arg, mode};
}

void release_interrupt(unsigned line) { g_interrupts.erase(line); }

void enable_interrupt(unsigned line, bool enabled) { hw::set_interrupt_mask(line, enabled); }

void interrupt_state(bool suspended) { g_suspended = suspended; }
bool interrupts_suspended() { return g_suspended; }

void deliver_interrupts() {
    if (g_suspended || g_nesting) return;
    for (unsigned guard = 0; guard < 64; ++guard) {
        const int line = hw::next_pending_interrupt();
        if (line < 0) return;
        hw::acknowledge_interrupt(static_cast<unsigned>(line));
        auto it = g_interrupts.find(static_cast<unsigned>(line));
        if (it == g_interrupts.end() || !it->second.handler) continue;
        const Interrupt handler = it->second;
        if (g_trace_modules && line == 9) std::fprintf(stderr, "[iop] spu2 interrupt -> 0x%08X at %llu\n", handler.handler, static_cast<unsigned long long>(g_now));
        const std::uint32_t result = call_guest(handler.handler, handler.arg);
        if (result == 0 && line < 32) hw::set_interrupt_mask(static_cast<unsigned>(line), false);
    }
}

void trace_hle(const char *library, const char *function, const Cpu &c) {
    if (!g_trace_hle) return;
    std::fprintf(stderr, "[iop-hle] %s:%s(%08X, %08X, %08X, %08X) ra 0x%08X thread %d\n", library, function,
                 c.r[4], c.r[5], c.r[6], c.r[7], c.r[31], g_current);
}

void boot() {
    g_hle.clear();
    g_hle_ids.clear();
    g_hle_stubs.clear();
    g_hle_versions.clear();
    g_libraries.clear();
    g_hle_modules.clear();
    g_modules.clear();
    g_threads.clear();
    g_used.clear();
    while (!g_events.empty()) g_events.pop();
    g_current = 0;
    g_next_thread = g_next_module = 1;
    g_kernel_top = kKernelBase;
    g_now = g_target = 0;
    g_switch = g_suspended = false;
    g_nesting = 0;
    g_interrupts.clear();
    std::memset(&g_cpu, 0, sizeof g_cpu);
    g_return_stub = hle_stub([](Cpu &) { g_nested_done = true; });
    g_exit_stub = hle_stub([](Cpu &c) {
        Thread *t = current();
        if (!t) return;
        t->context = c;
        t->status = kDormant;
        auto on_exit = std::move(t->on_exit);
        t->on_exit = nullptr;
        if (on_exit) on_exit();
        g_switch = true;
    });
    install_libraries();
    install_io();
    install_cdvd();
    install_sif();
}

}

std::uint32_t register_hle(const std::string &library, std::uint32_t index, HleFunction function) {
    auto key = std::make_pair(library, index);
    auto it = kernel::g_hle_ids.find(key);
    if (it != kernel::g_hle_ids.end()) {
        kernel::g_hle[it->second].function = std::move(function);
        return it->second;
    }
    const auto id = static_cast<std::uint32_t>(kernel::g_hle.size());
    kernel::g_hle.push_back({library, index, std::move(function)});
    kernel::g_hle_ids[key] = id;
    return id;
}

void call_hle(Cpu &c, std::uint32_t id) {
    if (id >= kernel::g_hle.size()) {
        kernel::fault("bad HLE id");
        return;
    }
    kernel::g_hle[id].function(c);
}

void reset() {
    hw::reset();
    kernel::boot();
}

std::uint64_t cycles() { return kernel::g_now; }

void run(std::uint64_t ee_cycles) {
    using namespace kernel;
    const cost::Timer whole(cost::kIopRun);
    g_ee_remainder += ee_cycles;
    g_target += g_ee_remainder / kClockDivider;
    g_ee_remainder %= kClockDivider;
    std::vector<Event> deferred;
    while (g_now < g_target) {
        deferred.clear();
        const Thread *running = current();
        // A thread inside CpuSuspendIntr cannot be preempted: interrupt delivery and thread
        // switching wait for CpuResumeIntr. The SPU2 and the DMA controller, however, keep
        // running: a game that enters a critical section to wait for a voice transfer or a
        // DMA to finish (Dynasty Warriors 4 does, polling a voice's NAX) must not deadlock.
        const bool deliverable = !g_suspended || !running || running->status != kRun;
        while (!g_events.empty() && g_events.top().at <= g_now) {
            const Event due = g_events.top();
            const bool hardware = due.job.kind == kJobSpuTick || due.job.kind == kJobDmaDone;
            if (!deliverable && !hardware) {
                // Software events wait for CpuResumeIntr; hardware that is independent of interrupts
                // must not be stuck behind them, or a guest polling SPU2/DMA inside a critical section
                // deadlocks (Dynasty Warriors 4 waits like that for a voice's NAX).
                deferred.push_back(due);
                g_events.pop();
                continue;
            }
            g_events.pop();
            const cost::Timer job_time(cost::kIopJobs);
            if (due.job.kind) g_jobs[due.job.kind](due.job);
            else due.action();
        }
        for (Event &e : deferred) g_events.push(std::move(e));
        if (deliverable) {
            deliver_interrupts();
            if (g_switch) do_switch();
        }
        if (!current()) {
            const std::uint64_t next = g_events.empty() ? g_target : std::min(g_target, g_events.top().at);
            g_now = std::max(g_now, next);
            continue;
        }
        std::int64_t budget = static_cast<std::int64_t>(std::min<std::uint64_t>(kSlice, g_target - g_now));
        if (!g_events.empty() && g_events.top().at > g_now)
            budget = std::min<std::int64_t>(budget, static_cast<std::int64_t>(g_events.top().at - g_now));
        if (budget <= 0) budget = 1;
        const std::int64_t given = budget;
        {
            const cost::Timer exec_time(cost::kIopExec);
            execute(g_cpu, budget);
        }
        if (cost::g_on) cost::g_exec_cycles += static_cast<std::uint64_t>(given - std::max<std::int64_t>(0, budget));
        g_now += static_cast<std::uint64_t>(std::max<std::int64_t>(1, given - std::max<std::int64_t>(0, budget)));
    }
}

bool idle() { return !kernel::current() && kernel::g_events.empty(); }

void dump() {
    using namespace kernel;
    for (const auto &[id, m] : g_modules) std::fprintf(stderr, "[iop] module %d %s at 0x%08X\n", id, m.name.c_str(), m.base);
    for (const auto &[id, t] : g_threads)
        std::fprintf(stderr, "[iop] thread %d status %d prio %d wait %d/%d pc 0x%08X\n", id, t.status, t.priority, t.wait, t.wait_id,
                     id == g_current ? g_cpu.pc : t.context.pc);
}

void raise_interrupt(unsigned line) { hw::raise_interrupt(line); }

std::int32_t load_module_buffer(const std::vector<std::uint8_t> &data, const std::string &path, const std::string &args,
                                std::function<void(int, int)> done) {
    using namespace kernel;
    const std::string name = module_name_of(path);
    auto hle = g_hle_modules.find(name);
    if (hle != g_hle_modules.end()) {
        if (g_trace_modules) std::fprintf(stderr, "[iop] %s handled natively\n", name.c_str());
        const int id = g_next_module++;
        g_modules[id] = Module{id, name, 0, 0, 0, 0};
        if (hle->second) hle->second();
        if (done) done(id, 0);
        return id;
    }
    if (data.size() < 52 || std::memcmp(data.data(), "\x7f" "ELF", 4) != 0) {
        std::fprintf(stderr, "[iop] %s is not an ELF module\n", path.c_str());
        return -1;
    }
    const Elf elf{data};
    const auto refuse = [&](const char *why) {
        std::fprintf(stderr, "[iop] %s is damaged: %s (module refused)\n", path.c_str(), why);
        return -1;
    };
    const std::uint16_t type = elf.u16(16);
    const std::uint32_t phoff = elf.u32(28);
    const std::uint16_t phnum = elf.u16(44), phentsize = elf.u16(42);
    std::uint32_t iopmod = 0, load_off = 0, load_size = 0, mem_size = 0;
    for (std::uint32_t i = 0; i < phnum; ++i) {
        const std::uint64_t ph = phoff + std::uint64_t{i} * phentsize;
        const std::uint32_t ptype = elf.u32(ph);
        if (ptype == 0x70000080u) iopmod = elf.u32(ph + 4);
        if (ptype == 1 && !load_size) {
            load_off = elf.u32(ph + 4);
            load_size = elf.u32(ph + 16);
            mem_size = elf.u32(ph + 20);
        }
    }
    if (elf.bad) return refuse("a program header lies past the end of the file");
    if (!iopmod || !load_size) {
        std::fprintf(stderr, "[iop] %s has no iopmod/load segment (type 0x%X)\n", path.c_str(), type);
        return -1;
    }
    const std::uint32_t entry = elf.u32(std::uint64_t{iopmod} + 4), gp = elf.u32(std::uint64_t{iopmod} + 8);
    const std::uint64_t sizes = std::uint64_t{elf.u32(std::uint64_t{iopmod} + 12)} + elf.u32(std::uint64_t{iopmod} + 16) + elf.u32(std::uint64_t{iopmod} + 20);
    const std::uint64_t whole = std::max<std::uint64_t>(mem_size, sizes);
    // ELF spec: the file contents (p_filesz) lie inside the file and are no larger than the segment in memory; and the module has
    // to fit in the IOP's 2 MiB. Anything else would copy past the module's block or wrap the sizes round 4 GiB.
    if (elf.bad) return refuse("its iopmod record lies past the end of the file");
    if (!elf.has(load_off, load_size)) return refuse("its contents lie past the end of the file");
    if (whole >= kRamSize) return refuse("it is larger than the IOP's memory");
    if (load_size > whole) return refuse("its contents are larger than its memory");
    const std::uint32_t total = static_cast<std::uint32_t>(whole);
    const std::uint32_t base = alloc(total + 0x40u);
    if (!base) return -1;
    std::memset(ram_ptr(base), 0, total);
    std::memcpy(ram_ptr(base), data.data() + load_off, load_size);
    relocate(elf, base, total, type == 0xFF81u);
    if (elf.bad) {
        kernel::free(base);
        return refuse("a section header or relocation table lies past the end of the file");
    }
    link_imports(base, total);

    // A module the EE sent as a buffer has no file name: it is known by the name in its own header (the iopmod record, version first,
    // then the name), which is what SearchModuleByName looks for (Tekken 5 asks the IOP for "mcserv" before it starts the memory card).
    std::string module_name = name;
    if (path == "buffer" && iopmod + 26 < data.size()) {
        const char *text = reinterpret_cast<const char *>(data.data() + iopmod + 26);
        module_name.assign(text, strnlen(text, std::min<std::size_t>(data.size() - iopmod - 26, 56)));
    }
    const int id = g_next_module++;
    g_modules[id] = Module{id, module_name, base, total, base + entry, base + gp};
    if (g_trace_modules) std::fprintf(stderr, "[iop] module %d %s at 0x%08X size 0x%X entry 0x%08X\n", id, module_name.c_str(), base, total, base + entry);

    std::vector<std::string> argv{path};
    std::size_t start = 0;
    while (start < args.size()) {
        const auto end = args.find('\0', start);
        argv.push_back(args.substr(start, end == std::string::npos ? std::string::npos : end - start));
        if (end == std::string::npos) break;
        start = end + 1;
    }
    std::uint32_t argv_size = 4u * static_cast<std::uint32_t>(argv.size() + 1);
    for (const auto &s : argv) argv_size += static_cast<std::uint32_t>(s.size() + 1);
    const std::uint32_t argv_at = alloc(argv_size);
    std::uint32_t text = argv_at + 4u * static_cast<std::uint32_t>(argv.size() + 1);
    for (std::size_t i = 0; i < argv.size(); ++i) {
        set_ram32(argv_at + 4u * static_cast<std::uint32_t>(i), text);
        std::memcpy(ram_ptr(text), argv[i].c_str(), argv[i].size() + 1);
        text += static_cast<std::uint32_t>(argv[i].size() + 1);
    }
    set_ram32(argv_at + 4u * static_cast<std::uint32_t>(argv.size()), 0);

    const int tid = create_thread(base + entry, 0x4000u, kDefaultModulePriority, base + gp, 0x02000000u, 0);
    Thread *t = thread(tid);
    t->on_exit = [id, tid, argv_at, done]() {
        const std::uint32_t result = thread(tid)->context.r[2];
        if (done) done(id, static_cast<int>(result));
        free(argv_at);
        if (g_trace_modules) std::fprintf(stderr, "[iop] module %d started, result %d\n", id, static_cast<std::int32_t>(result));
        if ((result & 3u) == 1u) {
            const Module &m = g_modules[id];
            free(m.base);
        }
    };
    start_thread(tid, static_cast<std::uint32_t>(argv.size()), argv_at);
    return id;
}

void set_trace(bool modules_on, bool hle_on) {
    kernel::g_trace_modules = modules_on;
    kernel::g_trace_hle = hle_on;
}

}

namespace ps2::iop::kernel {

void state(ps2::state::Stream &s) {
    s.tag("iopkern");
    if (s.saving() && g_nesting) throw std::runtime_error("the IOP is inside a guest call");
    s.map(g_threads, [&s](Thread &t) {
        if (s.saving() && t.on_exit) throw std::runtime_error("an IOP module is still starting");
        s.pod(t.id);
        s.pod(t.status);
        s.pod(t.priority);
        s.pod(t.init_priority);
        s.pod(t.entry);
        s.pod(t.stack);
        s.pod(t.stack_size);
        s.pod(t.gp);
        s.pod(t.attr);
        s.pod(t.option);
        s.pod(t.wakeups);
        s.pod(t.wait);
        s.pod(t.wait_id);
        s.pod(t.order);
        s.pod(t.context);
        s.pod(t.rpc_queue);
        s.pod(t.rpc_server);
        s.pod(t.event_bits);
        s.pod(t.event_mode);
        s.pod(t.event_result);
        s.pod(t.message_out);
        s.pod(t.suspended);
    });
    s.map(g_modules, [&s](Module &m) {
        s.pod(m.id);
        s.str(m.name);
        s.pod(m.base);
        s.pod(m.size);
        s.pod(m.entry);
        s.pod(m.gp);
    });
    s.string_map(g_libraries, [&s](std::uint32_t &table) { s.pod(table); });
    s.map(g_used);
    s.map(g_interrupts);
    std::vector<Event> events;
    if (s.saving()) {
        auto pending = g_events;
        for (; !pending.empty(); pending.pop()) {
            if (!pending.top().job.kind) throw std::runtime_error("an IOP event cannot be saved");
            events.push_back(pending.top());
        }
    }
    s.vec(events, [&s](Event &e) {
        s.pod(e.at);
        s.pod(e.seq);
        s.pod(e.job);
    });
    if (s.loading()) {
        while (!g_events.empty()) g_events.pop();
        for (const Event &e : events) g_events.push(e);
    }
    s.pod(g_current);
    s.pod(g_next_thread);
    s.pod(g_next_module);
    s.pod(g_order);
    s.pod(g_switch);
    s.pod(g_kernel_top);
    s.pod(g_event_seq);
    s.pod(g_now);
    s.pod(g_target);
    s.pod(g_ee_remainder);
    s.pod(g_suspended);
    s.pod(g_nested_done);
    s.pod(g_return_stub);
    s.pod(g_exit_stub);
    if (s.loading()) relink_hle_imports();
}

}

namespace ps2::iop {

void state(ps2::state::Stream &s) {
    s.tag("iop");
    s.pod(g_cpu);
    s.bytes(g_ram, kRamSize);
    kernel::state(s);
    kernel::libs_state(s);
    kernel::cdvd_state(s);
    kernel::cdvdfsv_state(s);
    hw::state(s);
    sio2::state(s);
    spu2::state(s);
    io::state(s);
}

}
