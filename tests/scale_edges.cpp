// No game data: whether a scaled picture (PS2_SCALE > 1) of a 2D piece reads texels the piece does not show at 1x. Menus, text and
// HUDs are pieces cut from a texture sheet (an atlas): around each piece's texels lie other pieces' texels. A piece laid out for the
// console reads exactly its own texels at 1x; if the finer samples of a scaled picture read past them, the neighbour in the sheet
// shows as a thin line along the piece's edge (the "lines around text" players see at 2x and up).
//
// The sheet here is green except two blocks, red and blue. Every case draws the red block and the blue block as two pieces side
// by side, 1:1, as sprites or as triangle pairs, filtered or not, at whole or fractional positions, each laid out so that 1x reads
// only its own block. Prints one line per case: the samples of the pieces that picked up green (the sheet around them) or the
// other block's colour. Both must be 0 at every scale. tests/scale_edges.py builds it and runs it with PS2_SCALE=1, 2, 3 and 4.
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
constexpr std::uint32_t kTexBp = 0x1000;      // a 64x64 PSMCT32 sheet
constexpr int kBlockW = 12, kBlockH = 8;
constexpr int kRedU = 8, kBlueU = 28, kBlockV = 8;  // the blocks' first texels; green all around and between them

struct Case {
    const char *name;
    bool sprite;     // a sprite per piece, else two triangles
    bool bilinear;   // TEX1 linear filtering
    bool centred;    // texel centres at the pixel corners (u = U + 0.5 at x = X), else texel corners (u = U)
    double ox, oy;   // the first piece's top left corner on screen, in pixels
    bool fan;        // the triangles split along the other diagonal
};

State make_state(const Case &c) {
    State s{};
    s.prim = (c.sprite ? 6u : 3u) | 0x10u | 0x100u;  // textured, UV (FST), no blending
    s.prmodecont = 1;
    Context &x = s.ctx[0];
    x.frame = 0 | (std::uint64_t{kW / 64} << 16) | (std::uint64_t{PSMCT32} << 24);
    x.zbuf = 0x100ull | (std::uint64_t{1} << 32);  // depth not written
    x.scissor = (kW - 1ull) << 16 | ((kH - 1ull) << 48);
    x.test = (1ull << 16) | (1ull << 17);           // depth test on, always passes
    x.tex0 = kTexBp | (std::uint64_t{1} << 14) | (std::uint64_t{PSMCT32} << 20) | (std::uint64_t{6} << 26) | (std::uint64_t{6} << 30) |
             (std::uint64_t{1} << 34) | (std::uint64_t{1} << 35);  // 64x64, RGBA, decal
    x.tex1 = c.bilinear ? (1ull << 5) | (1ull << 6) : 0;           // MMAG, MMIN linear
    x.clamp = 1 | (1 << 2);                                         // clamp both ways (the sheet's edge is far away anyway)
    return s;
}

Vertex vtx(double x, double y, double u, double v) {
    Vertex p{};
    p.x = static_cast<std::uint32_t>(std::lround(x * 16.0));
    p.y = static_cast<std::uint32_t>(std::lround(y * 16.0));
    p.z = 0x100;
    p.r = p.g = p.b = 0x80;
    p.a = 0x80;
    p.q = 1.0f;
    p.u = static_cast<std::uint32_t>(std::lround(u * 16.0));
    p.v = static_cast<std::uint32_t>(std::lround(v * 16.0));
    return p;
}

void clear() {
    std::vector<std::uint32_t> words(kMemorySize / 4, 0), clut(512, 0);
    for (std::uint32_t y = 0; y < 64; ++y)
        for (std::uint32_t x = 0; x < 64; ++x) {
            std::uint32_t c = 0x8000FF00u;  // green: the sheet around the pieces
            const bool in_v = static_cast<int>(y) >= kBlockV && static_cast<int>(y) < kBlockV + kBlockH;
            if (in_v && static_cast<int>(x) >= kRedU && static_cast<int>(x) < kRedU + kBlockW) c = 0x800000FFu;
            if (in_v && static_cast<int>(x) >= kBlueU && static_cast<int>(x) < kBlueU + kBlockW) c = 0x80FF0000u;
            write_pixel(PSMCT32, kTexBp, 1, x, y, c);
        }
    std::memcpy(words.data(), memory(), kMemorySize);
    for (std::uint32_t y = 0; y < kH; ++y)
        for (std::uint32_t x = 0; x < kW; ++x) words[address32(0, kW / 64, x, y) / 4] = 0;
    gpu::write_state(words, 1, clut);
}

