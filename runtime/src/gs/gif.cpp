#include "gif.hpp"

#include "gs.hpp"
#include "queue.hpp"
#include "../vif.hpp"
#include "../vu_micro.hpp"

#include "ps2/state.hpp"
#include "ps2/system.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

namespace ps2::gs::trace { extern int g_path; }  // gs.cpp: the path the draw trace (PS2_TRACE_DRAWS) prints

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
            if (sink.runs(regs, nreg)) {
                // the rest of this tag's data, or of this message, in one call (gs::write_packed_run)
                const std::uint64_t left = static_cast<std::uint64_t>(nloop - loop) * nreg - reg;
                const std::uint32_t n = static_cast<std::uint32_t>(std::min<std::uint64_t>(left, qwc - i));
                sink.packed_run(data + static_cast<std::size_t>(i) * 16, n, regs, nreg, reg);
                const std::uint64_t done = static_cast<std::uint64_t>(loop) * nreg + reg + n;
                p.loop = static_cast<std::uint32_t>(done / nreg);
                p.reg = static_cast<std::uint32_t>(done % nreg);
                if (p.loop >= nloop) p.active = false;
                i += n - 1;
                break;
            }
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

// In the path argument of a GIF message: end that path's packet there (end_runaway_kick).
constexpr int kEndPacket = 0x100;
// The data of this message are a gs::TripTicket standing for qwc quadwords of image data: a depth download's bytes, sent back
// unchanged (gs.cpp).
constexpr int kTripFlag = 0x200;

// Signs that the GIF is reading something that is not a packet (vertex data taken for a tag, which then sprays the rest over random
// registers): writes to GS registers that do not exist, and XGKICK packets without an end. A game does neither on purpose. Counted
// per path on the drawing thread (render()), with the first of each kept, and printed by stats(), so a player's log shows which
// path and from which frame on (public issue #69, Nickelodeon Barnyard: about 15000 writes to every missing register).
struct Odd {
    std::uint64_t count = 0;
    std::uint32_t frame = 0, reg = 0, address = 0;
    char how = 0;  // 'a' an A+D address, 'p' a PACKED register descriptor, 'r' a REGLIST one
    std::uint64_t tag[2] = {0, 0};
};
Odd g_missing[4], g_runaway[4];
std::uint64_t g_last_tag[4][2];

// The GS registers by address (GS manual, register list; 0x7F is the address ps2sdk writes as a NOP).
constexpr std::uint64_t kRegsLow = 0xA8F000041FF037FFull, kRegsHigh = 0x80000007001FFFFFull;
bool exists(std::uint32_t reg) { return reg < 64 ? ((kRegsLow >> reg) & 1u) : reg < 128 && ((kRegsHigh >> (reg - 64)) & 1u); }

// The first of each is also printed at once: a run that freezes later never reaches stats().
void note(Odd &o, int path, std::uint32_t reg, char how, std::uint32_t address) {
    if (o.count++) return;
    o.frame = gs::present_frame();
    o.reg = reg;
    o.how = how;
    o.address = address;
    o.tag[0] = g_last_tag[path][0];
    o.tag[1] = g_last_tag[path][1];
    if (how)
        std::fprintf(stderr, "[gif] path %d writes a GS register that does not exist about frame %u (%s 0x%02X, packet tag %016llX %016llX)\n", path, o.frame,
                     how == 'a' ? "A+D address" : how == 'p' ? "PACKED descriptor" : "REGLIST descriptor", reg,
                     static_cast<unsigned long long>(o.tag[1]), static_cast<unsigned long long>(o.tag[0]));
    else
        std::fprintf(stderr, "[gif] path %d: an XGKICK packet did not end within VU1's data memory about frame %u (kicked at 0x%03X, last tag %016llX %016llX)\n",
                     path, o.frame, address, static_cast<unsigned long long>(o.tag[1]), static_cast<unsigned long long>(o.tag[0]));
}

