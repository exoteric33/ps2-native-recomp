// Counting only: see hwstat.hpp. Every register field below is read from the public GS register descriptions (ps2tek notes) and
// from how gpu.cpp's make_setup() reads the same registers. Nothing here is used to draw.
#include "hwstat.hpp"

#include <algorithm>
#include <bitset>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace ps2::gs::gpu::stat {

bool g_on = false;

namespace {

using u32 = std::uint32_t;
using u64 = std::uint64_t;

int g_level = 0;

struct Cnt {
    u64 shapes = 0, pixels = 0;
    void add(u64 px) { ++shapes, pixels += px; }
};

u64 mix(u64 h, u64 v) { return (h ^ (v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2))) * 0x100000001B3ull; }

// page size of a pixel format in pixels (the same table gpu.cpp's page_size() holds)
void page_dims(u32 psm, u32 &pw, u32 &ph) {
    switch (psm) {
    case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S: pw = 64; ph = 64; break;
    case PSMT8: pw = 128; ph = 64; break;
    case PSMT4: pw = 128; ph = 128; break;
    default: pw = 64; ph = 32; break;
    }
}

u32 bits_of(u32 psm) {
    switch (psm) {
    case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S: return 16;
    case PSMT8: case PSMT8H: return 8;
    case PSMT4: case PSMT4HL: case PSMT4HH: return 4;
    default: return 32;
    }
}

bool indexed(u32 psm) { return psm == PSMT8 || psm == PSMT4 || psm == PSMT8H || psm == PSMT4HL || psm == PSMT4HH; }

using Pages = std::bitset<512>;

// The 8 KB pages of memory a rectangle of a buffer touches (a buffer that starts inside a page touches the page after it too).
void pages_of(u32 bp, u32 bw, u32 psm, int x0, int y0, int x1, int y1, Pages &out) {
    u32 pw, ph;
    page_dims(psm, pw, ph);
    const u32 per_row = std::max<u32>(1, bw * 64 / pw);
    x0 = std::max(x0, 0), y0 = std::max(y0, 0);
    x1 = std::min(std::max(x1, x0), 4095), y1 = std::min(std::max(y1, y0), 4095);
    for (u32 py = static_cast<u32>(y0) / ph; py <= static_cast<u32>(y1) / ph; ++py)
        for (u32 px = static_cast<u32>(x0) / pw; px <= static_cast<u32>(x1) / pw; ++px) {
            const u64 at = static_cast<u64>(bp) * 256 + static_cast<u64>(px + py * per_row) * 8192;
            out.set((at >> 13) & 511u);
            if (bp % 32) out.set(((at + 8191) >> 13) & 511u);
        }
}

// --- who wrote each page last (counted here only) ---
enum Writer : u32 { kNone, kUpload, kDraw, kCopy, kState };
u64 g_stamp = 1;
u64 g_page_stamp[512] = {};
Writer g_page_writer[512] = {};

void bump(const Pages &p, Writer w) {
    ++g_stamp;
    for (u32 i = 0; i < 512; ++i)
        if (p.test(i)) g_page_stamp[i] = g_stamp, g_page_writer[i] = w;
}

// --- textures seen: one entry per (address, width, size, format) ---
struct Tex {
    Pages pages;
    u64 bytes = 0, stamp = 0, frame = ~0ull;
    bool indexed = false;
    int cls = 0;  // this frame: 0 unchanged, 1 first seen, 2 changed by an upload, 3 changed by a draw, 4 changed by a local copy
};
std::unordered_map<u64, Tex> g_tex;
u64 g_frame = 0;

// --- the draw under construction (a draw = a run of shapes with the same pipeline key, binding and no memory event in between) ---
struct Draw {
    bool open = false;
    u64 shapes = 0, pixels = 0;
    u32 groups = 0;
    bool z_uniform = true;
    u32 z0 = 0;
    bool partial_mask = false, wrap = false, date = false, tex_target = false, atest_fb = false, zwrite = false, ztst_always = false, ztst_gequal = false, ate = false;
    u32 afail = 0, ptype = 0;
} g_draw;

constexpr int kBuckets = 5;  // groups: 1, 2-8, 9-64, 65-512, more
const char *const kBucketName[kBuckets] = {"1", "2-8", "9-64", "65-512", ">512"};
int bucket(u32 groups) { return groups <= 1 ? 0 : groups <= 8 ? 1 : groups <= 64 ? 2 : groups <= 512 ? 3 : 4; }

enum Reason { kAny, kPartial, kWrap, kDate, kTexTarget, kAtestDep, kAll, kReasons };
const char *const kReasonName[kReasons] = {"any of the first four", "partial frame mask", "colour wrap-around (clamp off) with blending", "destination-alpha test",
                                           "texture is memory the draw writes", "alpha test, colour kept, depth dropped, shapes not independent", "all draws"};
