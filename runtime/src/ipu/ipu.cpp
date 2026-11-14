#include "ipu.hpp"

#include "tables.hpp"
#include "../dmac.hpp"
#include "../hw.hpp"

#include "ps2/os.hpp"
#include "ps2/state.hpp"
#include "ps2/system.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#if defined(__AVX2__)
#include <immintrin.h>
#endif

namespace ps2::ipu {

namespace {

using namespace tables;

struct NeedData {};
struct BadCode {};

constexpr std::uint32_t kEcd = 0x4000u, kScd = 0x8000u, kReset = 0x40000000u;
constexpr std::size_t kFifoBytes = 128;

constexpr std::uint8_t kDefaultIntra[64] = {
    8,  16, 19, 22, 26, 27, 29, 34, 16, 16, 22, 24, 27, 29, 34, 37, 19, 22, 26, 27, 29, 34, 34, 38, 22, 22, 26, 27, 29, 34, 37, 40,
    22, 26, 27, 29, 32, 35, 40, 48, 26, 27, 29, 32, 35, 40, 48, 58, 26, 27, 29, 34, 38, 46, 56, 69, 27, 29, 35, 38, 46, 56, 69, 83,
};

constexpr std::uint8_t kNonLinearScale[32] = {0,  1,  2,  3,  4,  5,  6,  7,  8,  10, 12, 14, 16, 18, 20, 22,
                                              24, 28, 32, 36, 40, 44, 48, 52, 56, 64, 72, 80, 88, 96, 104, 112};

struct Entry {
    std::uint8_t length = 0;
    std::uint32_t value = 0;
};

struct Lookup {
    unsigned bits = 0;
    std::vector<Entry> entries;
};

struct DctEntry {
    std::uint8_t length = 0;
    std::uint8_t run = 0;
    std::int16_t level = 0;
};

struct DctLookup {
    unsigned bits = 0;
    std::vector<DctEntry> entries;
};

template <std::size_t N>
Lookup build(const Vlc (&table)[N]) {
    Lookup l;
    for (const Vlc &e : table) l.bits = std::max<unsigned>(l.bits, e.length);
    l.entries.resize(std::size_t{1} << l.bits);
    for (const Vlc &e : table) {
        const unsigned shift = l.bits - e.length;
        for (std::uint32_t i = 0; i < (1u << shift); ++i) l.entries[(static_cast<std::uint32_t>(e.code) << shift) | i] = {e.length, e.value};
    }
    return l;
}

template <std::size_t N>
DctLookup build(const Dct (&table)[N]) {
    DctLookup l;
    for (const Dct &e : table) l.bits = std::max<unsigned>(l.bits, e.length);
    l.entries.resize(std::size_t{1} << l.bits);
    for (const Dct &e : table) {
        const unsigned shift = l.bits - e.length;
        for (std::uint32_t i = 0; i < (1u << shift); ++i) l.entries[(static_cast<std::uint32_t>(e.code) << shift) | i] = {e.length, e.run, e.level};
    }
    return l;
}

struct Tables {
    Lookup mbai = build(kMbAddressIncrement), mb_i = build(kMbTypeI), mb_p = build(kMbTypeP), mb_b = build(kMbTypeB);
    Lookup motion = build(kMotionCode), dmv = build(kDmVector), dc_luma = build(kDcSizeLuma), dc_chroma = build(kDcSizeChroma);
    Lookup cbp = build(kCodedBlockPattern);
    DctLookup dct0 = build(kDctTable0), dct1 = build(kDctTable1);
    double cosines[8][8];
    double cosines_t[8][8];  // the same values, indexed [u][x]: the inverse transform adds one coefficient to all eight outputs at a time

