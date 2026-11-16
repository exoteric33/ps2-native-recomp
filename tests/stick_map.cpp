// A controller stick's mapping to the console's pad bytes (host/stick_map.hpp), no game, no window, no SDL:
//   clang++ -std=c++17 -I host tests/stick_map.cpp -o stick_map_test && ./stick_map_test
#include "stick_map.hpp"

#include <cmath>
#include <cstdio>

namespace {

int g_failed = 0;

void expect(const char *what, int vx, int vy, int bx, int by, int tol = 0) {
    std::uint8_t x = 0, y = 0;
    stick_map::to_bytes(vx, vy, x, y);
    const bool ok = std::abs(x - bx) <= tol && std::abs(y - by) <= tol;
    std::printf("%s %-34s in %6d %6d -> %3u %3u (want %3d %3d)\n", ok ? "ok  " : "FAIL", what, vx, vy, x, y, bx, by);
    if (!ok) ++g_failed;
}

}  // namespace

int main() {
    expect("centre", 0, 0, 0x80, 0x80);
    expect("cardinal: right", 32767, 0, 255, 128);
    expect("cardinal: left", -32768, 0, 0, 128);
    expect("cardinal: down", 0, 32767, 128, 255);
    expect("cardinal: up", 0, -32768, 128, 0);
    expect("cardinal half push is not stretched", 16384, 0, 192, 128);
    // a round gate pushed fully on a diagonal reads about 0.707 per axis: the corner of the square
    expect("diagonal full, down-right", 23170, 23170, 255, 255);
    expect("diagonal full, up-left", -23170, -23170, 0, 0);
    expect("diagonal full, up-right", 23170, -23170, 255, 0);
    expect("diagonal full, down-left", -23170, 23170, 0, 255);
    // the direction is kept: 30 degrees, full push
    expect("30 degrees, full push", 28378, 16384, 255, 201, 1);  // y = x * tan 30 deg
    // half push on the diagonal: still half the length, now 0.5 per axis (the console's pad would read about the same)
    expect("diagonal half push", 11585, 11585, 192, 192, 1);
    // a push already in the corner (a square-gate pad) stays there; nothing is pushed past the byte range
    expect("square-gate corner", 32767, 32767, 255, 255);
    expect("square-gate corner, negative", -32768, -32768, 0, 0);
    // the dead zone is the game's own: small pushes stay small, near the middle, and a drift on one axis does not move
    expect("small drift on one axis", 800, 0, 0x80 + 3, 128);
    expect("small diagonal push", 800, 800, 0x80 + 4, 0x80 + 4, 1);
    // never shorter than before: each byte moves away from the middle by at least what the plain mapping gave
    int worse = 0;
    for (int vx = -32768; vx <= 32767; vx += 257)
        for (int vy = -32768; vy <= 32767; vy += 251) {
            std::uint8_t x, y;
            stick_map::to_bytes(vx, vy, x, y);
            const int px = (vx + 32768) >> 8, py = (vy + 32768) >> 8;
            const bool xs = std::abs(static_cast<int>(x) - 128) + 1 >= std::abs(px - 128);
            const bool ys = std::abs(static_cast<int>(y) - 128) + 1 >= std::abs(py - 128);
            const bool same_side = (vx == 0 || (x >= 128) == (vx > 0) || x == 128) && (vy == 0 || (y >= 128) == (vy > 0) || y == 128);
            if (!xs || !ys || !same_side) ++worse;
        }
    std::printf("%s a sweep of the whole range: no byte moves toward the middle or across it (%d odd)\n", worse ? "FAIL" : "ok  ", worse);
    if (worse) ++g_failed;
    std::printf("stick test: %d failed\n", g_failed);
    return g_failed ? 1 : 0;
}
