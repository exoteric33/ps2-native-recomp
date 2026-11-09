#include "vif.hpp"

#include "dmac.hpp"
#include "gs/gif.hpp"
#include "gs/queue.hpp"
#include "hw.hpp"
#include "vu_micro.hpp"

#include "ps2/state.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#if defined(__SSE4_1__)
#include <immintrin.h>
#endif

namespace ps2::vif {

namespace {

struct Unit {
    std::uint32_t stat = 0, err = 0, mark = 0, cycle = 0, mode = 0, num = 0, mask = 0, code = 0;
    std::uint32_t itops = 0, base = 0, ofst = 0, tops = 0, itop = 0, top = 0, vu_addr = 0;
    std::uint32_t row[4] = {}, col[4] = {};
    bool dbf = false, stalled = false, vu_pending = false;
    std::vector<std::uint32_t> pending;
};

constexpr std::uint32_t kStatMrk = 0x40u, kStatVis = 0x400u, kStatInt = 0x800u;
// What the DMA can push into a stalled unit: 4 quadwords on VIF0, 8 on VIF1.
constexpr std::uint32_t kFifoWords[2] = {16u, 32u};

Unit g_units[2];
Unit g_vu_side;
// How many programs VIF1 started on VU1, and how many of those it held back until the UNPACKs behind the MSCAL were done (run_vu).
// Only the thread that really runs VU1 counts; the emulation thread's shadow copy does not.
std::atomic<std::uint64_t> g_vu_starts{0}, g_vu_held{0};  // one writer (the thread that runs VU1), read at the end of a run and in a report
void count_up(std::atomic<std::uint64_t> &n) { n.store(n.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed); }
// PS2_VIF_DEFER=0 starts every program at its MSCAL, as before the deferral (public pull request 64) was added: a way to tell whether
// a game's trouble comes from it.
bool defer_enabled() {
    static const bool on = [] {
        const char *e = std::getenv("PS2_VIF_DEFER");
        return !(e && e[0] == '0');
    }();
    return on;
}
std::uint64_t g_commands[2][128];
std::uint8_t g_dry_run[16384];

bool on_vu_thread() { return gs::queue::role() == gs::queue::Role::Vu; }
bool shadow(int unit) { return unit == 1 && gs::queue::vu_active() && !on_vu_thread(); }
Unit &state(int unit) { return unit == 1 && on_vu_thread() ? g_vu_side : g_units[unit]; }

// The last 8 commands a unit decoded, for the log when a unit loses its place in the stream (a float or a vertex read as a command).
// Three slots: VIF0, VIF1 and VIF1's copy on the VU thread. Not part of the save state. Per command the cost is three stores.
struct Seen {
    std::uint32_t cmd = 0, words = 0;
    std::uint64_t at = 0;  // the command's place in the stream, counted in words from the last reset
};
struct History {
    Seen last[8];
    std::uint32_t head = 0;
    std::uint64_t received = 0, unknown = 0;
};
History g_hist[3];
constexpr const char *kLabel[3] = {"vif0", "vif1", "vif1/vu"};

int hist_index(int unit) { return unit == 1 && on_vu_thread() ? 2 : unit; }

const char *command_name(std::uint32_t cmd) {
    const std::uint32_t op = (cmd >> 24) & 0x7Fu;
    switch (op) {
    case 0x00: return "NOP";
    case 0x01: return "STCYCL";
    case 0x02: return "OFFSET";
    case 0x03: return "BASE";
    case 0x04: return "ITOP";
    case 0x05: return "STMOD";
    case 0x06: return "MSKPATH3";
    case 0x07: return "MARK";
    case 0x10: return "FLUSHE";
    case 0x11: return "FLUSH";
    case 0x13: return "FLUSHA";
    case 0x14: return "MSCAL";
    case 0x15: return "MSCALF";
    case 0x17: return "MSCNT";
    case 0x20: return "STMASK";
    case 0x30: return "STROW";
    case 0x31: return "STCOL";
    case 0x4A: return "MPG";
    case 0x50: return "DIRECT";
    case 0x51: return "DIRECTHL";
    default: return (op & 0x60u) == 0x60u ? "UNPACK" : "unknown";
    }
}

// The first unknown command of a unit prints the commands before it and the DMA channel's registers, a few more print short,
// then only a count: a unit that lost its place reads every following word as a command, thousands of lines.
void note_unknown(int unit, std::uint32_t cmd) {
    const int idx = hist_index(unit);
    History &h = g_hist[idx];
    const char *who = kLabel[idx];
    const std::uint64_t n = ++h.unknown;
    const Seen &now = h.last[(h.head - 1) & 7u];
    if (n == 1) {
        float as_float;
        std::memcpy(&as_float, &cmd, 4);
        std::fprintf(stderr, "[%s] lost the stream at word %llu: 0x%08X is not a command (as a float: %g). The commands before it, oldest first:\n",
                     who, static_cast<unsigned long long>(now.at), cmd, static_cast<double>(as_float));
        const std::uint32_t before = std::min<std::uint32_t>(h.head - 1, 7u);
        for (std::uint32_t i = before; i >= 1; --i) {
            const Seen &s = h.last[(h.head - 1 - i) & 7u];
            std::fprintf(stderr, "[%s]   word %llu: 0x%08X %s, %u data words\n", who, static_cast<unsigned long long>(s.at), s.cmd, command_name(s.cmd), s.words);
        }
        if (!before) std::fprintf(stderr, "[%s]   (none: it is the first command since the reset)\n", who);
        if (idx < 2) {
            const std::uint32_t base = idx ? 0x10009000u : 0x10008000u;
            const std::uint32_t chcr = dmac::read(base);
            std::fprintf(stderr, "[%s]   DMA channel %d now: CHCR 0x%08X (last tag id %u) MADR 0x%08X QWC %u TADR 0x%08X; the data may also have come through the direct FIFO port\n",
                         who, idx, chcr, (chcr >> 28) & 7u, dmac::read(base + 0x10), dmac::read(base + 0x20), dmac::read(base + 0x30));
        }
    } else if (n <= 5) {
        std::fprintf(stderr, "[%s] unknown command 0x%08X at word %llu\n", who, cmd, static_cast<unsigned long long>(now.at));
        if (n == 5) std::fprintf(stderr, "[%s] more unknown commands are counted, not printed\n", who);
    } else if (n == 100 || n == 10000 || n == 1000000 || n == 100000000) {
        std::fprintf(stderr, "[%s] %llu unknown commands so far\n", who, static_cast<unsigned long long>(n));
    }
}

std::uint8_t *vu_data(int unit) { return unit ? g_vu1_mem : g_vu0_mem; }
std::uint32_t vu_qwords(int unit) { return unit ? 1024u : 256u; }

std::uint32_t element_bits(std::uint32_t vl) {
    switch (vl) {
    case 0: return 32;
    case 1: return 16;
    case 2: return 8;
    default: return 16;
    }
}

// A write length of 0 acts like 256: the whole UNPACK is one write cycle, a fill write after the first CL.
std::uint32_t write_length(const Unit &u) {
    const std::uint32_t wl = (u.cycle >> 8) & 0xFFu;
    return wl ? wl : 256u;
}

// CYCLE = 0 (the state after power-on, before any STCYCL, or an explicit STCYCL 0) acts like CL = WL: a normal write, every output
// takes its own data and nothing is skipped. FIFA Street 2 sends an UNPACK there with its data behind it (Ridge Racer V's log shows
// CL 0 but not WL), and the stream stays in step on the console. A cycle length of 0 with a write length above 0 is not that: Tekken 5 sends STCYCL 0x0500, a
// STMASK and then several UNPACK headers back to back with no data, so that stays a filling write that takes none. What the console does
// at CL 0 is not measured (dma/vif/stcycl shows only where the quadwords land, and no test unpacks with CL 0); see docs/vif-cycle.md.
std::uint32_t cycle_length(const Unit &u, std::uint32_t wl) {
    const std::uint32_t cl = u.cycle & 0xFFu;
    return cl == 0 && (u.cycle & 0xFF00u) == 0 ? wl : cl;
}

std::uint32_t unpack_words(std::uint32_t cmd, const Unit &u, std::uint32_t num) {
    const std::uint32_t vn = (cmd >> 26) & 3u, vl = (cmd >> 24) & 3u;
    const std::uint32_t wl = write_length(u), cl = cycle_length(u, wl);
    std::uint32_t data = num;
    if (wl > cl) data = (num / wl) * cl + std::min(num % wl, cl);
    const std::uint32_t bits = (vn == 3 && vl == 3) ? 16u : (vn + 1) * element_bits(vl);
    return (data * bits + 31) / 32;
}

// What run() works out once per call instead of once per command: which copy of the unit it is (the vector thread keeps its own,
// g_vu_side) and whether it is the emulation thread only following VIF1 (shadow). Each of the two asks a function in another file.
struct Ctx {
    Unit &u;
    bool vu_thread;
    bool dry;
};

void flush_vu(int unit) {
    if (unit != 1) return;
    Unit &u = state(1);
    if (!u.vu_pending) return;
    u.vu_pending = false;
    if (shadow(1)) return;
    vu::set_tops(u.top, u.itop);
    vu::run(1, u.vu_addr);
}

void run_vu(int unit, std::uint32_t address, const Ctx &c) {
    Unit &u = c.u;
    if (unit == 1) {
        flush_vu(1);
        u.top = u.tops;
        u.itop = u.itops;
        u.dbf = !u.dbf;
        u.tops = u.base + (u.dbf ? u.ofst : 0);
        if (!c.dry) count_up(g_vu_starts);
        if (u.ofst != 0 && defer_enabled()) {
            if (!c.dry) count_up(g_vu_held);
            u.vu_pending = true;
            u.vu_addr = address;
            return;
        }
        if (c.dry) return;
        vu::set_tops(u.top, u.itop);
    } else {
        u.itop = u.itops;  // VU0 reads it from here (vif::itop); VU1's TOP and ITOP are not VU0's to change
    }
    vu::run(unit, address);
}

#if defined(__SSE4_1__)
// One element of an UNPACK as the four words it becomes in the vector unit's memory: sign or zero extended, a one-component element
// repeated, a two-component element as x y x y, a three-component one with w = 0. Reads exactly the bytes of the element (the last
// element of a command may end the data), so it never looks past them.
template <std::uint32_t Bits, std::uint32_t Components, bool Usn>
__attribute__((always_inline)) inline __m128i unpack_element(const std::uint8_t *p) {
    if constexpr (Bits == 32) {
        if constexpr (Components == 4) {
            return _mm_loadu_si128(reinterpret_cast<const __m128i *>(p));
        } else if constexpr (Components == 3) {
            std::uint32_t c;
            std::memcpy(&c, p + 8, 4);
            return _mm_insert_epi32(_mm_loadl_epi64(reinterpret_cast<const __m128i *>(p)), static_cast<int>(c), 2);
        } else if constexpr (Components == 2) {
            return _mm_shuffle_epi32(_mm_loadl_epi64(reinterpret_cast<const __m128i *>(p)), 0x44);
        } else {
            std::uint32_t a;
            std::memcpy(&a, p, 4);
            return _mm_set1_epi32(static_cast<int>(a));
        }
    } else if constexpr (Bits == 16) {
        if constexpr (Components == 1) {
            std::uint16_t h;
            std::memcpy(&h, p, 2);
            return _mm_set1_epi32(Usn ? static_cast<int>(h) : static_cast<int>(static_cast<std::int16_t>(h)));
        } else {
            __m128i x;
            if constexpr (Components == 4) {
                x = _mm_loadl_epi64(reinterpret_cast<const __m128i *>(p));
            } else {
                std::uint32_t a;
                std::memcpy(&a, p, 4);
                x = _mm_cvtsi32_si128(static_cast<int>(a));
                if constexpr (Components == 3) {
                    std::uint16_t c;
                    std::memcpy(&c, p + 4, 2);
                    x = _mm_insert_epi16(x, c, 2);
                }
            }
            const __m128i wide = Usn ? _mm_cvtepu16_epi32(x) : _mm_cvtepi16_epi32(x);
            if constexpr (Components == 2) return _mm_shuffle_epi32(wide, 0x44);
            else return wide;
        }
    } else {
        if constexpr (Components == 1) {
            return _mm_set1_epi32(Usn ? static_cast<int>(p[0]) : static_cast<int>(static_cast<std::int8_t>(p[0])));
        } else {
            std::uint32_t a;
            if constexpr (Components == 4) {
                std::memcpy(&a, p, 4);
            } else if constexpr (Components == 3) {
                std::uint16_t h;
                std::memcpy(&h, p, 2);
                a = h | (static_cast<std::uint32_t>(p[2]) << 16);
            } else {
                std::uint16_t h;
                std::memcpy(&h, p, 2);
                a = h;
            }
            const __m128i x = _mm_cvtsi32_si128(static_cast<int>(a));
            const __m128i wide = Usn ? _mm_cvtepu8_epi32(x) : _mm_cvtepi8_epi32(x);
            if constexpr (Components == 2) return _mm_shuffle_epi32(wide, 0x44);
            else return wide;
        }
    }
}

template <std::uint32_t Bits, std::uint32_t Components, bool Usn>
void unpack_fast(std::uint8_t *mem, std::uint32_t mask, std::uint32_t addr, std::uint32_t wl, std::uint32_t skip, const std::uint8_t *bytes, std::uint32_t num) {
    constexpr std::size_t stride = static_cast<std::size_t>(Components) * Bits / 8;
    if (skip == 0) {
        for (std::uint32_t n = 0; n < num; ++n)
            _mm_storeu_si128(reinterpret_cast<__m128i *>(mem + static_cast<std::size_t>((addr + n) & mask) * 16), unpack_element<Bits, Components, Usn>(bytes + n * stride));
        return;
    }
    std::uint32_t cycle = 0;
    for (std::uint32_t n = 0; n < num; ++n) {
        _mm_storeu_si128(reinterpret_cast<__m128i *>(mem + static_cast<std::size_t>(addr & mask) * 16), unpack_element<Bits, Components, Usn>(bytes + n * stride));
        ++addr;
        if (++cycle >= wl) {
            addr += skip;
            cycle = 0;
        }
    }
}
#else
template <std::uint32_t Bits, std::uint32_t Components, bool Usn>
void unpack_fast(std::uint8_t *mem, std::uint32_t mask, std::uint32_t addr, std::uint32_t wl, std::uint32_t skip, const std::uint8_t *bytes, std::uint32_t num) {
    std::uint32_t cycle = 0;
    for (std::uint32_t n = 0; n < num; ++n) {
        std::uint32_t value[4];
        for (std::uint32_t j = 0; j < Components; ++j) {
            const std::size_t at = static_cast<std::size_t>(n) * Components + j;
            if constexpr (Bits == 32) {
                std::memcpy(&value[j], bytes + at * 4, 4);
            } else if constexpr (Bits == 16) {
                std::uint16_t h;
                std::memcpy(&h, bytes + at * 2, 2);
                value[j] = Usn ? h : static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int16_t>(h)));
            } else {
                value[j] = Usn ? bytes[at] : static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int8_t>(bytes[at])));
            }
        }
        if constexpr (Components == 1) {
            value[1] = value[2] = value[3] = value[0];
        } else if constexpr (Components == 2) {
            value[2] = value[0];
            value[3] = value[1];
        } else if constexpr (Components == 3) {
            value[3] = 0;
        }
        std::memcpy(mem + (addr & mask) * 16, value, 16);
        ++addr;
        if (++cycle >= wl) {
            addr += skip;
            cycle = 0;
        }
    }
}