struct GroupHist {
    u64 draws[kBuckets] = {}, shapes[kBuckets] = {}, pixels[kBuckets] = {}, groups_total = 0, draws_total = 0;
} g_groups[kReasons];

// overlap groups: a shape starts a new group when its box touches a box of the group (boxes in 4-pixel cells, so this is an upper bound)
constexpr int kCell = 4, kCells = 512;
std::vector<u32> g_cells(kCells * kCells, 0);
u32 g_gen = 0;
int g_last_box[4] = {-1, -1, -1, -1};
bool g_last_tri = false;

// --- per-frame record ---
struct Frame {
    u64 shapes = 0, pixels = 0;
    u32 draws_a = 0, draws_b = 0, draws_c = 0, draws_be = 0;
    u32 batches = 0;
    u32 uploads = 0, copies = 0, cluts = 0;
    u64 upload_bytes = 0;
    u32 tex_n = 0, hit_n = 0, new_n = 0, up_n = 0, draw_n = 0, copy_n = 0;
    u64 tex_bytes = 0, hit_bytes = 0, new_bytes = 0, up_bytes = 0, draw_bytes = 0, copy_bytes = 0;
    u32 tex_idx_n = 0;
    u64 tex_idx_bytes = 0, hit_idx_bytes = 0;
    u32 target_groups = 0, target_draws = 0;  // groups and draws that read memory they write (texture is memory the draw writes)
    u64 sampled[5] = {};  // texel bytes the shapes read (covered pixels times bytes per texel), by the class of the texture they read
} g_cur;
std::vector<Frame> g_frames;

u64 g_last_a = ~0ull, g_last_b = ~0ull, g_last_c = ~0ull;
bool g_event = false;
u32 g_clut_serial = 0;

// --- histograms (shapes and covered pixels, summed over the counted frames) ---
std::map<int, Cnt> g_blend;     // 0..80 equation, 100 = blending off, +128 colour clamp off
std::map<int, Cnt> g_atest;     // 0 off, else 1 + atst*4 + afail
std::map<int, Cnt> g_zmode;     // ztst + 4*zmsk + 8*zte
std::map<u64, Cnt> g_fbmask;    // fpsm<<32 | mask
std::map<int, Cnt> g_maskclass; // 0 none, 1 whole channels, 2 partial bytes/bit groups, 3 everything masked
std::map<int, Cnt> g_zfmt, g_fbfmt, g_texfmt, g_ptype, g_date, g_filter;
std::map<std::string, u64> g_why;
u64 g_batches_total = 0, g_batch_prims = 0;
u64 g_uploads = 0, g_upload_bytes = 0, g_copies = 0, g_cluts = 0;
Cnt g_all;
u64 g_shapes_ztie = 0;

const char *const kAtst[8] = {"NEVER", "ALWAYS", "LESS", "LEQUAL", "EQUAL", "GEQUAL", "GREATER", "NOTEQUAL"};
const char *const kAfail[4] = {"KEEP", "FB_ONLY", "ZB_ONLY", "RGB_ONLY"};
const char *const kZtst[4] = {"NEVER", "ALWAYS", "GEQUAL", "GREATER"};

std::string psm_name(int psm) {
    switch (psm) {
    case PSMCT32: return "CT32";
    case PSMCT24: return "CT24";
    case PSMCT16: return "CT16";
    case PSMCT16S: return "CT16S";
    case PSMT8: return "T8";
    case PSMT4: return "T4";
    case PSMT8H: return "T8H";
    case PSMT4HL: return "T4HL";
    case PSMT4HH: return "T4HH";
    case PSMZ32: return "Z32";
    case PSMZ24: return "Z24";
    case PSMZ16: return "Z16";
    case PSMZ16S: return "Z16S";
    default: return "psm" + std::to_string(psm);
    }
}

std::string blend_name(int k) {
    if (k == 100 || k == 228) return "blending off";
    const bool wrap = k >= 128;
    if (wrap) k -= 128;
    static const char *const cc[3] = {"Cs", "Cd", "0"};
    static const char *const cf[3] = {"As", "Ad", "FIX"};
    const int a = k / 27, b = k / 9 % 3, c = k / 3 % 3, d = k % 3;
    return std::string("(") + cc[a] + "-" + cc[b] + ")*" + cf[c] + "+" + cc[d] + (wrap ? " [clamp off]" : "");
}

std::string mask_class_name(int k) {
    static const char *const n[4] = {"mask 0 (everything written)", "whole channels masked", "partial channel masked", "everything masked"};
    return n[k];
}

