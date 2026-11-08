#include "raster.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ps2::gs::raster {

namespace {

struct Setup {
    bool iip, tme, fge, abe, fst;
    std::uint32_t fbp, fbw, fpsm, fbmsk;
    std::uint32_t zbp, zpsm;
    bool zmsk;
    bool ate, date, datm, zte;
    int atst, afail, ztst;
    std::uint32_t aref;
    int a, b, c, d;
    std::uint32_t fix;
    bool pabe, colclamp, fba;
    int scax0, scax1, scay0, scay1;
    int ofx, ofy;
    std::uint32_t tbp, tbw, tpsm, tw, th, cpsm, csa;
    bool tcc, bilinear;
    int tfx, wms, wmt;
    // Mipmapping (TEX1, MIPTBP1/2; docs/mipmap.md). `mip` is false when the level and the filter cannot depend on the pixel (one level
    // and the same filter for magnification and reduction): then the texture is sampled as before, level 0 with `bilinear`.
    bool mip;
    int lcm, mxl, mmag, mmin, lodl;
    float lodk;
    std::uint32_t lbp[7], lbw[7];
    std::uint32_t minu, maxu, minv, maxv;
    std::uint32_t ta0, ta1;
    bool aem;
    std::uint32_t fogcol;
    int scanmsk;
    const std::uint32_t *clut;
};

struct Attr {
    float r, g, b, a, s, t, q, f;
    double z;
};

int clamp255(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

std::uint32_t expand16(std::uint32_t c, const Setup &s) {
    const std::uint32_t r = (c & 0x1Fu) << 3, g = ((c >> 5) & 0x1Fu) << 3, b = ((c >> 10) & 0x1Fu) << 3;
    std::uint32_t a;
    if (c & 0x8000u) a = s.ta1;
    else a = (s.aem && !(c & 0x7FFFu)) ? 0u : s.ta0;
    return r | (g << 8) | (b << 16) | (a << 24);
}

std::uint32_t fetch(const Setup &s, int x, int y) {
    const auto wrap = [](int v, int size, int mode, std::uint32_t min, std::uint32_t max) {
        switch (mode) {
        case 0: return v & (size - 1);
        case 1: return std::clamp(v, 0, size - 1);
        case 2: return std::clamp(v, static_cast<int>(min), static_cast<int>(max));
        default: return static_cast<int>((static_cast<std::uint32_t>(v) & min) | max);
        }
    };
    x = wrap(x, 1 << s.tw, s.wms, s.minu, s.maxu);
    y = wrap(y, 1 << s.th, s.wmt, s.minv, s.maxv);
    const std::uint32_t raw = read_pixel(s.tpsm, s.tbp, s.tbw, static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y));
    switch (s.tpsm) {
    case PSMCT32: case PSMZ32: return raw;
    case PSMCT24: case PSMZ24: return (raw & 0xFFFFFFu) | (((s.aem && !(raw & 0xFFFFFFu)) ? 0u : s.ta0) << 24);
    case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S: return expand16(raw, s);
    case PSMT8: case PSMT8H: {
        const std::uint32_t e = s.clut[raw & 255u];
        return s.cpsm == PSMCT32 || s.cpsm == PSMCT24 ? e : expand16(e, s);
    }
    case PSMT4: case PSMT4HL: case PSMT4HH: {
        const std::uint32_t e = s.clut[((s.csa & 15u) * 16u + (raw & 15u)) & 511u];
        return s.cpsm == PSMCT32 || s.cpsm == PSMCT24 ? e : expand16(e, s);
    }
    default: return raw;
    }
}

std::uint32_t sample_with(const Setup &s, float u, float v, bool bilinear);

std::uint32_t sample(const Setup &s, float u, float v) {
    return sample_with(s, u, v, s.bilinear);
}

// The texture as mip level `n` sees it: its base pointer and buffer width (from MIPTBP1/2 or worked out, make_setup), its size (TW and TH
// shifted by the level, at least one texel) and the region of the clamp modes 2 and 3 shifted the same way.
Setup level_setup(const Setup &s, int n) {
    Setup l = s;
    l.tbp = s.lbp[n];
    l.tbw = s.lbw[n];
    l.tw = s.tw > static_cast<std::uint32_t>(n) ? s.tw - static_cast<std::uint32_t>(n) : 0u;
    l.th = s.th > static_cast<std::uint32_t>(n) ? s.th - static_cast<std::uint32_t>(n) : 0u;
    l.minu = s.minu >> n;
    l.maxu = s.maxu >> n;
    l.minv = s.minv >> n;
    l.maxv = s.maxv >> n;
    return l;
}

// The level of detail and what it picks (GS User's Manual, TEX1): LOD = K with LCM = 1, else (log2(1/|Q|) << L) + K, K a signed 7.4 number.
// LOD <= 0 magnifies (filter MMAG, level 0); LOD > 0 reduces (filter MMIN): see docs/mipmap.md for why 0 counts as magnifying. The *_MIPMAP_NEAREST modes take the level nearest to LOD,
// the *_MIPMAP_LINEAR modes blend the two levels around it by its fraction, never past MXL. NEAREST_* and LINEAR_* name the filter inside a level.
struct Pick {
    int level;
    float frac;  // weight of level + 1 (0: one level)
    bool linear;
};

Pick pick_level(const Setup &s, float q) {
    const float lod = s.lcm ? s.lodk : std::log2(1.0f / std::fabs(q)) * static_cast<float>(1 << s.lodl) + s.lodk;
    if (!(lod > 0.0f)) return {0, 0.0f, s.mmag != 0};
    if (s.mmin < 2) return {0, 0.0f, (s.mmin & 1) != 0};
    const bool linear = s.mmin >= 4;
    if (s.mxl == 0) return {0, 0.0f, linear};
    if (!(s.mmin & 1)) return {static_cast<int>(std::min(std::floor(lod + 0.5f), static_cast<float>(s.mxl))), 0.0f, linear};
    const float whole = std::floor(lod);
    if (whole >= static_cast<float>(s.mxl)) return {s.mxl, 0.0f, linear};
    return {static_cast<int>(whole), lod - whole, linear};
}

std::uint32_t blend_levels(std::uint32_t a, std::uint32_t b, float f) {
    std::uint32_t out = 0;
    for (int ch = 0; ch < 32; ch += 8) {
        const float x = static_cast<float>((a >> ch) & 0xFFu), y = static_cast<float>((b >> ch) & 0xFFu);
        out |= static_cast<std::uint32_t>(std::clamp(x + (y - x) * f + 0.5f, 0.0f, 255.0f)) << ch;
    }
    return out;
}

// u, v: texel coordinates of level 0; nu, nv: the normalized coordinates S/Q and T/Q (STQ) that the levels scale by their own size.
std::uint32_t sample_mip(const Setup &s, float u, float v, float nu, float nv, float q) {
    const Pick p = pick_level(s, q);
    const auto at = [&](int n) {
        if (n == 0) return sample_with(s, u, v, p.linear);
        const Setup l = level_setup(s, n);
        if (s.fst) return sample_with(l, u / static_cast<float>(1 << n), v / static_cast<float>(1 << n), p.linear);
        return sample_with(l, nu * static_cast<float>(1 << l.tw), nv * static_cast<float>(1 << l.th), p.linear);
    };
    const std::uint32_t c = at(p.level);
    return p.frac > 0.0f ? blend_levels(c, at(p.level + 1), p.frac) : c;
}

std::uint32_t sample_with(const Setup &s, float u, float v, bool bilinear) {
    if (!bilinear) return fetch(s, static_cast<int>(std::floor(u)), static_cast<int>(std::floor(v)));
    u -= 0.5f;
    v -= 0.5f;
    const int x0 = static_cast<int>(std::floor(u)), y0 = static_cast<int>(std::floor(v));
    const float fx = u - static_cast<float>(x0), fy = v - static_cast<float>(y0);
    const std::uint32_t c00 = fetch(s, x0, y0), c10 = fetch(s, x0 + 1, y0), c01 = fetch(s, x0, y0 + 1), c11 = fetch(s, x0 + 1, y0 + 1);
    std::uint32_t out = 0;
    for (int ch = 0; ch < 32; ch += 8) {
        const float a = static_cast<float>((c00 >> ch) & 0xFFu), b = static_cast<float>((c10 >> ch) & 0xFFu);
        const float c = static_cast<float>((c01 >> ch) & 0xFFu), d = static_cast<float>((c11 >> ch) & 0xFFu);
        const float top = a + (b - a) * fx, bottom = c + (d - c) * fx;
        out |= static_cast<std::uint32_t>(std::clamp(top + (bottom - top) * fy + 0.5f, 0.0f, 255.0f)) << ch;
    }
    return out;
}

std::uint32_t read_frame(const Setup &s, int x, int y) {
    const std::uint32_t raw = read_pixel(s.fpsm, s.fbp, s.fbw, static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y));
    switch (s.fpsm) {
    case PSMCT24: return raw | 0x80000000u;
    case PSMCT16: case PSMCT16S:
        return ((raw & 0x1Fu) << 3) | (((raw >> 5) & 0x1Fu) << 11) | (((raw >> 10) & 0x1Fu) << 19) | ((raw & 0x8000u) ? 0x80000000u : 0u);
    default: return raw;
    }
}