#endif

using FastUnpack = void (*)(std::uint8_t *, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, const std::uint8_t *, std::uint32_t);

template <std::uint32_t Bits>
constexpr FastUnpack fast_pair(std::uint32_t components, bool usn) {
    switch (components) {
    case 1: return usn ? unpack_fast<Bits, 1, true> : unpack_fast<Bits, 1, false>;
    case 2: return usn ? unpack_fast<Bits, 2, true> : unpack_fast<Bits, 2, false>;
    case 3: return usn ? unpack_fast<Bits, 3, true> : unpack_fast<Bits, 3, false>;
    default: return usn ? unpack_fast<Bits, 4, true> : unpack_fast<Bits, 4, false>;
    }
}

constexpr FastUnpack kFastUnpack[4][4][2] = {
    {{fast_pair<32>(1, false), fast_pair<32>(1, true)}, {fast_pair<32>(2, false), fast_pair<32>(2, true)}, {fast_pair<32>(3, false), fast_pair<32>(3, true)}, {fast_pair<32>(4, false), fast_pair<32>(4, true)}},
    {{fast_pair<16>(1, false), fast_pair<16>(1, true)}, {fast_pair<16>(2, false), fast_pair<16>(2, true)}, {fast_pair<16>(3, false), fast_pair<16>(3, true)}, {fast_pair<16>(4, false), fast_pair<16>(4, true)}},
    {{fast_pair<8>(1, false), fast_pair<8>(1, true)}, {fast_pair<8>(2, false), fast_pair<8>(2, true)}, {fast_pair<8>(3, false), fast_pair<8>(3, true)}, {fast_pair<8>(4, false), fast_pair<8>(4, true)}},
    {{fast_pair<16>(1, false), fast_pair<16>(1, true)}, {fast_pair<16>(2, false), fast_pair<16>(2, true)}, {fast_pair<16>(3, false), fast_pair<16>(3, true)}, {fast_pair<16>(4, false), fast_pair<16>(4, true)}},
};