// The state of one mask in the fields a format stores: 0 nothing masked, 1 whole channels, 2 partial, 3 all masked.
int mask_class(u32 fpsm, u32 mask) {
    u32 groups[4];
    u32 n = 0;
    if (fpsm == PSMCT16 || fpsm == PSMCT16S) {
        const u32 m = mask & 0xFFFFu;
        const u32 w[4] = {m & 31u, (m >> 5) & 31u, (m >> 10) & 31u, (m >> 15) & 1u};
        const u32 full[4] = {31u, 31u, 31u, 1u};
        for (u32 i = 0; i < 4; ++i) groups[n++] = w[i] == 0 ? 0 : (w[i] == full[i] ? 2 : 1);
    } else {
        const u32 bytes = fpsm == PSMCT24 || fpsm == PSMZ24 ? 3 : 4;
        for (u32 i = 0; i < bytes; ++i) {
            const u32 byte = (mask >> (8 * i)) & 0xFFu;
            groups[n++] = byte == 0 ? 0 : (byte == 0xFF ? 2 : 1);
        }
    }
    bool any_part = false, any_full = false, any_free = false;
    for (u32 i = 0; i < n; ++i) any_part |= groups[i] == 1, any_full |= groups[i] == 2, any_free |= groups[i] == 0;
    if (any_part) return 2;
    if (any_full && !any_free) return 3;
    return any_full ? 1 : 0;
}

void finish_draw() {
    Draw &d = g_draw;
    if (!d.open) return;
    d.open = false;
    ++g_cur.draws_be;
    const bool need = d.partial_mask || d.wrap || d.date || d.tex_target;
    const bool independent = !d.zwrite || d.ztst_always || (d.ztst_gequal && d.z_uniform) || d.groups <= 1;
    const bool atest_dep = d.ate && d.atest_fb && d.zwrite && !independent;
    const auto put = [&](Reason r) {
        GroupHist &h = g_groups[r];
        const int k = bucket(d.groups);
        ++h.draws[k];
        h.shapes[k] += d.shapes;
        h.pixels[k] += d.pixels;
        h.groups_total += d.groups;
        ++h.draws_total;
    };
    put(kAll);
    if (need) put(kAny);
    if (d.partial_mask) put(kPartial);
    if (d.wrap) put(kWrap);
    if (d.date) put(kDate);
    if (d.tex_target) put(kTexTarget), g_cur.target_groups += d.groups, ++g_cur.target_draws;
    if (atest_dep) put(kAtestDep);
    d = Draw{};
}

u32 shape_pixels(u32 prim, const Vertex *v, int minx, int miny, int maxx, int maxy) {
    const double box = static_cast<double>(maxx - minx + 1) * static_cast<double>(maxy - miny + 1);
    switch (prim) {
    case 3: case 4: case 5: {
        const double area = std::fabs(static_cast<double>(static_cast<std::int64_t>(v[1].x) - v[0].x) * (static_cast<double>(v[2].y) - v[0].y) -
                                      (static_cast<double>(v[2].x) - v[0].x) * (static_cast<double>(v[1].y) - v[0].y)) / 512.0;
        return static_cast<u32>(std::min(area, box));
    }
    case 1: case 2: return static_cast<u32>(std::max(maxx - minx, maxy - miny) + 1);
    case 0: return 1;
    default: return static_cast<u32>(box);
    }
}

}  // namespace

void init() {
    const char *e = std::getenv("PS2_GPU_STAT");
    g_level = e ? std::atoi(e) : 0;
    g_on = g_level > 0;
}

