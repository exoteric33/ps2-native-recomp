// No game data: how the draw program (gs.wgsl) covers the samples of a scaled picture (PS2_SCALE > 1) for shapes that tile a
// rectangle without gaps or overlaps (a mesh of triangle pairs, a row of sprites), against 1x. Every shape adds the same colour to
// what is there (additive blend), so a sample's red says how many shapes covered it: inside a mesh that tiles the area it must be
// exactly one at every scale. A sample covered twice is a bright line, one covered never a dark one; both are seams the console
// does not draw. tests/scale_cover.py builds it against the newest native runtime and runs it with PS2_SCALE=1 and 4.
//
// Prints one line per case: the samples inside the mesh (one pixel kept off its outer edge) covered 0, 1 and 2+ times.
#include "gs/gs.hpp"
#include "gs/gpu/gpu.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace ps2::gs;

namespace {

constexpr std::uint32_t kW = 256, kH = 128;  // the frame: PSMCT32 at block 0, width 4
constexpr std::uint32_t kTexBp = 0x1000;      // a 64x64 PSMCT32 texture, every texel red 0x10

struct Mesh {
    const char *name;
    bool jitter;    // vertices moved off the grid by a few sixteenths (slanted edges)
    bool depth;     // depth differs per vertex (not one depth: the plain sample path)
    bool textured;  // TME (the straight-edge rule applies only to textured shapes)
    bool flip;      // every other cell splits along the other diagonal
    bool sprite;    // one sprite per cell instead of two triangles
    double ox, oy, cw, ch;  // first corner and cell size in pixels
};

std::uint32_t jit(int i, int j, int salt) {
    std::uint32_t h = static_cast<std::uint32_t>(i * 73856093) ^ static_cast<std::uint32_t>(j * 19349663) ^ static_cast<std::uint32_t>(salt * 83492791);
    h ^= h >> 13, h *= 0x5bd1e995u, h ^= h >> 15;
    return h;
}

State make_state(const Mesh &m) {
    State s{};
    // triangle list (or sprite), textured, alpha blend, UV (FST), context 1
    s.prim = (m.sprite ? 6u : 3u) | (m.textured ? 0x10u : 0u) | 0x40u | 0x100u;
    s.prmodecont = 1;
    Context &x = s.ctx[0];
    x.frame = 0 | (std::uint64_t{kW / 64} << 16) | (std::uint64_t{PSMCT32} << 24);
    x.zbuf = 0x100ull | (std::uint64_t{1} << 32);  // depth not written
    x.scissor = (kW - 1ull) << 16 | ((kH - 1ull) << 48);
    x.test = (1ull << 16) | (1ull << 17);           // depth test on, always passes
    x.alpha = 0 | (2ull << 2) | (2ull << 4) | (1ull << 6) | (0x80ull << 32);  // (Cs - 0) * 0x80 / 128 + Cd: adds the colour
    x.tex0 = kTexBp | (std::uint64_t{1} << 14) | (std::uint64_t{PSMCT32} << 20) | (std::uint64_t{6} << 26) | (std::uint64_t{6} << 30) |
             (std::uint64_t{1} << 34) | (std::uint64_t{1} << 35);  // 64x64, RGBA, decal
    return s;
}

Vertex vtx(double x, double y, std::uint32_t z, bool textured) {
    Vertex v{};
    v.x = static_cast<std::uint32_t>(std::lround(x * 16.0));
    v.y = static_cast<std::uint32_t>(std::lround(y * 16.0));
    v.z = z;
    v.r = textured ? 0x80 : 0x10;
    v.g = v.b = 0;
    v.a = 0x80;
    v.q = 1.0f;
    v.u = static_cast<std::uint32_t>((v.x & 0x3FFu) / 2);  // somewhere inside the texture, all texels are the same
    v.v = static_cast<std::uint32_t>((v.y & 0x3FFu) / 2);
    return v;
}

// The cells' corners: a grid from (ox, oy), moved by up to 3/16 pixel when jitter is on (shared by the cells that meet there).
void corner(const Mesh &m, int i, int j, double &x, double &y) {
    x = m.ox + i * m.cw;
    y = m.oy + j * m.ch;
    // Two corners of a cell side never get the same move across it, so no edge is exactly straight: every triangle takes the
    // plain sample path (a straight edge left by chance next to slanted ones is the straight-edge rule's known limit).
    if (m.jitter) {
        x += ((j * 3 + i * 7 + 100) % 5 - 2) / 16.0;
        y += ((i * 3 + j * 7 + 100) % 5 - 2) / 16.0;
    }
}

void clear() {
    std::vector<std::uint32_t> words(kMemorySize / 4, 0), clut(512, 0);
    for (std::uint32_t y = 0; y < 64; ++y)
        for (std::uint32_t x = 0; x < 64; ++x) {
            write_pixel(PSMCT32, kTexBp, 1, x, y, 0x80000010u);
        }
    std::memcpy(words.data(), memory(), kMemorySize);
    for (std::uint32_t y = 0; y < kH; ++y)
        for (std::uint32_t x = 0; x < kW; ++x) words[address32(0, kW / 64, x, y) / 4] = 0;
    gpu::write_state(words, 1, clut);
}

std::string run(const Mesh &m) {
    clear();
    const State s = make_state(m);
    ++g_setup_generation;
    const int ni = static_cast<int>((kW - 2 * m.ox) / m.cw), nj = static_cast<int>((kH - 2 * m.oy) / m.ch);
    for (int j = 0; j < nj; ++j)
        for (int i = 0; i < ni; ++i) {
            double x[4], y[4];
            corner(m, i, j, x[0], y[0]);
            corner(m, i + 1, j, x[1], y[1]);
            corner(m, i, j + 1, x[2], y[2]);
            corner(m, i + 1, j + 1, x[3], y[3]);
            const std::uint32_t z = m.depth ? 0x1000u : 0x100u;
            auto zf = [&](int c) { return m.depth ? z + static_cast<std::uint32_t>(jit(i + (c & 1), j + (c >> 1), 3) % 4096) : z; };
            if (m.sprite) {
                Vertex v[2] = {vtx(x[0], y[0], z, m.textured), vtx(x[3], y[3], z, m.textured)};
                gpu::draw(s, v, 2);
                continue;
            }
            Vertex c[4];
            for (int k = 0; k < 4; ++k) c[k] = vtx(x[k], y[k], zf(k), m.textured);
            const bool other = m.flip && ((i + j) & 1);
            if (!other) {
                Vertex a[3] = {c[0], c[1], c[2]}, b[3] = {c[1], c[3], c[2]};
                gpu::draw(s, a, 3);
                gpu::draw(s, b, 3);
            } else {
                Vertex a[3] = {c[0], c[1], c[3]}, b[3] = {c[0], c[3], c[2]};
                gpu::draw(s, a, 3);
                gpu::draw(s, b, 3);
            }
        }
    std::vector<std::uint32_t> vram, clut;
    std::uint32_t scale = 1;
    gpu::read_state(vram, scale, clut);
    const std::uint32_t n = scale * scale;
    // the area the mesh tiles, one pixel in from its outer edge (the outer edges are not shared, they follow the plain rules)
    const int x0 = static_cast<int>(std::ceil(m.ox)) + 1, y0 = static_cast<int>(std::ceil(m.oy)) + 1;
    const int x1 = static_cast<int>(std::floor(m.ox + ni * m.cw)) - 2, y1 = static_cast<int>(std::floor(m.oy + nj * m.ch)) - 2;
    std::uint64_t count[3] = {};
    int shown = 0;
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) {
            const std::uint32_t w = address32(0, kW / 64, static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y)) / 4;
            for (std::uint32_t k = 0; k < n; ++k) {
                const std::uint32_t r = vram[static_cast<std::size_t>(w) * n + k] & 0xFFu;
                const std::uint32_t times = r / 0x10u;
                ++count[times > 2 ? 2 : times];
                if (times != 1 && shown < 4 && std::getenv("SCALE_COVER_SHOW")) {
                    std::fprintf(stderr, "  %s: pixel %d,%d sample %u covered %u times\n", m.name, x, y, k, times);
                    ++shown;
                }
            }
        }
    char line[256];
    std::snprintf(line, sizeof line, "%-44s scale %u: never %llu, once %llu, twice or more %llu", m.name, scale,
                  static_cast<unsigned long long>(count[0]), static_cast<unsigned long long>(count[1]), static_cast<unsigned long long>(count[2]));
    return line;
}