void write_frame(const Setup &s, int x, int y, std::uint32_t c, std::uint32_t mask) {
    switch (s.fpsm) {
    case PSMCT16: case PSMCT16S: {
        const std::uint32_t old = read_pixel(s.fpsm, s.fbp, s.fbw, static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y));
        const auto pack = [](std::uint32_t v) {
            return ((v >> 3) & 0x1Fu) | (((v >> 11) & 0x1Fu) << 5) | (((v >> 19) & 0x1Fu) << 10) | ((v & 0x80000000u) ? 0x8000u : 0u);
        };
        const std::uint32_t m16 = pack(mask) | ((mask & 0x80000000u) ? 0x8000u : 0u);
        write_pixel(s.fpsm, s.fbp, s.fbw, static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y), (pack(c) & ~m16) | (old & m16));
        break;
    }
    case PSMCT24: {
        const std::uint32_t old = read_pixel(PSMCT32, s.fbp, s.fbw, static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y));
        const std::uint32_t m = mask | 0xFF000000u;
        write_pixel(PSMCT32, s.fbp, s.fbw, static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y), (c & ~m) | (old & m));
        break;
    }
    default: {
        if (mask) {
            const std::uint32_t old = read_pixel(s.fpsm, s.fbp, s.fbw, static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y));
            c = (c & ~mask) | (old & mask);
        }
        write_pixel(s.fpsm, s.fbp, s.fbw, static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y), c);
        break;
    }
    }
}