void unpack(int unit, std::uint32_t cmd, const std::uint32_t *data, const Ctx &c) {
    Unit &u = c.u;
    const bool dry = c.dry;
    if (dry && u.mode < 2) {
        u.num = 0;
        return;
    }
    const std::uint32_t vn = (cmd >> 26) & 3u, vl = (cmd >> 24) & 3u;
    const bool masked = (cmd >> 28) & 1u;
    const bool usn = (cmd >> 14) & 1u;
    std::uint32_t num = (cmd >> 16) & 0xFFu;
    if (!num) num = 256;
    std::uint32_t addr = cmd & 0x3FFu;
    if (unit == 1 && ((cmd >> 15) & 1u)) addr += u.tops;
    const std::uint32_t wl = write_length(u), cl = cycle_length(u, wl);
    const std::uint32_t components = vn + 1;
    const std::uint32_t bits = element_bits(vl);
    std::size_t bit_pos = 0;
    auto read_element = [&]() -> std::uint32_t {
        const std::size_t word = bit_pos / 32, shift = bit_pos % 32;
        std::uint32_t raw = data[word] >> shift;
        if (bits < 32) raw &= (1u << bits) - 1u;
        bit_pos += bits;
        if (bits == 16) return usn ? raw : static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int16_t>(raw)));
        if (bits == 8) return usn ? raw : static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int8_t>(raw)));
        return raw;
    };
    std::uint32_t cycle = 0;
    std::uint8_t *mem = dry ? g_dry_run : vu_data(unit);
    const std::uint32_t wrap = vu_qwords(unit);
    if (!masked && u.mode == 0 && !(wl > cl) && !(vn == 3 && vl == 3)) {
        kFastUnpack[vl][vn][usn ? 1 : 0](mem, wrap - 1, addr, wl, cl - wl, reinterpret_cast<const std::uint8_t *>(data), num);
        u.num = 0;
        return;
    }
    for (std::uint32_t n = 0; n < num; ++n) {
        const bool fill = wl > cl && cycle >= cl;
        std::uint32_t value[4] = {0, 0, 0, 0};
        if (!fill) {
            if (vn == 3 && vl == 3) {
                const std::uint32_t c = read_element() & 0xFFFFu;
                value[0] = (c & 0x1Fu) << 3;
                value[1] = ((c >> 5) & 0x1Fu) << 3;
                value[2] = ((c >> 10) & 0x1Fu) << 3;
                value[3] = ((c >> 15) & 1u) << 7;
            } else {
                for (std::uint32_t j = 0; j < components; ++j) value[j] = read_element();
                if (components == 1) value[1] = value[2] = value[3] = value[0];
                else if (components == 2) { value[2] = value[0]; value[3] = value[1]; }
            }
        }
        std::uint8_t *q = mem + (addr % wrap) * 16;
        const std::uint32_t mrow = std::min<std::uint32_t>(cycle, 3u);
        for (std::uint32_t j = 0; j < 4; ++j) {
            std::uint32_t m = masked ? (u.mask >> ((mrow * 4 + j) * 2)) & 3u : 0u;
            if (fill && m == 0) m = 1;
            std::uint32_t out;
            if (m == 3) continue;
            if (m == 1) out = u.row[j];
            else if (m == 2) out = u.col[mrow];
            else if (u.mode == 1) out = value[j] + u.row[j];
            else if (u.mode == 2) { out = value[j] + u.row[j]; u.row[j] = out; }
            else if (u.mode == 3) { out = value[j]; u.row[j] = out; }
            else out = value[j];
            std::memcpy(q + j * 4, &out, 4);
        }
        ++addr;
        ++cycle;
        if (cl >= wl) {
            if (cycle >= wl) {
                addr += cl - wl;
                cycle = 0;
            }
        } else if (cycle >= wl) {
            cycle = 0;
        }
    }
    u.num = 0;
}