void shape(const State &st, std::uint64_t attr, const Vertex *v, int count, std::uint32_t, int minx, int miny, int maxx, int maxy, std::uint64_t tex_lo, std::uint64_t tex_hi) {
    (void)count;
    const u32 prim = static_cast<u32>(st.prim) & 7u;
    const Context &c = st.ctx[(attr >> 9) & 1u];
    const bool tme = (attr >> 4) & 1u, fge = (attr >> 5) & 1u, abe = (attr >> 6) & 1u, aa1 = (attr >> 7) & 1u, fst = (attr >> 8) & 1u, iip = (attr >> 3) & 1u;
    const u32 fbp = c.frame & 0x1FFu, fbw = (c.frame >> 16) & 0x3Fu, fpsm = (c.frame >> 24) & 0x3Fu;
    const u32 fbmsk = static_cast<u32>(c.frame >> 32);
    const u32 zbp = c.zbuf & 0x1FFu, zpsm = ((c.zbuf >> 24) & 0xFu) | 0x30u;
    const bool zmsk = (c.zbuf >> 32) & 1u;
    const bool ate = c.test & 1u;
    const u32 atst = (c.test >> 1) & 7u, aref = (c.test >> 4) & 0xFFu, afail = (c.test >> 12) & 3u;
    const bool date = (c.test >> 14) & 1u, datm = (c.test >> 15) & 1u, zte = (c.test >> 16) & 1u;
    const u32 ztst = (c.test >> 17) & 3u;
    const u32 ba = c.alpha & 3u, bb = (c.alpha >> 2) & 3u, bc = (c.alpha >> 4) & 3u, bd = (c.alpha >> 6) & 3u, fix = (c.alpha >> 32) & 0xFFu;
    const bool clamp_on = st.colclamp & 1u, pabe = st.pabe & 1u;
    const u32 tbp = c.tex0 & 0x3FFFu, tbw = (c.tex0 >> 14) & 0x3Fu, tpsm = (c.tex0 >> 20) & 0x3Fu, tw = (c.tex0 >> 26) & 0xFu, th = (c.tex0 >> 30) & 0xFu;
    const u32 tfx = (c.tex0 >> 35) & 3u, tcc = (c.tex0 >> 34) & 1u, cpsm = (c.tex0 >> 51) & 0xFu, csm = (c.tex0 >> 55) & 1u;
    const u32 mmag = (c.tex1 >> 5) & 1u, mmin = (c.tex1 >> 6) & 7u;
    const bool dither = (st.dthe & 1u) && (fpsm == PSMCT16 || fpsm == PSMCT16S);
    const bool zwrite = zte && !zmsk;
    const u32 ptype = prim == 0 ? 0 : prim <= 2 ? 1 : prim <= 5 ? 2 : prim == 6 ? 3 : 4;
    const u32 maskc = mask_class(fpsm, fbmsk);

    const u32 pixels = shape_pixels(prim, v, minx, miny, maxx, maxy);
    ++g_cur.shapes;
    g_cur.pixels += pixels;
    g_all.add(pixels);

    // ---- histograms ----
    const int bkey = !abe ? 100 : static_cast<int>(ba * 27 + bb * 9 + bc * 3 + bd) + (clamp_on ? 0 : 128);
    g_blend[bkey].add(pixels);
    g_atest[ate ? static_cast<int>(1 + atst * 4 + afail) : 0].add(pixels);
    g_zmode[static_cast<int>(ztst + (zmsk ? 4 : 0) + (zte ? 8 : 0))].add(pixels);
    g_fbmask[(static_cast<u64>(fpsm) << 32) | (fbmsk & (fpsm == PSMCT16 || fpsm == PSMCT16S ? 0xFFFFu : 0xFFFFFFFFu))].add(pixels);
    g_maskclass[static_cast<int>(maskc)].add(pixels);
    g_date[date ? (datm ? 2 : 1) : 0].add(pixels);
    g_fbfmt[static_cast<int>(fpsm)].add(pixels);
    if (zte) g_zfmt[static_cast<int>(zpsm)].add(pixels);
    if (zte) ++g_shapes_ztie;
    g_ptype[static_cast<int>(prim)].add(pixels);
    if (tme) {
        g_texfmt[static_cast<int>(tpsm)].add(pixels);
        g_filter[static_cast<int>(mmag)].add(pixels);
    }

    // ---- the three keys (cut when something that matters changes) ----
    u64 ha = 0xCBF29CE484222325ull;
    ha = mix(ha, ptype), ha = mix(ha, tme | fge << 1 | (abe ? 4 : 0) | aa1 << 3 | fst << 4 | iip << 5);
    ha = mix(ha, fbp | static_cast<u64>(fbw) << 9 | static_cast<u64>(fpsm) << 15), ha = mix(ha, zbp | static_cast<u64>(zpsm) << 9);
    ha = mix(ha, maskc), ha = mix(ha, zte | (zmsk ? 2u : 0u) | ztst << 2);
    ha = mix(ha, ate ? 1 + atst + 8 * afail : 0), ha = mix(ha, date ? 1 + datm : 0);
    ha = mix(ha, abe ? ba | bb << 2 | bc << 4 | bd << 6 | (pabe ? 256u : 0u) : 0xFFFF), ha = mix(ha, clamp_on | dither << 1 | static_cast<u32>(st.scanmsk & 3u) << 2 | static_cast<u32>(c.fba & 1u) << 4);
    if (tme) {
        ha = mix(ha, tpsm | tfx << 8 | tcc << 10 | mmag << 11 | mmin << 12 | static_cast<u64>((c.clamp & 15u)) << 16);
        if (indexed(tpsm)) ha = mix(ha, cpsm | csm << 4);
    }
    u64 hb = ha;
    if (tme) hb = mix(hb, tbp | static_cast<u64>(tbw) << 14 | static_cast<u64>(tw) << 20 | static_cast<u64>(th) << 24);
    u64 hc = hb;
    if (ate) hc = mix(hc, aref);
    if (abe && bc == 2) hc = mix(hc, fix);
    if (fge) hc = mix(hc, st.fogcol);
    hc = mix(hc, fbmsk), hc = mix(hc, c.xyoffset), hc = mix(hc, c.scissor);
    if (tme) hc = mix(hc, c.clamp), hc = mix(hc, st.texa), hc = mix(hc, g_clut_serial);
    if (dither) hc = mix(hc, st.dimx);
    if (ha != g_last_a) ++g_cur.draws_a;
    if (hb != g_last_b) ++g_cur.draws_b;
    if (hc != g_last_c) ++g_cur.draws_c;
    const bool new_draw = hb != g_last_b || g_event || !g_draw.open;
    g_last_a = ha, g_last_b = hb, g_last_c = hc;
    g_event = false;
    if (new_draw) {
        finish_draw();
        g_draw.open = true;
        ++g_gen;
        g_last_tri = false;
        g_draw.groups = 0;
        g_draw.partial_mask = maskc == 2;
        g_draw.wrap = abe && !clamp_on;
        g_draw.date = date && fpsm != PSMCT24;
        g_draw.atest_fb = afail == 1 || afail == 3;  // FB_ONLY and RGB_ONLY: the colour is written where the test fails, the depth is not
        g_draw.zwrite = zwrite;
        g_draw.ztst_always = ztst == 1;
        g_draw.ztst_gequal = ztst == 2;
        g_draw.ate = ate;
        g_draw.afail = afail;
        g_draw.ptype = ptype;
    }
    Draw &d = g_draw;
    ++d.shapes;
    d.pixels += pixels;
    const u32 z = prim == 6 ? v[1].z : v[0].z;
    if (d.shapes == 1) d.z0 = z;
    d.z_uniform = d.z_uniform && z == d.z0 && (prim < 3 || prim == 6 || (v[1].z == z && v[2].z == z));

    // ---- overlap groups ----
    {
        const int cx0 = std::clamp(minx / kCell, 0, kCells - 1), cx1 = std::clamp(maxx / kCell, 0, kCells - 1);
        const int cy0 = std::clamp(miny / kCell, 0, kCells - 1), cy1 = std::clamp(maxy / kCell, 0, kCells - 1);
        const bool tri = prim == 3;
        // two triangles that make one axis-aligned rectangle count as one shape
        const bool pair = tri && g_last_tri && g_last_box[0] == minx && g_last_box[1] == miny && g_last_box[2] == maxx && g_last_box[3] == maxy;
        bool touch = false;
        if (!pair && d.shapes > 1)
            for (int y = cy0; y <= cy1 && !touch; ++y)
                for (int x = cx0; x <= cx1; ++x)
                    if (g_cells[static_cast<std::size_t>(y * kCells + x)] == g_gen) { touch = true; break; }
        if (d.groups == 0 || touch) ++d.groups, ++g_gen;
        for (int y = cy0; y <= cy1; ++y)
            for (int x = cx0; x <= cx1; ++x) g_cells[static_cast<std::size_t>(y * kCells + x)] = g_gen;
        g_last_tri = tri && !pair;
        g_last_box[0] = minx, g_last_box[1] = miny, g_last_box[2] = maxx, g_last_box[3] = maxy;
    }

    // ---- pages written by this shape, and the texture it reads ----
    Pages written;
    pages_of(fbp * 32, fbw, fpsm, minx, miny, maxx, maxy, written);  // FRAME and ZBUF addresses count in 2048-word pages, texture and BITBLTBUF addresses in 64-word blocks
    if (zwrite) pages_of(zbp * 32, fbw, zpsm, minx, miny, maxx, maxy, written);
    if (tme && tex_hi > tex_lo) {
        const u64 key = tbp | static_cast<u64>(tbw) << 14 | static_cast<u64>(tpsm) << 20 | static_cast<u64>(tw) << 26 | static_cast<u64>(th) << 30 |
                        (indexed(tpsm) ? static_cast<u64>(cpsm) << 34 | static_cast<u64>(csm) << 38 : 0);
        Tex &t = g_tex[key];
        if (t.stamp == 0 && t.pages.none()) {
            pages_of(tbp, tbw, tpsm, 0, 0, (1 << tw) - 1, (1 << th) - 1, t.pages);
            t.bytes = (static_cast<u64>(1) << tw) * (static_cast<u64>(1) << th) * bits_of(tpsm) / 8;
            t.indexed = indexed(tpsm);
        }
        if (t.frame != g_frame) {
            u64 newest = 0;
            Writer who = kNone;
            for (u32 i = 0; i < 512; ++i)
                if (t.pages.test(i) && g_page_stamp[i] > newest) newest = g_page_stamp[i], who = g_page_writer[i];
            ++g_cur.tex_n;
            g_cur.tex_bytes += t.bytes;
            if (t.indexed) ++g_cur.tex_idx_n, g_cur.tex_idx_bytes += t.bytes;
            if (t.stamp == 0) ++g_cur.new_n, g_cur.new_bytes += t.bytes, t.cls = 1;
            else if (newest <= t.stamp) ++g_cur.hit_n, g_cur.hit_bytes += t.bytes, g_cur.hit_idx_bytes += t.indexed ? t.bytes : 0, t.cls = 0;
            else if (who == kDraw) ++g_cur.draw_n, g_cur.draw_bytes += t.bytes, t.cls = 3;
            else if (who == kCopy) ++g_cur.copy_n, g_cur.copy_bytes += t.bytes, t.cls = 4;
            else ++g_cur.up_n, g_cur.up_bytes += t.bytes, t.cls = 2;
            t.stamp = std::max<u64>(newest, 1);
            t.frame = g_frame;
        }
        if (!d.tex_target && (t.pages & written).any()) d.tex_target = true;
        g_cur.sampled[t.cls] += static_cast<u64>(pixels) * std::max<u32>(bits_of(tpsm), 8) / 8;
    }
    bump(written, kDraw);
}