std::uint32_t z_max(std::uint32_t psm) {
    switch (psm) {
    case PSMZ24: return 0xFFFFFFu;
    case PSMZ16: case PSMZ16S: return 0xFFFFu;
    default: return 0xFFFFFFFFu;
    }
}

// Returns what became of the pixel, for the pixel history.
const char *shade(const Setup &s, int x, int y, const Attr &at) {
    if (x < s.scax0 || x > s.scax1 || y < s.scay0 || y > s.scay1) return "outside the scissor";
    if (s.scanmsk == 2 && !(y & 1)) return "scanmsk";
    if (s.scanmsk == 3 && (y & 1)) return "scanmsk";

    const std::uint32_t zmax = z_max(s.zpsm);
    const double zf = std::clamp(at.z, 0.0, static_cast<double>(zmax));
    const std::uint32_t z = static_cast<std::uint32_t>(zf);

    int r = static_cast<int>(at.r), g = static_cast<int>(at.g), b = static_cast<int>(at.b), a = static_cast<int>(at.a);
    if (s.tme) {
        float u, v, nu = 0.0f, nv = 0.0f, q = 1.0f;
        if (s.fst) {
            u = at.s / 16.0f;
            v = at.t / 16.0f;
        } else {
            q = at.q != 0.0f ? at.q : 1.0f;
            nu = at.s / q;
            nv = at.t / q;
            u = nu * static_cast<float>(1 << s.tw);
            v = nv * static_cast<float>(1 << s.th);
        }
        const std::uint32_t t = s.mip ? sample_mip(s, u, v, nu, nv, q) : sample(s, u, v);
        const int tr = static_cast<int>(t & 0xFFu), tg = static_cast<int>((t >> 8) & 0xFFu), tb = static_cast<int>((t >> 16) & 0xFFu), ta = static_cast<int>(t >> 24);
        switch (s.tfx) {
        case 0:
            r = clamp255((tr * r) >> 7);
            g = clamp255((tg * g) >> 7);
            b = clamp255((tb * b) >> 7);
            a = s.tcc ? clamp255((ta * a) >> 7) : a;
            break;
        case 1:
            r = tr; g = tg; b = tb;
            a = s.tcc ? ta : a;
            break;
        case 2:
            r = clamp255(((tr * r) >> 7) + a);
            g = clamp255(((tg * g) >> 7) + a);
            b = clamp255(((tb * b) >> 7) + a);
            a = s.tcc ? clamp255(ta + a) : a;
            break;
        default:
            r = clamp255(((tr * r) >> 7) + a);
            g = clamp255(((tg * g) >> 7) + a);
            b = clamp255(((tb * b) >> 7) + a);
            a = s.tcc ? ta : a;
            break;
        }
    }
    if (s.fge) {
        const int f = static_cast<int>(at.f);
        const int fr = static_cast<int>(s.fogcol & 0xFFu), fg = static_cast<int>((s.fogcol >> 8) & 0xFFu), fb = static_cast<int>((s.fogcol >> 16) & 0xFFu);
        r = (f * r + (255 - f) * fr) >> 8;
        g = (f * g + (255 - f) * fg) >> 8;
        b = (f * b + (255 - f) * fb) >> 8;
    }

    bool write_fb = true, write_z = !s.zmsk, write_alpha = true;
    if (s.ate) {
        bool pass;
        const std::uint32_t av = static_cast<std::uint32_t>(a);
        switch (s.atst) {
        case 0: pass = false; break;
        case 1: pass = true; break;
        case 2: pass = av < s.aref; break;
        case 3: pass = av <= s.aref; break;
        case 4: pass = av == s.aref; break;
        case 5: pass = av >= s.aref; break;
        case 6: pass = av > s.aref; break;
        default: pass = av != s.aref; break;
        }
        if (!pass) {
            switch (s.afail) {
            case 0: return "alpha test failed, nothing written";
            case 1: write_z = false; break;
            case 2: write_fb = false; break;
            default: write_z = false; write_alpha = false; break;
            }
        }
    }

    std::uint32_t dst = 0;
    bool have_dst = false;
    if (s.date) {
        dst = read_frame(s, x, y);
        have_dst = true;
        const bool bit = (s.fpsm == PSMCT16 || s.fpsm == PSMCT16S) ? (dst & 0x80000000u) != 0 : (dst & 0x80000000u) != 0;
        if (bit != s.datm) return "destination alpha test failed";
    }

    if (s.zte) {
        const std::uint32_t old = read_pixel(s.zpsm, s.zbp, s.fbw, static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y)) & zmax;
        bool pass;
        switch (s.ztst) {
        case 0: pass = false; break;
        case 1: pass = true; break;
        case 2: pass = z >= old; break;
        default: pass = z > old; break;
        }
        if (!pass) return "depth test failed";
    }

    if (write_fb) {
        if (s.abe && (!s.pabe || (a & 0x80))) {
            if (!have_dst) dst = read_frame(s, x, y);
            const int cd[3] = {static_cast<int>(dst & 0xFFu), static_cast<int>((dst >> 8) & 0xFFu), static_cast<int>((dst >> 16) & 0xFFu)};
            const int cs[3] = {r, g, b};
            const int ad = static_cast<int>(dst >> 24);
            const int cc = s.c == 0 ? a : s.c == 1 ? ad : static_cast<int>(s.fix);
            int out[3];
            for (int i = 0; i < 3; ++i) {
                const int av = s.a == 0 ? cs[i] : s.a == 1 ? cd[i] : 0;
                const int bv = s.b == 0 ? cs[i] : s.b == 1 ? cd[i] : 0;
                const int dv = s.d == 0 ? cs[i] : s.d == 1 ? cd[i] : 0;
                const int v = (((av - bv) * cc) >> 7) + dv;
                out[i] = s.colclamp ? clamp255(v) : (v & 0xFF);
            }
            r = out[0];
            g = out[1];
            b = out[2];
        } else if (!s.colclamp) {
            r &= 0xFF;
            g &= 0xFF;
            b &= 0xFF;
        }
        std::uint32_t out_a = static_cast<std::uint32_t>(a & 0xFF);
        if (s.fba) out_a |= 0x80u;
        std::uint32_t mask = s.fbmsk;
        if (!write_alpha) mask |= 0xFF000000u;
        write_frame(s, x, y, static_cast<std::uint32_t>(r) | (static_cast<std::uint32_t>(g) << 8) | (static_cast<std::uint32_t>(b) << 16) | (out_a << 24), mask);
    }
    if (write_z && s.zte) {
        if (s.zpsm == PSMZ24) {
            write_pixel(PSMZ24, s.zbp, s.fbw, static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y), z);
        } else {
            write_pixel(s.zpsm, s.zbp, s.fbw, static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y), z);
        }
    }
    return write_fb ? "drawn" : "alpha test failed, depth only";
}

