// No game data: how many lines of the picture the display shows in each video mode (the rule of shown_lines in gs.cpp).
// The TV modes of SetGsCrt (NTSC 2, PAL 3, 0 = never called) cap the picture at what the mode has and crop it by DY; progressive scan
// (480p 0x50), 1080i (0x51) and 720p (0x52) show DH+1 lines, whatever DY says.
#include "gs/gs.hpp"
#include "hw.hpp"

#include <cstdint>
#include <cstdio>
#include <vector>

namespace {
int failed = 0;

// DISPLAY register: DX, DY (video clocks and lines), MAGH, MAGV, DW, DH
std::uint64_t display_reg(std::uint32_t dx, std::uint32_t dy, std::uint32_t magh, std::uint32_t magv, std::uint32_t dw, std::uint32_t dh) {
    return dx | (std::uint64_t{dy} << 12) | (std::uint64_t{magh} << 23) | (std::uint64_t{magv} << 27) | (std::uint64_t{dw} << 32) | (std::uint64_t{dh} << 44);
}

// Sets the mode as SetGsCrt does, then asks for the picture of one display rectangle; returns its height
std::uint32_t lines(std::uint32_t mode, std::uint64_t smode2, std::uint32_t dy, std::uint32_t dh, std::uint32_t width) {
    ps2::hw::gs_set_crt(smode2 & 1u, mode, (smode2 >> 1) & 1u);
    ps2::gs::DisplayRegs r{};
    r.pmode = 1;
    r.smode2 = smode2;
    r.dispfb[0] = (std::uint64_t{(width + 63) / 64} << 9); // frame 0, PSMCT32
    r.display[0] = display_reg(232, dy, 0, 0, width - 1, dh);
    std::vector<std::uint32_t> rgba;
    std::uint32_t w = 0, h = 0;
    ps2::gs::display(r, rgba, w, h);
    return h;
}

void check(const char *what, std::uint32_t got, std::uint32_t want) {
    if (got != want) {
        ++failed;
        std::fprintf(stderr, "FAIL: %s: %u lines, want %u\n", what, got, want);
    }
}
}

int main() {
    // progressive scan 640x480 (DY 35, the first line of 480p), 1080i and 720p: every line of the rectangle
    check("480p 0x50", lines(0x50, 0, 35, 479, 640), 480);
    check("480p 0x50 with DY 0", lines(0x50, 0, 0, 479, 640), 480);
    check("1080i 0x51 frame mode", lines(0x51, 3, 40, 1079, 1280), 1080);
    check("720p 0x52", lines(0x52, 0, 24, 719, 1280), 720);
    // the TV modes keep their limits: the same 480 lines in a non-interlaced NTSC picture are cut to what a field has
    check("NTSC mode 2 cut below 480", lines(2, 0, 35, 479, 640) < 480 ? 1 : 0, 1);
    check("never called (0) cut like NTSC", lines(0, 0, 35, 479, 640) == lines(2, 0, 35, 479, 640) ? 1 : 0, 1);
    check("NTSC interlaced 640x448", lines(2, 3, 50, 447, 640), 448);
    check("PAL interlaced 640x512", lines(3, 3, 72, 511, 640), 512);
    std::printf("display lines: %d failed\n", failed);
    return failed ? 1 : 0;
}
