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
#include "gs/gif.hpp"
#include "gs/gs.hpp"
#include "gs/queue.hpp"
#include "ps2/memory.hpp"
#include "ps2/system.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
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

}  // namespace

int main(int argc, char **argv) {
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