void plot(const Setup &s, int x, int y, const Attr &at) {
    if (g_probe.on) [[unlikely]] {
        static const char *spec = std::getenv("PS2_TRACE_PIXEL");
        static const int px = spec ? std::atoi(spec) : -1, py = spec && std::strchr(spec, ',') ? std::atoi(std::strchr(spec, ',') + 1) : -1;
        if (x == px && y == py) {
            const std::uint32_t before = read_pixel(s.fpsm, s.fbp, s.fbw, static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y));
            const std::uint32_t zbefore = read_pixel(s.zpsm, s.zbp, s.fbw, static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y)) & z_max(s.zpsm);
            const char *what = shade(s, x, y, at);
            const std::uint32_t after = read_pixel(s.fpsm, s.fbp, s.fbw, static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y));
            std::fprintf(stderr, "[gs-pixel] f%llu #%llu %d,%d fbp %X psm %02X: %s | pixel %08X -> %08X | z %.1f, buffer %X | rgba %.3f,%.3f,%.3f,%.3f stq %g,%g,%g\n",
                         static_cast<unsigned long long>(g_probe.frame), static_cast<unsigned long long>(g_probe.number), x, y, s.fbp, s.fpsm, what, before, after, at.z,
                         zbefore, at.r, at.g, at.b, at.a, at.s, at.t, at.q);
            return;
        }
    }
    shade(s, x, y, at);
}