std::uint32_t data_words(std::uint32_t cmd, const Unit &u) {
    const std::uint32_t op = (cmd >> 24) & 0x7Fu;
    const std::uint32_t num = (cmd >> 16) & 0xFFu;
    switch (op) {
    case 0x20: return 1;
    case 0x30: case 0x31: return 4;
    case 0x4A: return (num ? num : 256u) * 2u;
    case 0x50: case 0x51: return ((cmd & 0xFFFFu) ? (cmd & 0xFFFFu) : 0x10000u) * 4u;
    default:
        if ((op & 0x60u) == 0x60u) return unpack_words(cmd, u, num ? num : 256u);
        return 0;
    }
}

void execute(int unit, std::uint32_t cmd, const std::uint32_t *data, const Ctx &c) {
    Unit &u = c.u;
    const std::uint32_t op = (cmd >> 24) & 0x7Fu, imm = cmd & 0xFFFFu, num = (cmd >> 16) & 0xFFu;
    if (unit == 1 && u.vu_pending) {
        const bool skipping = (u.cycle & 0xFFu) > write_length(u);
        const bool keep = op == 0x00 || op == 0x01 ||
                          ((op == 0x05 || op == 0x20 || op == 0x30 || op == 0x31) && skipping) ||
                          ((op & 0x60u) == 0x60u && skipping && ((cmd >> 15) & 1u) && u.ofst != 0);
        if (!keep) flush_vu(1);
    }
    u.code = cmd;
    if (!c.vu_thread) ++g_commands[unit][op];
    switch (op) {
    case 0x00: break;
    case 0x01:
        u.cycle = imm;
        if (unit == 1 && u.vu_pending && (u.cycle & 0xFFu) <= write_length(u)) flush_vu(1);
        break;
    case 0x02: u.ofst = imm & 0x3FFu; u.dbf = false; u.tops = u.base; break;
    case 0x03: u.base = imm & 0x3FFu; break;
    case 0x04: u.itops = imm & 0x3FFu; break;
    case 0x05: u.mode = imm & 3u; break;
    case 0x06: gif::set_path3_masked((imm >> 15) & 1u); break;
    case 0x07: u.mark = imm; u.stat |= kStatMrk; break;
    case 0x10: case 0x11: case 0x13: break;
    case 0x14: case 0x15: run_vu(unit, imm * 8u, c); break;
    case 0x17: run_vu(unit, c.dry ? 0u : vu::tpc(unit), c); break;
    case 0x20: u.mask = data[0]; break;
    case 0x30: std::memcpy(u.row, data, 16); break;
    case 0x31: std::memcpy(u.col, data, 16); break;
    case 0x4A: {
        if (c.dry) break;
        std::uint8_t *code = unit ? g_vu1_code : g_vu0_code;
        const std::uint32_t size = unit ? 16384u : 4096u;
        const std::uint32_t count = num ? num : 256u;
        bool changed = false;
        for (std::uint32_t i = 0; i < count; ++i) {
            std::uint8_t *at = code + ((imm * 8u + i * 8u) & (size - 1));
            if (std::memcmp(at, data + i * 2, 8) == 0) continue;
            std::memcpy(at, data + i * 2, 8);
            changed = true;
        }
        if (changed) vu::code_changed(unit);
        break;
    }
    case 0x50: case 0x51:
        if (c.dry) gif::scan_signals(2, reinterpret_cast<const std::uint8_t *>(data), imm ? imm : 0x10000u);
        else gif::transfer(2, reinterpret_cast<const std::uint8_t *>(data), imm ? imm : 0x10000u);
        break;
    default:
        if ((op & 0x60u) == 0x60u) unpack(unit, cmd, data, c);
        else note_unknown(unit, cmd);
        break;
    }
    if ((cmd & 0x80000000u) && !(u.err & 1u)) {
        flush_vu(unit);
        // The interrupt bit raises INT and stalls the unit before the next command, except on MARK, which only raises INT.
        u.stat |= kStatInt;
        if (op != 0x07) {
            u.stat |= kStatVis;
            u.stalled = true;
        }
        if (!c.vu_thread) hw::intc_raise(4u + static_cast<unsigned>(unit));
    }
}

