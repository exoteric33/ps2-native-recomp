// The depth round trip (runtime/src/gs/gs.cpp), no game needed: tests/depth_trip.py builds and runs it.
//
// A made-up GS stream like Digital Devil Saga's (issue #122): draw with depth, store the depth buffer into main memory, draw over the
// depth, load it back, draw against it. Each case prints one line with checksums of what the game could see (the downloaded bytes when
// it reads them) and of graphics memory at the end (picture and depth, read back the exact way). The script runs this program with
// PS2_DEPTH_TRIP=0 (every depth download waits, the exact path) and =1 and wants the same lines; with =1 the trip log must show the
// round trips put back from the copy where the case allows it. The cases:
//   trip      store, draw over, load the same bytes back, draw: the upload is put back from the copy
//   read      as trip, but the EE reads the bytes before the load (a game that looks at its depth): it must see the real depth
//   write     as trip, but the EE changes one word before the load: the load must upload the changed bytes
//   other     the bytes go back to another place (a different rectangle): uploaded from the copy's bytes
//   neighbour the EE touches a byte next to the buffer in the same page: the bytes are made right, then the load is a plain one
//   twice     two store/load rounds into the same buffer, then the EE reads it (the second download replaces the first)
//   z16       the same as trip with PSMZ16
//   timing    the trip case 200 times; prints the time per round, compared by the script, not part of the equal lines
//
// `depth_trip vif` (issue #122, Digital Devil Saga loads its depth back through VIF1): the round trip with the load sent back four ways,
// while VU1 programs kick thousands of small GIF packets (XGKICK) ahead of it: PATH3 as above (trip), a VIF1 chain with DIRECT in the
// REF tag's upper words (vif), a VIF1 chain whose DIRECT starts in a CNT packet and ends in the REF (vif-split), and a GIF channel chain
// (gif-chain). It runs them with one thread, with a drawing thread, and with the vector thread too, in that order, and wants the same
// line everywhere (one thread never takes the trip: it is the exact answer). With PS2_DEPTH_TRIP=1 every threaded load must be put back
// from the copy, with no guard stop. Before issue #122's fixes it hung with the vector thread (the emulation thread waited for the
// drawing thread inside the trip's lock, the vector thread blocked on that lock at its next XGKICK) and the VIF1 loads were not put back
// with the drawing thread alone. Prints "depth trip through VIF1: 0 failed" when all is well. Built by CMake: ninja ps2depthtriptest.
#include "gs/gif.hpp"
#include "gs/gs.hpp"
#include "gs/queue.hpp"
#include "hw.hpp"
#include "ps2/memory.hpp"
#include "ps2/system.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace ps2;

