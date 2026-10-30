#include "ps2/ee_live.hpp"
#include "ps2/generated.hpp"
#include "ps2/os.hpp"
#include "ps2/system.hpp"

#include <algorithm>
#include <cstdio>
#include <deque>
#include <fstream>
#include <set>
#include <unordered_map>
#include <string>
#include <vector>

namespace ps2 {

namespace {

std::vector<UnitInfo> g_units;
std::vector<const UnitInfo *> g_index;
std::uint32_t g_origin = 0;
std::uint32_t g_unit_size = 1;
std::set<std::uint32_t> g_misses;
std::set<std::string> g_module_misses;

constexpr std::uint32_t kModuleMagic = 0x32524E53u;
// The "xff2" objects of Shadow of the Colossus: same search, the size sits at 0x14
constexpr std::uint32_t kXffMagic = 0x32666678u;

// The soft checks of one module, by entry offset (ee_live.hpp, SoftChecks).
struct SoftIndex {
    std::unordered_map<std::uint32_t, const std::uint32_t *> groups;   // entry offset -> {entry offset, n, n * {offset, word}}
};

struct Instance {
    std::uint32_t base;
    const ModuleInfo *module;
    int rank;
    const SoftIndex *soft = nullptr;
};

// Every module there is: the ones the build was translated with and the ones translated while the game ran (ee_live.cpp, added by
// add_modules). A deque, so that an Instance can point at a module while more are added.
std::deque<ModuleInfo> g_modules;
// How good each module's code is, in step with g_modules: of two modules of the same content that both have an entry, the higher rank
// runs (ee_live.cpp translates a window quickly first, with little optimisation, and again with more in the background).
std::deque<int> g_ranks;
// The soft checks of each module, in step with g_modules (empty for a module that has none).
std::deque<SoftIndex> g_soft;
int g_max_rank = 0;
std::uint32_t g_module_span = 0;
std::vector<Instance> g_instances;

bool loaded_at(const ModuleInfo &m, std::uint32_t base) {
    if (m.fixed_base) {
        if (base != m.fixed_base) return false;
    } else if (ld32(base + (ld32(base) == kXffMagic ? 0x14u : 0x2Cu)) != m.size) {
        return false;
    }
    for (std::uint32_t k = 0; k < m.check_count; ++k)
        if (ld32(base + m.checks[2 * k]) != m.checks[2 * k + 1]) return false;
    return true;
}

// The word a call checks turns with each call, so every translated code word
// is proven again within check_count calls: one word per call keeps calls
// cheap while even a one word change of the image is caught in time.
std::uint32_t g_spot_cursor = 0;

const Instance *in_place(const Instance &i) {
    static const bool full = std::getenv("PS2_WINDOW_FULL") != nullptr;
    if (full || !i.module->fixed_base) return loaded_at(*i.module, i.base) ? &i : nullptr;
    if (!i.module->check_count) return &i;
    const std::uint32_t c = g_spot_cursor++ % i.module->check_count;
    return ld32(i.base + i.module->checks[2 * c]) == i.module->checks[2 * c + 1] ? &i : nullptr;
}

// Adds an instance, keeping g_instances in order of rank (higher first, the earlier one first among equals): the first one in place that
// has an entry is the one that runs.
Instance *add_instance(std::uint32_t base, const ModuleInfo *m, int rank, const SoftIndex *soft = nullptr) {
    auto at = g_instances.begin();
    while (at != g_instances.end() && at->rank >= rank) ++at;
    return &*g_instances.insert(at, Instance{base, m, rank, soft && !soft->groups.empty() ? soft : nullptr});
}

const UnitInfo *module_unit(const ModuleInfo &m, std::uint32_t offset) {
    for (std::uint32_t k = 0; k < m.count; ++k)
        if (offset - m.units[k].base < m.units[k].size) return &m.units[k];
    return nullptr;
}

// The translated code of a module at pc: its unit and the entry number there, zero when the module does not have that entry.
std::uint16_t module_entry(const Instance &i, std::uint32_t pc, const UnitInfo *&unit) {
    unit = module_unit(*i.module, pc - i.base);
    const std::uint16_t entry = unit ? unit->entries[(pc - i.base - unit->base) >> 2] : 0;
    if (entry && i.soft) {
        // An entry that only a guess found: it is served while its own first words are what was translated.
        if (const auto at = i.soft->groups.find(pc - i.base); at != i.soft->groups.end()) {
            const std::uint32_t *group = at->second;
            for (std::uint32_t k = 0; k < group[1]; ++k)
                if (ld32(i.base + group[2 + 2 * k]) != group[3 + 2 * k]) return 0;
        }
    }
    return entry;
}

// The module that serves a window or relocatable module at pc. With `needs_entry` the first module in place that has code for pc
// itself: a window can hold several modules of the same content, the ones the build was translated with and the ones added while the
// game ran, each knowing different entry points (ee_live.cpp translates only what the game reached).
const Instance *instance_for(std::uint32_t pc, bool needs_entry = false) {
    const UnitInfo *unit = nullptr;
    bool serving = false;  // a module in place covers pc, but has no code for it
    for (std::size_t k = 0; k < g_instances.size(); ++k) {
        const Instance &i = g_instances[k];
        if (pc - i.base >= i.module->size) continue;
        if (!in_place(i)) {
            g_instances.erase(g_instances.begin() + static_cast<std::ptrdiff_t>(k));
            --k;
            continue;
        }
        if (!needs_entry || module_entry(i, pc, unit)) return &i;
        serving = true;
    }
    if (g_modules.empty() || pc >= kRamSize) return nullptr;
    // A code window rewritten by the game holds fixed base modules, each an
    // image of one content. The ones whose check words are in place serve it,
    // the best ones first.
    for (int rank = g_max_rank; rank >= 0; --rank) {
        for (std::size_t k = 0; k < g_modules.size(); ++k) {
            const ModuleInfo &m = g_modules[k];
            if (g_ranks[k] != rank) continue;
            if (m.fixed_base && pc - m.fixed_base < m.size && loaded_at(m, m.fixed_base)) {
                if (std::any_of(g_instances.begin(), g_instances.end(), [&](const Instance &i) { return i.module == &m; })) continue;
                const Instance *added = add_instance(m.fixed_base, &m, rank, &g_soft[k]);
                if (!needs_entry || module_entry(*added, pc, unit)) return added;
                serving = true;
            }
        }
    }
    if (serving) return nullptr;
    for (std::uint32_t at = pc & ~15u; pc - at < g_module_span; at -= 16) {
        if (const std::uint32_t magic = ld32(at); magic == kModuleMagic || magic == kXffMagic) {
            for (const ModuleInfo &m : g_modules)
                if (pc - at < m.size && loaded_at(m, at)) {
                    return add_instance(at, &m, 0);
                }
            // A stray "xff2" word (a string, data) is not a header: look further back
            if (magic == kModuleMagic) return nullptr;
        }
        if (at < 16) break;
    }
    return nullptr;
}

const UnitInfo *unit_for(std::uint32_t pc) {
    const std::uint32_t slot = (pc - g_origin) / g_unit_size;
    if (pc < g_origin || slot >= g_index.size()) return nullptr;
    return g_index[slot];
}

void record_miss(std::uint32_t pc) {
    if (const Instance *m = instance_for(pc)) {
        char token[96];
        std::snprintf(token, sizeof token, "%s:%x", m->module->name, pc - m->base);
        if (!g_module_misses.insert(token).second || g_options.misses_path.empty()) return;
        std::ofstream(os::path(g_options.misses_path), std::ios::app | std::ios::binary) << token << "\n";
        return;
    }
    if (!g_misses.insert(pc).second || g_options.misses_path.empty()) return;
    std::ofstream out(os::path(g_options.misses_path), std::ios::app | std::ios::binary);
    out << std::hex << pc << "\n";
}

}

void register_units(const UnitInfo *units, std::uint32_t count) {
    g_units.assign(units, units + count);
    if (g_units.empty()) return;
    std::sort(g_units.begin(), g_units.end(), [](const UnitInfo &a, const UnitInfo &b) { return a.base < b.base; });
    g_origin = g_units.front().base;
    g_unit_size = g_units.front().size;
    g_index.assign((g_units.back().base - g_origin) / g_unit_size + 1, nullptr);
    for (const UnitInfo &u : g_units) g_index[(u.base - g_origin) / g_unit_size] = &u;
}

void register_modules(const ModuleInfo *modules, std::uint32_t count) {
    g_modules.assign(modules, modules + count);
    g_ranks.assign(count, 0);
    g_soft.assign(count, SoftIndex{});
    g_max_rank = 0;
    g_instances.clear();
    g_module_span = 0;
    for (std::uint32_t k = 0; k < count; ++k) g_module_span = std::max(g_module_span, modules[k].size);
}

void add_modules(const ModuleInfo *modules, std::uint32_t count) { add_modules_ranked(modules, count, 0); }

void add_modules_ranked(const ModuleInfo *modules, std::uint32_t count, int rank, const SoftChecks *soft) {
    for (std::uint32_t k = 0; k < count; ++k) {
        g_modules.push_back(modules[k]);
        g_ranks.push_back(rank);
        SoftIndex &index = g_soft.emplace_back();
        if (soft && soft[k].words)
            for (std::uint32_t at = 0; at + 2 <= soft[k].count && at + 2 + 2 * soft[k].words[at + 1] <= soft[k].count; at += 2 + 2 * soft[k].words[at + 1])
                index.groups.emplace(soft[k].words[at], soft[k].words + at);
        g_module_span = std::max(g_module_span, modules[k].size);
    }
    // A module of a better rank than the ones in use must be looked at first: the instances made so far are made again when they are
    // needed (they are only a cache of which modules are in place; a function that is running goes on in the code it started in).
    if (rank > g_max_rank) g_max_rank = rank;
    if (rank > 0) g_instances.clear();
}

bool window_module_in_place(std::uint32_t pc) { return instance_for(pc) != nullptr; }

bool window_covered(std::uint32_t base, std::uint32_t size) {
    // A module that holds the range, whatever base it begins at (windows are steps of 16 KiB in one area, and a window inside a
    // translated one holds the same code).
    for (const ModuleInfo &m : g_modules)
        if (m.fixed_base && base >= m.fixed_base && base - m.fixed_base <= m.size && size <= m.size - (base - m.fixed_base) && loaded_at(m, m.fixed_base)) return true;
    return false;
}

std::string module_report(std::uint32_t pc) {
    std::size_t covering = 0, changed = 0;
    std::string example;
    for (const ModuleInfo &m : g_modules) {
        if (!m.fixed_base || pc - m.fixed_base >= m.size) continue;
        ++covering;
        std::uint32_t bad = 0, first = 0;
        for (std::uint32_t k = 0; k < m.check_count; ++k)
            if (ld32(m.fixed_base + m.checks[2 * k]) != m.checks[2 * k + 1] && !bad++) first = m.checks[2 * k];
        if (!bad) continue;
        if (!changed++) {
            char text[160];
            std::snprintf(text, sizeof text, "; first of them: %s, %u of %u checks differ, the first at 0x%08X", m.name, bad, m.check_count, m.fixed_base + first);
            example = text;
        }
    }
    char text[96];
    std::snprintf(text, sizeof text, "%zu modules cover it, %zu of them are not in place", covering, changed);
    return text + example;
}

bool has_code(std::uint32_t pc) {
    const UnitInfo *u = unit_for(pc);
    if (u && u->entries[(pc - u->base) >> 2] != 0) return true;
    return instance_for(pc, true) != nullptr;
}

void dispatch(Cpu &c, std::uint32_t, std::uint32_t depth) {
    const std::uint32_t pc = c.pc;
    if (pc & kUnwind) return;
    if (pc == kHandlerReturn || pc == kIdle) return;
    const UnitInfo *u = unit_for(pc);
    const std::uint16_t id = u ? u->entries[(pc - u->base) >> 2] : 0;
    if (!id) {
        for (bool translated = false;; translated = true) {
            if (const Instance *m = instance_for(pc, true)) {
                const std::uint32_t base = m->base;
                const UnitInfo *mu = nullptr;
                const std::uint16_t entry = module_entry(*m, pc, mu);
                c.base = base;
                mu->function(c, entry, depth);
                return;
            }
            // Code the build does not have: translate it now, while the game waits, and look again (ee_live.cpp, docs/ee-live.md).
            if (translated || !ee::live::translate_missing(pc)) break;
        }
        record_miss(pc);
        char reason[64];
        std::snprintf(reason, sizeof reason, "missing code at 0x%08X (ra 0x%08X)", pc, c.r[31].w[0]);
        std::fprintf(stderr, "[ee] no translated code at 0x%08X (ra 0x%08X)\n", pc, c.r[31].w[0]);
        for (int i = 1; i < 32; i += 4)
            std::fprintf(stderr, "[ee]   r%-2d %08X %08X %08X %08X\n", i, c.r[i].w[0], c.r[i + 1].w[0], c.r[i + 2].w[0], i + 3 < 32 ? c.r[i + 3].w[0] : 0u);
        halt(reason);
        c.pc = pc | kUnwind;
        return;
    }
    u->function(c, id, depth);
}

void call(Cpu &c, std::uint32_t target, std::uint32_t depth) {
    c.pc = target;
    if (depth >= kMaxDepth) return;
    dispatch(c, 0, depth);
}

void bad_entry(Cpu &c, std::uint32_t unit_base) {
    std::fprintf(stderr, "[ee] bad entry into unit 0x%08X for pc 0x%08X\n", unit_base, c.pc);
    halt("bad entry");
    c.pc |= kUnwind;
}

void stop(Cpu &c, std::uint32_t pc, const char *what) {
    std::fprintf(stderr, "[ee] stopped at 0x%08X: %s\n", pc, what);
    halt(what);
    c.pc = pc | kUnwind;
}

void probe(Cpu &c, std::uint32_t pc) {
    std::fprintf(stderr, "[probe] 0x%08X a0 %08X a1 %08X a2 %08X a3 %08X ra %08X v0 %08X\n", pc, c.r[4].w[0], c.r[5].w[0], c.r[6].w[0],
                 c.r[7].w[0], c.r[31].w[0], c.r[2].w[0]);
}

void breakpoint(Cpu &c, std::uint32_t pc) {
    std::fprintf(stderr, "[ee] break at 0x%08X (ra 0x%08X)\n", pc, c.r[31].w[0]);
}

void trap(Cpu &c, std::uint32_t pc) {
    std::fprintf(stderr, "[ee] trap at 0x%08X (ra 0x%08X)\n", pc, c.r[31].w[0]);
}

}