    Tables() {
        for (int x = 0; x < 8; ++x)
            for (int u = 0; u < 8; ++u) {
                cosines[x][u] = (u ? 0.5 : 0.5 / std::sqrt(2.0)) * std::cos((2 * x + 1) * u * 3.14159265358979323846 / 16.0);
                cosines_t[u][x] = cosines[x][u];
            }
    }
};

const Tables &tables() {
    static const Tables t;
    return t;
}

enum Command { kBclr, kIdec, kBdec, kVdec, kFdec, kSetiq, kSetvq, kCsc, kPack, kSetth };

std::vector<std::uint8_t> g_in;
std::size_t g_bit = 0;
std::vector<std::uint8_t> g_out;
std::size_t g_out_pos = 0;

std::uint32_t g_ctrl = 0, g_result = 0, g_command = 0;
int g_last = -1;
bool g_busy = false, g_decoding = false, g_interrupt = true, g_starved = false;
int g_phase = 0;
std::uint32_t g_qsc = 0, g_count = 0;
bool g_first = true;
std::uint8_t g_intra_iq[64], g_nonintra_iq[64];
std::uint16_t g_vqclut[16];
std::uint16_t g_th0 = 0, g_th1 = 0;
std::int16_t g_dc[3];
std::uint64_t g_commands[16], g_stalls = 0, g_out_qwords = 0;

// PS2_DUMP_IPU=file writes every byte the IPU takes in (DMA or FIFO writes) to that file, to compare the game's feed with the disc.
void dump_input(const std::uint8_t *bytes, std::size_t n) {
    static std::FILE *file = [] {
        const std::string name = os::env("PS2_DUMP_IPU");
        return name.empty() ? nullptr : os::open(name, "wb");
    }();
    if (file) std::fwrite(bytes, 1, n, file);
}

// PS2_DUMP_IPU_BITS=file writes only the bytes the decoder commands used up, in order and once (not the raw macroblocks CSC takes
// from the same FIFO, not data thrown away by BCLR): the film's own bit stream as the player fed it, so it can be parsed offline.
bool g_csc_active = false;
std::size_t g_dump_pos = 0;
void dump_bits_to(std::size_t end);

bool pull() {
    std::uint8_t q[16];
    if (!dmac::ipu_pull(q)) return false;
    dump_input(q, 16);
    g_in.insert(g_in.end(), q, q + 16);
    return true;
}

bool have_slow(std::size_t bits) {
    while (g_in.size() * 8 < g_bit + bits)
        if (!pull()) return false;
    return true;
}

// Read many times per macroblock: the check is inline, the refill from the DMA is not
inline bool have(std::size_t bits) {
    if (g_in.size() * 8 >= g_bit + bits) [[likely]] return true;
    return have_slow(bits);
}

std::size_t available() { return g_in.size() * 8 > g_bit ? g_in.size() * 8 - g_bit : 0; }

__attribute__((always_inline)) inline std::uint32_t bits_at(std::size_t bit, unsigned n) {
    if (!n) return 0;
    std::uint64_t v = 0;
    const std::size_t byte = bit >> 3;
    if (byte + 8 <= g_in.size()) {  // the usual case: eight bytes in one go (one load and a byte swap, the same value as the loop below)
        const std::uint8_t *p = g_in.data() + byte;
        v = (static_cast<std::uint64_t>(p[0]) << 56) | (static_cast<std::uint64_t>(p[1]) << 48) | (static_cast<std::uint64_t>(p[2]) << 40) | (static_cast<std::uint64_t>(p[3]) << 32) |
            (static_cast<std::uint64_t>(p[4]) << 24) | (static_cast<std::uint64_t>(p[5]) << 16) | (static_cast<std::uint64_t>(p[6]) << 8) | static_cast<std::uint64_t>(p[7]);
    } else {
        for (std::size_t i = 0; i < 8; ++i) v = (v << 8) | (byte + i < g_in.size() ? g_in[byte + i] : 0u);
    }
    return static_cast<std::uint32_t>((v << (bit & 7u)) >> (64 - n));
}

std::uint32_t peek(unsigned n) {
    if (!have(n)) throw NeedData{};
    return bits_at(g_bit, n);
}

std::uint32_t get(unsigned n) {
    const std::uint32_t v = peek(n);
    g_bit += n;
    return v;
}

void skip(unsigned n) {
    if (!have(n)) throw NeedData{};
    g_bit += n;
}

std::uint32_t peek_soft(unsigned n, std::size_t &avail) {
    have(n);
    avail = available();
    return bits_at(g_bit, n);
}

std::uint32_t decode(const Lookup &l) {
    std::size_t avail;
    const Entry &e = l.entries[peek_soft(l.bits, avail)];
    if (!e.length) {
        if (avail < l.bits) throw NeedData{};
        throw BadCode{};
    }
    if (e.length > avail) throw NeedData{};
    g_bit += e.length;
    return e.value;
}

std::uint32_t peek_now() {
    const unsigned n = static_cast<unsigned>(std::min<std::size_t>(32, available()));
    return n ? bits_at(g_bit, n) << (32 - n) : 0u;
}

void dump_bits_to(std::size_t end) {
    static std::FILE *file = [] {
        const std::string name = os::env("PS2_DUMP_IPU_BITS");
        return name.empty() ? nullptr : os::open(name, "wb");
    }();
    end = std::min(end, g_in.size());
    if (file && end > g_dump_pos) std::fwrite(g_in.data() + g_dump_pos, 1, end - g_dump_pos, file);
    g_dump_pos = std::max(g_dump_pos, end);
}

void commit() {
    const std::size_t drop = std::min(g_bit / 128 * 16, g_in.size() / 16 * 16);
    if (!drop) return;  // nothing to drop: the rest below would do nothing (most commands use only a few bits)
    if (!g_csc_active) dump_bits_to(drop);
    g_dump_pos = g_dump_pos > drop ? g_dump_pos - drop : 0;
    g_in.erase(g_in.begin(), g_in.begin() + static_cast<std::ptrdiff_t>(drop));
    g_bit -= drop * 8;
}

void top_up() {
    while (g_in.size() < kFifoBytes && pull()) {}
}

unsigned dc_precision() { return (g_ctrl >> 16) & 3u; }
bool alternate_scan() { return g_ctrl & 0x100000u; }
bool intra_vlc_one() { return g_ctrl & 0x200000u; }
bool nonlinear_q() { return g_ctrl & 0x400000u; }
bool mpeg1() { return g_ctrl & 0x800000u; }
unsigned picture_type() { return (g_ctrl >> 24) & 7u; }

int read_dc_diff(int channel) {
    const unsigned size = decode(channel ? tables().dc_chroma : tables().dc_luma) & 0xFFFFu;
    if (!size) return 0;
    const int v = static_cast<int>(get(size));
    return v < (1 << (size - 1)) ? v + 1 - (1 << size) : v;
}

bool read_coefficient(const DctLookup &l, int &run, int &level) {
    std::size_t avail;
    const DctEntry &e = l.entries[peek_soft(l.bits, avail)];
    if (!e.length) {
        if (avail < l.bits) throw NeedData{};
        throw BadCode{};
    }
    if (e.length > avail) throw NeedData{};
    g_bit += e.length;
    if (e.run == kEscape) {
        run = static_cast<int>(get(6));
        if (mpeg1()) {
            int v = static_cast<int>(get(8));
            if (v == 0) v = static_cast<int>(get(8));
            else if (v == 128) v = static_cast<int>(get(8)) - 256;
            else if (v > 128) v -= 256;
            level = v;
        } else {
            const int v = static_cast<int>(get(12));
            level = v & 0x800 ? v - 0x1000 : v;
        }
        return true;
    }
    run = e.run;
    level = get(1) ? -e.level : e.level;
    return true;
}

void idct(const int *in, std::int16_t *out) {
    // Same sums in the same order as the plain 8 x 8 x 8 version, but the terms whose coefficient is zero are left out: such a term adds
    // +0.0 or -0.0, and a sum that starts at +0.0 is never -0.0 (round to nearest), so the sum and the picture stay bit for bit the
    // same. A video block has few non-zero coefficients, so most of the 1024 multiplications are gone (docs/speed-cpu.md).
    // The eight outputs of a row are summed side by side (one coefficient added to all eight, in the order of the coefficients), which
    // is the same sum for every output as one at a time, only the compiler can now do four or two of them in one instruction.
    const Tables &t = tables();
    alignas(32) double rows[8][8];
    int live[8], live_rows = 0;
    for (int v = 0; v < 8; ++v) {
        const int *row = in + v * 8;
        int at[8], count = 0;
        for (int u = 0; u < 8; ++u)
            if (row[u]) at[count++] = u;
        if (!count) continue;  // an all-zero row adds nothing in the second pass either
        live[live_rows++] = v;
        double acc[8] = {};
        for (int k = 0; k < count; ++k) {
            const double w = row[at[k]];
            const double *ct = t.cosines_t[at[k]];
            for (int x = 0; x < 8; ++x) acc[x] += ct[x] * w;
        }
        for (int x = 0; x < 8; ++x) rows[v][x] = acc[x];
    }
    for (int y = 0; y < 8; ++y) {
        double acc[8] = {};
        for (int k = 0; k < live_rows; ++k) {
            const double cy = t.cosines[y][live[k]];
            const double *r = rows[live[k]];
            for (int x = 0; x < 8; ++x) acc[x] += cy * r[x];
        }
        for (int x = 0; x < 8; ++x) out[y * 8 + x] = static_cast<std::int16_t>(std::clamp(static_cast<int>(std::floor(acc[x] + 0.5)), -256, 255));
    }
}

// The next 57 bits, most significant bit first, bits past the end of the input read as zero (the same bits as bits_at gives).
inline std::uint64_t window(std::size_t bit) {
    const std::size_t byte = bit >> 3;
    std::uint64_t v;
    if (byte + 8 <= g_in.size()) [[likely]] {
        std::memcpy(&v, g_in.data() + byte, 8);
        v = __builtin_bswap64(v);
    } else {
        v = 0;
        for (std::size_t i = 0; i < 8; ++i) v = (v << 8) | (byte + i < g_in.size() ? g_in[byte + i] : 0u);
    }
    return v << (bit & 7u);
}

// How the plain calls treat one symbol of a block: with as many bits left as the table's look-ahead (peek_soft asks for table.bits, 16
// at the most) asking for the look-ahead fetches nothing, and a symbol that then turns out to need no more bits than are left is read without any further
// question: the plain calls would not have fetched either. A symbol that needs more than is left (a long escape code near the end of
// what has arrived) is handed to the plain calls, from the same start, so it fetches, waits for data or fails as it always did.
inline std::size_t bits_left() { return g_in.size() * 8 > g_bit ? g_in.size() * 8 - g_bit : 0; }

// `n` bytes as `for (i < n) out[i] = get(8)` gives them, with the same fetches and the same failure point: the bytes already in the
// input are taken in one go (get(8) would fetch for none of them), and only a byte that needs a fetch goes through get(), which fetches,
// waits for data or fails exactly as the plain loop does at that byte. CSC reads its 384-byte macroblocks this way (the God of War II
// film converts millions of them; one get() per byte was about a tenth of the emulation thread).
void get_bytes(std::uint8_t *out, std::size_t n) {
    std::size_t i = 0;
    while (i < n) {
        const std::size_t ready = std::min(n - i, bits_left() / 8);
        if (!ready) {
        } else if (!(g_bit & 7u)) {
            std::memcpy(out + i, g_in.data() + g_bit / 8, ready);
        } else {
            for (std::size_t k = 0; k < ready; ++k) out[i + k] = static_cast<std::uint8_t>(bits_at(g_bit + 8 * k, 8));
        }
        g_bit += ready * 8;
        i += ready;
        if (i < n) out[i++] = static_cast<std::uint8_t>(get(8));
    }
}

void decode_block(std::int16_t *out, int channel, bool intra, std::uint32_t qsc) {
    int coef[64] = {};
    int index = 0;
    const DctLookup &table = intra && intra_vlc_one() ? tables().dct1 : tables().dct0;
    const bool table_one = &table == &tables().dct1;
    const bool mp1 = mpeg1();
    if (intra) {
        g_dc[channel] = static_cast<std::int16_t>(g_dc[channel] + read_dc_diff(channel));
        coef[0] = g_dc[channel];
        index = 1;
    } else if (bits_left() >= 16 ? (window(g_bit) >> 63) != 0 : peek(1) != 0) {
        coef[0] = get(2) & 1u ? -1 : 1;
        index = 1;
    }
    for (;;) {
        const std::size_t left = bits_left();
        if (left >= std::max<std::size_t>(table.bits, 4)) {
            const std::uint64_t w = window(g_bit);
            if (index && (table_one ? (w >> 60) == 6u : (w >> 62) == 2u)) {
                g_bit += table_one ? 4 : 2;
                break;
            }
            const DctEntry &e = table.entries[w >> (64 - table.bits)];
            if (!e.length) throw BadCode{};
            unsigned used = e.length;
            int run, level;
            if (e.run == kEscape) {
                run = static_cast<int>((w << used) >> 58);
                used += 6;
                if (mp1) {
                    int v = static_cast<int>((w << used) >> 56);
                    used += 8;
                    if (v == 0) {
                        v = static_cast<int>((w << used) >> 56);
                        used += 8;
                    } else if (v == 128) {
                        v = static_cast<int>((w << used) >> 56) - 256;
                        used += 8;
                    } else if (v > 128) {
                        v -= 256;
                    }
                    level = v;
                } else {
                    const int v = static_cast<int>((w << used) >> 52);
                    used += 12;
                    level = v & 0x800 ? v - 0x1000 : v;
                }
            } else {
                run = e.run;
                level = (w << used) >> 63 ? -e.level : e.level;
                used += 1;
            }
            if (used <= left) {
                g_bit += used;
                index += run;
                if (index >= 64) throw BadCode{};
                coef[index++] = level;
                continue;
            }
        }
        if (index) {
            if (table_one ? peek(4) == 6u : peek(2) == 2u) {
                skip(table_one ? 4 : 2);
                break;
            }
        }
        int run = 0, level = 0;
        read_coefficient(table, run, level);
        index += run;
        if (index >= 64) throw BadCode{};
        coef[index++] = level;
    }
    const std::uint8_t *scan = alternate_scan() ? kAlternate : kZigzag;
    const std::uint8_t *weights = intra ? g_intra_iq : g_nonintra_iq;
    const int scale = mpeg1() || !nonlinear_q() ? static_cast<int>(qsc) * 2 : kNonLinearScale[qsc & 31u];
    int f[64] = {};
    int sum = 0;
    // The coefficients are stored at rising positions, so everything from `index` on is zero (and a zero is skipped below anyway)
    for (int k = 0; k < index; ++k) {
        const int v = coef[k];
        const int r = scan[k];
        int x;
        if (intra && k == 0) {
            x = v * (8 >> dc_precision());
        } else {
            if (!v) continue;
            const int sign = v > 0 ? 1 : -1;
            x = intra ? (v * weights[r] * scale * 2) / 32 : ((2 * v + sign) * weights[r] * scale) / 32;
            if (mpeg1() && x && !(x & 1)) x -= sign;
        }
        x = std::clamp(x, -2048, 2047);
        f[r] = x;
        sum += x;
    }
    if (!mpeg1() && !(sum & 1)) f[63] += f[63] & 1 ? -1 : 1;
    idct(f, out);
}

void decode_macroblock(std::int16_t *raw, bool intra, bool field, bool dc_reset, std::uint32_t qsc) {
    const unsigned cbp = intra ? 0x3Fu : decode(tables().cbp) & 0xFFu;
    if (dc_reset) g_dc[0] = g_dc[1] = g_dc[2] = static_cast<std::int16_t>(128 << dc_precision());
    std::int16_t blocks[6][64] = {};
    for (int b = 0; b < 6; ++b)
        if (cbp & (32u >> b)) decode_block(blocks[b], b < 4 ? 0 : b - 3, intra, qsc);
    for (int i = 0; i < 8; ++i) {
        const int top = field ? 2 * i : i, bottom = field ? 2 * i + 1 : i + 8;
        std::memcpy(raw + top * 16, blocks[0] + i * 8, 16);
        std::memcpy(raw + top * 16 + 8, blocks[1] + i * 8, 16);
        std::memcpy(raw + bottom * 16, blocks[2] + i * 8, 16);
        std::memcpy(raw + bottom * 16 + 8, blocks[3] + i * 8, 16);
    }
    std::memcpy(raw + 256, blocks[4], 128);
    std::memcpy(raw + 320, blocks[5], 128);
}

// The colour conversion of one 16 x 16 macroblock (4:2:0 in, RGB32 or RGB16 out), written straight to the output. The formulas are the
// plain ones; the chroma terms are worked out once per chroma sample instead of once per pixel, and the output is a template choice.
template <bool Rgb16>
void convert_block(const std::uint8_t *raw, std::uint8_t *dst) {
    const int th0 = g_th0, th1 = g_th1;
    for (int y = 0; y < 16; ++y) {
        const std::uint8_t *luma = raw + y * 16, *cb_row = raw + 256 + (y / 2) * 8, *cr_row = raw + 320 + (y / 2) * 8;
        for (int x = 0; x < 16; x += 2) {
            const int cb = cb_row[x / 2] - 128, cr = cr_row[x / 2] - 128;
            const int add_r = 204 * cr + 64, add_g = -50 * cb - 104 * cr + 64, add_b = 258 * cb + 64;
            for (int k = 0; k < 2; ++k) {
                const int lum = 149 * std::max(0, luma[x + k] - 16);
                const int r = std::clamp((lum + add_r) >> 7, 0, 255);
                const int g = std::clamp((lum + add_g) >> 7, 0, 255);
                const int b = std::clamp((lum + add_b) >> 7, 0, 255);
                int a = 0x80;
                if (r < th0 && g < th0 && b < th0) a = 0;
                else if (r < th1 && g < th1 && b < th1) a = 0x40;
                if constexpr (Rgb16) {
                    const std::uint16_t p = static_cast<std::uint16_t>((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10) | (a & 0x80 ? 0x8000 : 0));
                    std::memcpy(dst, &p, 2);
                    dst += 2;
                } else {
                    dst[0] = static_cast<std::uint8_t>(r);
                    dst[1] = static_cast<std::uint8_t>(g);
                    dst[2] = static_cast<std::uint8_t>(b);
                    dst[3] = static_cast<std::uint8_t>(a);
                    dst += 4;
                }
            }
        }
    }
}

#if defined(__AVX2__)
// The same conversion eight pixels at a time (integer arithmetic only, so the same values in every pixel, checked against the plain
// version above on random macroblocks and thresholds). One row of 16 pixels is two vectors; the chroma terms are shared by two pixels.
template <bool Rgb16>
void convert_block_avx2(const std::uint8_t *raw, std::uint8_t *dst) {
    const __m256i th0 = _mm256_set1_epi32(g_th0), th1 = _mm256_set1_epi32(g_th1);
    const __m256i zero = _mm256_setzero_si256(), c255 = _mm256_set1_epi32(255), c128 = _mm256_set1_epi32(128);
    const __m256i first_four = _mm256_setr_epi32(0, 0, 1, 1, 2, 2, 3, 3), last_four = _mm256_setr_epi32(4, 4, 5, 5, 6, 6, 7, 7);
    const __m256i a_full = _mm256_set1_epi32(0x80), a_half = _mm256_set1_epi32(0x40);
    for (int y = 0; y < 16; ++y) {
        const std::uint8_t *luma = raw + y * 16;
        const __m256i cb8 = _mm256_sub_epi32(_mm256_cvtepu8_epi32(_mm_loadl_epi64(reinterpret_cast<const __m128i *>(raw + 256 + (y / 2) * 8))), c128);
        const __m256i cr8 = _mm256_sub_epi32(_mm256_cvtepu8_epi32(_mm_loadl_epi64(reinterpret_cast<const __m128i *>(raw + 320 + (y / 2) * 8))), c128);
        __m256i out[2];
        for (int half = 0; half < 2; ++half) {
            const __m256i pick = half ? last_four : first_four;
            const __m256i cb = _mm256_permutevar8x32_epi32(cb8, pick), cr = _mm256_permutevar8x32_epi32(cr8, pick);
            const __m256i sixty4 = _mm256_set1_epi32(64);
            const __m256i add_r = _mm256_add_epi32(_mm256_mullo_epi32(cr, _mm256_set1_epi32(204)), sixty4);
            const __m256i add_g = _mm256_add_epi32(_mm256_add_epi32(_mm256_mullo_epi32(cb, _mm256_set1_epi32(-50)), _mm256_mullo_epi32(cr, _mm256_set1_epi32(-104))), sixty4);
            const __m256i add_b = _mm256_add_epi32(_mm256_mullo_epi32(cb, _mm256_set1_epi32(258)), sixty4);
            const __m256i y8 = _mm256_cvtepu8_epi32(_mm_loadl_epi64(reinterpret_cast<const __m128i *>(luma + half * 8)));
            const __m256i lum = _mm256_mullo_epi32(_mm256_max_epi32(_mm256_sub_epi32(y8, _mm256_set1_epi32(16)), zero), _mm256_set1_epi32(149));
            const __m256i r = _mm256_min_epi32(_mm256_max_epi32(_mm256_srai_epi32(_mm256_add_epi32(lum, add_r), 7), zero), c255);
            const __m256i g = _mm256_min_epi32(_mm256_max_epi32(_mm256_srai_epi32(_mm256_add_epi32(lum, add_g), 7), zero), c255);
            const __m256i b = _mm256_min_epi32(_mm256_max_epi32(_mm256_srai_epi32(_mm256_add_epi32(lum, add_b), 7), zero), c255);
            const __m256i below0 = _mm256_and_si256(_mm256_and_si256(_mm256_cmpgt_epi32(th0, r), _mm256_cmpgt_epi32(th0, g)), _mm256_cmpgt_epi32(th0, b));
            const __m256i below1 = _mm256_and_si256(_mm256_and_si256(_mm256_cmpgt_epi32(th1, r), _mm256_cmpgt_epi32(th1, g)), _mm256_cmpgt_epi32(th1, b));
            const __m256i a = _mm256_andnot_si256(below0, _mm256_blendv_epi8(a_full, a_half, below1));
            if constexpr (Rgb16) {
                const __m256i p = _mm256_or_si256(
                    _mm256_or_si256(_mm256_srli_epi32(r, 3), _mm256_slli_epi32(_mm256_srli_epi32(g, 3), 5)),
                    _mm256_or_si256(_mm256_slli_epi32(_mm256_srli_epi32(b, 3), 10), _mm256_slli_epi32(_mm256_and_si256(a, a_full), 8)));
                out[half] = p;
            } else {
                out[half] = _mm256_or_si256(_mm256_or_si256(r, _mm256_slli_epi32(g, 8)), _mm256_or_si256(_mm256_slli_epi32(b, 16), _mm256_slli_epi32(a, 24)));
            }
        }
        if constexpr (Rgb16) {
            const __m256i packed = _mm256_permute4x64_epi64(_mm256_packus_epi32(out[0], out[1]), 0xD8);
            _mm256_storeu_si256(reinterpret_cast<__m256i *>(dst), packed);
            dst += 32;
        } else {
            _mm256_storeu_si256(reinterpret_cast<__m256i *>(dst), out[0]);
            _mm256_storeu_si256(reinterpret_cast<__m256i *>(dst + 32), out[1]);
            dst += 64;
        }
    }
}
#endif

void convert(const std::uint8_t *raw, bool rgb16) {
    const std::size_t at = g_out.size();
    g_out.resize(at + (rgb16 ? 512 : 1024));
#if defined(__AVX2__)
    if (rgb16) convert_block_avx2<true>(raw, g_out.data() + at);
    else convert_block_avx2<false>(raw, g_out.data() + at);
#else
    if (rgb16) convert_block<true>(raw, g_out.data() + at);
    else convert_block<false>(raw, g_out.data() + at);
#endif
}

void emit_raw16(const std::int16_t *raw) {
    const auto *bytes = reinterpret_cast<const std::uint8_t *>(raw);
    g_out.insert(g_out.end(), bytes, bytes + 768);
}

bool start_code_follows() {
    std::size_t avail;
    const std::uint32_t next = peek_soft(8, avail);
    return avail >= 8 && next == 0;
}

template <typename F>
bool atomic(F &&body) {
    const std::size_t bit = g_bit;
    std::int16_t dc[3];
    std::memcpy(dc, g_dc, sizeof dc);
    const std::size_t out = g_out.size();
    try {
        body();
        return true;
    } catch (const NeedData &) {
        ++g_stalls;
        g_bit = bit;
        std::memcpy(g_dc, dc, sizeof dc);
        g_out.resize(out);
        return false;
    }
}

bool run_idec() {
    const bool dtd = (g_command >> 24) & 1u, rgb16 = (g_command >> 27) & 1u;
    for (;;) {
        bool ok = true;
        switch (g_phase) {
        case 0:
            ok = atomic([&] { skip(g_command & 0x3Fu); });
            if (ok) g_phase = 1;
            break;
        case 1:
            ok = atomic([&] {
                const std::uint32_t type = decode(tables().mb_i) & 0xFFFFu;
                const bool field = dtd && get(1);
                std::uint32_t qsc = g_qsc;
                if (type & 0x10u) qsc = get(5);
                std::int16_t raw[384];
                decode_macroblock(raw, true, field, g_first, qsc);
                std::uint8_t raw8[384];
                for (int i = 0; i < 384; ++i) raw8[i] = static_cast<std::uint8_t>(std::clamp<int>(raw[i], 0, 255));
                convert(raw8, rgb16);
                g_qsc = qsc;
            });
            if (ok) {
                g_first = false;
                g_phase = 2;
            }
            break;
        case 2:
            ok = atomic([&] {
                std::size_t avail;
                const std::uint32_t next = peek_soft(8, avail);
                if (avail < 8) throw NeedData{};
                if (next == 0) {
                    g_bit = (g_bit + 7) & ~std::size_t{7};
                    g_phase = 3;
                    return;
                }
                std::uint32_t v;
                do v = decode(tables().mbai) & 0xFFFFu;
                while (v == 0x22u || v == 0x23u);
                g_phase = 1;
            });
            break;
        case 3:
            ok = atomic([&] {
                for (;;) {
                    std::size_t avail;
                    const std::uint32_t code = peek_soft(24, avail);
                    if (avail < 24 || code == 1) {
                        g_phase = 4;
                        return;
                    }
                    if (code != 0) throw BadCode{};
                    g_bit += 8;
                }
            });
            break;
        default:
            g_ctrl |= kScd;
            return true;
        }
        if (!ok) return false;
        commit();
    }
}

bool run_command() {
    const std::uint32_t cmd = g_command;
    switch (cmd >> 28) {
    case kBclr:
        dump_bits_to((g_bit + 7) / 8);
        g_in.clear();
        g_bit = cmd & 0x7Fu;
        g_dump_pos = g_bit / 8;
        return true;
    case kIdec:
        return run_idec();
    case kBdec: {
        const bool ok = atomic([&] {
            skip(cmd & 0x3Fu);
            std::int16_t raw[384];
            decode_macroblock(raw, (cmd >> 27) & 1u, (cmd >> 25) & 1u, (cmd >> 26) & 1u, (cmd >> 16) & 0x1Fu);
            emit_raw16(raw);
        });
        if (!ok) return false;
        commit();
        if (start_code_follows()) {
            g_ctrl |= kScd;
            g_interrupt = false;
        }
        return true;
    }
    case kVdec: {
        const bool ok = atomic([&] {
            skip(cmd & 0x3Fu);
            const Lookup *table = nullptr;
            switch ((cmd >> 26) & 3u) {
            case 0: table = &tables().mbai; break;
            case 1:
                switch (picture_type()) {
                case 1: table = &tables().mb_i; break;
                case 2: table = &tables().mb_p; break;
                case 3: table = &tables().mb_b; break;
                default: throw BadCode{};
                }
                break;
            case 2: table = &tables().motion; break;
            default: table = &tables().dmv; break;
            }
            g_result = decode(*table);
        });
        if (ok) commit();
        return ok;
    }
    case kFdec: {
        // A player that ends a film looks for the sequence end code (00 00 01 B7) with FDEC, and waits for the IPU to be idle OR for
        // ECD in IPU_CTRL (Robotech: Battlecry's film player does). When the end code is at the read position, or where this
        // command's result will be taken from, ECD is set at once: the command itself may then wait for bits behind the end of the
        // last film, which nothing sends (the stream ends with the code).
        for (const std::size_t at : {g_bit, g_bit + (cmd & 0x3Fu)})
            if (g_in.size() * 8 >= at + 32 && bits_at(at, 32) == 0x1B7u) g_ctrl |= kEcd;
        const bool ok = atomic([&] {
            skip(cmd & 0x3Fu);
            g_result = peek(32);
        });
        if (ok) commit();
        return ok;
    }
    case kSetiq: {
        std::uint8_t *matrix = (cmd >> 27) & 1u ? g_nonintra_iq : g_intra_iq;
        std::uint8_t values[64];
        const bool ok = atomic([&] {
            skip(cmd & 0x3Fu);
            for (std::uint8_t &v : values) v = static_cast<std::uint8_t>(get(8));
        });
        if (!ok) return false;
        for (int k = 0; k < 64; ++k) matrix[kZigzag[k]] = values[k];
        commit();
        return true;
    }
    case kSetvq: {
        std::uint16_t values[16];
        const bool ok = atomic([&] {
            skip(cmd & 0x3Fu);
            for (std::uint16_t &v : values) v = static_cast<std::uint16_t>(get(16));
        });
        if (!ok) return false;
        std::memcpy(g_vqclut, values, sizeof values);
        commit();
        return true;
    }
    case kCsc:
        dump_bits_to((g_bit + 7) / 8);
        g_csc_active = true;
        while (g_count) {
            std::uint8_t raw[384];
            const bool ok = atomic([&] {
                get_bytes(raw, sizeof raw);
                convert(raw, (cmd >> 27) & 1u);
            });
            if (!ok) {
                g_csc_active = false;
                g_dump_pos = (g_bit + 7) / 8;
                return false;
            }
            commit();
            --g_count;
        }
        g_csc_active = false;
        g_dump_pos = (g_bit + 7) / 8;
        return true;
    case kSetth:
        g_th0 = static_cast<std::uint16_t>(cmd & 0x1FFu);
        g_th1 = static_cast<std::uint16_t>((cmd >> 16) & 0x1FFu);
        return true;
    default:
        std::fprintf(stderr, "[ipu] command 0x%08X is not implemented\n", cmd);
        return true;
    }
}

void drain() {
    while (g_out_pos + 16 <= g_out.size()) {
        const std::size_t done = dmac::ipu_push_many(g_out.data() + g_out_pos, (g_out.size() - g_out_pos) / 16);
        if (!done) break;
        g_out_pos += done * 16;
        g_out_qwords += done;
    }
    if (g_out_pos == g_out.size()) {
        g_out.clear();
        g_out_pos = 0;
    }
}

void step() {
    if (g_busy && g_decoding) {
        try {
            g_starved = !run_command();
            if (!g_starved) g_decoding = false;
        } catch (const BadCode &) {
            static int shown = 0;
            if (shown++ < 8)
                std::fprintf(stderr, "[ipu] bad code in command 0x%08X (frame %llu, bit %zu of %zu, next %08X %08X, picture type %u)\n", g_command,
                             static_cast<unsigned long long>(ps2::frames_run()), g_bit, g_in.size() * 8, bits_at(g_bit, 32), bits_at(g_bit + 32, 32),
                             picture_type());
            g_ctrl |= kEcd;
            g_decoding = false;
            g_busy = false;
        }
    }
    drain();
    if (g_busy && !g_decoding && g_out.empty()) {
        g_busy = false;
        if (g_interrupt) hw::intc_raise(8);
    }
    top_up();
}

}

void reset() {
    g_in.clear();
    g_out.clear();
    g_bit = g_out_pos = 0;
    g_ctrl = g_result = g_command = 0;
    g_last = -1;
    g_busy = g_decoding = false;
    std::memcpy(g_intra_iq, kDefaultIntra, 64);
    std::fill(std::begin(g_nonintra_iq), std::end(g_nonintra_iq), 16);
    std::memset(g_vqclut, 0, sizeof g_vqclut);
    g_th0 = g_th1 = 0;
    std::memset(g_dc, 0, sizeof g_dc);
}

std::uint32_t read32(std::uint32_t a) {
    switch (a & 0xFFFFFFFCu) {
    case 0x10002000u:
        return g_last == kVdec || g_last == kFdec ? g_result : peek_now();
    case 0x10002004u:
        return g_busy ? 0x80000000u : 0u;
    case 0x10002010u: {
        const std::uint32_t ifc = g_starved ? 0u : static_cast<std::uint32_t>(std::min<std::size_t>(8, g_in.size() / 16));
        const std::uint32_t ofc = static_cast<std::uint32_t>(std::min<std::size_t>(8, (g_out.size() - g_out_pos) / 16));
        return (g_ctrl & ~0x800000FFu) | (g_busy ? 0x80000000u : 0u) | ifc | (ofc << 4);
    }
    case 0x10002020u: {
        if (g_starved) return 0;
        const std::uint32_t bp = static_cast<std::uint32_t>(g_bit & 127u);
        std::uint32_t ifc = static_cast<std::uint32_t>(g_in.size() / 16 - std::min(g_in.size() / 16, g_bit / 128));
        std::uint32_t fp = 0;
        if (bp && ifc) {
            fp = 1;
            --ifc;
        }
        return (fp << 16) | (std::min<std::uint32_t>(ifc, 8) << 8) | bp;
    }
    case 0x10002030u:
        return g_busy ? 0u : peek_now();
    case 0x10002034u:
        return g_busy || available() < 32 ? 0x80000000u : 0u;
    default:
        return 0;
    }
}

void write32(std::uint32_t a, std::uint32_t v) {
    switch (a) {
    case 0x10002000u: {
    }
        g_ctrl &= ~(kEcd | kScd);
        g_starved = false;
        ++g_commands[v >> 28];
        g_command = v;
        g_last = static_cast<int>(v >> 28);
        g_busy = g_decoding = g_interrupt = true;
        g_phase = 0;
        g_qsc = (v >> 16) & 0x1Fu;
        g_first = true;
        g_count = v & 0x7FFu;
        step();
        {
            static const char *trace = std::getenv("PS2_TRACE_IPU");
            static std::uint64_t traced = 0, from = trace ? std::strtoull(trace, nullptr, 10) : 0;
            if (trace && ++traced >= from && traced < from + 200000)
                std::fprintf(stderr, "[ipu] cmd 0x%08X -> result %08X ctrl %08X bit %zu next %08X%08X\n", v, g_result, g_ctrl, g_bit,
                             bits_at(g_bit, 32), bits_at(g_bit + 32, 32));
        }
        break;
    case 0x10002010u:
        if (v & kReset) {
            g_starved = false;
            g_in.clear();
            g_out.clear();
            g_bit = g_out_pos = 0;
            g_busy = g_decoding = false;
            g_th0 = g_th1 = 0;
        }
        g_ctrl = (g_ctrl & ~0x3FFF0000u) | (v & 0x3FFF0000u);
        break;
    default:
        if (a >= 0x10007010u && a < 0x10007020u) {
            const std::uint8_t *bytes = reinterpret_cast<const std::uint8_t *>(&v);
            dump_input(bytes, 4);
            g_in.insert(g_in.end(), bytes, bytes + 4);
            step();
        }
        break;
    }
}

void write_fifo(const std::uint8_t *qword) {
    dump_input(qword, 16);
    g_in.insert(g_in.end(), qword, qword + 16);
    step();
}

void read_fifo(std::uint8_t *qword) {
    if (g_out_pos + 16 <= g_out.size()) {
        std::memcpy(qword, g_out.data() + g_out_pos, 16);
        g_out_pos += 16;
    } else {
        std::memset(qword, 0, 16);
    }
    step();
}

void dma_started(int) { step(); }

void stats() {
    std::fprintf(stderr, "[ipu] commands:");
    for (int i = 0; i < 16; ++i)
        if (g_commands[i]) std::fprintf(stderr, " %X:%llu", i, static_cast<unsigned long long>(g_commands[i]));
    std::fprintf(stderr, " stalls %llu out %llu qwords, busy %d decoding %d cmd 0x%08X in %zu bytes bit %zu out %zu\n",
                 static_cast<unsigned long long>(g_stalls), static_cast<unsigned long long>(g_out_qwords), g_busy, g_decoding, g_command,
                 g_in.size(), g_bit, g_out.size() - g_out_pos);
}

}

namespace ps2::ipu {

void state(ps2::state::Stream &s) {
    s.tag("ipu");
    s.vec(g_in);
    s.size(g_bit);
    s.vec(g_out);
    s.size(g_out_pos);
    s.pod(g_ctrl);
    s.pod(g_result);
    s.pod(g_command);
    s.pod(g_last);
    s.pod(g_busy);
    s.pod(g_decoding);
    s.pod(g_interrupt);
    s.pod(g_starved);
    s.pod(g_phase);
    s.pod(g_qsc);
    s.pod(g_count);
    s.pod(g_first);
    s.pod(g_intra_iq);
    s.pod(g_nonintra_iq);
    s.pod(g_vqclut);
    s.pod(g_th0);
    s.pod(g_th1);
    s.pod(g_dc);
}

}