Setup make_setup(const State &st) {
    Setup s{};
    std::uint64_t attr = st.prim;
    if (!(st.prmodecont & 1u)) attr = (st.prim & 7u) | (st.prmode & ~7ull);
    s.iip = attr & 0x8u;
    s.tme = attr & 0x10u;
    s.fge = attr & 0x20u;
    s.abe = attr & 0x40u;
    s.fst = attr & 0x100u;
    const Context &c = st.ctx[(attr >> 9) & 1u];
    s.fbp = (static_cast<std::uint32_t>(c.frame) & 0x1FFu) * 32u;
    s.fbw = static_cast<std::uint32_t>(c.frame >> 16) & 0x3Fu;
    s.fpsm = static_cast<std::uint32_t>(c.frame >> 24) & 0x3Fu;
    s.fbmsk = static_cast<std::uint32_t>(c.frame >> 32);
    s.zbp = (static_cast<std::uint32_t>(c.zbuf) & 0x1FFu) * 32u;
    s.zpsm = 0x30u | (static_cast<std::uint32_t>(c.zbuf >> 24) & 0xFu);
    s.zmsk = (c.zbuf >> 32) & 1u;
    s.ate = c.test & 1u;
    s.atst = static_cast<int>(c.test >> 1) & 7;
    s.aref = static_cast<std::uint32_t>(c.test >> 4) & 0xFFu;
    s.afail = static_cast<int>(c.test >> 12) & 3;
    s.date = (c.test >> 14) & 1u;
    s.datm = (c.test >> 15) & 1u;
    s.zte = (c.test >> 16) & 1u;
    s.ztst = static_cast<int>(c.test >> 17) & 3;
    s.a = static_cast<int>(c.alpha) & 3;
    s.b = static_cast<int>(c.alpha >> 2) & 3;
    s.c = static_cast<int>(c.alpha >> 4) & 3;
    s.d = static_cast<int>(c.alpha >> 6) & 3;
    s.fix = static_cast<std::uint32_t>(c.alpha >> 32) & 0xFFu;
    s.pabe = st.pabe & 1u;
    s.colclamp = st.colclamp & 1u;
    s.fba = c.fba & 1u;
    s.scax0 = static_cast<int>(c.scissor & 0x7FFu);
    s.scax1 = static_cast<int>((c.scissor >> 16) & 0x7FFu);
    s.scay0 = static_cast<int>((c.scissor >> 32) & 0x7FFu);
    s.scay1 = static_cast<int>((c.scissor >> 48) & 0x7FFu);
    s.ofx = static_cast<int>(c.xyoffset & 0xFFFFu);
    s.ofy = static_cast<int>((c.xyoffset >> 32) & 0xFFFFu);
    s.tbp = static_cast<std::uint32_t>(c.tex0) & 0x3FFFu;
    s.tbw = static_cast<std::uint32_t>(c.tex0 >> 14) & 0x3Fu;
    s.tpsm = static_cast<std::uint32_t>(c.tex0 >> 20) & 0x3Fu;
    s.tw = std::min<std::uint32_t>(static_cast<std::uint32_t>(c.tex0 >> 26) & 0xFu, 10u);
    s.th = std::min<std::uint32_t>(static_cast<std::uint32_t>(c.tex0 >> 30) & 0xFu, 10u);
    s.tcc = (c.tex0 >> 34) & 1u;
    s.tfx = static_cast<int>(c.tex0 >> 35) & 3;
    s.cpsm = static_cast<std::uint32_t>(c.tex0 >> 51) & 0xFu;
    s.csa = static_cast<std::uint32_t>(c.tex0 >> 56) & 0x1Fu;
    s.bilinear = (c.tex1 >> 5) & 1u;
    s.lcm = static_cast<int>(c.tex1 & 1u);
    s.mxl = std::min(static_cast<int>(c.tex1 >> 2) & 7, 6);
    s.mmag = static_cast<int>(c.tex1 >> 5) & 1;
    s.mmin = static_cast<int>(c.tex1 >> 6) & 7;
    s.lodl = static_cast<int>(c.tex1 >> 19) & 3;
    s.lodk = static_cast<float>(static_cast<std::int32_t>(static_cast<std::uint32_t>(c.tex1 >> 32) << 20) >> 20) / 16.0f;
    const bool min_linear = s.mmin == 1 || s.mmin >= 4;
    static const bool off = std::getenv("PS2_MIPMAP") && std::getenv("PS2_MIPMAP")[0] == '0';  // as in gpu.cpp make_mip
    s.mip = !off && (s.mmag != static_cast<int>(min_linear) || (s.mxl > 0 && s.mmin >= 2));
    mip_levels(c, s.lbp, s.lbw);
    s.wms = static_cast<int>(c.clamp) & 3;
    s.wmt = static_cast<int>(c.clamp >> 2) & 3;
    s.minu = static_cast<std::uint32_t>(c.clamp >> 4) & 0x3FFu;
    s.maxu = static_cast<std::uint32_t>(c.clamp >> 14) & 0x3FFu;
    s.minv = static_cast<std::uint32_t>(c.clamp >> 24) & 0x3FFu;
    s.maxv = static_cast<std::uint32_t>(c.clamp >> 34) & 0x3FFu;
    s.ta0 = static_cast<std::uint32_t>(st.texa) & 0xFFu;
    s.aem = (st.texa >> 15) & 1u;
    s.ta1 = static_cast<std::uint32_t>(st.texa >> 32) & 0xFFu;
    s.fogcol = static_cast<std::uint32_t>(st.fogcol) & 0xFFFFFFu;
    s.scanmsk = static_cast<int>(st.scanmsk) & 3;
    s.clut = st.clut;
    return s;
}

