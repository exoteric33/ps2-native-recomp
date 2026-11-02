#include "gif.hpp"

#include "gs.hpp"
#include "queue.hpp"

#include "ps2/state.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace ps2::gif {

namespace {

struct Path {
    bool active = false;
    std::uint32_t nloop = 0, loop = 0, nreg = 0, reg = 0, flg = 0;
    std::uint64_t regs = 0;
    bool eop = false;
    bool address_data = false;
};

Path g_paths[4];
std::uint32_t g_stat = 0;
std::uint64_t g_tags[4][4];

template <typename Sink>
void walk(Path &p, const std::uint8_t *data, std::uint32_t qwc, Sink &sink) {
    for (std::uint32_t i = 0; i < qwc; ++i) {
        Reg128 q;
        std::memcpy(&q, data + static_cast<std::size_t>(i) * 16, 16);
        if (!p.active) {
            p.nloop = static_cast<std::uint32_t>(q.d[0]) & 0x7FFFu;
            p.eop = (q.d[0] >> 15) & 1u;
            p.flg = static_cast<std::uint32_t>(q.d[0] >> 58) & 3u;
            p.nreg = static_cast<std::uint32_t>(q.d[0] >> 60) & 0xFu;
            if (!p.nreg) p.nreg = 16;
            p.regs = q.d[1];
            p.loop = 0;
            p.reg = 0;
            sink.tag(p, q);
            p.active = p.nloop != 0;
            continue;
        }
        switch (p.flg) {
        case 0: {
            // PACKED data is nearly all of a draw: take the qwords one after another with the loop counters in registers (the sink is a call
            // the compiler cannot see into, so with `p` in memory it would load and store them for every qword) and write them back at the end.
            std::uint32_t reg = p.reg, loop = p.loop;
            const std::uint32_t nreg = p.nreg, nloop = p.nloop;
            const std::uint64_t regs = p.regs;
            for (;;) {
                sink.packed(static_cast<std::uint32_t>(regs >> (reg * 4)) & 0xFu, q);
                if (++reg >= nreg) {
                    reg = 0;
                    if (++loop >= nloop) {
                        p.active = false;
                        break;
                    }
                }
                if (i + 1 >= qwc) break;
                ++i;
                std::memcpy(&q, data + static_cast<std::size_t>(i) * 16, 16);
            }
            p.reg = reg;
            p.loop = loop;
            break;
        }
        case 1:
            for (int half = 0; half < 2 && p.active; ++half) {
                sink.reglist(static_cast<std::uint32_t>(p.regs >> (p.reg * 4)) & 0xFu, q.d[half]);
                if (++p.reg >= p.nreg) {
                    p.reg = 0;
                    if (++p.loop >= p.nloop) p.active = false;
                }
            }
            break;
        default: {
            const std::uint32_t run = std::min(qwc - i, p.nloop - p.loop);
            sink.image(data + static_cast<std::size_t>(i) * 16, run);
            p.loop += run;
            i += run - 1;
            if (p.loop >= p.nloop) p.active = false;
            break;
        }
        }
    }
}

struct GsSink {
    int path;
    void tag(const Path &p, const Reg128 &q) {
        ++g_tags[path][p.flg];
        gs::g_state.q = 1.0f;
        if (((q.d[0] >> 46) & 1u) && p.flg == 0) gs::write(0x00, (q.d[0] >> 47) & 0x7FFu);
    }
    void packed(std::uint32_t reg, const Reg128 &q) { gs::write_packed(reg, q); }
    void reglist(std::uint32_t reg, std::uint64_t value) { gs::write(reg, value); }
    void image(const std::uint8_t *data, std::uint32_t qwords) { gs::write_image(data, qwords); }
};

void scan(Path &p, const std::uint8_t *data, std::uint32_t qwc) {
    std::uint32_t i = 0;
    while (i < qwc) {
        Reg128 q;
        if (!p.active) {
            std::memcpy(&q, data + static_cast<std::size_t>(i) * 16, 16);
            p.nloop = static_cast<std::uint32_t>(q.d[0]) & 0x7FFFu;
            p.flg = static_cast<std::uint32_t>(q.d[0] >> 58) & 3u;
            p.nreg = static_cast<std::uint32_t>(q.d[0] >> 60) & 0xFu;
            if (!p.nreg) p.nreg = 16;
            p.regs = q.d[1];
            p.loop = 0;
            p.reg = 0;
            p.address_data = false;
            if (p.flg == 0)
                for (std::uint32_t r = 0; r < p.nreg; ++r) p.address_data = p.address_data || ((p.regs >> (r * 4)) & 0xFu) == 0xEu;
            p.active = p.nloop != 0;
            ++i;
            continue;
        }
        if (p.flg == 0 && p.address_data) {
            for (; i < qwc && p.active; ++i) {
                const std::uint8_t *at = data + static_cast<std::size_t>(i) * 16;
                if (((p.regs >> (p.reg * 4)) & 0xFu) == 0xEu && at[8] >= 0x50 && at[8] <= 0x62) {
                    std::uint64_t value;
                    std::memcpy(&value, at, 8);
                    if (at[8] >= 0x60) gs::signal(at[8], value);
                    else if (at[8] <= 0x53) gs::note_transfer(at[8], value);
                }
                if (++p.reg >= p.nreg) {
                    p.reg = 0;
                    if (++p.loop >= p.nloop) p.active = false;
                }
            }
            continue;
        }
        const std::uint64_t done = static_cast<std::uint64_t>(p.loop) * p.nreg + p.reg;
        const std::uint64_t left = static_cast<std::uint64_t>(p.nloop) * p.nreg - done;
        std::uint64_t take, used;
        if (p.flg == 0) {
            take = used = std::min<std::uint64_t>(left, qwc - i);
        } else if (p.flg == 1) {
            take = std::min<std::uint64_t>((left + 1) / 2, qwc - i);
            used = std::min<std::uint64_t>(take * 2, left);
        } else {
            take = std::min<std::uint64_t>(p.nloop - p.loop, qwc - i);
            p.loop += static_cast<std::uint32_t>(take);
            if (p.loop >= p.nloop) p.active = false;
            i += static_cast<std::uint32_t>(take);
            continue;
        }
        p.loop = static_cast<std::uint32_t>((done + used) / p.nreg);
        p.reg = static_cast<std::uint32_t>((done + used) % p.nreg);
        if (used == left) p.active = false;
        i += static_cast<std::uint32_t>(take);
    }
}

Path g_scan[4];

// PATH3 under VIF1's MSKPATH3: while the mask is up a real GIF leaves channel 2's data
// waiting, and a mask that comes up while a packet is under way stops PATH3 at the packet's
// end (the tag with EOP). Games stream textures through a few slots of GS memory this way:
// the VIF1 list drops the mask and raises it again right away before each draw that needs
// the next texture, which lets exactly one packet through, and leaves it down at the end of
// the list, which lets the rest through. Channel 2 finishes at once here, so its data is
// held until then, in the thread that runs VIF1 for real so that it reaches the GS in order
// with VIF1's own.
struct Held {
    bool masked = false, mid_packet = false, flowing = false;
    std::uint64_t left = 0;
    bool eop = false;
    std::vector<std::uint8_t> data;
} g_held;

bool hold_here() { return !gs::queue::vu_active() || gs::queue::role() == gs::queue::Role::Vu; }

void out(int path, const std::uint8_t *data, std::uint32_t qwc) {
    if (!qwc) return;
    if (!gs::queue::active()) {
        render(path, data, qwc);
        return;
    }
    if (gs::queue::role() != gs::queue::Role::Vu) scan(g_scan[path], data, qwc);
    gs::queue::post(gs::queue::kGif, static_cast<std::uint32_t>(path), qwc, data, qwc * 16);
}

// How many of these qwords belong to the packet under way; mid_packet is left set when
// the packet goes on past them.
std::uint32_t packet_length(const std::uint8_t *data, std::uint32_t qwc) {
    Held &h = g_held;
    std::uint32_t i = 0;
    while (i < qwc) {
        if (!h.left) {
            std::uint64_t tag;
            std::memcpy(&tag, data + static_cast<std::size_t>(i) * 16, 8);
            const std::uint64_t nloop = tag & 0x7FFFu, flg = (tag >> 58) & 3u;
            std::uint64_t nreg = (tag >> 60) & 0xFu;
            if (!nreg) nreg = 16;
            h.eop = (tag >> 15) & 1u;
            h.left = flg == 0 ? nloop * nreg : flg == 1 ? (nloop * nreg + 1) / 2 : nloop;
            h.mid_packet = true;
            ++i;
        } else {
            const std::uint32_t take = static_cast<std::uint32_t>(std::min<std::uint64_t>(h.left, qwc - i));
            h.left -= take;
            i += take;
        }
        if (!h.left && h.eop) {
            h.mid_packet = false;
            return i;
        }
    }
    return qwc;
}

// Lets held data out while PATH3 may run; with only_one, just the next packet.
void release(bool only_one) {
    Held &h = g_held;
    std::size_t done = 0;
    const std::uint32_t total = static_cast<std::uint32_t>(h.data.size() / 16);
    while (done < total && (!h.masked || h.mid_packet)) {
        const std::uint8_t *at = h.data.data() + done * 16;
        const std::uint32_t n = packet_length(at, static_cast<std::uint32_t>(total - done));
        out(3, at, n);
        done += n;
        if (only_one && !h.mid_packet) break;
    }
    h.data.erase(h.data.begin(), h.data.begin() + static_cast<std::ptrdiff_t>(done * 16));
}

}

