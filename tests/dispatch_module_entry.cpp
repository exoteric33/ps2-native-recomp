// A relocatable module (SNR2 or xff2 header) found by the backwards header scan must answer for a missing entry the same way on the
// first lookup as on every later one. No game needed: ninja ps2dispatchmoduletest.
#include "ps2/generated.hpp"
#include "ps2/system.hpp"

#include <cstdio>
#include <cstring>

namespace {
int g_calls = 0;
int g_calls_entry_zero = 0;
void unit_fn(ps2::Cpu &c, std::uint32_t entry, std::uint32_t) {
    ++g_calls;
    if (!entry) ++g_calls_entry_zero;
    ps2::set32(c.r[2], entry);
    c.pc = c.r[31].w[0];
}

constexpr std::uint32_t kBase = 0x01000000;
constexpr std::uint32_t kSize = 0x100;
constexpr std::uint32_t kKnown = 0x40, kHole = 0x48;   // the entry at +0x40 exists, +0x48 is inside that function but has no entry
std::uint16_t g_entries[kSize / 4];
std::uint32_t g_checks[8];
ps2::UnitInfo g_unit{0, kSize, unit_fn, g_entries};

int g_failed = 0;
void check(bool ok, const char *what, const char *format) {
    std::printf("  %s %s: %s\n", ok ? "ok  " : "FAIL", format, what);
    if (!ok) ++g_failed;
}

void run(const char *format, std::uint32_t magic, std::uint32_t size_at) {
    std::memset(ps2::g_ram + kBase, 0, kSize);
    std::memcpy(ps2::g_ram + kBase, &magic, 4);
    const std::uint32_t size = kSize;
    std::memcpy(ps2::g_ram + kBase + size_at, &size, 4);
    const std::uint32_t code = 0x03E00008;   // jr ra, so the "code" is not all zero
    std::memcpy(ps2::g_ram + kBase + kKnown, &code, 4);
    std::memset(g_entries, 0, sizeof g_entries);
    g_entries[kKnown / 4] = 1;
    g_checks[0] = 0;       g_checks[1] = magic;
    g_checks[2] = kKnown;  g_checks[3] = code;
    const ps2::ModuleInfo module{"synthetic", 0, kSize, g_checks, 2, &g_unit, 1};
    const ps2::UnitInfo none{};

    // cold, the entry exists
    ps2::register_units(&none, 0);
    ps2::register_modules(&module, 1);
    check(ps2::has_code(kBase + kKnown), "cold lookup of a known entry has code", format);
    check(ps2::has_code(kBase + kKnown), "warm lookup of a known entry has code", format);

    // cold, the hole: the first lookup is the one that finds the module
    ps2::register_modules(&module, 1);
    const bool cold = ps2::has_code(kBase + kHole);
    const bool warm = ps2::has_code(kBase + kHole);
    check(!cold, "cold lookup of a missing entry says no", format);
    check(!warm, "warm lookup of a missing entry says no", format);

    // the hole through dispatch, cold: it must take the miss path, not call the unit with entry 0
    ps2::register_modules(&module, 1);
    g_calls = g_calls_entry_zero = 0;
    ps2::Cpu c{};
    c.pc = kBase + kHole;
    c.r[31].w[0] = 0x00100000;
    ps2::dispatch(c, 0, 0);
    check(g_calls == 0 && g_calls_entry_zero == 0, "cold dispatch into the hole does not run the unit", format);
    check(c.pc == ((kBase + kHole) | ps2::kUnwind), "cold dispatch into the hole takes the miss path (nothing translates it, the call unwinds)", format);

    // the known entry through dispatch, cold: runs the unit with its entry number
    ps2::register_modules(&module, 1);
    g_calls = g_calls_entry_zero = 0;
    ps2::Cpu d{};
    d.pc = kBase + kKnown;
    d.r[31].w[0] = 0x00100000;
    ps2::dispatch(d, 0, 0);
    check(g_calls == 1 && g_calls_entry_zero == 0 && d.r[2].w[0] == 1, "cold dispatch to the known entry runs the unit with entry 1", format);
}
}

int main() {
    std::printf("relocatable module lookup, missing entry:\n");
    run("SNR2", 0x32524E53u, 0x2C);
    run("xff2", 0x32666678u, 0x14);
    if (g_failed) {
        std::printf("dispatch module entry: %d checks FAILED\n", g_failed);
        return 1;
    }
    std::printf("dispatch module entry: PASS\n");
    return 0;
}