Attr attr_of(const Setup &s, const Vertex &v) {
    Attr a;
    a.r = v.r;
    a.g = v.g;
    a.b = v.b;
    a.a = v.a;
    if (s.fst) {
        a.s = static_cast<float>(v.u);
        a.t = static_cast<float>(v.v);
        a.q = 1.0f;
    } else {
        a.s = v.s;
        a.t = v.t;
        a.q = v.q;
    }
    a.f = v.f;
    a.z = static_cast<double>(v.z);
    return a;
}

// Colour, fog and depth are the first vertex's value plus the weighted differences to the others, so a value that is the
// same at all three vertices (flat shading, one colour, no fog) comes out exactly, as on the GS. Summing a*wa + b*wb + c*wc
// instead gives 254.99 for 255 now and then, which truncates to 254: God of War II counts its shadow volumes with +1 and -1
// (0x01 and 0xFF) colours added into the green channel, and those slips left dots in every shadow.
Attr lerp3(const Attr &a, const Attr &b, const Attr &c, float wa, float wb, float wc) {
    Attr r;
    r.r = a.r + (b.r - a.r) * wb + (c.r - a.r) * wc;
    r.g = a.g + (b.g - a.g) * wb + (c.g - a.g) * wc;
    r.b = a.b + (b.b - a.b) * wb + (c.b - a.b) * wc;
    r.a = a.a + (b.a - a.a) * wb + (c.a - a.a) * wc;
    r.s = a.s * wa + b.s * wb + c.s * wc;
    r.t = a.t * wa + b.t * wb + c.t * wc;
    r.q = a.q * wa + b.q * wb + c.q * wc;
    r.f = a.f + (b.f - a.f) * wb + (c.f - a.f) * wc;
    r.z = a.z + (b.z - a.z) * wb + (c.z - a.z) * wc;
    return r;
}