// A command whose data has not all arrived is already decoded: CODE shows it, and NUM holds the length of an UNPACK.
void decode(int unit, std::uint32_t cmd) {
    Unit &u = state(unit);
    u.code = cmd;
    if (((cmd >> 24) & 0x60u) == 0x60u) {
        const std::uint32_t num = (cmd >> 16) & 0xFFu;
        u.num = unit == 1 && !num ? 256u : num;
    }
}

// `base` is the stream position of words[0], for the history.
std::size_t run(int unit, const std::uint32_t *words, std::size_t count, std::uint64_t base) {
    const bool vu_thread = on_vu_thread();
    const bool side = unit == 1 && vu_thread;  // the vector thread's own copy of VIF1
    const Ctx c{side ? g_vu_side : g_units[unit], vu_thread, unit == 1 && gs::queue::vu_active() && !vu_thread};
    Unit &u = c.u;
    History &h = g_hist[side ? 2 : unit];
    std::size_t pos = 0;
    while (pos < count && !u.stalled) {
        const std::uint32_t cmd = words[pos];
        const std::size_t start = pos + 1;
        const std::uint32_t need = data_words(cmd, u);
        if (start + need > count) {
            decode(unit, cmd);
            break;
        }
        h.last[h.head++ & 7u] = {cmd, need, base + pos};
        execute(unit, cmd, words + start, c);
        pos = start + need;
    }
    return pos;
}