struct GsSink {
    int path;
    void tag(const Path &p, const Reg128 &q) {
        ++g_tags[path][p.flg];
        g_last_tag[path][0] = q.d[0];
        g_last_tag[path][1] = q.d[1];
        gs::g_state.q = 1.0f;
        if (((q.d[0] >> 46) & 1u) && p.flg == 0) gs::write(0x00, (q.d[0] >> 47) & 0x7FFu);
    }
    void packed(std::uint32_t reg, const Reg128 &q) {
        // 0xB is the one PACKED descriptor the GS manual leaves reserved; 0xE is A+D, whose address byte must name a register.
        if ((reg == 0xEu && !exists(static_cast<std::uint32_t>(q.d[1]) & 0xFFu)) || reg == 0xBu) [[unlikely]]
            note(g_missing[path], path, reg == 0xEu ? static_cast<std::uint32_t>(q.d[1]) & 0xFFu : reg, reg == 0xEu ? 'a' : 'p', 0);
        gs::write_packed(reg, q);
    }
    // True when none of the tag's descriptors is A+D (0xE, whose address is checked qword by qword) or the reserved 0xB: its data can then
    // go to the GS in runs (packed_run) with no check per qword.
    static bool runs(std::uint64_t regs, std::uint32_t nreg) {
        for (std::uint32_t r = 0; r < nreg; ++r) {
            const std::uint32_t d = static_cast<std::uint32_t>(regs >> (r * 4)) & 0xFu;
            if (d == 0xEu || d == 0xBu) return false;
        }
        return true;
    }
    void packed_run(const std::uint8_t *data, std::uint32_t count, std::uint64_t regs, std::uint32_t nreg, std::uint32_t reg) {
        gs::write_packed_run(data, count, regs, nreg, reg);
    }
    void reglist(std::uint32_t reg, std::uint64_t value) {
        if (reg == 0xBu || reg == 0xEu) [[unlikely]] note(g_missing[path], path, reg, 'r', 0);
        gs::write(reg, value);
    }
    void image(const std::uint8_t *data, std::uint32_t qwords) { gs::write_image(data, qwords); }
};

// PATH1's SIGNAL, FINISH and LABEL (gif.hpp): found by the thread that runs VU1 (the emulation thread or the vector thread), kept here
// in order until the emulation thread hands them on. Each carries the number of fences (path1_fence) that thread had passed when it found
// it, so the emulation thread can tell what was sent before a fence it posted. The count lets a delivery with nothing waiting skip the lock.
struct Found {
    std::uint32_t reg, fence;
    std::uint64_t value;
};
std::mutex g_path1_lock;
std::vector<Found> g_path1;
std::atomic<std::uint32_t> g_path1_count{0};
std::atomic<std::uint32_t> g_path1_fences{0};  // written only by the thread that runs VU1
std::uint64_t g_path1_delivered = 0;

void keep_path1(std::uint32_t reg, std::uint64_t value) {
    std::lock_guard<std::mutex> lock(g_path1_lock);
    g_path1.push_back({reg, g_path1_fences.load(std::memory_order_relaxed), value});
    g_path1_count.fetch_add(1, std::memory_order_release);
}

