#include "hw.hpp"
#include "ipu/ipu.hpp"
#include "vu_micro.hpp"
#include "ps2/memory.hpp"
#include "ps2/system.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ps2 {

alignas(64) std::uint8_t g_ram[kRamSize + 64];
alignas(64) std::uint8_t g_spr[kScratchpadSize];
alignas(64) std::uint8_t g_vu0_code[4096];
alignas(64) std::uint8_t g_vu0_mem[4096];
alignas(64) std::uint8_t g_vu1_code[16384];
alignas(64) std::uint8_t g_vu1_mem[16384];

namespace {

enum class Region { Ram, Scratchpad, Io, Vu, Rom, None };

}

// A watchpoint for porting work: PS2_WATCH=base,len makes every write into
// that range of main memory print itself, guest store or bulk copy, with the
// frame and the cpu state at the time.
bool g_watch_on = false;
std::uint32_t g_watch_base = 0, g_watch_end = 0;

void init_watch() {
    if (const char *spec = std::getenv("PS2_WATCH")) {
        std::uint32_t base = 0, len = 0;
        if (std::sscanf(spec, "%x,%x", &base, &len) == 2 && len) {
            g_watch_base = base & (kRamSize - 1);
            g_watch_end = g_watch_base + len;
            g_watch_on = true;
            std::fprintf(stderr, "[watch] writes to 0x%08X..0x%08X will print\n", g_watch_base, g_watch_end);
        }
    }
}

void watch_hit(std::uint32_t a, std::uint32_t v, const char *via) {
    const std::uint8_t *at = (a < kRamSize) ? g_ram + a : ((a >> 14) == (kScratchpadBase >> 14) ? g_spr + (a & (kScratchpadSize - 1)) : nullptr);
    std::uint32_t was = 0;
    if (at) std::memcpy(&was, at, 4);
    std::fprintf(stderr, "[watch] frame %llu: write %08X = %08X (was %08X) via %s, pc 0x%08X ra 0x%08X\n",
                 static_cast<unsigned long long>(frame_count()), a, v, was, via, g_cpu.pc, g_cpu.r[31].w[0]);
    static const bool regs = std::getenv("PS2_WATCH_REGS") != nullptr;  // PS2_WATCH_REGS=1: also a0-a3, s0-s7 and sp at each write
    if (regs) {
        std::fprintf(stderr, "[watch]   a0-a3");
        for (int i = 4; i < 8; ++i) std::fprintf(stderr, " %08X", g_cpu.r[i].w[0]);
        std::fprintf(stderr, " s0-s7");
        for (int i = 16; i < 24; ++i) std::fprintf(stderr, " %08X", g_cpu.r[i].w[0]);
        std::fprintf(stderr, " sp %08X\n", g_cpu.r[29].w[0]);
    }
}

void watch_span(std::uint32_t a, std::uint32_t n, const char *via) {
    if (!g_watch_on) return;
    const std::uint32_t s = a & (kRamSize - 1), e = s + n;
    if (e <= g_watch_base || s >= g_watch_end) return;
    for (std::uint32_t k = s & ~3u; k < e; k += 4) {
        if (k < g_watch_base || k + 4 > g_watch_end) continue;
        std::uint32_t v;
        std::memcpy(&v, g_ram + k, 4);
        std::fprintf(stderr, "[watch] frame %llu: span from %08X: word %08X = %08X via %s, pc 0x%08X\n",
                     static_cast<unsigned long long>(frame_count()), a, k, v, via, g_cpu.pc);
    }
}