void process(int unit) {
    Unit &u = state(unit);
    const std::size_t pos = run(unit, u.pending.data(), u.pending.size(), g_hist[hist_index(unit)].received - u.pending.size());
    u.pending.erase(u.pending.begin(), u.pending.begin() + static_cast<std::ptrdiff_t>(pos));
}

}

void reset() {
    for (Unit &u : g_units) u = Unit{};
    for (History &h : g_hist) h = History{};
}

void reset_vu_side() {
    g_vu_side = Unit{};
    g_hist[2] = History{};
}

std::uint32_t itop(int unit) { return g_units[unit].itop; }

void stats() {
    for (int u = 0; u < 2; ++u) {
        std::fprintf(stderr, "[vif%d]", u);
        for (int op = 0; op < 128; ++op)
            if (g_commands[u][op]) std::fprintf(stderr, " %02X:%llu", op, static_cast<unsigned long long>(g_commands[u][op]));
        std::fprintf(stderr, "\n");
    }
    if (const std::uint64_t held = g_vu_held.load(std::memory_order_relaxed))
        std::fprintf(stderr, "[vif1] %llu programs started on VU1, %llu held back until the UNPACKs behind their MSCAL were done\n", static_cast<unsigned long long>(g_vu_starts.load(std::memory_order_relaxed)),
                     static_cast<unsigned long long>(held));
    for (int i = 0; i < 3; ++i)
        if (g_hist[i].unknown) std::fprintf(stderr, "[%s] %llu unknown commands in this run\n", kLabel[i], static_cast<unsigned long long>(g_hist[i].unknown));
}