namespace {

constexpr std::uint32_t kWidth = 256, kHeight = 64;  // 4 pages wide, 2 page rows of PSMZ32
constexpr std::uint32_t kFbp = 0, kZbp = 32;         // in pages (8 KiB): picture at 0, depth after it
constexpr std::uint32_t kPackets = 0x00100000;       // main memory for the GIF packets
constexpr std::uint32_t kBuffer = 0x00200000;        // the depth buffer's copy in main memory (page-aligned)

std::uint32_t g_at = kPackets;

std::uint8_t *ram(std::uint32_t a) { return g_ram + a; }

struct Packet {
    std::vector<std::uint64_t> q;
    void ad(std::uint32_t reg, std::uint64_t value) { q.push_back(value), q.push_back(reg); }
};

// One A+D packet through PATH3, as a DMA would hand it over from main memory.
void send(const Packet &p) {
    const std::uint32_t n = static_cast<std::uint32_t>(p.q.size() / 2);
    std::uint64_t tag[2] = {n | (1ull << 15) | (1ull << 60), 0xE};
    std::uint8_t *at = ram(g_at);
    std::memcpy(at, tag, 16);
    std::memcpy(at + 16, p.q.data(), p.q.size() * 8);
    gif::transfer(3, at, n + 1);
    g_at += (n + 1) * 16;
    if (g_at > kPackets + 0x80000) g_at = kPackets;
}

std::uint64_t bitbltbuf(std::uint32_t sbp, std::uint32_t sbw, std::uint32_t spsm, std::uint32_t dbp, std::uint32_t dbw, std::uint32_t dpsm) {
    return sbp | static_cast<std::uint64_t>(sbw) << 16 | static_cast<std::uint64_t>(spsm) << 24 | static_cast<std::uint64_t>(dbp) << 32 |
           static_cast<std::uint64_t>(dbw) << 48 | static_cast<std::uint64_t>(dpsm) << 56;
}

void setup(std::uint32_t zpsm) {
    Packet p;
    p.ad(0x4C, kFbp | (kWidth / 64) << 16);                             // FRAME_1: PSMCT32
    p.ad(0x4E, (kZbp) | static_cast<std::uint64_t>(zpsm & 15u) << 24);  // ZBUF_1
    p.ad(0x40, static_cast<std::uint64_t>(kWidth - 1) << 16 | static_cast<std::uint64_t>(kHeight - 1) << 48);  // SCISSOR_1
    p.ad(0x18, 0);  // XYOFFSET_1
    p.ad(0x1A, 1);  // PRMODECONT
    send(p);
}

// A flat triangle pair over x0..x1, y0..y1 with depth z0 at the left edge and z1 at the right (so depth differs per pixel), colour c,
// depth test ztst (1 always, 2 greater-equal, 3 greater), depth write on.
void quad(int x0, int y0, int x1, int y1, std::uint32_t z0, std::uint32_t z1, std::uint32_t c, std::uint32_t ztst) {
    Packet p;
    p.ad(0x47, 1ull << 16 | static_cast<std::uint64_t>(ztst) << 17);  // TEST_1: ZTE, ZTST
    p.ad(0x00, 3);                                                     // PRIM: triangle list
    p.ad(0x01, c);
    auto xyz = [&](int x, int y, std::uint32_t z) { p.ad(0x05, static_cast<std::uint64_t>(x * 16) | static_cast<std::uint64_t>(y * 16) << 16 | static_cast<std::uint64_t>(z) << 32); };
    xyz(x0, y0, z0), xyz(x1, y0, z1), xyz(x0, y1, z0);
    xyz(x1, y0, z1), xyz(x1, y1, z1), xyz(x0, y1, z0);
    send(p);
}

void transfer(std::uint64_t buf, std::uint32_t x, std::uint32_t y, std::uint32_t w, std::uint32_t h, std::uint32_t dir) {
    Packet p;
    p.ad(0x50, buf);
    p.ad(0x51, dir == 1 ? (x | y << 16) : static_cast<std::uint64_t>(x) << 32 | static_cast<std::uint64_t>(y) << 48);
    p.ad(0x52, w | static_cast<std::uint64_t>(h) << 32);
    p.ad(0x53, dir);
    send(p);
}

std::uint32_t bytes_of(std::uint32_t psm, std::uint32_t w, std::uint32_t h) { return w * h * (psm == gs::PSMZ32 ? 4u : 2u); }

// StoreImage of the depth rectangle into main memory at `to` (the DMA's read_image).
void store_depth(std::uint32_t zpsm, std::uint32_t to) {
    transfer(bitbltbuf(kZbp * 32, kWidth / 64, zpsm, 0, 0, 0), 0, 0, kWidth, kHeight, 1);
    gs::read_image(ram(to), bytes_of(zpsm, kWidth, kHeight) / 16);
}

// LoadImage of main memory at `from` into (x, y) of a buffer: the IMAGE tag, then the data by reference (DMA REF), as sceGsExecLoadImage.
void load(std::uint32_t from, std::uint32_t dbp, std::uint32_t dpsm, std::uint32_t x, std::uint32_t y, std::uint32_t w, std::uint32_t h) {
    transfer(bitbltbuf(0, 0, 0, dbp, kWidth / 64, dpsm), x, y, w, h, 0);
    const std::uint32_t qwc = bytes_of(dpsm == gs::PSMCT32 ? gs::PSMZ32 : dpsm, w, h) / 16;
    std::uint64_t tag[2] = {qwc | (1ull << 15) | (2ull << 58), 0};
    std::memcpy(ram(g_at), tag, 16);
    gif::transfer(3, ram(g_at), 1);
    g_at += 16;
    gif::transfer(3, ram(from), qwc);
}

std::uint64_t fnv(const std::uint8_t *p, std::size_t n) {
    std::uint64_t h = 1469598103934665603ull;
    for (std::size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

// The picture and the depth as they are now, read the exact way (rectangles that are not whole pages never take the trip).
std::uint64_t memory_sum(std::uint32_t zpsm) {
    std::vector<std::uint8_t> out(kWidth * kHeight * 4 + 16);
    std::uint64_t h = 0;
    for (int part = 0; part < 2; ++part) {
        const std::uint32_t psm = part ? zpsm : gs::PSMCT32, bp = part ? kZbp * 32 : kFbp * 32;
        transfer(bitbltbuf(bp, kWidth / 64, psm, 0, 0, 0), 0, 0, kWidth, kHeight - 1, 1);
        const std::uint32_t qwc = (bytes_of(psm == gs::PSMCT32 ? gs::PSMZ32 : psm, kWidth, kHeight - 1) + 15) / 16;
        std::uint8_t *at = ram(0x00300000);
        gs::read_image(at, qwc);
        h = h * 31 + fnv(at, static_cast<std::size_t>(qwc) * 16);
    }
    return h;
}

void scene(std::uint32_t zpsm) {
    setup(zpsm);
    quad(0, 0, kWidth, kHeight, 0, 0, 0xFF000000u, 1);                        // clear: depth 0, black
    quad(8, 4, 200, 60, 0x1000, zpsm == gs::PSMZ32 ? 0x00F00000 : 0xF000, 0xFF0000FFu, 1);  // the scene: a depth slope
}

void over_and_back(std::uint32_t zpsm) {
    quad(40, 10, 180, 50, 0x0800, 0x0800, 0xFF00FF00u, 1);  // something else that writes depth (the background objects)
}

void after(std::uint32_t zpsm) {
    // drawn against the depth that came back (the light glow quads): where the slope is in front, it does not show
    quad(0, 0, kWidth, kHeight, zpsm == gs::PSMZ32 ? 0x00080000 : 0x8000, zpsm == gs::PSMZ32 ? 0x00080000 : 0x8000, 0xFFFF0000u, 2);
}

std::string run_case(const std::string &name) {
    const std::uint32_t zpsm = name == "z16" ? gs::PSMZ16 : gs::PSMZ32;
    const std::uint32_t size = bytes_of(zpsm, kWidth, kHeight);
    std::memset(ram(kBuffer - 4096), 0x5A, size + 8192);
    scene(zpsm);
    store_depth(zpsm, kBuffer);
    std::uint64_t seen = 0;
    if (name == "read") seen = fnv(ram(kBuffer), size);  // the game looks at its depth
    if (name == "write") ram(kBuffer)[(55 * kWidth + 190) * 4 + 2] ^= 0x40;  // the game changes it (a pixel the last draw keeps)
    if (name == "neighbour") seen = ram(kBuffer - 1)[0];  // the game reads its own data next to the buffer
    over_and_back(zpsm);
    if (name == "other") load(kBuffer, 0, gs::PSMCT32, 0, 0, kWidth, kHeight);  // into the picture instead, as a 32-bit image
    else load(kBuffer, kZbp * 32, zpsm, 0, 0, kWidth, kHeight);
    if (name == "twice") {
        quad(100, 20, 250, 40, 0x0400, 0x0400, 0xFF00FFFFu, 1);
        store_depth(zpsm, kBuffer);
        over_and_back(zpsm);
        load(kBuffer, kZbp * 32, zpsm, 0, 0, kWidth, kHeight);
        seen = fnv(ram(kBuffer), size);
    }
    after(zpsm);
    char line[200];
    std::snprintf(line, sizeof line, "%-9s seen %016llx memory %016llx buffer-after %016llx", name.c_str(), static_cast<unsigned long long>(seen),
                  static_cast<unsigned long long>(memory_sum(zpsm)), static_cast<unsigned long long>(fnv(ram(kBuffer), size)));
    return line;
}

// ---- `depth_trip vif` ----

constexpr std::uint32_t kD1Chcr = 0x10009000u, kD1Madr = 0x10009010u, kD1Qwc = 0x10009020u, kD1Tadr = 0x10009030u;  // DMA channel 1 (VIF1)
constexpr std::uint32_t kD2Chcr = 0x1000A000u, kD2Qwc = 0x1000A020u, kD2Tadr = 0x1000A030u;                                           // DMA channel 2 (GIF)
constexpr std::uint32_t kLists = 0x00400000;  // main memory for the DMA lists

void put_words(std::uint32_t at, const std::vector<std::uint32_t> &w) { std::memcpy(ram(at), w.data(), w.size() * 4); }

// VU1 programs that kick small packets: the program (XGKICK vi0, then the end), a packet of one A+D write to TEXFLUSH unpacked to VU1
// address 0, then MSCAL 0 `kicks` times, as one normal-mode VIF1 DMA. With the vector thread on, it is still kicking them (gif::out,
// which asks trip_match each time) when the load further down is sent.
void busy(std::uint32_t kicks) {
    std::vector<std::uint32_t> w = {0, 0, 0, 0x4A000000u | (3u << 16),  // MPG: 3 instruction pairs at 0
                                    0x800006FCu, 0x000002FFu,           // XGKICK vi0             | NOP
                                    0x8000033Cu, 0x400002FFu,           // NOP                    | NOP with the E bit
                                    0x8000033Cu, 0x000002FFu,           // NOP (the pair after E) | NOP
                                    0, 0x6C000000u | (2u << 16)};       // UNPACK V4-32 of 2 qwords to VU1 address 0
    const std::uint64_t tag = 1 | (1ull << 15) | (1ull << 60);           // NLOOP 1, EOP, PACKED, one register: A+D
    w.insert(w.end(), {static_cast<std::uint32_t>(tag), static_cast<std::uint32_t>(tag >> 32), 0xEu, 0, 0, 0, 0x3Fu, 0});  // TEXFLUSH
    for (std::uint32_t i = 0; i < kicks; ++i) w.push_back(0x14000000u);  // MSCAL 0
    while (w.size() % 4) w.push_back(0);
    put_words(kLists, w);
    hw::write32(kD1Madr, kLists);
    hw::write32(kD1Qwc, static_cast<std::uint32_t>(w.size() / 4));
    hw::write32(kD1Chcr, 0x101u);  // STR, from memory, normal mode
}

std::uint64_t dma_tag(std::uint32_t id, std::uint32_t qwc, std::uint32_t addr = 0) { return qwc | static_cast<std::uint64_t>(id) << 28 | static_cast<std::uint64_t>(addr) << 32; }

// The GIF packet ahead of the image data: A+D BITBLTBUF, TRXPOS, TRXREG, TRXDIR, then the IMAGE tag (6 qwords).
std::vector<std::uint64_t> load_head(std::uint32_t qwc) {
    const std::uint64_t buf = bitbltbuf(0, 0, 0, kZbp * 32, kWidth / 64, gs::PSMZ32);
    return {4 | (1ull << 15) | (1ull << 60), 0xE, buf, 0x50, 0, 0x51, kWidth | static_cast<std::uint64_t>(kHeight) << 32, 0x52, 0, 0x53,
            qwc | (1ull << 15) | (2ull << 58), 0};
}

// The load of the depth buffer at kBuffer as a VIF1 chain (sceGsExecLoadImage done through VIF1, as Digital Devil Saga's fldDrwZLoad):
// split = false: CNT with DIRECT 6 in its upper words and the packet, REF with DIRECT qwc in its upper words to the buffer, END
//                (tag transfer on);
// split = true:  CNT with NOP NOP NOP DIRECT 6+qwc and the packet as its data, REF to the buffer (the DIRECT's last qwc quadwords), END.
void vif_load(bool split) {
    const std::uint32_t qwc = bytes_of(gs::PSMZ32, kWidth, kHeight) / 16;
    std::vector<std::uint64_t> q;
    const std::vector<std::uint64_t> head = load_head(qwc);
    if (!split) {
        q.push_back(dma_tag(1, 6)), q.push_back(static_cast<std::uint64_t>(0x50000000u | 6u) << 32);  // CNT, upper words NOP, DIRECT 6
        q.insert(q.end(), head.begin(), head.end());
        q.push_back(dma_tag(3, qwc, kBuffer)), q.push_back(static_cast<std::uint64_t>(0x50000000u | qwc) << 32);  // REF, NOP, DIRECT qwc
        q.push_back(dma_tag(7, 0)), q.push_back(0);  // END
    } else {
        q.push_back(dma_tag(1, 7)), q.push_back(0);  // CNT
        q.push_back(0), q.push_back(static_cast<std::uint64_t>(0x50000000u | (6u + qwc)) << 32);  // NOP NOP NOP DIRECT 6+qwc
        q.insert(q.end(), head.begin(), head.end());
        q.push_back(dma_tag(3, qwc, kBuffer)), q.push_back(0);  // REF
        q.push_back(dma_tag(7, 0)), q.push_back(0);  // END
    }
    std::memcpy(ram(kLists), q.data(), q.size() * 8);
    hw::write32(kD1Qwc, 0);
    hw::write32(kD1Tadr, kLists);
    hw::write32(kD1Chcr, 0x100u | (split ? 0u : 0x40u) | 0x4u | 0x1u);  // STR, tag transfer, chain mode, from memory
}

// The same load as a GIF channel chain: CNT with the packet, REF to the buffer, END.
void gif_chain_load() {
    const std::uint32_t qwc = bytes_of(gs::PSMZ32, kWidth, kHeight) / 16;
    std::vector<std::uint64_t> q = {dma_tag(1, 6), 0};
    const std::vector<std::uint64_t> head = load_head(qwc);
    q.insert(q.end(), head.begin(), head.end());
    q.push_back(dma_tag(3, qwc, kBuffer)), q.push_back(0);
    q.push_back(dma_tag(7, 0)), q.push_back(0);
    std::memcpy(ram(kLists), q.data(), q.size() * 8);
    hw::write32(kD2Qwc, 0);
    hw::write32(kD2Tadr, kLists);
    hw::write32(kD2Chcr, 0x100u | 0x4u | 0x1u);  // STR, chain mode, from memory
}

struct VifResult {
    std::string line;
    gs::TripCounts before, after;
};

VifResult run_vif_case(const std::string &name) {
    const std::uint32_t size = bytes_of(gs::PSMZ32, kWidth, kHeight);
    std::memset(ram(kBuffer - 4096), 0x5A, size + 8192);
    scene(gs::PSMZ32);
    busy(16000);
    VifResult r;
    r.before = gs::trip_counts();
    store_depth(gs::PSMZ32, kBuffer);
    over_and_back(gs::PSMZ32);
    if (name == "trip") load(kBuffer, kZbp * 32, gs::PSMZ32, 0, 0, kWidth, kHeight);
    else if (name == "vif") vif_load(false);
    else if (name == "vif-split") vif_load(true);
    else gif_chain_load();
    after(gs::PSMZ32);
    const std::uint64_t memory = memory_sum(gs::PSMZ32);  // waits for the drawing thread
    r.after = gs::trip_counts();
    char line[200];
    std::snprintf(line, sizeof line, "memory %016llx buffer-after %016llx", static_cast<unsigned long long>(memory), static_cast<unsigned long long>(fnv(ram(kBuffer), size)));
    r.line = line;
    return r;
}

std::atomic<int> g_mode_serial{0};

// One thread, a drawing thread, the vector thread too: each case's line must be the one-thread "trip" line.
int vif_main() {
    gs::init();
    ps2::boot(0x00100000u, {});
    hw::write32(0x1000E000u, 1);  // D_CTRL.DMAE
    const char *env = std::getenv("PS2_DEPTH_TRIP");
    const bool trip_on = env && std::atoi(env) != 0;
    std::thread([] {  // a hang is a failure, not a test that never ends
        for (int seen = -1;;) {
            const int now = g_mode_serial.load();
            if (now == seen) {
                std::printf("HANG: a mode took more than 120 s (the depth trip deadlock, issue #122)\n");
                std::fflush(stdout);
                std::_Exit(3);
            }
            seen = now;
            std::this_thread::sleep_for(std::chrono::seconds(120));
        }
    }).detach();
    int failed = 0;
    std::string reference;
    std::atomic<bool> stop{false};
    std::thread render;
    for (int mode = 0; mode < 3; ++mode) {
        const char *mode_name = mode == 0 ? "one thread" : mode == 1 ? "drawing thread" : "vector thread";
        if (mode == 1) {
            gs::start_thread();
            render = std::thread([&] {
                while (!stop.load())
                    if (gs::render_pending([](const gs::DisplayRegs &) {}, 8.0) == Rendered::idle) gs::queue::render().wait_for_work(4);
            });
        } else if (mode == 2) {
            ps2::start_vu_thread();
            std::thread(ps2::run_vu_thread).detach();
        }
        for (const char *name : {"trip", "vif", "vif-split", "gif-chain"}) {
            g_mode_serial.fetch_add(1);
            const VifResult r = run_vif_case(name);
            const std::uint32_t restored = r.after.restored - r.before.restored, touches = r.after.touches - r.before.touches;
            if (reference.empty()) reference = r.line;
            const bool same = r.line == reference;
            const bool taken = !trip_on || mode == 0 || (restored == 1 && touches == 0);
            failed += !same + !taken;
            std::printf("%-14s %-9s %s  put back from the copy %u, guard stops %u%s%s\n", mode_name, name, r.line.c_str(), restored, touches, same ? "" : "  DIFF",
                        taken ? "" : "  NOT PUT BACK");
            std::fflush(stdout);
        }
    }
    std::printf("depth trip through VIF1 (PS2_DEPTH_TRIP=%d): %d failed\n", trip_on ? 1 : 0, failed);
    std::fflush(stdout);
    std::_Exit(failed ? 1 : 0);  // the vector thread runs on
}

}  // namespace

int main(int argc, char **argv) {
    if (argc > 1 && std::string(argv[1]) == "vif") return vif_main();
    gs::init();
    gs::start_thread();
    std::atomic<bool> stop{false};
    std::thread render([&] {
        while (!stop.load()) {
            if (gs::render_pending([](const gs::DisplayRegs &) {}, 8.0) == Rendered::idle) gs::queue::render().wait_for_work(4);
        }
    });
    if (argc > 1 && std::string(argv[1]) == "timing") {
        const int rounds = argc > 2 ? std::atoi(argv[2]) : 200;
        setup(gs::PSMZ32);
        memory_sum(gs::PSMZ32);
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < rounds; ++i) {
            scene(gs::PSMZ32);
            store_depth(gs::PSMZ32, kBuffer);
            over_and_back(gs::PSMZ32);
            load(kBuffer, kZbp * 32, gs::PSMZ32, 0, 0, kWidth, kHeight);
            after(gs::PSMZ32);
        }
        const double emu = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        memory_sum(gs::PSMZ32);  // waits for the drawing thread
        const double all = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        std::printf("timing: %d rounds, emulation thread %.3f ms a round, until drawn %.3f ms a round\n", rounds, emu / rounds, all / rounds);
    } else {
        for (const char *name : {"trip", "read", "write", "other", "neighbour", "twice", "z16"}) std::printf("%s\n", run_case(name).c_str());
    }
    std::fflush(stdout);
    stop = true;
    gs::queue::render().wake();
    render.join();
    return 0;
}