namespace {

struct Target {
    Region region;
    std::uint8_t *host;
    std::uint32_t address;
};

std::uint8_t *vu_memory(std::uint32_t p) {
    switch ((p >> 12) & 0xFu) {
    case 0x0: return g_vu0_code + (p & 0xFFFu);
    case 0x4: return g_vu0_mem + (p & 0xFFFu);
    case 0x8: case 0x9: case 0xA: case 0xB: return g_vu1_code + (p & 0x3FFFu);
    case 0xC: case 0xD: case 0xE: case 0xF: return g_vu1_mem + (p & 0x3FFFu);
    default: return nullptr;
    }
}

Target resolve(std::uint32_t a) {
    ++hw::g_slow_seq;
    if ((a >> 14) == (kScratchpadBase >> 14)) return {Region::Scratchpad, g_spr + (a & (kScratchpadSize - 1)), a};
    const std::uint32_t p = physical(a);
    if (p < 0x10000000u) return {Region::Ram, g_ram + (p & (kRamSize - 1)), p};
    if (p >= 0x11000000u && p < 0x11010000u) {
        if (std::uint8_t *host = vu_memory(p)) {
            // VU1 runs on its own thread beside the emulation: what the game wrote to it with the VIF1 must have landed before it
            // reads or writes the memory itself, and VU1 must be done with it (a game asks the status registers, which say "idle").
            if (p & 0x8000u) {
                static const bool trace = std::getenv("PS2_TRACE_VU_ACCESS") != nullptr;
                static int shown = 0;
                if (trace && shown++ < 40) std::fprintf(stderr, "[vu1-ee] the EE touches VU1 memory at 0x%08X (pc 0x%08X, frame %llu)\n", p, g_cpu.pc, static_cast<unsigned long long>(frame_count()));
                drain_vu_thread();
            }
            return {Region::Vu, host, p};
        }
    }
    if (p >= 0x10000000u && p < 0x13000000u) return {Region::Io, nullptr, p};
    if (p >= 0x1FC00000u && p < 0x20000000u) return {Region::Rom, nullptr, p};
    return {Region::None, nullptr, p};
}

// PS2_TRAP_UNMAPPED=1 (porting work): the first access to an address that is not memory also prints the main registers and the
// return addresses found on the stack (a word that points just behind a jal/jalr counts), the guest's call chain, so one can see
// who handed the function its bad pointer. The scan is a guess: a left-over value on the stack can look like a return address.
void trap_unmapped(const char *what, std::uint32_t a) {
    static const bool on = std::getenv("PS2_TRAP_UNMAPPED") != nullptr;
    static int shown = 0;
    if (!on || shown++ >= 3) return;
    std::fprintf(stderr, "[trap] %s at 0x%08X, frame %llu, pc 0x%08X\n", what, a, static_cast<unsigned long long>(frame_count()), g_cpu.pc);
    static const char *const names[32] = {"zero", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
                                          "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"};
    for (int i = 1; i < 32; ++i) std::fprintf(stderr, "%s=%08X%s", names[i], g_cpu.r[i].w[0], (i % 8 == 7) ? "\n" : " ");
    std::fprintf(stderr, "\n[trap] return addresses on the stack:");
    const std::uint32_t sp = g_cpu.r[29].w[0];
    for (std::uint32_t off = 0; off < 0x800 && (sp & (kRamSize - 1)) + off + 4 <= kRamSize; off += 4) {
        std::uint32_t v, before;
        std::memcpy(&v, g_ram + ((sp + off) & (kRamSize - 1)), 4);
        if (v < 0x100000u || v >= 0x800000u || (v & 3u)) continue;
        std::memcpy(&before, g_ram + (v - 8), 4);
        const bool jal = (before >> 26) == 3u, jalr = (before >> 26) == 0u && (before & 0x3Fu) == 9u;
        if (jal || jalr) std::fprintf(stderr, " [sp+%X]=%08X%s", off, v, jal ? "" : "(jalr)");
    }
    std::fprintf(stderr, "\n");
}

void unmapped(const char *what, std::uint32_t a) {
    std::fprintf(stderr, "[mem] %s at unmapped 0x%08X (pc 0x%08X)\n", what, a, g_cpu.pc);
    trap_unmapped(what, a);
}

template <typename T>
T read_slow(std::uint32_t a) {
    const Target t = resolve(a);
    if (t.host) {
        T v;
        std::memcpy(&v, t.host, sizeof v);
        return v;
    }
    if (t.region == Region::Io) {
        if constexpr (sizeof(T) == 8) return hw::read64(t.address);
        else {
            const std::uint32_t word = hw::read32(t.address & ~3u);
            return static_cast<T>(word >> ((t.address & 3u) * 8u));
        }
    }
    if (t.region != Region::Rom) unmapped("read", a);
    return 0;
}

template <typename T>
void write_slow(std::uint32_t a, T v) {
    const Target t = resolve(a);
    if (t.host) {
        if (g_watch_on && t.region == Region::Ram) {
            const std::uint32_t base = t.address & (kRamSize - 1);
            if (base >= g_watch_base && base < g_watch_end) watch_hit(base, static_cast<std::uint32_t>(v), "store-slow");
        }
        std::memcpy(t.host, &v, sizeof v);
        if (t.region == Region::Vu && (t.address & 0x4000u) == 0) vu::code_changed((t.address >> 15) & 1u);
        return;
    }
    if (t.region == Region::Io) {
        if constexpr (sizeof(T) == 8) hw::write64(t.address, v);
        else hw::write32(t.address, static_cast<std::uint32_t>(v));
        return;
    }
    unmapped("write", a);
}

}

std::uint32_t physical(std::uint32_t a) {
    if (a >= 0x80000000u && a < 0xC0000000u) return a & 0x1FFFFFFFu;
    const std::uint32_t top = a >> 28;
    if (top == 2u || top == 3u) return a & 0x0FFFFFFFu;
    return a;
}