void upload(std::uint32_t dbp, std::uint32_t dbw, std::uint32_t dpsm, int x, int y, std::uint32_t w, std::uint32_t h, std::uint64_t bytes) {
    Pages p;
    pages_of(dbp, dbw, dpsm, x, y, x + static_cast<int>(w) - 1, y + static_cast<int>(h) - 1, p);
    bump(p, kUpload);
    ++g_cur.uploads, g_cur.upload_bytes += bytes;
    ++g_uploads, g_upload_bytes += bytes;
    g_event = true;
}

void local_copy(std::uint32_t dbp, std::uint32_t dbw, std::uint32_t dpsm, int x, int y, std::uint32_t w, std::uint32_t h) {
    Pages p;
    pages_of(dbp, dbw, dpsm, x, y, x + static_cast<int>(w) - 1, y + static_cast<int>(h) - 1, p);
    bump(p, kCopy);
    ++g_cur.copies;
    ++g_copies;
    g_event = true;
}

void clut_load() {
    ++g_cur.cluts;
    ++g_cluts;
    ++g_clut_serial;
}

void batch(std::uint32_t prims, const char *why) {
    ++g_cur.batches;
    ++g_batches_total;
    g_batch_prims += prims;
    ++g_why[why];
}

void state_loaded() {
    ++g_stamp;
    for (u32 i = 0; i < 512; ++i) g_page_stamp[i] = g_stamp, g_page_writer[i] = kState;
    g_tex.clear();
}

