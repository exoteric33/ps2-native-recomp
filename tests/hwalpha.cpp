// Differential test of the card path against the compute painter.
//
//   ninja -C build/native-<game> ps2hwalpha && build/native-<game>/ps2hwalpha [--cases N] [--seed S] [--family a,b] [--row text] [--lazy|--eager]
//
// It draws many small random frames through the runtime's own gpu::draw (so the painter and the card path see exactly what a game would
// send), lets the replay switches of gpu.cpp run the painter once and the card path once on each frame (PS2_GPU_HW_DIFF=1, which keeps both
// memories), and compares the frame buffer and the depth buffer pixel by pixel. One "row" is one setting of one feature (an alpha test
// compare with a failure mode, one of the 81 blend equations, a texture format with a texture function, ...), all other registers at a plain
// baseline; each row gets N random frames (random shapes, textures, colours, alpha, depth). The table says how many of the pixels the draws
// changed were exact, within one level, within 8 levels, or further off. Nothing from any game is used: the data is random.
//
// "Level" is one step of the frame's own format: 1/255 in a 32-bit or 24-bit frame, 1/31 (8/255) in a 16-bit frame.
// Shapes the card path leaves to the painter (PS2_GPU_HW_MATCH=1 is on, so the painter leaves them out as well) are counted, not compared.
#include "gs/gpu/gpu.hpp"
#include "gs/gpu/hwdraw.hpp"
#include "gs/gs.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <random>
#include <string>
#include <vector>

#ifdef _WIN32
#define setenv(name, value, overwrite) _putenv_s(name, value)  // the test sets the switches of the card path before the first frame
#endif

using namespace ps2::gs;
namespace hw = ps2::gs::gpu::hw;

