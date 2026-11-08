// No game data: the GS's mipmapping (TEX1, MIPTBP1/2; docs/mipmap.md) in the software reference (raster.cpp) and the draw program (gs.wgsl).
// A triangle whose Q falls from 1 to 1/64 across it (so log2(1/Q) runs from 0 to 6) is drawn over a texture whose seven levels each have one
// colour of their own; the colour of every pixel then says which level it read. Checked against the level the TEX1 formula gives at that pixel,
// for LCM 0 and 1, MXL limits, L and K, MTBA, and the blend between levels; patterned levels compare the two renderers with each other.
// ninja ps2mipmaptest. Without a graphics adapter only the software reference is checked.
#include "gs/gs.hpp"
#include "gs/gpu/gpu.hpp"
#include "gs/raster.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace ps2::gs;

namespace {

int failed = 0;
bool have_gpu = false;

constexpr std::uint32_t kW = 256, kH = 64;               // the frame: 256x64 PSMCT32 at block 0, width 4
constexpr std::uint32_t kTexBp = 0x1000;                 // level 0: 64x64 PSMCT32, width 1
constexpr std::uint32_t kLevelBp[7] = {0x1000, 0x1040, 0x1050, 0x1054, 0x1055, 0x1056, 0x1057};  // levels packed one after the other (the MTBA rule)

std::uint32_t level_colour(int n) {
    return 0x80000000u | (static_cast<std::uint32_t>(n * 36) << 16) | (static_cast<std::uint32_t>(255 - n * 36) << 8) | static_cast<std::uint32_t>(n * 20 + 10);
}

void write_levels(bool pattern) {
    for (int n = 0; n <= 6; ++n) {
        const std::uint32_t size = 64u >> n;
        for (std::uint32_t y = 0; y < size; ++y)
            for (std::uint32_t x = 0; x < size; ++x) {
                std::uint32_t c = level_colour(n);
                if (pattern && ((x ^ y) & 1u)) c ^= 0x00FFFFFFu;
                write_pixel(PSMCT32, kLevelBp[n], 1, x, y, c);
            }
    }
}

void clear_frame() {
    for (std::uint32_t y = 0; y < kH; ++y)
        for (std::uint32_t x = 0; x < kW; ++x) write_pixel(PSMCT32, 0, 4, x, y, 0);
}

struct Case {
    const char *name;
    std::uint32_t lcm, mxl, mmag, mmin, mtba, l;
    int k16;  // K in sixteenths
    bool explicit_levels;  // MIPTBP1/2 point at the levels (else zero)
};

std::uint64_t tex1_of(const Case &c) {
    return c.lcm | (c.mxl << 2) | (c.mmag << 5) | (c.mmin << 6) | (c.mtba << 9) | (std::uint64_t{c.l} << 19) |
           (std::uint64_t{static_cast<std::uint32_t>(c.k16) & 0xFFFu} << 32);
}

State make_state(const Case &c) {
    State s{};
    s.prim = 3u | 0x10u;  // triangle, textured, STQ, context 1
    s.prmodecont = 1;
    Context &x = s.ctx[0];
    x.frame = 0 | (std::uint64_t{4} << 16) | (std::uint64_t{PSMCT32} << 24);
    x.zbuf = (std::uint64_t{0x100}) | (std::uint64_t{1} << 32);  // depth not written
    x.scissor = (kW - 1ull) << 16 | ((kH - 1ull) << 48);
    x.tex0 = kTexBp | (std::uint64_t{1} << 14) | (std::uint64_t{PSMCT32} << 20) | (std::uint64_t{6} << 26) | (std::uint64_t{6} << 30) | (std::uint64_t{1} << 34) |
             (std::uint64_t{1} << 35);  // 64x64, RGBA, decal
    x.tex1 = tex1_of(c);
    if (c.explicit_levels) {
        for (int n = 1; n <= 6; ++n) {
            std::uint64_t &r = n <= 3 ? x.miptbp1 : x.miptbp2;
            const int shift = ((n - 1) % 3) * 20;
            r |= (std::uint64_t{kLevelBp[n]} << shift) | (std::uint64_t{1} << (shift + 14));
        }
    }
    return s;
}

constexpr float kQFar = 1.0f / 64.0f;

void make_vertices(Vertex v[3]) {
    const float xs[3] = {0, 256, 0}, ys[3] = {0, 0, 64}, qs[3] = {1.0f, kQFar, 1.0f}, ss[3] = {0, 4, 0}, ts[3] = {0, 0, 1};
    for (int i = 0; i < 3; ++i) {
        v[i] = Vertex{};
        v[i].x = static_cast<std::uint32_t>(xs[i] * 16.0f);
        v[i].y = static_cast<std::uint32_t>(ys[i] * 16.0f);
        v[i].r = v[i].g = v[i].b = v[i].a = 0x80;
        v[i].q = qs[i];
        v[i].s = ss[i] * qs[i];
        v[i].t = ts[i] * qs[i];
    }
}

// The level of detail at pixel x (Q falls linearly with x, as the renderers interpolate it at the pixel's corner)
float lod_at(const Case &c, std::uint32_t x) {
    const float k = static_cast<float>(c.k16) / 16.0f;
    if (c.lcm) return k;
    const float w2 = static_cast<float>(x) / 256.0f;
    const float q = 1.0f + (kQFar - 1.0f) * w2;
    return std::log2(1.0f / q) * static_cast<float>(1 << c.l) + k;
}

std::uint32_t blend(std::uint32_t a, std::uint32_t b, float f) {
    std::uint32_t out = 0;
    for (int ch = 0; ch < 32; ch += 8) {
        const float x = static_cast<float>((a >> ch) & 0xFFu), y = static_cast<float>((b >> ch) & 0xFFu);
        out |= static_cast<std::uint32_t>(x + (y - x) * f + 0.5f) << ch;
    }
    return out;
}

bool close(std::uint32_t a, std::uint32_t b, int tol) {
    for (int ch = 0; ch < 32; ch += 8)
        if (std::abs(static_cast<int>((a >> ch) & 0xFFu) - static_cast<int>((b >> ch) & 0xFFu)) > tol) return false;
    return true;
}

// Draws the case with both renderers; returns the frames (software, chip)
void draw_both(const Case &c, std::vector<std::uint32_t> &cpu, std::vector<std::uint32_t> &chip) {
    clear_frame();
    const State s = make_state(c);
    Vertex v[3];
    make_vertices(v);
    ++g_setup_generation;  // the chip keeps its setup until the registers change (gs::write counts that)
    if (have_gpu) {
        std::vector<std::uint32_t> words(kMemorySize / 4), clut(512, 0);
        std::memcpy(words.data(), memory(), kMemorySize);
        gpu::write_state(words, 1, clut);
        gpu::draw(s, v, 3);
    }
    raster::draw(s, v, 3);
    cpu.assign(kW * kH, 0);
    for (std::uint32_t y = 0; y < kH; ++y)
        for (std::uint32_t x = 0; x < kW; ++x) cpu[y * kW + x] = read_pixel(PSMCT32, 0, 4, x, y);
    chip.clear();
    if (have_gpu) {
        std::vector<std::uint8_t> mem(kMemorySize);
        gpu::download(mem.data(), 0, kMemorySize);
        int differ = 0;  // the chip's memory outside the frame must be the processor's (the textures it was given)
        for (std::uint32_t w = 0x10000; w < kMemorySize / 4; ++w) {
            std::uint32_t a;
            std::memcpy(&a, mem.data() + w * 4, 4);
            differ += a != reinterpret_cast<const std::uint32_t *>(memory())[w] ? 1 : 0;
        }
        if (differ) std::fprintf(stderr, "  %s: the chip's memory differs from the processor's in %d words outside the frame\n", c.name, differ);
        chip.assign(kW * kH, 0);
        for (std::uint32_t y = 0; y < kH; ++y)
            for (std::uint32_t x = 0; x < kW; ++x) {
                std::uint32_t w;
                std::memcpy(&w, mem.data() + address32(0, 4, x, y), 4);
                chip[y * kW + x] = w;
            }
    }
}

// Every drawn pixel against the colour the formula gives (one-colour levels)
void check_levels(const Case &c) {
    write_levels(false);
    std::vector<std::uint32_t> cpu, chip;
    draw_both(c, cpu, chip);
    int drawn = 0, bad[2] = {0, 0}, skipped = 0, levels_seen = 0;
    bool seen[7] = {};
    for (std::uint32_t y = 0; y < kH; ++y)
        for (std::uint32_t x = 0; x < kW; ++x) {
            if (!cpu[y * kW + x] && (chip.empty() || !chip[y * kW + x])) continue;
            ++drawn;
            const float lod = lod_at(c, x);
            const bool linear_between = c.mmin >= 2 && (c.mmin & 1u) && c.mxl > 0;
            std::uint32_t want;
            int tol = 0;
            if (lod <= 0.0f || c.mmin < 2 || c.mxl == 0) {
                want = level_colour(0);
                if (std::fabs(lod) < 0.01f) ++skipped;
            } else if (!linear_between) {
                const float r = std::floor(lod + 0.5f);
                if (std::fabs(lod + 0.5f - r) < 0.01f) {  // on the edge between two levels: the float log2 of either side may win
                    ++skipped;
                    continue;
                }
                const int n = static_cast<int>(std::fmin(r, static_cast<float>(c.mxl)));
                want = level_colour(n);
                seen[n] = true;
            } else {
                const float whole = std::floor(lod);
                if (whole >= static_cast<float>(c.mxl)) {
                    want = level_colour(static_cast<int>(c.mxl));
                } else {
                    const int n = static_cast<int>(whole);
                    want = blend(level_colour(n), level_colour(n + 1), lod - whole);
                    seen[n] = true;
                }
                tol = 3;
            }
            const std::uint32_t got[2] = {cpu[y * kW + x], chip.empty() ? want : chip[y * kW + x]};
            for (int r = 0; r < 2; ++r) {
                if (close(got[r], want, tol)) continue;
                if (bad[r]++ < 3)
                    std::fprintf(stderr, "  %s %s: pixel %u,%u lod %.3f got %08X want %08X\n", c.name, r ? "chip" : "software", x, y, lod, got[r], want);
            }
        }
    for (bool b : seen) levels_seen += b ? 1 : 0;
    const bool ok = drawn > 4000 && !bad[0] && !bad[1];
    if (!ok) ++failed;
    std::printf("%s %-44s drawn %d, wrong: software %d, chip %s, on a level edge %d, levels seen %d\n", ok ? "ok  " : "FAIL", c.name, drawn, bad[0],
                have_gpu ? std::to_string(bad[1]).c_str() : "-", skipped, levels_seen);
}

// Patterned levels: the two renderers must agree (nearly: a pixel on a level edge may go either way)
void check_agree(const Case &c) {
    if (!have_gpu) return;
    write_levels(true);
    std::vector<std::uint32_t> cpu, chip;
    draw_both(c, cpu, chip);
    int drawn = 0, off = 0;
    for (std::size_t i = 0; i < cpu.size(); ++i) {
        if (!cpu[i] && !chip[i]) continue;
        ++drawn;
        if (!close(cpu[i], chip[i], 3)) {
            if (off < 3) std::fprintf(stderr, "  %s: pixel %zu,%zu software %08X chip %08X\n", c.name, i % kW, i / kW, cpu[i], chip[i]);
            ++off;
        }
    }
    const bool ok = drawn > 4000 && off * 200 <= drawn;
    if (!ok) ++failed;
    std::printf("%s %-44s drawn %d, software and chip differ on %d\n", ok ? "ok  " : "FAIL", c.name, drawn, off);
}

void set_env(const char *name, const char *value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

}  // namespace

int main(int argc, char **argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const bool cpu_only = argc > 1 && std::strcmp(argv[1], "--cpu") == 0;
    set_env("PS2_GS", cpu_only ? "cpu" : "gpu");
    set_env("PS2_SCALE", "1");
    init();
    have_gpu = gpu::mode() == gpu::Mode::primary;
    std::printf("mipmap test: %s\n", have_gpu ? "software reference and chip" : "software reference only (no graphics adapter)");
    //                 name                                       lcm mxl mmag mmin mtba l  k16  explicit
    check_levels({"LCM 0, nearest level, MXL 6", 0, 6, 0, 2, 0, 0, 0, true});
    check_levels({"LCM 0, nearest level, MXL 3 caps it", 0, 3, 0, 2, 0, 0, 0, true});
    check_levels({"LCM 0, L 1, K -0.5, linear inside a level", 0, 6, 1, 4, 0, 1, -8, true});
    check_levels({"LCM 1, K 2: level 2 everywhere", 1, 6, 0, 2, 0, 0, 32, true});
    check_levels({"LCM 1, K 1.75: level 2 (nearest)", 1, 6, 0, 2, 0, 0, 28, true});
    check_levels({"LCM 1, K -1: magnified, level 0", 1, 6, 0, 2, 0, 0, -16, true});
    check_levels({"LCM 0, MXL 0: level 0 only", 0, 0, 0, 2, 0, 0, 0, true});
    check_levels({"LCM 0, blend between levels, MXL 6", 0, 6, 0, 3, 0, 0, 0, true});
    check_levels({"LCM 0, blend between levels, MXL 2", 0, 2, 0, 3, 0, 0, 0, true});
    check_levels({"MTBA: levels 1-3 found without MIPTBP1", 0, 3, 0, 2, 1, 0, 0, false});
    check_agree({"agree: LINEAR_MIPMAP_LINEAR, patterned", 0, 6, 1, 5, 0, 0, 0, true});
    check_agree({"agree: NEAREST_MIPMAP_NEAREST, patterned", 0, 6, 0, 2, 0, 0, 0, true});
    check_agree({"agree: no mipmaps, MMAG 1 MMIN 0, K -3", 0, 0, 1, 0, 0, 0, -48, true});
    check_agree({"agree: no mipmaps, both linear (as before)", 0, 0, 1, 1, 0, 0, 0, true});
    std::printf("mipmap test: %d failed\n", failed);
    return failed ? 1 : 0;
}