namespace {

constexpr std::uint32_t kIpuBase = 0x10002000u;  // the IPU registers, 0x10002000..0x1000203F

// Short ways for the two kinds of access a film player makes by the million (God of War II's opening film: 26 million accesses to the
// RAM mirrors and 16 million to the IPU registers in a 2,600 frame run). They give what the long way through resolve() and hw::read32 /
// hw::write32 gives, bit for bit; the access counter for the waiting-loop skip (hw::g_slow_seq) is still moved once per access.
//  - RAM through a mirror: the inline path in memory.hpp only takes the first 32 MB at their own address; the same memory seen at
//    0x20000000 / 0x30000000 (uncached), 0x80000000 / 0xA0000000 and so on comes here.
//  - IPU registers, 32 and 64 bits: hw.cpp hands them to ipu::read32 / ipu::write32 after checks that no IPU address passes.
inline std::uint8_t *ram_mirror(std::uint32_t p) { return p < 0x10000000u ? g_ram + (p & (kRamSize - 1)) : nullptr; }

template <typename T>
inline T load_fast(std::uint32_t a) {
    const std::uint32_t p = physical(a);
    if (const std::uint8_t *host = ram_mirror(p)) {
        ++hw::g_slow_seq;
        return load<T>(host);
    }
    if constexpr (sizeof(T) >= 4) {
        if (p - kIpuBase < (sizeof(T) == 8 ? 0x38u : 0x40u)) {
            ++hw::g_slow_seq;
            if constexpr (sizeof(T) == 8) return ipu::read32(p) | (static_cast<std::uint64_t>(ipu::read32(p + 4)) << 32);
            else return static_cast<T>(ipu::read32(p & ~3u) >> ((p & 3u) * 8u));
        }
    }
    return read_slow<T>(a);
}

template <typename T>
inline void store_fast(std::uint32_t a, T v) {
    const std::uint32_t p = physical(a);
    if (std::uint8_t *host = ram_mirror(p)) {
        ++hw::g_slow_seq;
        if (g_watch_on) {
            const std::uint32_t base = p & (kRamSize - 1);
            if (base >= g_watch_base && base < g_watch_end) watch_hit(base, static_cast<std::uint32_t>(v), "store-slow");
        }
        store(host, v);
        return;
    }
    if constexpr (sizeof(T) >= 4) {
        if (p - kIpuBase < (sizeof(T) == 8 ? 0x38u : 0x40u)) {
            ++hw::g_slow_seq;
            if constexpr (sizeof(T) == 8) {
                ipu::write32(p, static_cast<std::uint32_t>(v));
                ipu::write32(p + 4, static_cast<std::uint32_t>(v >> 32));
            } else {
                ipu::write32(p, v);
            }
            return;
        }
    }
    write_slow(a, v);
}

}

std::uint8_t slow_ld8(std::uint32_t a) { return load_fast<std::uint8_t>(a); }
std::uint16_t slow_ld16(std::uint32_t a) { return load_fast<std::uint16_t>(a); }
std::uint32_t slow_ld32(std::uint32_t a) {
    // INTC_STAT is read in a tight loop by every game that waits for the next picture (sceGsSyncV): a short way for it, same value
    if (a == 0x1000F000u || a == 0xB000F000u) return hw::read_intc_stat();
    return load_fast<std::uint32_t>(a);
}
std::uint64_t slow_ld64(std::uint32_t a) { return load_fast<std::uint64_t>(a); }

void slow_ld128(std::uint32_t a, Reg128 &out) {
    if (const std::uint8_t *host = ram_mirror(physical(a))) {
        ++hw::g_slow_seq;
        std::memcpy(&out, host, 16);
        return;
    }
    const Target t = resolve(a);
    if (t.host) std::memcpy(&out, t.host, 16);
    else if (t.region == Region::Io) hw::read128(t.address, out);
    else std::memset(&out, 0, 16);
}

void slow_st8(std::uint32_t a, std::uint8_t v) { store_fast(a, v); }
void slow_st16(std::uint32_t a, std::uint16_t v) { store_fast(a, v); }
void slow_st32(std::uint32_t a, std::uint32_t v) { store_fast(a, v); }
void slow_st64(std::uint32_t a, std::uint64_t v) { store_fast(a, v); }

void slow_st128(std::uint32_t a, const Reg128 &v) {
    if (std::uint8_t *host = ram_mirror(physical(a))) {
        ++hw::g_slow_seq;
        std::memcpy(host, &v, 16);
        return;
    }
    const Target t = resolve(a);
    if (t.host) std::memcpy(t.host, &v, 16);
    else if (t.region == Region::Io) hw::write128(t.address, v);
    else unmapped("write128", a);
}

}