namespace {

constexpr std::uint32_t kFbp = 0, kZbp = 2, kTbp = 512, kCbp = 640;  // frame at page 0 (2 pages), depth at page 2, texture at word 32768, palette at word 40960
constexpr int kSize = 64;                                             // the drawing area is 64 x 64 pixels

struct Cfg {
    std::string family, row;
    int fpsm = 0;  // 0 PSMCT32, 1 PSMCT24, 2 PSMCT16
    bool zte = true, zmsk = false;
    int ztst = 2;  // 0 never, 1 always, 2 greater or equal, 3 greater
    bool ate = false;
    int atst = 1, afail = 0, aref = 0x40;
    bool abe = false;
    int ba = 0, bb = 1, bc = 0, bd = 1, fix = 0x40;
    bool tme = true, tcc = true, linear = false;
    int tfx = 0, tpsm = 0, wms = 0, wmt = 0;
    std::uint32_t fbmask = 0;
    bool colclamp = true, fba = false, iip = true, fst = true;
    int prim = 3;  // 3 triangle, 6 sprite
    int probe_u0 = 0, probe_v0 = 0, probe_x0 = 8, probe_y0 = 8, probe_w = 16;
    bool probe = false;  // one fixed 1:1 sprite at 8,8 (16 x 16) showing texel 0,0 of the texture: for looking at the sampling by eye
    bool one_to_one = false;  // sprites: texture extent equal to the rectangle (the usual case in the games)
    bool flat = false;  // all shapes at one depth (the "independent" case)
    int shapes = 8;
    int snap = 0;  // coordinates: 0 sixteenths of a pixel, 1 whole pixels, 2 half pixels (the edge cases of the coverage rule)
    int alpha_max = 0x80;  // vertex alpha range
    int ta0 = 0, ta1 = 0x80, aem = 0;
};

struct Stat {
    std::uint64_t cases = 0, shapes = 0, shapes_card = 0, draws = 0, two = 0;
    std::uint64_t touched = 0, exact = 0, d1 = 0, d8 = 0, bad = 0, a_touched = 0, a_exact = 0, z_touched = 0, z_exact = 0, z_near = 0;
    int worst = 0;
    std::map<std::string, std::uint64_t> why;
    void add(const Stat &o) {
        cases += o.cases, shapes += o.shapes, shapes_card += o.shapes_card, draws += o.draws, two += o.two;
        touched += o.touched, exact += o.exact, d1 += o.d1, d8 += o.d8, bad += o.bad, a_touched += o.a_touched, a_exact += o.a_exact;
        z_touched += o.z_touched, z_exact += o.z_exact, z_near += o.z_near;
        worst = std::max(worst, o.worst);
        for (const auto &[k, v] : o.why) why[k] += v;
    }
};

std::mt19937 g_rng;
std::uint32_t rnd(std::uint32_t n) { return n ? static_cast<std::uint32_t>(g_rng() % n) : 0; }
std::uint32_t rnd(std::uint32_t lo, std::uint32_t hi) { return lo + rnd(hi - lo + 1); }

State g_s;

void upload(std::uint32_t dbp, std::uint32_t dbw, std::uint32_t dpsm, int w, int h, const std::vector<std::uint8_t> &bytes) {
    g_s.bitbltbuf = (static_cast<std::uint64_t>(dbp) << 32) | (static_cast<std::uint64_t>(dbw) << 48) | (static_cast<std::uint64_t>(dpsm) << 56);
    g_s.trxpos = 0;
    g_s.trxreg = static_cast<std::uint64_t>(w) | (static_cast<std::uint64_t>(h) << 32);
    g_s.trxdir = 0;
    gpu::image_begin(g_s);
    gpu::image_data(bytes.data(), bytes.size());
}

bool g_alpha_hi = false;  // texture and palette alpha above 0x80 (1.0): only in the rows that ask for it, the games' textures stay at or below it

std::vector<std::uint8_t> random_bytes(std::size_t n, int alpha_every4 = -1) {
    std::vector<std::uint8_t> v(n);
    for (auto &b : v) b = static_cast<std::uint8_t>(rnd(256));
    if (alpha_every4 >= 0)  // alpha byte of 32-bit pixels: a mix of clear, half, full and random
        for (std::size_t i = alpha_every4; i < n; i += 4) {
            const std::uint32_t k = rnd(8);
            v[i] = k == 0 ? 0 : k == 1 ? 0x80 : k == 2 ? (g_alpha_hi ? 0xFF : 0x80) : static_cast<std::uint8_t>(rnd(g_alpha_hi ? 256u : 0x81u));
        }
    return v;
}

int g_alpha_force = -1;
int g_ramp = -1;  // --ramp: the shapes' red, green and blue are the frame number modulo 256 (every level once, for fitting the card's blender with --csv)  // --alpha N: every shape's vertex alpha is N (for fitting the card's blender with --csv)

Vertex make_vertex(const Cfg &c, int x16, int y16, std::uint32_t z, bool flat_colour_of, const Vertex *first) {
    if (c.snap == 1) x16 &= ~15, y16 &= ~15;
    else if (c.snap == 2) x16 &= ~7, y16 &= ~7;
    Vertex v{};
    v.x = static_cast<std::uint32_t>(x16);
    v.y = static_cast<std::uint32_t>(y16);
    v.z = z;
    (void)flat_colour_of;
    (void)first;
    v.r = static_cast<std::uint8_t>(rnd(256));
    v.g = static_cast<std::uint8_t>(rnd(256));
    v.b = static_cast<std::uint8_t>(rnd(256));
    v.a = static_cast<std::uint8_t>(rnd(static_cast<std::uint32_t>(c.alpha_max) + 1));
    if (g_alpha_force >= 0) v.a = static_cast<std::uint8_t>(g_alpha_force);
    if (g_ramp >= 0) v.r = v.g = v.b = static_cast<std::uint8_t>(g_ramp);
    v.q = 1.0f;
    return v;
}

std::uint32_t g_clear_z = 0, g_clear_rgba = 0;
std::vector<std::uint8_t> g_tex;

// All registers of the setting `c` (context 1, PRIM selects it).
void set_state(const Cfg &c) {
    State &s = g_s;
    std::memset(&s.ctx[0], 0, sizeof(Context));
    Context &x = s.ctx[0];
    x.frame = kFbp | (1ull << 16) | (static_cast<std::uint64_t>(c.fpsm) << 24) | (static_cast<std::uint64_t>(c.fbmask) << 32);
    x.zbuf = kZbp | (1ull << 24) | (static_cast<std::uint64_t>(c.zmsk) << 32);  // Z24
    x.test = (c.ate ? 1u : 0u) | (static_cast<std::uint64_t>(c.atst) << 1) | (static_cast<std::uint64_t>(c.aref) << 4) | (static_cast<std::uint64_t>(c.afail) << 12) |
             ((c.zte ? 1ull : 0ull) << 16) | (static_cast<std::uint64_t>(c.ztst) << 17);
    x.tex0 = kTbp | (1ull << 14) | (static_cast<std::uint64_t>(c.tpsm) << 20) | (5ull << 26) | (5ull << 30) | ((c.tcc ? 1ull : 0ull) << 34) | (static_cast<std::uint64_t>(c.tfx) << 35) |
             (static_cast<std::uint64_t>(kCbp) << 37);
    x.clamp = static_cast<std::uint64_t>(c.wms) | (static_cast<std::uint64_t>(c.wmt) << 2);
    x.tex1 = c.linear ? (1ull << 5) | (1ull << 6) : 0;
    x.alpha = static_cast<std::uint64_t>(c.ba) | (static_cast<std::uint64_t>(c.bb) << 2) | (static_cast<std::uint64_t>(c.bc) << 4) | (static_cast<std::uint64_t>(c.bd) << 6) |
              (static_cast<std::uint64_t>(c.fix) << 32);
    x.scissor = 0 | (static_cast<std::uint64_t>(kSize - 1) << 16) | (0ull << 32) | (static_cast<std::uint64_t>(kSize - 1) << 48);
    x.xyoffset = 0;
    x.fba = c.fba ? 1 : 0;
    s.prim = static_cast<std::uint64_t>(c.prim) | ((c.iip ? 1ull : 0ull) << 3) | ((c.tme ? 1ull : 0ull) << 4) | ((c.abe ? 1ull : 0ull) << 6) | ((c.fst ? 1ull : 0ull) << 8);
    s.prmodecont = 1;
    s.colclamp = c.colclamp ? 1 : 0;
    s.pabe = 0;
    s.scanmsk = 0;
    s.texa = static_cast<std::uint64_t>(c.ta0) | (static_cast<std::uint64_t>(c.aem) << 15) | (static_cast<std::uint64_t>(c.ta1) << 32);
    s.fogcol = 0;
    s.texclut = 1;  // buffer width 1, offsets 0
    ++g_setup_generation;

}

bool g_csv = false;  // --csv: one flat sprite per frame, prints the colour, the alpha and what painter and card made of the first pixel (for fitting the card's blender)
std::uint32_t g_last_rgba = 0;
int g_shapes_override = 0;  // --shapes N: N shapes per frame in every row (1 = no stacking: one blend over the clear colour)

// Draw one frame: the clear, then random shapes with the setting `c`.
void draw_case(const Cfg &c) {
    g_alpha_hi = c.alpha_max > 0x80;
    // Two frames: the first only uploads (the frame buffer, depth, texture and palette), so that the second one, the one that is compared, starts from memory
    // that holds them ("before" is the memory a frame starts from, and the uploads of a frame belong to the frame).
    gpu::flush();  // starts the capture of the upload frame (the bench is armed for it)
    // memory: frame, depth, texture, palette
    {
        if (c.fpsm == 2) upload(kFbp * 32, 1, PSMCT16, kSize, kSize, random_bytes(kSize * kSize * 2));
        else upload(kFbp * 32, 1, PSMCT32, kSize, kSize, random_bytes(kSize * kSize * 4, 3));
        std::vector<std::uint8_t> z(kSize * kSize * 4, 0);
        for (std::size_t i = 0; i < z.size(); i += 4) {
            const std::uint32_t zv = rnd(0x1000000);
            std::memcpy(&z[i], &zv, 4);
        }
        upload(kZbp * 32, 1, PSMZ32, kSize, kSize, z);
        const int tw = 32, th = 32;
        switch (c.tpsm) {
        case 0: g_tex = random_bytes(tw * th * 4, 3); upload(kTbp, 1, PSMCT32, tw, th, g_tex); break;
        case 1: upload(kTbp, 1, PSMCT24, tw, th, random_bytes(tw * th * 3)); break;
        case 2: upload(kTbp, 1, PSMCT16, tw, th, random_bytes(tw * th * 2)); break;
        case 0x13: upload(kTbp, 1, PSMT8, tw, th, random_bytes(tw * th)); break;
        case 0x14: upload(kTbp, 1, PSMT4, tw, th, random_bytes(tw * th / 2)); break;
        }
        if (c.tpsm == 0x13 || c.tpsm == 0x14) upload(kCbp, 1, PSMCT32, 16, 16, random_bytes(16 * 16 * 4, 3));
    }
    gpu::flush();  // ends the upload frame
    gpu::flush();  // starts the capture of the frame with the shapes
    // the frame starts with a clear of the frame buffer and the depth buffer (a constant sprite, depth test ALWAYS): the card path makes new targets by clear, not by
    // loading memory so a test that starts from random memory would test the load, which is a different test. Pixels the shapes did not reach hold the clear
    // values, which is how the comparison finds the pixels the shapes changed.
    {
        Cfg k;
        k.fpsm = c.fpsm;
        k.tme = false, k.zte = true, k.ztst = 1, k.prim = 6, k.shapes = 1, k.tpsm = c.tpsm;
        set_state(k);
        g_clear_z = rnd(0x200000, 0xE00000);
        Vertex v[2];
        v[0] = make_vertex(k, 0, 0, g_clear_z, false, nullptr);
        v[1] = make_vertex(k, kSize * 16, kSize * 16, g_clear_z, false, nullptr);
        v[1].a = static_cast<std::uint8_t>(rnd(0x81));
        g_clear_rgba = v[1].r | (static_cast<std::uint32_t>(v[1].g) << 8) | (static_cast<std::uint32_t>(v[1].b) << 16) | (static_cast<std::uint32_t>(v[1].a) << 24);
        gpu::draw(g_s, v, 2);
    }
    State &s = g_s;
    set_state(c);
    if (c.tme && (c.tpsm == 0x13 || c.tpsm == 0x14)) gpu::load_clut(s.ctx[0].tex0, s.texclut);

    const std::uint32_t zflat = rnd(0x200000, 0xE00000);
    for (int n = 0; n < (g_shapes_override ? g_shapes_override : c.shapes); ++n) {
        Vertex v[3];
        const int count = c.prim == 6 ? 2 : 3;
        const std::uint32_t z = c.flat ? zflat : rnd(0x100000, 0xF00000);
        if (c.prim == 6) {
            int x0 = static_cast<int>(rnd(2, 40)), y0 = static_cast<int>(rnd(2, 40));
            int w = static_cast<int>(rnd(4, 24)), h = static_cast<int>(rnd(4, 24));
            if (c.probe) x0 = c.probe_x0, y0 = c.probe_y0, w = c.probe_w, h = c.probe_w;
            v[0] = make_vertex(c, x0 * 16 + static_cast<int>(rnd(16)) * 0, y0 * 16, z, false, nullptr);
            v[1] = make_vertex(c, (x0 + w) * 16, (y0 + h) * 16, z, false, nullptr);
        } else {
            for (int i = 0; i < 3; ++i) v[i] = make_vertex(c, static_cast<int>(rnd(2 * 16, 62 * 16)), static_cast<int>(rnd(2 * 16, 62 * 16)), c.flat ? z : rnd(0x100000, 0xF00000), false, nullptr);
        }
        const int u0 = c.probe ? c.probe_u0 : static_cast<int>(rnd(0, 20)), v0 = c.probe ? c.probe_v0 : static_cast<int>(rnd(0, 20));
        for (int i = 0; i < count; ++i) {
            if (c.prim == 6) {
                const int du = c.one_to_one ? static_cast<int>((v[1].x - v[0].x) / 16) : static_cast<int>(rnd(4, 24)), dv = c.one_to_one ? static_cast<int>((v[1].y - v[0].y) / 16) : static_cast<int>(rnd(4, 24));
                v[i].u = static_cast<std::uint32_t>((u0 + (i ? du : 0)) * 16);
                v[i].v = static_cast<std::uint32_t>((v0 + (i ? dv : 0)) * 16);
            } else {
                v[i].u = rnd(0, 36 * 16);
                v[i].v = rnd(0, 36 * 16);
            }
            // ST mode: s,t in 0..1 over the texture, q = 1
            v[i].s = static_cast<float>(v[i].u) / 16.0f / 32.0f;
            v[i].t = static_cast<float>(v[i].v) / 16.0f / 32.0f;
        }
        if (!c.iip && count == 3) {  // flat shading takes the last vertex's colour; the vertices say so too
            v[0].r = v[1].r = v[2].r, v[0].g = v[1].g = v[2].g, v[0].b = v[1].b = v[2].b, v[0].a = v[1].a = v[2].a;
        }
        g_last_rgba = v[count - 1].r | (static_cast<std::uint32_t>(v[count - 1].g) << 8) | (static_cast<std::uint32_t>(v[count - 1].b) << 16) | (static_cast<std::uint32_t>(v[count - 1].a) << 24);
        gpu::draw(s, v, count);
    }
    gpu::flush();  // ends the frame: the painter and the card path replay it, the memories are kept in hw::diff_result()
}

int g_dump = 0;

// Fold one frame's comparison into `st`.
void compare(const Cfg &c, Stat &st) {
    hw::DiffResult &R = hw::diff_result();
    ++st.cases;
    if (!R.valid) return;
    st.shapes += R.shapes, st.shapes_card += R.shapes_card, st.draws += R.draw_calls, st.two += R.two_pass_shapes;
    for (const auto &[why, n] : R.left_out) st.why[why] += n;
    const std::size_t bytes = static_cast<std::size_t>(kMemorySize);
    std::vector<std::uint32_t> col[3], dep[3];
    const std::vector<std::uint32_t> *mem[3] = {&R.before, &R.painter, &R.card};
    const std::uint32_t cpsm = c.fpsm == 0 ? PSMCT32 : c.fpsm == 1 ? PSMCT24 : PSMCT16;
    for (int m = 0; m < 3; ++m) {
        if (mem[m]->size() * 4 < bytes) return;
        std::memcpy(memory(), mem[m]->data(), bytes);
        for (int y = 0; y < kSize; ++y)
            for (int x = 0; x < kSize; ++x) {
                col[m].push_back(read_pixel(cpsm, kFbp * 32, 1, x, y));
                dep[m].push_back(read_pixel(PSMZ24, kZbp * 32, 1, x, y));
            }
    }
    if (g_dump < 0)
        for (int n = 0; n < 6; ++n) {
            const std::size_t i = static_cast<std::size_t>(n) * 677 % col[0].size();
            std::fprintf(stderr, "[probe] pixel %zu,%zu before %08X painter %08X card %08X | z before %06X painter %06X card %06X\n", i % kSize, i / kSize, col[0][i], col[1][i], col[2][i], dep[0][i], dep[1][i], dep[2][i]);
        }
    const auto channels = [&](std::uint32_t p, int ch) -> int {
        if (c.fpsm == 2) {
            switch (ch) {
            case 0: return p & 31;
            case 1: return (p >> 5) & 31;
            case 2: return (p >> 10) & 31;
            default: return (p >> 15) & 1;
            }
        }
        return (p >> (8 * ch)) & 0xFF;
    };
    const int nrgb = 3;
    if (c.probe && g_tex.size() == 32 * 32 * 4) {
        const auto find = [&](std::uint32_t px) {
            std::string r;
            for (int ty = 0; ty < 32; ++ty)
                for (int tx = 0; tx < 32; ++tx) {
                    std::uint32_t t;
                    std::memcpy(&t, &g_tex[(static_cast<std::size_t>(ty) * 32 + tx) * 4], 4);
                    if (t == px) r += "(" + std::to_string(tx) + "," + std::to_string(ty) + ")";
                }
            return r.empty() ? std::string("none") : r;
        };
        if (c.row == "sprite 1:1 nearest tfx 0")
        for (int y : {8, 9, 15, 23})
            for (int x : {8, 9, 16, 23}) {
                const std::size_t i = static_cast<std::size_t>(y) * kSize + x;
                std::fprintf(stderr, "[probe] pixel %d,%d painter texel %s card texel %s\n", x, y, find(col[1][i]).c_str(), find(col[2][i]).c_str());
            }
    }
    std::uint64_t csv_cases = 0;
    std::uint32_t clr = g_clear_rgba;  // what the frame holds where no shape reached
    if (c.fpsm == 1) clr &= 0xFFFFFFu;
    if (c.fpsm == 2) clr = ((g_clear_rgba & 0xFFu) >> 3) | ((((g_clear_rgba >> 8) & 0xFFu) >> 3) << 5) | ((((g_clear_rgba >> 16) & 0xFFu) >> 3) << 10) | (((g_clear_rgba >> 24) & 0x80u) ? 0x8000u : 0u);
    for (std::size_t i = 0; i < col[0].size(); ++i) {
        const std::uint32_t b = clr, p = col[1][i], k = col[2][i];
        if (p != b || k != b) {
            ++st.touched;
            if (g_csv && csv_cases != st.cases && (csv_cases = st.cases))
                std::fprintf(stderr, "csv,%s,%d,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u\n", c.row.c_str(), c.fix, g_last_rgba & 255, (g_last_rgba >> 8) & 255, (g_last_rgba >> 16) & 255, g_last_rgba >> 24, b & 255, (b >> 8) & 255,
                             (b >> 16) & 255, b >> 24, p & 255, (p >> 8) & 255, (p >> 16) & 255, k & 255, (k >> 8) & 255, (k >> 16) & 255, 0u);
            if (g_dump > 0) {
                --g_dump;
                std::fprintf(stderr, "[dump] %s %s pixel %zu,%zu before %08X painter %08X card %08X\n", c.family.c_str(), c.row.c_str(), i % kSize, i / kSize, b, p, k);
            }
            int worst = 0;
            for (int ch = 0; ch < nrgb; ++ch) worst = std::max(worst, std::abs(channels(p, ch) - channels(k, ch)));
            if (worst == 0) ++st.exact;
            else if (worst <= 1) ++st.d1;
            else if (worst <= (c.fpsm == 2 ? 1 : 8)) ++st.d8;
            else ++st.bad;
            st.worst = std::max(st.worst, worst);
            if (c.fpsm != 1) {
                const int ab = channels(b, 3), ap = channels(p, 3), ak = channels(k, 3);
                if (ap != ab || ak != ab) {
                    ++st.a_touched;
                    if (ap == ak) ++st.a_exact;
                }
            }
        }
        if (dep[1][i] != g_clear_z || dep[2][i] != g_clear_z) {
            ++st.z_touched;
            const std::int64_t dz = static_cast<std::int64_t>(dep[1][i]) - static_cast<std::int64_t>(dep[2][i]);
            if (dz == 0) ++st.z_exact;
            if (std::llabs(dz) <= 64) ++st.z_near;
        }
    }
}

// ---- the rows --------------------------------------------------------------------------------------------------------------------------

struct Row {
    Cfg cfg;
    std::function<void(Cfg &)> vary;  // per frame: random values inside the row's setting
};

std::vector<Row> make_rows() {
    std::vector<Row> rows;
    const auto add = [&](const std::string &family, const std::string &name, const std::function<void(Cfg &)> &set, std::function<void(Cfg &)> vary = {}) {
        Row r;
        r.cfg.family = family;
        r.cfg.row = name;
        set(r.cfg);
        r.vary = std::move(vary);
        rows.push_back(std::move(r));
    };
    static const char *kTest[] = {"NEVER", "ALWAYS", "LESS", "LEQUAL", "EQUAL", "GEQUAL", "GREATER", "NOTEQUAL"};
    static const char *kFail[] = {"KEEP", "FB_ONLY", "ZB_ONLY", "RGB_ONLY"};
    static const char *kSrc[] = {"Cs", "Cd", "0"};
    static const char *kFac[] = {"As", "Ad", "FIX"};
    // alpha test: 8 compares x 4 failure modes, random overlapping triangles and independent flat sprites
    for (int flat = 0; flat < 2; ++flat)
        for (int t = 0; t < 8; ++t)
            for (int f = 0; f < 4; ++f)
                add(flat ? "alphatest-flat" : "alphatest", std::string(kTest[t]) + " / " + kFail[f], [=](Cfg &c) {
                    c.ate = true, c.atst = t, c.afail = f, c.flat = flat, c.prim = flat ? 6 : 3;
                }, [](Cfg &c) { c.aref = static_cast<int>(rnd(0x10, 0x70)); });
    // blend: all 81 equations (A - B) * C + D, alpha up to 1.0 and fix up to 1.0; then the factor above 1.0 for the FIX rows; then alpha above 0x80 for the usual rows
    for (int a = 0; a < 3; ++a)
        for (int b = 0; b < 3; ++b)
            for (int cc = 0; cc < 3; ++cc)
                for (int d = 0; d < 3; ++d) {
                    const std::string name = std::string("(") + kSrc[a] + "-" + kSrc[b] + ")*" + kFac[cc] + "+" + kSrc[d];
                    add("blend", name, [=](Cfg &c) { c.abe = true, c.ba = a, c.bb = b, c.bc = cc, c.bd = d; }, [](Cfg &c) { c.fix = static_cast<int>(rnd(0, 0x80)); });
                    if (cc == 2)
                        add("blend-fix>1", name, [=](Cfg &c) { c.abe = true, c.ba = a, c.bb = b, c.bc = cc, c.bd = d; }, [](Cfg &c) { c.fix = static_cast<int>(rnd(0x81, 0xFF)); });
                    const bool usual = (a == 0 && b == 1 && cc == 0 && d == 1) || (a == 0 && b == 2 && cc == 0 && d == 1) || (a == 1 && b == 0 && cc == 0 && d == 0) ||
                                       (a == 2 && b == 0 && cc == 0 && d == 1) || (a == 0 && b == 1 && cc == 2 && d == 1);
                    if (usual)
                        add("blend-alpha>1", name, [=](Cfg &c) { c.abe = true, c.ba = a, c.bb = b, c.bc = cc, c.bd = d, c.alpha_max = 0xFF; }, [](Cfg &c) { c.fix = static_cast<int>(rnd(0, 0x80)); });
                }
    // depth test and write
    for (int zte = 0; zte < 2; ++zte)
        for (int zt = 0; zt < (zte ? 4 : 1); ++zt)
            for (int zm = 0; zm < 2; ++zm) {
                static const char *names[] = {"NEVER", "ALWAYS", "GEQUAL", "GREATER"};
                add("depth", zte ? std::string(names[zt]) + (zm ? " no write" : " write") : std::string("test off") + (zm ? " no write" : " write"),
                    [=](Cfg &c) { c.zte = zte, c.ztst = zt, c.zmsk = zm; });
            }
    // coverage (the card's top-left rule against the chip's at pixel centres) and equal depths (decals at one depth): untextured, flat colour, so a pixel covered by only one of the two shows as far off
    for (int snap = 0; snap < 3; ++snap)
        for (int prim : {3, 6}) {
            static const char *sn[] = {"sixteenths", "whole pixels", "half pixels"};
            add("cover", std::string(prim == 6 ? "sprites " : "triangles ") + sn[snap], [=](Cfg &c) { c.prim = prim, c.snap = snap, c.tme = false, c.zte = false, c.iip = false, c.shapes = 16; });
        }
    for (int zt : {2, 3})
        for (int prim : {3, 6})
            for (int snap : {0, 1})
                add("tie", std::string(zt == 2 ? "GEQUAL " : "GREATER ") + (prim == 6 ? "sprites" : "triangles") + (snap ? " whole pixels" : ""), [=](Cfg &c) { c.prim = prim, c.snap = snap, c.tme = false, c.flat = true, c.ztst = zt, c.iip = false, c.shapes = 16; });
    // texture: format x function x TCC x filter
    for (int tp : {0x00, 0x01, 0x02, 0x13, 0x14})
        for (int tfx = 0; tfx < 4; ++tfx)
            for (int tcc = 0; tcc < 2; ++tcc)
                for (int lin = 0; lin < 2; ++lin) {
                    static const char *tn[] = {"MODULATE", "DECAL", "HIGHLIGHT", "HIGHLIGHT2"};
                    const char *fn = tp == 0 ? "CT32" : tp == 1 ? "CT24" : tp == 2 ? "CT16" : tp == 0x13 ? "T8" : "T4";
                    add("texture", std::string(fn) + " " + tn[tfx] + (tcc ? " RGBA" : " RGB") + (lin ? " bilinear" : " nearest"), [=](Cfg &c) {
                        c.tpsm = tp, c.tfx = tfx, c.tcc = tcc, c.linear = lin;
                    }, [tp](Cfg &c) { if (tp == 1 || tp == 2) c.ta0 = static_cast<int>(rnd(0, 0x80)), c.aem = static_cast<int>(rnd(2)); });
                }
    // wrap modes
    for (int wm = 0; wm < 2; ++wm)
        for (int wt = 0; wt < 2; ++wt)
            for (int lin = 0; lin < 2; ++lin)
                add("wrap", std::string(wm ? "clamp" : "repeat") + "/" + (wt ? "clamp" : "repeat") + (lin ? " bilinear" : " nearest"),
                    [=](Cfg &c) { c.wms = wm, c.wmt = wt, c.linear = lin; });
    // frame format and masks
    for (int fp = 0; fp < 3; ++fp) {
        static const char *fn[] = {"CT32", "CT24", "CT16"};
        add("frame", std::string(fn[fp]) + " plain", [=](Cfg &c) { c.fpsm = fp; });
        add("frame", std::string(fn[fp]) + " blend", [=](Cfg &c) { c.fpsm = fp, c.abe = true, c.ba = 0, c.bb = 1, c.bc = 0, c.bd = 1; });
        add("frame", std::string(fn[fp]) + " whole-channel mask", [=](Cfg &c) { c.fpsm = fp; }, [fp](Cfg &c) {
            if (fp == 2) { c.fbmask = rnd(2) ? 0xFFFF0000u | (rnd(2) ? 0x7C00u : 0) : 0xFFFF0000u | 0x7C00u; return; }
            c.fbmask = 0;
            for (int ch = 0; ch < 4; ++ch) if (rnd(3) == 0) c.fbmask |= 0xFFu << (8 * ch);
        });
        add("frame", std::string(fn[fp]) + " partial mask", [=](Cfg &c) { c.fpsm = fp; c.fbmask = fp == 2 ? 0xFFFF3FFFu : 0x0F0F0F0Fu; });
    }
    // colour clamp
    for (int cl = 0; cl < 2; ++cl)
        for (int bl = 0; bl < 2; ++bl)
            add("clamp", std::string(cl ? "clamp" : "wrap") + (bl ? " with blend" : " plain"), [=](Cfg &c) { c.colclamp = cl, c.abe = bl, c.ba = 0, c.bb = 2, c.bc = 0, c.bd = 1; });
    // FBA and shapes
    for (int f = 0; f < 2; ++f) add("fba", f ? "FBA on" : "FBA off", [=](Cfg &c) { c.fba = f; });
    for (int prim : {3, 6})
        for (int iip : {0, 1})
            for (int fst : {0, 1}) {
                if (prim == 6 && iip == 0) continue;
                add("shape", std::string(prim == 6 ? "sprite" : "triangle") + (iip ? " Gouraud" : " flat") + (fst ? " UV" : " STQ"), [=](Cfg &c) { c.prim = prim, c.iip = iip, c.fst = fst; });
            }
    add("shape", "sprite untextured", [](Cfg &c) { c.prim = 6, c.tme = false; });
    add("shape", "sprite untextured flat depth", [](Cfg &c) { c.prim = 6, c.tme = false, c.flat = true; });
    add("shape", "sprite textured nearest flat depth", [](Cfg &c) { c.prim = 6, c.flat = true; });
    add("shape", "sprite textured 1:1 nearest", [](Cfg &c) { c.prim = 6, c.one_to_one = true; });
    add("shape", "sprite textured 1:1 bilinear", [](Cfg &c) { c.prim = 6, c.one_to_one = true, c.linear = true; });
    for (int t = 0; t < 4; ++t)
        add("probe", "sprite 1:1 nearest tfx " + std::to_string(t), [t](Cfg &c) { c.prim = 6, c.one_to_one = true, c.probe = true, c.tfx = t, c.shapes = 1; });
    add("probe", "sprite 1:1 tfx 0, u0 5", [](Cfg &c) { c.prim = 6, c.one_to_one = true, c.probe = true, c.probe_u0 = 5, c.shapes = 1; });
    add("probe", "sprite 1:1 tfx 0, v0 5", [](Cfg &c) { c.prim = 6, c.one_to_one = true, c.probe = true, c.probe_v0 = 5, c.shapes = 1; });
    add("probe", "sprite 1:1 tfx 0, x0 21", [](Cfg &c) { c.prim = 6, c.one_to_one = true, c.probe = true, c.probe_x0 = 21, c.shapes = 1; });
    add("probe", "sprite 1:1 tfx 0, w 7", [](Cfg &c) { c.prim = 6, c.one_to_one = true, c.probe = true, c.probe_w = 7, c.shapes = 1; });
    add("probe", "sprite 1:1 tfx 0, w 24, u0 12", [](Cfg &c) { c.prim = 6, c.one_to_one = true, c.probe = true, c.probe_w = 24, c.probe_u0 = 12, c.shapes = 1; });
    add("probe", "sprite random 1:1 modulate, one shape", [](Cfg &c) { c.prim = 6, c.one_to_one = true, c.shapes = 1; });
    add("probe", "sprite random 1:1 modulate, one shape, no depth test", [](Cfg &c) { c.prim = 6, c.one_to_one = true, c.shapes = 1, c.zte = false; });
    add("shape", "triangle untextured", [](Cfg &c) { c.prim = 3, c.tme = false; });
    return rows;
}

std::string pct(std::uint64_t n, std::uint64_t d) {
    char b[32];
    if (!d) return "-";
    std::snprintf(b, sizeof b, "%.1f", 100.0 * static_cast<double>(n) / static_cast<double>(d));
    return b;
}

std::string zs(const Stat &s) { return s.z_touched ? pct(s.z_exact, s.z_touched) + " / " + pct(s.z_near, s.z_touched) : std::string("-"); }

}  // namespace