void triangle(const Setup &s, const Vertex *v) {
    const float x0 = (static_cast<float>(v[0].x) - static_cast<float>(s.ofx)) / 16.0f, y0 = (static_cast<float>(v[0].y) - static_cast<float>(s.ofy)) / 16.0f;
    const float x1 = (static_cast<float>(v[1].x) - static_cast<float>(s.ofx)) / 16.0f, y1 = (static_cast<float>(v[1].y) - static_cast<float>(s.ofy)) / 16.0f;
    const float x2 = (static_cast<float>(v[2].x) - static_cast<float>(s.ofx)) / 16.0f, y2 = (static_cast<float>(v[2].y) - static_cast<float>(s.ofy)) / 16.0f;
    const float area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
    if (area == 0.0f) return;
    Attr a0 = attr_of(s, v[0]), a1 = attr_of(s, v[1]), a2 = attr_of(s, v[2]);
    if (!s.iip) {
        a0.r = a1.r = a2.r;
        a0.g = a1.g = a2.g;
        a0.b = a1.b = a2.b;
        a0.a = a1.a = a2.a;
    }
    const int minx = std::max(s.scax0, static_cast<int>(std::ceil(std::min({x0, x1, x2}))));
    const int maxx = std::min(s.scax1, static_cast<int>(std::ceil(std::max({x0, x1, x2}))) - 1);
    const int miny = std::max(s.scay0, static_cast<int>(std::ceil(std::min({y0, y1, y2}))));
    const int maxy = std::min(s.scay1, static_cast<int>(std::ceil(std::max({y0, y1, y2}))) - 1);
    if (minx > maxx || miny > maxy) return;
    const float inv = 1.0f / area;
    const float sg = area > 0.0f ? 1.0f : -1.0f;
    const auto edge_in = [](float e, float gx, float gy) { return e > 0.0f || (e == 0.0f && (gx > 0.0f || (gx == 0.0f && gy > 0.0f))); };
    for (int y = miny; y <= maxy; ++y) {
        const float py = static_cast<float>(y);
        for (int x = minx; x <= maxx; ++x) {
            const float px = static_cast<float>(x);
            const float e0 = (x1 - px) * (y2 - py) - (x2 - px) * (y1 - py);
            const float e1 = (x2 - px) * (y0 - py) - (x0 - px) * (y2 - py);
            const float e2 = (x0 - px) * (y1 - py) - (x1 - px) * (y0 - py);
            if (!edge_in(e0 * sg, (y1 - y2) * sg, (x2 - x1) * sg) || !edge_in(e1 * sg, (y2 - y0) * sg, (x0 - x2) * sg) ||
                !edge_in(e2 * sg, (y0 - y1) * sg, (x1 - x0) * sg))
                continue;
            const float w0 = e0 * inv;
            const float w1 = e1 * inv;
            const float w2 = 1.0f - w0 - w1;
            plot(s, x, y, lerp3(a0, a1, a2, w0, w1, w2));
        }
    }
}