void set_env(const char *name, const char *value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    set_env("PS2_GS", "gpu");
    if (!std::getenv("PS2_SCALE")) set_env("PS2_SCALE", "1");
    init();
    if (gpu::mode() != gpu::Mode::primary) {
        std::printf("scale cover: no graphics adapter, nothing checked\n");
        return 0;
    }
    const Mesh cases[] = {
        //  name                                         jitter depth  tex    flip   sprite  ox      oy      cw       ch
        {"grid, textured, one depth", false, false, true, false, false, 8.25, 8.5, 13.5625, 9.3125},
        {"grid, textured, one depth, both diagonals", false, false, true, true, false, 8.25, 8.5, 13.5625, 9.3125},
        {"grid, textured, one depth, whole pixels", false, false, true, false, false, 8.0, 8.0, 16.0, 8.0},
        {"grid, textured, one depth, half pixels", false, false, true, true, false, 8.5, 8.5, 12.5, 6.5},
        {"grid, untextured, one depth, whole pixels", false, false, false, false, false, 8.0, 8.0, 16.0, 8.0},
        {"grid, untextured, one depth", false, false, false, false, false, 8.25, 8.5, 13.5625, 9.3125},
        {"grid, textured, depth per vertex", false, true, true, false, false, 8.25, 8.5, 13.5625, 9.3125},
        {"jittered mesh, textured, one depth", true, false, true, false, false, 8.25, 8.5, 13.5625, 9.3125},
        {"jittered mesh, textured, one depth, flipped", true, false, true, true, false, 8.25, 8.5, 13.5625, 9.3125},
        {"thin rows, textured, one depth", false, false, true, true, false, 8.25, 8.5, 31.5, 1.4375},
        {"thin columns, textured, one depth", false, false, true, true, false, 8.25, 8.5, 1.4375, 21.5},
        {"sprites", false, false, true, false, true, 8.25, 8.5, 13.5625, 9.3125},
    };
    for (const Mesh &m : cases) std::printf("%s\n", run(m).c_str());
    return 0;
}