int main(int argc, char **argv) {
    int cases = 6;
    unsigned seed = 1;
    std::string only_family, only_row;
    bool first_why = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--cases" && i + 1 < argc) cases = std::atoi(argv[++i]);
        else if (a == "--seed" && i + 1 < argc) seed = static_cast<unsigned>(std::atoi(argv[++i]));
        else if (a == "--family" && i + 1 < argc) only_family = argv[++i];
        else if (a == "--row" && i + 1 < argc) only_row = argv[++i];
        else if (a == "--why") first_why = true;
        else if (a == "--csv") g_csv = true;
        else if (a == "--ramp") g_ramp = 0;
        else if (a == "--alpha" && i + 1 < argc) g_alpha_force = std::atoi(argv[++i]);
        else if (a == "--shapes" && i + 1 < argc) g_shapes_override = std::atoi(argv[++i]);
        else if (a == "--dump" && i + 1 < argc) g_dump = std::atoi(argv[++i]);
        else if (a == "--eager") setenv("PS2_GPU_HW_EAGER", "1", 1);
        else if (a == "--lazy") setenv("PS2_GPU_HW_EAGER", "0", 1);
        else {
            std::fprintf(stderr, "usage: ps2hwalpha [--cases N] [--seed S] [--family a,b] [--row text] [--shapes N] [--why] [--eager|--lazy]\n");
            return 2;
        }
    }
    setenv("PS2_GPU_HW", "1", 0);
    setenv("PS2_GPU_BENCH", "2,1", 0);
    setenv("PS2_GPU_HW_DIRECT", "1", 0);
    setenv("PS2_GPU_HW_MATCH", "1", 0);
    setenv("PS2_GPU_HW_DIFF", "1", 0);
    setenv("PS2_GPU_BENCH_DETAIL", "0", 0);
    ps2::gs::init();  // builds the swizzle tables read_pixel needs and opens the card
    if (gpu::mode() != gpu::Mode::primary) {
        std::fprintf(stderr, "no graphics card could be opened\n");
        return 1;
    }
    std::memset(&g_s, 0, sizeof g_s);
    const std::vector<Row> rows = make_rows();
    std::map<std::string, Stat> by_family;
    Stat total;
    std::printf("| family | row | frames | shapes on card | pixels changed | exact %% | within 1 | within 8 | further | worst | alpha exact %% | depth exact / within 64% | draws (two-pass shapes) |\n");
    std::printf("|---|---|---|---|---|---|---|---|---|---|---|---|---|\n");
    for (const Row &row : rows) {
        if (!only_family.empty() && ("," + only_family + ",").find("," + row.cfg.family + ",") == std::string::npos) continue;
        if (!only_row.empty() && row.cfg.row.find(only_row) == std::string::npos) continue;
        g_rng.seed(seed * 7919u + static_cast<unsigned>(std::hash<std::string>{}(row.cfg.family + row.cfg.row) & 0xFFFFu));
        Stat st;
        for (int n = 0; n < cases; ++n) {
            Cfg c = row.cfg;
            if (row.vary) row.vary(c);
            if (g_ramp >= 0) g_ramp = n & 255;
            if (g_csv) c.prim = 6, c.iip = false, c.tme = false, c.shapes = 1;
            hw::diff_result() = hw::DiffResult{};
            draw_case(c);
            compare(c, st);
        }
        std::string why;
        for (const auto &[k, v] : st.why) why += (why.empty() ? "" : ", ") + k + " " + std::to_string(v);
        std::printf("| %s | %s | %llu | %s | %llu | %s | %s | %s | %s | %d | %s | %s | %llu (%llu) |%s\n", row.cfg.family.c_str(), row.cfg.row.c_str(),
                    static_cast<unsigned long long>(st.cases), pct(st.shapes_card, st.shapes).c_str(), static_cast<unsigned long long>(st.touched), pct(st.exact, st.touched).c_str(),
                    pct(st.d1, st.touched).c_str(), pct(st.d8, st.touched).c_str(), pct(st.bad, st.touched).c_str(), st.worst, pct(st.a_exact, st.a_touched).c_str(),
                    zs(st).c_str(), static_cast<unsigned long long>(st.draws), static_cast<unsigned long long>(st.two),
                    first_why && !why.empty() ? (" left to painter: " + why).c_str() : "");
        std::fflush(stdout);
        by_family[row.cfg.family].add(st);
        total.add(st);
    }
    std::printf("\n| family | frames | shapes on card | pixels changed | exact %% | within 1 | within 8 | further | alpha exact %% | depth exact / within 64% |\n|---|---|---|---|---|---|---|---|---|---|\n");
    for (const auto &[f, st] : by_family)
        std::printf("| %s | %llu | %s | %llu | %s | %s | %s | %s | %s | %s |\n", f.c_str(), static_cast<unsigned long long>(st.cases), pct(st.shapes_card, st.shapes).c_str(),
                    static_cast<unsigned long long>(st.touched), pct(st.exact, st.touched).c_str(), pct(st.d1, st.touched).c_str(), pct(st.d8, st.touched).c_str(),
                    pct(st.bad, st.touched).c_str(), pct(st.a_exact, st.a_touched).c_str(), zs(st).c_str());
    std::printf("| all | %llu | %s | %llu | %s | %s | %s | %s | %s | %s |\n", static_cast<unsigned long long>(total.cases), pct(total.shapes_card, total.shapes).c_str(),
                static_cast<unsigned long long>(total.touched), pct(total.exact, total.touched).c_str(), pct(total.d1, total.touched).c_str(), pct(total.d8, total.touched).c_str(),
                pct(total.bad, total.touched).c_str(), pct(total.a_exact, total.a_touched).c_str(), zs(total).c_str());
    return 0;
}
