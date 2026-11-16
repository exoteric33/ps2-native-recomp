// No game data: every UNPACK form (V1 to V4, 32/16/8 bits, V4-5, signed and unsigned), with and without the write mask, under every
// STMOD (none, offset, difference, keep row) and write cycle (normal, skipping, filling), fed through the VIF and compared with a plain
// lane-by-lane model of the unpack rules, on both units. The VIF has fast forms for the common cases; this checks they write exactly
// what the lane-by-lane rules write, the row registers included (a later UNPACK in offset mode shows them).
// Build and run: ninja ps2vifunpacktest && build/<dir>/ps2vifunpacktest
#include "hw.hpp"
#include "vif.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

namespace {

std::uint8_t *vu_mem(int unit) { return unit ? ps2::g_vu1_mem : ps2::g_vu0_mem; }
std::uint32_t vu_qwords(int unit) { return unit ? 1024u : 256u; }

// The unpack rules, one lane at a time (the PS2's VIF documentation: STCYCL, STMOD, STMASK, STROW, STCOL and the UNPACK formats).
struct Model {
    std::uint32_t cycle = 0, mode = 0, mask = 0, row[4] = {}, col[4] = {};
    std::vector<std::uint8_t> mem;

    std::uint32_t wl() const { const std::uint32_t w = (cycle >> 8) & 0xFFu; return w ? w : 256u; }
    std::uint32_t cl() const { const std::uint32_t c = cycle & 0xFFu; return c == 0 && (cycle & 0xFF00u) == 0 ? wl() : c; }

    void unpack(std::uint32_t cmd, const std::uint32_t *data) {
        const std::uint32_t vn = (cmd >> 26) & 3u, vl = (cmd >> 24) & 3u;
        const bool masked = (cmd >> 28) & 1u, usn = (cmd >> 14) & 1u;
        std::uint32_t num = (cmd >> 16) & 0xFFu;
        if (!num) num = 256;
        std::uint32_t addr = cmd & 0x3FFu;
        const std::uint32_t w = wl(), c = cl(), wrap = static_cast<std::uint32_t>(mem.size() / 16);
        const std::uint32_t bits = vl == 0 ? 32 : vl == 2 ? 8 : 16;
        std::size_t bit = 0;
        auto element = [&]() {
            std::uint32_t raw = data[bit / 32] >> (bit % 32);
            if (bits < 32) raw &= (1u << bits) - 1u;
            bit += bits;
            if (!usn && bits == 16) raw = static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int16_t>(raw)));
            if (!usn && bits == 8) raw = static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int8_t>(raw)));
            return raw;
        };
        std::uint32_t cyc = 0;
        for (std::uint32_t n = 0; n < num; ++n) {
            const bool fill = w > c && cyc >= c;
            std::uint32_t v[4] = {};
            if (!fill) {
                if (vn == 3 && vl == 3) {
                    const std::uint32_t x = element() & 0xFFFFu;
                    v[0] = (x & 0x1Fu) << 3; v[1] = ((x >> 5) & 0x1Fu) << 3; v[2] = ((x >> 10) & 0x1Fu) << 3; v[3] = ((x >> 15) & 1u) << 7;
                } else {
                    for (std::uint32_t j = 0; j <= vn; ++j) v[j] = element();
                    if (vn == 0) v[1] = v[2] = v[3] = v[0];
                    if (vn == 1) { v[2] = v[0]; v[3] = v[1]; }
                }
            }
            const std::uint32_t r = std::min<std::uint32_t>(cyc, 3u);
            for (std::uint32_t j = 0; j < 4; ++j) {
                std::uint32_t m = masked ? (mask >> ((r * 4 + j) * 2)) & 3u : 0u;
                if (fill && m == 0) m = 1;
                std::uint32_t out;
                if (m == 3) continue;
                if (m == 1) out = row[j];
                else if (m == 2) out = col[r];
                else if (mode == 1) out = v[j] + row[j];
                else if (mode == 2) out = row[j] = v[j] + row[j];
                else if (mode == 3) out = row[j] = v[j];
                else out = v[j];
                std::memcpy(&mem[(addr % wrap) * 16 + j * 4], &out, 4);
            }
            ++addr;
            ++cyc;
            if (c >= w) {
                if (cyc >= w) { addr += c - w; cyc = 0; }
            } else if (cyc >= w) {
                cyc = 0;
            }
        }
    }
};