void drop_held() {
    const bool masked = g_held.masked;
    g_held = Held{};
    g_held.masked = masked;
}

void reset() {
    g_stat = 0;
    if (hold_here()) drop_held();
    if (gs::queue::active()) {
        for (Path &p : g_scan) p = Path{};
        gs::queue::post(gs::queue::kGifReset, 0, 0, nullptr, 0);
        return;
    }
    render_reset();
}

void render_reset() {
    for (Path &p : g_paths) p = Path{};
}

void render(int path, const std::uint8_t *data, std::uint32_t qwc) {
    GsSink sink{path};
    walk(g_paths[path], data, qwc, sink);
}

void transfer(int path, const std::uint8_t *data, std::uint32_t qwc) {
    Held &h = g_held;
    if (!hold_here()) {
        out(path, data, qwc);
        return;
    }
    // Once the mask is down, whatever is still held goes ahead of anything new.
    if (h.flowing && !h.masked) {
        h.flowing = false;
        release(false);
    }
    if (path != 3) {
        out(path, data, qwc);
        return;
    }
    if (h.data.empty() && (!h.masked || h.mid_packet)) {
        std::uint32_t done = 0;
        while (done < qwc && (!h.masked || h.mid_packet)) done += packet_length(data + static_cast<std::size_t>(done) * 16, qwc - done);
        out(3, data, done);
        data += static_cast<std::size_t>(done) * 16;
        qwc -= done;
    }
    h.data.insert(h.data.end(), data, data + static_cast<std::size_t>(qwc) * 16);
}