// `path1`: the packet is an XGKICK one, read by the thread that runs VU1. Its SIGNAL, FINISH and LABEL are kept for deliver_path1, and
// the transfer registers are noted only on the emulation thread (they are its shadow; with the vector thread on it does not see them,
// as before).
void scan(Path &p, const std::uint8_t *data, std::uint32_t qwc, bool path1 = false) {
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
                    if (at[8] >= 0x60) {
                        if (path1) keep_path1(at[8], value);
                        else gs::signal(at[8], value);
                    } else if (at[8] <= 0x53 && (!path1 || gs::queue::role() != gs::queue::Role::Vu)) {
                        gs::note_transfer(at[8], value);
                    }
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

// The emulation thread reads PATH2 and PATH3 packets for SIGNAL, FINISH and LABEL before it sends them on (the vector thread only passes
// them on). PATH1 packets come only from the thread that runs VU1, which reads them itself (g_scan[1] is that thread's).
void scan_here(int path, const std::uint8_t *data, std::uint32_t qwc) {
    if (path == 1) scan(g_scan[1], data, qwc, true);
    else if (gs::queue::role() != gs::queue::Role::Vu) scan(g_scan[path], data, qwc);
}

void out(int path, const std::uint8_t *data, std::uint32_t qwc) {
    if (!qwc) return;
    if (!gs::queue::active()) {
        if (path == 1) {  // one thread: PATH1's SIGNAL, FINISH and LABEL wait for deliver_path1 here too, so the run matches one with threads
            scan(g_scan[1], data, qwc, true);
            gs::path1_drawing(true);
            render(path, data, qwc);
            gs::path1_drawing(false);
            return;
        }
        render(path, data, qwc);
        return;
    }
    if (path & kTripFlag) {  // a ticket the vector thread passes on
        gs::queue::post(gs::queue::kGif, static_cast<std::uint32_t>(path), qwc, data, sizeof(gs::TripTicket));
        return;
    }
    if (gs::TripTicket ticket; gs::trip_match(data, qwc, ticket)) {
        // a depth download's bytes going back untouched: the ticket goes instead (the scan reads image data only if it is not)
        scan_here(path, data, qwc);
        gs::queue::post(gs::queue::kGif, static_cast<std::uint32_t>(path | kTripFlag), qwc, &ticket, sizeof ticket);
        return;
    }
    scan_here(path, data, qwc);
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

// The emulation thread's: this run has handed PATH1 writes on since it started or since its last state save or load, so it hands them on
// at every delivery point from now on, not only at a vblank. Read inline by deliver_path1 and path1_live (gif.hpp).
bool g_path1_live = false;

void drop_held() {
    const bool masked = g_held.masked;
    g_held = Held{};
    g_held.masked = masked;
}

void reset() {
    g_stat = 0;
    if (hold_here()) {
        drop_held();
        g_scan[1] = Path{};  // PATH1's reader belongs to the thread that runs VU1; the vector thread resets its own (reset_path1_scan)
    }
    if (gs::queue::active()) {
        g_scan[0] = g_scan[2] = g_scan[3] = Path{};
        gs::queue::post(gs::queue::kGifReset, 0, 0, nullptr, 0);
        return;
    }
    render_reset();
}

void render_reset() {
    for (Path &p : g_paths) p = Path{};
}

void render(int path, const std::uint8_t *data, std::uint32_t qwc) {
    gs::trace::g_path = path & 3;  // one store a message; the draw trace prints it with each shape
    if (path & kTripFlag) [[unlikely]] {
        path &= 3;
        gs::TripTicket ticket;
        std::memcpy(&ticket, data, sizeof ticket);
        Path &p = g_paths[path];
        if (p.active && p.flg >= 2 && p.nloop - p.loop >= qwc && gs::trip_restore(ticket, qwc)) {  // as walk() takes image data
            p.loop += qwc;
            if (p.loop >= p.nloop) p.active = false;
            return;
        }
        const std::vector<std::uint8_t> &bytes = gs::trip_bytes(ticket);
        const std::vector<std::uint8_t> none(bytes.size() >= static_cast<std::size_t>(qwc) * 16 ? 0 : static_cast<std::size_t>(qwc) * 16);
        GsSink sink{path};
        walk(p, none.empty() ? bytes.data() : none.data(), qwc, sink);
        return;
    }
    if (path & kEndPacket) [[unlikely]] {
        path &= 3;
        note(g_runaway[path], path, 0, 0, qwc);  // qwc carries the kick address here
        g_paths[path].active = false;
        return;
    }
    GsSink sink{path};
    walk(g_paths[path], data, qwc, sink);
}

// VU1's GIF reads a kicked packet round the data memory until a tag with EOP has had all its data, and the next XGKICK waits for
// that and then starts with a tag of its own (EE manual, PATH1). vu::xgkick sends at most one pass over the memory and stops before a
// tag whose data is more than the memory holds; when the packet has not ended by then (only a tag that is not one, such as vertex
// data, does that) this ends it, in order with the data, so the next kick is read as a new packet instead of as more of this one. Without it one bad tag with a large NLOOP swallowed the
// packets of many kicks after it and sprayed them over random GS registers.
void end_runaway_kick(std::uint32_t address) {
    if (!gs::queue::active()) {
        render(1 | kEndPacket, nullptr, address);
        return;
    }
    g_scan[1].active = false;  // called on the thread that runs VU1, whose reader this is
    gs::queue::post(gs::queue::kGif, static_cast<std::uint32_t>(1 | kEndPacket), address, nullptr, 0);
}

void transfer(int path, const std::uint8_t *data, std::uint32_t qwc) {
    if (path & kEndPacket) [[unlikely]] {  // the message above, should it come through the vector thread's queue
        end_runaway_kick(qwc);
        return;
    }
    Held &h = g_held;
    if (path & kTripFlag) [[unlikely]] {  // a depth trip's ticket (out() above, or VIF1 DIRECT data: transfer_ticket), here on the vector thread
        if (h.flowing && !h.masked) {  // as below: once the mask is down, whatever is still held goes ahead of anything new
            h.flowing = false;
            release(false);
        }
        if ((path & 3) == 3) {
            if (!h.data.empty() || (h.masked && !h.mid_packet) || h.left < qwc) {  // it would be held or cut: as bytes, then
                gs::TripTicket ticket;
                std::memcpy(&ticket, data, sizeof ticket);
                std::vector<std::uint8_t> bytes = gs::trip_fetch(ticket);
                bytes.resize(static_cast<std::size_t>(qwc) * 16);
                transfer(3, bytes.data(), qwc);
                return;
            }
            h.left -= qwc;  // what packet_length does with data inside a tag
            if (!h.left && h.eop) h.mid_packet = false;
        }
        out(path, data, qwc);
        return;
    }
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

void transfer_ticket(int path, const gs::TripTicket &ticket, std::uint32_t qwc) {
    transfer(path | kTripFlag, reinterpret_cast<const std::uint8_t *>(&ticket), qwc);
}

void scan_signals(int path, const std::uint8_t *data, std::uint32_t qwc) { scan(g_scan[path], data, qwc); }

namespace {
std::uint64_t g_path1_waits = 0;  // deliveries that had to let the vector thread finish first (stats)
bool trace_path1() {
    static const bool on = std::getenv("PS2_TRACE_PATH1") != nullptr;  // debugging: a line for every PATH1 SIGNAL, FINISH and LABEL handed on
    return on;
}

// Hands on, in order, everything kept (all) or what was found before the thread that runs VU1 passed fence `before`.
bool hand_on(bool all, std::uint32_t before) {
    if (!g_path1_count.load(std::memory_order_acquire)) return false;
    std::vector<Found> found;
    {
        std::lock_guard<std::mutex> lock(g_path1_lock);
        std::size_t n = 0;
        while (n < g_path1.size() && (all || static_cast<std::int32_t>(g_path1[n].fence - before) < 0)) ++n;
        found.assign(g_path1.begin(), g_path1.begin() + static_cast<std::ptrdiff_t>(n));
        g_path1.erase(g_path1.begin(), g_path1.begin() + static_cast<std::ptrdiff_t>(n));
        g_path1_count.store(static_cast<std::uint32_t>(g_path1.size()), std::memory_order_relaxed);
    }
    for (const Found &f : found) {
        if (trace_path1())
            std::fprintf(stderr, "[gif] PATH1 %s 0x%016llX handed to the GS at cycle %llu (frame %llu)\n", f.reg == 0x60 ? "SIGNAL" : f.reg == 0x61 ? "FINISH" : "LABEL",
                         static_cast<unsigned long long>(f.value), static_cast<unsigned long long>(now()), static_cast<unsigned long long>(frame_count()));
        gs::signal(f.reg, f.value);
    }
    g_path1_delivered += found.size();
    if (!found.empty()) g_path1_live = true;
    return !found.empty();
}

// Everything the emulation thread sent before this point has been read for PATH1 writes: with the vector thread on, an empty queue
// means it has (a message is taken off after its work); otherwise it is let finish first.
void read_all_sent() {
    if (gs::queue::vu_active() && gs::queue::role() == gs::queue::Role::Emulation && !gs::queue::vu().empty()) {
        ++g_path1_waits;
        drain_vu_thread();
    }
}

}

// Only called once the run is live (deliver_path1 in gif.hpp checks that inline): a run that has not used them yet gets them at the
// vblank (deliver_path1_before) and pays nothing at the delivery points.
bool deliver_path1_live() {
    read_all_sent();
    return hand_on(true, 0);
}

void deliver_path1_before(std::uint32_t fence) { hand_on(false, fence); }

void path1_fence() { g_path1_fences.store(g_path1_fences.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed); }

void save_path1() {
    read_all_sent();
    hand_on(true, 0);
    g_path1_live = false;
}

void forget_path1() {
    std::lock_guard<std::mutex> lock(g_path1_lock);
    g_path1.clear();
    g_path1_count.store(0, std::memory_order_relaxed);
    g_path1_live = false;
}

void reset_path1_scan() { g_scan[1] = Path{}; }

bool busy(int path) { return g_paths[path].active; }

void stats() {
    for (int p = 1; p < 4; ++p)
        std::fprintf(stderr, "[gif] path %d tags: packed %llu reglist %llu image %llu\n", p,
                     static_cast<unsigned long long>(g_tags[p][0]), static_cast<unsigned long long>(g_tags[p][1]),
                     static_cast<unsigned long long>(g_tags[p][2] + g_tags[p][3]));
    for (int p = 1; p < 4; ++p) {
        if (const Odd &o = g_missing[p]; o.count)
            std::fprintf(stderr, "[gif] path %d: %llu writes to GS registers that do not exist (the first about frame %u): the GIF read something that is not a packet\n",
                         p, static_cast<unsigned long long>(o.count), o.frame);
        if (const Odd &o = g_runaway[p]; o.count)
            std::fprintf(stderr, "[gif] path %d: %llu XGKICK packets did not end within VU1's data memory (the first about frame %u); each was cut there\n",
                         p, static_cast<unsigned long long>(o.count), o.frame);
    }
    vu::runaway_summary();
    if (g_path1_delivered || trace_path1())
        std::fprintf(stderr, "[gif] path 1: %llu SIGNAL, FINISH and LABEL writes handed to the GS; the emulation thread waited %llu times for the vector thread to read its packets first\n",
                     static_cast<unsigned long long>(g_path1_delivered), static_cast<unsigned long long>(g_path1_waits));
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
    vif::flush(1);
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