void frame_end() {
    finish_draw();
    if (g_cur.shapes || g_cur.uploads || g_cur.cluts) {
        if (g_level >= 2)
            std::fprintf(stderr, "[stat] frame %llu: shapes %llu pixels %llu draws A %u B %u C %u B+event %u painter batches %u uploads %u (%llu bytes) copies %u palettes %u textures %u (hit %u new %u by-upload %u by-draw %u by-copy %u) self-reading draws %u with %u groups\n",
                         static_cast<unsigned long long>(g_frame), static_cast<unsigned long long>(g_cur.shapes), static_cast<unsigned long long>(g_cur.pixels), g_cur.draws_a, g_cur.draws_b, g_cur.draws_c, g_cur.draws_be,
                         g_cur.batches, g_cur.uploads, static_cast<unsigned long long>(g_cur.upload_bytes), g_cur.copies, g_cur.cluts, g_cur.tex_n, g_cur.hit_n, g_cur.new_n, g_cur.up_n, g_cur.draw_n, g_cur.copy_n, g_cur.target_draws, g_cur.target_groups);
        if (g_cur.shapes) g_frames.push_back(g_cur);
    }
    g_cur = Frame{};
    g_last_a = g_last_b = g_last_c = ~0ull;
    g_event = false;
    ++g_frame;
}

namespace {

double pct(double a, double b) { return b > 0 ? 100.0 * a / b : 0.0; }

template <class M, class Name>
void print_hist(const char *title, const M &m, double nframes, Name name, std::size_t top = 40) {
    u64 ts = 0, tp = 0;
    for (const auto &kv : m) ts += kv.second.shapes, tp += kv.second.pixels;
    std::vector<std::pair<u64, typename M::key_type>> order;
    for (const auto &kv : m) order.push_back({kv.second.pixels, kv.first});
    std::sort(order.begin(), order.end(), [](auto &a, auto &b) { return a.first > b.first; });
    std::fprintf(stderr, "[stat] %s (shapes/frame, share of shapes, Mpixels/frame, share of pixels)\n", title);
    std::size_t shown = 0;
    for (const auto &o : order) {
        if (shown++ >= top) break;
        const Cnt &c = m.at(o.second);
        std::fprintf(stderr, "[stat]   %-44s %9.1f %6.2f%% %9.3f %6.2f%%\n", name(o.second).c_str(), c.shapes / nframes, pct(c.shapes, ts), c.pixels / nframes / 1e6, pct(c.pixels, tp));
    }
}

}  // namespace