// What VIF1 looks like at the moment VU1 kicks a packet that does not end (vu_micro.cpp, the first one of a run): the double buffer, the
// unpack settings, how many programs it held back, and the last commands it ran. Called on the thread that runs VU1.
void describe_vu1() {
    const Unit &u = state(1);
    const History &h = g_hist[hist_index(1)];
    std::fprintf(stderr, "[vif1] now: BASE %u OFFSET %u TOPS %u (%s), the running program got TOP %u ITOP %u; CYCLE CL %u WL %u, STMOD %u, MASK 0x%08X\n", u.base, u.ofst, u.tops,
                 u.ofst ? "double buffer" : "single buffer", u.top, u.itop, u.cycle & 0xFFu, write_length(u), u.mode, u.mask);
    std::fprintf(stderr, "[vif1] %llu programs started so far, %llu of them held back until the UNPACKs behind their MSCAL were done (PS2_VIF_DEFER=0 starts them at the MSCAL)\n",
                 static_cast<unsigned long long>(g_vu_starts.load(std::memory_order_relaxed)), static_cast<unsigned long long>(g_vu_held.load(std::memory_order_relaxed)));
    const std::uint32_t before = std::min<std::uint32_t>(h.head, 8u);
    std::fprintf(stderr, "[vif1] the last %u commands it ran, oldest first:\n", before);
    for (std::uint32_t i = before; i >= 1; --i) {
        const Seen &s = h.last[(h.head - i) & 7u];
        std::fprintf(stderr, "[vif1]   word %llu: 0x%08X %s, %u data words\n", static_cast<unsigned long long>(s.at), s.cmd, command_name(s.cmd), s.words);
    }
}

// `count` words, not quadwords: the 64 bits of a DMA tag that a chain with the tag transfer enabled sends ahead of its data are two
// words, and a command waiting for data (an UNPACK that spans several tags) takes exactly those two.
void transfer_words(int unit, const std::uint32_t *words, std::uint32_t count) {
    if (shadow(unit)) gs::queue::vu().post(gs::queue::kVif1, count, 0, words, count * 4);
    Unit &u = state(unit);
    g_hist[hist_index(unit)].received += count;
    if (u.pending.empty() && !u.stalled) {
        const std::size_t pos = run(unit, words, count, g_hist[hist_index(unit)].received - count);
        if (pos < count) u.pending.assign(words + pos, words + count);
        return;
    }
    const std::size_t old = u.pending.size();
    u.pending.resize(old + static_cast<std::size_t>(count));
    std::memcpy(u.pending.data() + old, words, static_cast<std::size_t>(count) * 4);
    process(unit);
}

void transfer(int unit, const std::uint8_t *data, std::uint32_t qwc) {
    transfer_words(unit, reinterpret_cast<const std::uint32_t *>(data), qwc * 4);
}

// The DMA keeps sending into a stalled unit until the unit's FIFO is full.
bool stalled(int unit) {
    const Unit &u = state(unit);
    return u.stalled && u.pending.size() > kFifoWords[unit];
}