std::uint32_t data_words(std::uint32_t cmd, const Model &m) {
    const std::uint32_t vn = (cmd >> 26) & 3u, vl = (cmd >> 24) & 3u;
    std::uint32_t num = (cmd >> 16) & 0xFFu;
    if (!num) num = 256;
    const std::uint32_t w = m.wl(), c = m.cl();
    std::uint32_t d = num;
    if (w > c) d = (num / w) * c + std::min(num % w, c);
    const std::uint32_t bits = (vn == 3 && vl == 3) ? 16u : (vn + 1) * (vl == 0 ? 32u : vl == 2 ? 8u : 16u);
    return (d * bits + 31) / 32;
}

}  // namespace

int main() {
    std::mt19937 rng(20261111);
    auto rnd = [&](std::uint32_t n) { return static_cast<std::uint32_t>(rng() % n); };
    int failed = 0;
    long unpacks = 0, fast_like = 0;
    for (int round = 0; round < 6000 && failed < 10; ++round) {
        const int unit = static_cast<int>(rnd(2));
        ps2::vif::reset();
        Model m;
        m.mem.assign(vu_qwords(unit) * 16, 0);
        for (std::size_t i = 0; i < m.mem.size(); ++i) m.mem[i] = static_cast<std::uint8_t>(rng());
        std::memcpy(vu_mem(unit), m.mem.data(), m.mem.size());
        std::vector<std::uint32_t> w;
        for (int k = 0; k < 12; ++k) {
            switch (rnd(8)) {
            case 0: {  // STCYCL: normal, skipping or filling, sometimes 0
                const std::uint32_t cl = rnd(5), wl = rnd(4) ? cl : rnd(5);
                m.cycle = cl | (wl << 8);
                w.push_back(0x01000000u | m.cycle);
                break;
            }
            case 1: m.mode = rnd(4); w.push_back(0x05000000u | m.mode); break;
            case 2: m.mask = static_cast<std::uint32_t>(rng()); w.push_back(0x20000000u); w.push_back(m.mask); break;
            case 3:
                w.push_back(0x30000000u);
                for (std::uint32_t &r : m.row) { r = rnd(4) ? static_cast<std::uint32_t>(rng()) : 0xFFFFFFF0u + rnd(32); w.push_back(r); }
                break;
            case 4:
                w.push_back(0x31000000u);
                for (std::uint32_t &c : m.col) { c = static_cast<std::uint32_t>(rng()); w.push_back(c); }
                break;
            default: {  // UNPACK, mostly without the mask (the fast forms)
                const bool masked = rnd(4) == 0;
                const std::uint32_t vn = rnd(4), vl = rnd(4), usn = rnd(2);
                const std::uint32_t num = rnd(16) == 0 ? 0u : 1u + rnd(40);
                const std::uint32_t addr = rnd(1024);
                const std::uint32_t cmd = (0x60u | (masked ? 0x10u : 0u) | (vn << 2) | vl) << 24 | (num << 16) | (usn << 14) | addr;
                const std::uint32_t words = data_words(cmd, m);
                std::vector<std::uint32_t> d(words + 1);
                for (std::uint32_t &x : d) x = static_cast<std::uint32_t>(rng());
                w.push_back(cmd);
                w.insert(w.end(), d.begin(), d.begin() + words);
                m.unpack(cmd, d.data());
                ++unpacks;
                if (!masked && !(m.wl() > m.cl()) && !(vn == 3 && vl == 3)) ++fast_like;
                break;
            }
            }
        }
        // Fed whole, or cut at a random word (a DMA tag boundary inside a command).
        const std::size_t cut = rnd(3) == 0 ? rnd(static_cast<std::uint32_t>(w.size())) : 0;
        if (cut) {
            ps2::vif::transfer_words(unit, w.data(), static_cast<std::uint32_t>(cut));
            ps2::vif::transfer_words(unit, w.data() + cut, static_cast<std::uint32_t>(w.size() - cut));
        } else {
            ps2::vif::transfer_words(unit, w.data(), static_cast<std::uint32_t>(w.size()));
        }
        if (std::memcmp(vu_mem(unit), m.mem.data(), m.mem.size()) != 0) {
            std::size_t at = 0;
            while (vu_mem(unit)[at] == m.mem[at]) ++at;
            std::fprintf(stderr, "FAIL round %d (VIF%d): VU memory differs first at byte %zu (quadword %zu)\n", round, unit, at, at / 16);
            ++failed;
        }
    }
    std::printf("%ld UNPACKs (%ld of a fast form) in 6000 streams: %s\n", unpacks, fast_like, failed ? "FAIL" : "all equal to the lane-by-lane rules");
    return failed ? 1 : 0;
}