// One piece: block (u0, v0) of kBlockW x kBlockH texels to the screen at (x0, y0), 1:1. The first covered pixel is ceil(x0); its
// texture coordinate is the block's first texel (centre or corner as the case says), as a game lays a piece out for 1x.
void piece(const Case &c, const State &s, double x0, double y0, int u0, int v0) {
    const double fx = std::ceil(x0) - x0, fy = std::ceil(y0) - y0;  // from the corner to the first covered pixel
    const double half = c.centred ? 0.5 : 0.0;
    const double ua = u0 + half - fx, va = v0 + half - fy;
    const double x1 = x0 + kBlockW, y1 = y0 + kBlockH, ub = ua + kBlockW, vb = va + kBlockH;
    if (c.sprite) {
        Vertex v[2] = {vtx(x0, y0, ua, va), vtx(x1, y1, ub, vb)};
        gpu::draw(s, v, 2);
        return;
    }
    const Vertex tl = vtx(x0, y0, ua, va), tr = vtx(x1, y0, ub, va), bl = vtx(x0, y1, ua, vb), br = vtx(x1, y1, ub, vb);
    if (!c.fan) {
        Vertex a[3] = {tl, tr, bl}, b[3] = {tr, br, bl};
        gpu::draw(s, a, 3);
        gpu::draw(s, b, 3);
    } else {
        Vertex a[3] = {tl, tr, br}, b[3] = {tl, br, bl};
        gpu::draw(s, a, 3);
        gpu::draw(s, b, 3);
    }
}

std::string run(const Case &c) {
    clear();
    const State s = make_state(c);
    ++g_setup_generation;
    piece(c, s, c.ox, c.oy, kRedU, kBlockV);
    piece(c, s, c.ox + kBlockW, c.oy, kBlueU, kBlockV);
    std::vector<std::uint32_t> vram, clut;
    std::uint32_t scale = 1;
    gpu::read_state(vram, scale, clut);
    const std::uint32_t n = scale * scale;
    const int x0 = static_cast<int>(std::ceil(c.ox)), y0 = static_cast<int>(std::ceil(c.oy));
    const int split = static_cast<int>(std::ceil(c.ox + kBlockW));  // first pixel of the blue piece
    const int x1 = static_cast<int>(std::ceil(c.ox + 2 * kBlockW)), y1 = static_cast<int>(std::ceil(c.oy + kBlockH));
    std::uint64_t green = 0, other = 0, empty = 0;
    int shown = 0;
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) {
            const std::uint32_t w = address32(0, kW / 64, static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y)) / 4;
            for (std::uint32_t k = 0; k < n; ++k) {
                const std::uint32_t p = vram[static_cast<std::size_t>(w) * n + k];
                const std::uint32_t r = p & 0xFFu, g = (p >> 8) & 0xFFu, b = (p >> 16) & 0xFFu;
                const bool red_piece = x < split;
                const bool bad_green = g > 2, bad_other = red_piece ? b > 2 : r > 2;
                green += bad_green;
                other += bad_other;
                empty += (r | g | b) == 0;
                if ((bad_green || bad_other) && shown < 4 && std::getenv("SCALE_EDGES_SHOW")) {
                    std::fprintf(stderr, "  %s: pixel %d,%d sample %u reads %06x\n", c.name, x, y, k, p & 0xFFFFFFu);
                    ++shown;
                }
            }
        }
    char line[256];
    std::snprintf(line, sizeof line, "%-48s scale %u: sheet %llu, other piece %llu, empty %llu", c.name, scale,
                  static_cast<unsigned long long>(green), static_cast<unsigned long long>(other), static_cast<unsigned long long>(empty));
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
        std::printf("scale edges: no graphics adapter, nothing checked\n");
        return 0;
    }
    const Case cases[] = {
        //  name                                              sprite bilin  centred ox      oy     fan
        {"sprites, filtered, texel centres", true, true, true, 40.0, 40.0, false},
        {"sprites, filtered, fractional place", true, true, true, 40.25, 40.5, false},
        {"sprites, unfiltered, texel corners", true, false, false, 40.0, 40.0, false},
        {"sprites, unfiltered, texel centres", true, false, true, 40.0, 40.0, false},
        {"triangles, filtered, texel centres", false, true, true, 40.0, 40.0, false},
        {"triangles, filtered, texel centres, other diagonal", false, true, true, 40.0, 40.0, true},
        {"triangles, filtered, fractional place", false, true, true, 40.25, 40.5, false},
        {"triangles, unfiltered, texel corners", false, false, false, 40.0, 40.0, false},
        {"triangles, unfiltered, texel centres", false, false, true, 40.0, 40.0, false},
        {"triangles, unfiltered, fractional place", false, false, true, 40.25, 40.5, true},
    };
    for (const Case &c : cases) std::printf("%s\n", run(c).c_str());
    return 0;
}