std::uint32_t read(int unit, std::uint32_t reg) {
    const Unit &u = state(unit);
    switch (reg) {
    case 0x00: {
        // VPS: 2 while a command waits behind a stall, 1 while a command waits for its data.
        const std::uint32_t vps = u.pending.empty() ? 0u : u.stalled ? 2u : 1u;
        std::uint32_t stat = u.stat | vps | (u.dbf ? 0x80u : 0u);
        // With FDR set the FIFO runs from the GS, and a download keeps it as full as the
        // game drains it: sceGsExecStoreImage polls FQC before each qword it reads.
        if (unit == 1 && (u.stat & 0x800000u)) stat |= 16u << 24;
        return stat;
    }
    case 0x20: return u.err;
    case 0x30: return u.mark;
    case 0x40: return u.cycle;
    case 0x50: return u.mode;
    case 0x60: return u.num;
    case 0x70: return u.mask;
    case 0x80: return u.code;
    case 0x90: return u.itops;
    case 0xA0: return u.base;
    case 0xB0: return u.ofst;
    case 0xC0: return u.tops;
    case 0xD0: return u.itop;
    case 0xE0: return u.top;
    default:
        if (reg >= 0x100 && reg < 0x140) return u.row[(reg - 0x100) >> 4];
        if (reg >= 0x140 && reg < 0x180) return u.col[(reg - 0x140) >> 4];
        return 0;
    }
}

void write(int unit, std::uint32_t reg, std::uint32_t value) {
    if (shadow(unit)) gs::queue::vu().post(gs::queue::kVif1Write, reg, value, nullptr, 0);
    Unit &u = state(unit);
    switch (reg) {
    case 0x00:
        if (unit == 1) u.stat = (u.stat & ~0x800000u) | (value & 0x800000u);
        break;
    case 0x10:
        if (value & 1u) {
            // A reset stops the unit and clears its status, error and mark and the command it was decoding. The registers that
            // VIF commands set (the write cycle, the add-row mode, the mask, ROW and COL, ITOPS, BASE and OFFSET) keep their values:
            // Downhill Domination sets ROW once and resets the VIF before every frame, and Zone of the Enders sets the write
            // cycle (STCYCL 4,4) once at start-up, resets the VIF later without sending it again, and then unpacks with it. A cycle
            // that a reset had cleared to 0 made every UNPACK take no data, so the vectors that follow were read as commands (CL 0
            // is a normal write now, see cycle_length, but the cycle a game set is still the one it unpacks with).
            Unit fresh{};
            std::memcpy(fresh.row, u.row, sizeof fresh.row);
            std::memcpy(fresh.col, u.col, sizeof fresh.col);
            fresh.cycle = u.cycle;
            fresh.mode = u.mode;
            fresh.mask = u.mask;
            fresh.itops = u.itops;
            fresh.base = u.base;
            fresh.ofst = u.ofst;
            fresh.tops = u.base;  // the double-buffer flag is clear again, so the next buffer is at BASE
            u = fresh;
        }
        if (value & 8u) {
            u.stat &= ~0x3F00u;
            if (u.stalled) {
                u.stalled = false;
                process(unit);
                if (!stalled(unit) && !on_vu_thread()) dmac::resume(unit);
            }
        }
        break;
    case 0x20: u.err = value; break;
    case 0x30: u.mark = value; u.stat &= ~kStatMrk; break;
    default:
        if (reg >= 0x100 && reg < 0x140) u.row[(reg - 0x100) >> 4] = value;
        else if (reg >= 0x140 && reg < 0x180) u.col[(reg - 0x140) >> 4] = value;
        break;
    }
}

void flush(int unit) { flush_vu(unit); }

}

namespace ps2::vif {

void state(ps2::state::Stream &s) {
    if (!s.loading()) flush_vu(1);
    s.tag("vif");
    for (Unit *u : {&g_units[0], &g_units[1], &g_vu_side}) {
        s.pod(u->stat);
        s.pod(u->err);
        s.pod(u->mark);
        s.pod(u->cycle);
        s.pod(u->mode);
        s.pod(u->num);
        s.pod(u->mask);
        s.pod(u->code);
        s.pod(u->itops);
        s.pod(u->base);
        s.pod(u->ofst);
        s.pod(u->tops);
        s.pod(u->itop);
        s.pod(u->top);
        s.pod(u->row);
        s.pod(u->col);
        s.pod(u->dbf);
        s.pod(u->stalled);
        s.vec(u->pending);
    }
    // The vector thread keeps its own copy of VIF1 (g_vu_side) and the emulation thread tracks the same unit in g_units[1] without
    // running the vector unit. A run without the vector thread never touches g_vu_side, so a state it saved carries an empty one:
    // loaded into a run with the thread, VIF1 would restart with cycle, mask, base and offset at zero, UNPACK would write
    // nonsense into the vector unit's memory, and the program would loop on it forever (Robotech: Battlecry, issue #10). Both
    // copies describe the same unit, so the one that is always right (g_units[1]) is also the vector thread's start.
    if (s.loading()) g_vu_side = g_units[1];
}

}