void sprite(const Setup &s, const Vertex *v) {
    const Vertex &p = v[0], &q = v[1];
    const float x0 = (static_cast<float>(p.x) - static_cast<float>(s.ofx)) / 16.0f, y0 = (static_cast<float>(p.y) - static_cast<float>(s.ofy)) / 16.0f;
    const float x1 = (static_cast<float>(q.x) - static_cast<float>(s.ofx)) / 16.0f, y1 = (static_cast<float>(q.y) - static_cast<float>(s.ofy)) / 16.0f;
    const Attr a = attr_of(s, p), b = attr_of(s, q);
    const float left = std::min(x0, x1), right = std::max(x0, x1), top = std::min(y0, y1), bottom = std::max(y0, y1);
    const int minx = std::max(s.scax0, static_cast<int>(std::ceil(left)));
    const int maxx = std::min(s.scax1, static_cast<int>(std::ceil(right)) - 1);
    const int miny = std::max(s.scay0, static_cast<int>(std::ceil(top)));
    const int maxy = std::min(s.scay1, static_cast<int>(std::ceil(bottom)) - 1);
    if (minx > maxx || miny > maxy) return;
    const float dx = x1 - x0, dy = y1 - y0;
    Attr at = b;
    for (int y = miny; y <= maxy; ++y) {
        const float ty = dy != 0.0f ? (static_cast<float>(y) - y0) / dy : 0.0f;
        at.t = a.t + (b.t - a.t) * ty;
        for (int x = minx; x <= maxx; ++x) {
            const float tx = dx != 0.0f ? (static_cast<float>(x) - x0) / dx : 0.0f;
            at.s = a.s + (b.s - a.s) * tx;
            at.q = s.fst ? 1.0f : b.q;
            plot(s, x, y, at);
        }
    }
}

void line(const Setup &s, const Vertex *v) {
    const float x0 = (static_cast<float>(v[0].x) - static_cast<float>(s.ofx)) / 16.0f, y0 = (static_cast<float>(v[0].y) - static_cast<float>(s.ofy)) / 16.0f;
    const float x1 = (static_cast<float>(v[1].x) - static_cast<float>(s.ofx)) / 16.0f, y1 = (static_cast<float>(v[1].y) - static_cast<float>(s.ofy)) / 16.0f;
    Attr a = attr_of(s, v[0]), b = attr_of(s, v[1]);
    if (!s.iip) a = Attr{b.r, b.g, b.b, b.a, a.s, a.t, a.q, a.f, a.z};
    const int steps = static_cast<int>(std::max(std::fabs(x1 - x0), std::fabs(y1 - y0)));
    for (int i = 0; i < std::max(steps, 1); ++i) {
        const float t = steps ? static_cast<float>(i) / static_cast<float>(steps) : 0.0f;
        plot(s, static_cast<int>(std::lround(x0 + (x1 - x0) * t)), static_cast<int>(std::lround(y0 + (y1 - y0) * t)),
             lerp3(a, b, b, 1.0f - t, t, 0.0f));
    }
}

}

Probe g_probe;

void reset() {}

void draw(const State &st, const Vertex *v, int count) {
    const Setup s = make_setup(st);
    switch (st.prim & 7u) {
    case 0: {
        const Attr a = attr_of(s, v[0]);
        plot(s, static_cast<int>((static_cast<float>(v[0].x) - static_cast<float>(s.ofx)) / 16.0f),
             static_cast<int>((static_cast<float>(v[0].y) - static_cast<float>(s.ofy)) / 16.0f), a);
        break;
    }
    case 1: case 2: line(s, v); break;
    case 3: case 4: case 5: if (count >= 3) triangle(s, v); break;
    case 6: sprite(s, v); break;
    default: break;
    }
}

}