void report() {
    if (!g_on) return;
    frame_end();
    const double n = std::max<std::size_t>(g_frames.size(), 1);
    std::fprintf(stderr, "[stat] ==== PS2_GPU_STAT: %zu frames with shapes (of %llu vblanks) ====\n", g_frames.size(), static_cast<unsigned long long>(g_frame));
    std::fprintf(stderr, "[stat] per frame: %.0f shapes, %.3f Mpixels covered (before depth test; triangle area, sprite rectangle)\n", g_all.shapes / n, g_all.pixels / n / 1e6);
    const auto series = [&](const char *name, auto field) {
        std::vector<double> v;
        double sum = 0;
        for (const Frame &f : g_frames) v.push_back(static_cast<double>(field(f))), sum += v.back();
        std::sort(v.begin(), v.end());
        if (v.empty()) return;
        std::fprintf(stderr, "[stat] %-34s mean %9.1f  min %8.0f  median %8.0f  p95 %8.0f  max %8.0f\n", name, sum / v.size(), v.front(), v[v.size() / 2], v[static_cast<std::size_t>(v.size() * 0.95)], v.back());
    };
    std::fprintf(stderr, "[stat] draws per frame: A = pipeline key and target only, B = A + texture binding, C = B + constants (reference alpha, fixed alpha, fog colour, clamp, scissor, offset, palette load), B+event = B and also cut at every host upload and local copy\n");
    series("shapes", [](const Frame &f) { return f.shapes; });
    series("draws A (pipeline)", [](const Frame &f) { return f.draws_a; });
    series("draws B (pipeline + texture)", [](const Frame &f) { return f.draws_b; });
    series("draws C (+ constants)", [](const Frame &f) { return f.draws_c; });
    series("draws B + memory events", [](const Frame &f) { return f.draws_be; });
    series("painter batches today", [](const Frame &f) { return f.batches; });
    series("groups of self-reading draws", [](const Frame &f) { return f.target_groups; });
    series("host uploads", [](const Frame &f) { return f.uploads; });
    series("upload bytes", [](const Frame &f) { return f.upload_bytes; });
    series("local copies", [](const Frame &f) { return f.copies; });
    series("palette loads", [](const Frame &f) { return f.cluts; });
    std::fprintf(stderr, "[stat] painter batches ended by:");
    for (const auto &kv : g_why) std::fprintf(stderr, " %s %.1f/frame", kv.first.c_str(), kv.second / n);
    std::fprintf(stderr, "; %.1f shapes per batch\n", g_batches_total ? static_cast<double>(g_batch_prims) / g_batches_total : 0.0);

    print_hist("blend equations ((A-B)*C+D; Cs source colour, Cd destination colour, As source alpha, Ad destination alpha)", g_blend, n, [](int k) { return blend_name(k); });
    print_hist("alpha test mode", g_atest, n, [](int k) { return k == 0 ? std::string("off") : std::string(kAtst[(k - 1) / 4]) + ", fail: " + kAfail[(k - 1) % 4]; });
    print_hist("depth test (ZTE, ZMSK=depth write off)", g_zmode, n, [](int k) { return std::string(kZtst[k & 3]) + ((k & 4) ? ", no depth write" : ", depth write") + ((k & 8) ? "" : " [ZTE off]"); });
    print_hist("frame mask class", g_maskclass, n, [](int k) { return mask_class_name(k); });
    print_hist("frame masks (format, FBMSK value)", g_fbmask, n, [](u64 k) { char b[64]; std::snprintf(b, sizeof b, "%s 0x%08X", psm_name(static_cast<int>(k >> 32)).c_str(), static_cast<u32>(k)); return std::string(b); }, 14);
    print_hist("destination-alpha test (DATE)", g_date, n, [](int k) { return k == 0 ? std::string("off") : k == 1 ? "DATM 0" : "DATM 1"; });
    print_hist("depth buffer format, shapes with depth test or write enabled", g_zfmt, n, [](int k) { return psm_name(k); });
    print_hist("frame buffer format", g_fbfmt, n, [](int k) { return psm_name(k); });
    print_hist("texture format (textured shapes)", g_texfmt, n, [](int k) { return psm_name(k); });
    print_hist("texture filter (textured shapes, 0 nearest, 1 linear)", g_filter, n, [](int k) { return k ? std::string("linear") : std::string("nearest"); });
    print_hist("primitive type", g_ptype, n, [](int k) { static const char *const nm[8] = {"point", "line", "line strip", "triangle", "triangle strip", "triangle fan", "sprite", "?"}; return std::string(nm[k & 7]); });

    std::fprintf(stderr, "[stat] overlap groups per draw (draws = B + memory events; upper bound: cells of %d pixels; two triangles of one rectangle count as one shape)\n", kCell);
    for (int r = 0; r < kReasons; ++r) {
        const GroupHist &h = g_groups[r];
        if (!h.draws_total) continue;
        std::fprintf(stderr, "[stat]   %s: %.1f draws/frame, %.1f groups/frame; groups per draw:", kReasonName[r], h.draws_total / n, h.groups_total / n);
        for (int b = 0; b < kBuckets; ++b)
            std::fprintf(stderr, " [%s] %.1f draws %.0f shapes %.3f Mpx;", kBucketName[b], h.draws[b] / n, h.shapes[b] / n, h.pixels[b] / n / 1e6);
        std::fprintf(stderr, "\n");
    }

    double tex_n = 0, hit_n = 0, new_n = 0, up_n = 0, dr_n = 0, cp_n = 0, tex_b = 0, hit_b = 0, new_b = 0, up_b = 0, dr_b = 0, cp_b = 0, idx_b = 0, hit_idx_b = 0;
    for (const Frame &f : g_frames) {
        tex_n += f.tex_n, hit_n += f.hit_n, new_n += f.new_n, up_n += f.up_n, dr_n += f.draw_n, cp_n += f.copy_n;
        tex_b += f.tex_bytes, hit_b += f.hit_bytes, new_b += f.new_bytes, up_b += f.up_bytes, dr_b += f.draw_bytes, cp_b += f.copy_bytes, idx_b += f.tex_idx_bytes, hit_idx_b += f.hit_idx_bytes;
    }
    std::fprintf(stderr, "[stat] textures per frame (distinct by address, width, size, format; bytes = declared size; a texture is 'unchanged' when no page it covers was written since its last use, an infinite cache):\n");
    std::fprintf(stderr, "[stat]   distinct %.1f, %.1f MB;  unchanged %.1f (%.1f MB);  first seen %.1f (%.1f MB);  changed by host upload %.1f (%.1f MB);  by a draw (drawn picture used as texture) %.1f (%.1f MB);  by local copy %.1f (%.1f MB)\n",
                 tex_n / n, tex_b / n / 1e6, hit_n / n, hit_b / n / 1e6, new_n / n, new_b / n / 1e6, up_n / n, up_b / n / 1e6, dr_n / n, dr_b / n / 1e6, cp_n / n, cp_b / n / 1e6);
    std::fprintf(stderr, "[stat]   hit rate by bytes %.1f%%, by count %.1f%%; palette-index textures are %.1f%% of the bytes, hit rate of those %.1f%%\n", pct(hit_b, tex_b), pct(hit_n, tex_n), pct(idx_b, tex_b), pct(hit_idx_b, idx_b));
    double smp[5] = {};
    for (const Frame &f : g_frames)
        for (int i = 0; i < 5; ++i) smp[i] += static_cast<double>(f.sampled[i]);
    const double smp_all = smp[0] + smp[1] + smp[2] + smp[3] + smp[4];
    std::fprintf(stderr, "[stat]   texel bytes read by the shapes per frame (covered pixels x bytes per texel): %.1f MB, from textures unchanged since their last use %.1f%%, first seen %.1f%%, changed by host upload %.1f%%, by a draw %.1f%%, by local copy %.1f%%\n",
                 smp_all / n / 1e6, pct(smp[0], smp_all), pct(smp[1], smp_all), pct(smp[2], smp_all), pct(smp[3], smp_all), pct(smp[4], smp_all));
    std::fprintf(stderr, "[stat]   without the pictures the game drew itself: hit rate by distinct bytes %.1f%%, by texel bytes read %.1f%%\n", pct(hit_b, tex_b - dr_b), pct(smp[0], smp_all - smp[3]));
    u64 ever = 0;
    for (const auto &kv : g_tex) ever += kv.second.bytes;
    std::fprintf(stderr, "[stat]   %zu distinct textures seen in the whole run, %.1f MB if none were ever dropped\n", g_tex.size(), ever / 1e6);
    std::fprintf(stderr, "[stat] total over the run: %llu host uploads (%.1f MB), %llu local copies, %llu palette loads\n", static_cast<unsigned long long>(g_uploads), g_upload_bytes / 1e6, static_cast<unsigned long long>(g_copies), static_cast<unsigned long long>(g_cluts));
}

}