void scan_signals(int path, const std::uint8_t *data, std::uint32_t qwc) { scan(g_scan[path], data, qwc); }

bool busy(int path) { return g_paths[path].active; }

void stats() {
    for (int p = 1; p < 4; ++p)
        std::fprintf(stderr, "[gif] path %d tags: packed %llu reglist %llu image %llu\n", p,
                     static_cast<unsigned long long>(g_tags[p][0]), static_cast<unsigned long long>(g_tags[p][1]),
                     static_cast<unsigned long long>(g_tags[p][2] + g_tags[p][3]));
}

std::uint32_t read(std::uint32_t a) {
    // While PATH3 is masked a real GIF holds the stalled channel 2 transfer in its FIFO, and
    // games wait for FQC to read non-zero before they touch the GS; channel 2 finishes at
    // once here, so report the FIFO full for as long as the mask is up.
    if ((a & 0xF0u) == 0x20u) return g_stat | ((g_stat & 2u) ? 16u << 24 : 0u);
    return 0;
}

void write(std::uint32_t a, std::uint32_t v) {
    if ((a & 0xF0u) == 0x00u && (v & 1u)) reset();
    if ((a & 0xF0u) == 0x10u) g_stat = (g_stat & ~1u) | (v & 1u);
}

void vif1_idle() {
    if (!hold_here()) {
        gs::queue::vu().post(gs::queue::kPath3Idle, 0, 0, nullptr, 0);
        return;
    }
    Held &h = g_held;
    h.flowing = false;
    if (!h.masked) release(false);
}

void set_path3_masked(bool masked) {
    if (gs::queue::role() != gs::queue::Role::Vu) g_stat = (g_stat & ~2u) | (masked ? 2u : 0u);
    static const bool trace = std::getenv("PS2_TRACE_PATH3") != nullptr;
    if (trace) std::fprintf(stderr, "[gif] path3 %s\n", masked ? "masked" : "unmasked");
    if (!hold_here()) return;
    Held &h = g_held;
    const bool was = h.masked;
    h.masked = masked;
    // The packet that starts as the mask drops gets through even if the mask goes straight
    // back up; the rest waits for more GIF traffic or for VIF1 to go idle with it still down.
    if (was && !masked && !h.data.empty()) {
        release(true);
        h.flowing = !h.data.empty();
    }
}

}

namespace ps2::gif {

void state(ps2::state::Stream &s) {
    s.tag("gif");
    s.pod(g_paths);
    s.pod(g_stat);
    s.pod(g_tags);
    s.pod(g_scan);
}

}
