// Hardware drawing trial: the graphics chip's triangles and sprites drawn by the card's fixed-function rasterizer.
// The vertex stage places the corners (GS pixel x,y is the centre of pixel x,y here, hence the + 0.5), the fixed-function part covers the pixels
// and interpolates colour, texture coordinates, fog and depth linearly in screen space (the chip does the same, w is 1 everywhere), and the
// fragment stage does what the chip does to ONE pixel after that (gs.wgsl, plot()): texture read, texture function, fog, alpha test.
// Blending, the depth test and the colour write mask are the card's own (render pipeline state), chosen per run by hw.cpp from the
// chip's ALPHA/TEST/ZBUF/FRAME registers. Textures come from the decoded copy (`decode` in gs.wgsl) in `decoded`, or, with DIRECT, are read
// straight from graphics memory by this shader (the same swizzle and palette rules as gs.wgsl: addr, convert, expand16).
// Rules used are the ones in gs.wgsl; nothing here comes from another emulator.

struct VIn {
    @location(0) pos: vec2<f32>,
    @location(1) z: f32,
    @location(2) col: vec4<f32>,
    @location(3) st: vec3<f32>,
    @location(4) fog: f32,
    @location(5) si: u32,
}

struct VOut {
    @builtin(position) pos: vec4<f32>,
    @location(0) col: vec4<f32>,
    @location(1) st: vec3<f32>,
    @location(2) fog: f32,
    @location(3) @interpolate(flat) si: u32,
}

@group(0) @binding(0) var<storage, read> setups: array<u32>;
@group(0) @binding(1) var<storage, read> decoded: array<u32>;
@group(0) @binding(2) var<uniform> view: vec4<f32>;  // target width and height in pixels
@group(0) @binding(3) var<storage, read> vram: array<u32>;  // graphics memory (the render passes never write it; scale 1 only)
@group(0) @binding(4) var<storage, read> tables: array<u32>;  // the swizzle tables of gs.wgsl
@group(0) @binding(5) var<storage, read> clut: array<u32>;  // the palettes, 512 entries per slot
@group(0) @binding(6) var src_tex: texture_2d<f32>;  // the picture another target holds, when a shape's texture IS that picture (setup word 41 bit 0)

override ALPHA_ONLY: bool = false;  // the draw that writes only the alpha channel (raw alpha), for shapes that blend with their alpha as the factor
override PROBE: u32 = 0u;  // PS2_GPU_HW_PROBE (measuring aid, wrong picture): 1 = nearest sampling only
override DIRECT: bool = false;  // textures are read from graphics memory here instead of from `decoded`
override BMODE: u32 = 0u;  // 0 the card blends the colour as it is, 1 the colour is moved down by whole levels (rounding), 2 / 3 the colour is the product floor / ceil(Cs * C)
override BFAC: u32 = 0u;  // the chip's factor C: 0 the shape's alpha, 1 FIX (setup word 9); both count 0x80 as 1.0 and may be larger
override BINV: bool = false;  // BMODE 1: the card's factor for the colour is one minus the blend factor
override RND: f32 = 0.0;  // BMODE 1: the shift, in levels of the result (PS2_GPU_HW_ROUND, 0 = off)
override DEPTH_ONLY: bool = false;  // the second draw of a shape whose depth write depends on its alpha test: depth only, discards what failed

const STRIDE: u32 = 48u;
const F_TME: u32 = 2u;
const F_FGE: u32 = 4u;
const F_FST: u32 = 16u;
const F_ATE: u32 = 64u;
const F_TCC: u32 = 8192u;
const F_BILINEAR: u32 = 16384u;
const F_AEM: u32 = 32768u;
const F_FBA: u32 = 4096u;
const CLUT_ENTRIES: u32 = 512u;

@vertex
fn vs(v: VIn) -> VOut {
    var o: VOut;
    o.pos = vec4<f32>((v.pos.x + 0.5) / view.x * 2.0 - 1.0, 1.0 - (v.pos.y + 0.5) / view.y * 2.0, v.z, 1.0);
    o.col = v.col;
    o.st = v.st;
    o.fog = v.fog;
    o.si = v.si;
    return o;
}

fn wrap_coord(v: i32, size: i32, m: u32, mn: u32, mx: u32) -> i32 {
    switch m {
        case 0u {
            return v & (size - 1);
        }
        case 1u {
            return clamp(v, 0, size - 1);
        }
        case 2u {
            return clamp(v, i32(mn), i32(mx));
        }
        default {
            return i32((u32(v) & mn) | mx);
        }
    }
}

fn unpack(c: u32) -> vec4<f32> {
    return vec4<f32>(vec4<u32>(c & 0xFFu, (c >> 8u) & 0xFFu, (c >> 16u) & 0xFFu, c >> 24u));
}


// ---- Reading a texel straight from graphics memory (the rules of gs.wgsl: addr, convert, expand16). Everything about a texture sits in the
// setup: the format words of the texture at [25..29] (base, pages per row, table, bits, mask), PSM [14], palette format [16], palette offset
// [17], alpha values [20], palette slot [22].
struct Fmt {
    base: u32,
    per_row: u32,
    table: u32,
    bits: u32,
    mask: u32,
}

struct Loc {
    word: u32,
    shift: u32,
    mask: u32,
}

struct Tex {
    tf: Fmt,
    tpsm: u32,
    cpsm: u32,
    csa: u32,
    cbase: u32,  // first palette entry of this texture: 512 per slot, plus 16 per palette offset for 4-bit textures
    ta0: u32,
    ta1: u32,
    aem: bool,
}

fn load_tex(b: u32) -> Tex {
    var t: Tex;
    t.tf = Fmt(setups[b + 25u], setups[b + 26u], setups[b + 27u], setups[b + 28u], setups[b + 29u]);
    t.tpsm = setups[b + 14u];
    t.cpsm = setups[b + 16u];
    t.csa = setups[b + 17u];
    t.cbase = setups[b + 22u] * CLUT_ENTRIES;
    if (t.tpsm == 0x14u || t.tpsm == 0x24u || t.tpsm == 0x2Cu) {
        t.cbase += (t.csa & 15u) * 16u;
    }
    t.ta0 = setups[b + 20u] & 0xFFu;
    t.ta1 = (setups[b + 20u] >> 8u) & 0xFFu;
    t.aem = (setups[b] & F_AEM) != 0u;
    return t;
}

// The chip's address of a texel is a sum of three parts: the page of the x part, the page of the y part, and the position inside the page from a
// table (gs.wgsl, addr()). The two page parts and the table row depend on x or y alone, so a bilinear sample works them out twice, not four times.
struct Axis {
    lo: u32,    // x: position inside the page (column); y: the table row, already shifted by the page width
    page: u32,  // x: byte offset of the page column; y: the memory base plus the byte offset of the page row
}

fn axis_x(t: Tex, x: u32) -> Axis {
    let pws = t.tf.bits & 15u;
    return Axis(x & ((1u << pws) - 1u), (x >> pws) * 8192u);
}

fn axis_y(t: Tex, y: u32) -> Axis {
    let pws = t.tf.bits & 15u;
    let phs = (t.tf.bits >> 4u) & 15u;
    return Axis((y & ((1u << phs) - 1u)) << pws, t.tf.base + (y >> phs) * t.tf.per_row * 8192u);
}

// the raw texel (palette index, 16-bit colour or 32-bit colour) at one position, no branches, so the four taps of a bilinear sample can be in flight together
fn raw_axes(t: Tex, ax: Axis, ay: Axis) -> u32 {
    let f = t.tf;
    let n = tables[f.table + ay.lo + ax.lo];
    let nbit = (f.bits >> 10u) & 1u;
    let a = ax.page + ay.page + (n >> nbit);
    let shift = (f.bits >> 11u) + (a & ((f.bits >> 8u) & 3u)) * 8u + (n & nbit) * 4u;
    return (vram[(a & 0x3FFFFFu) >> 2u] >> shift) & f.mask;
}

// A texel of a picture the card holds (hw.cpp: same base address, same width, a format whose texel can be rebuilt from the four channels). The channels are
// the raw texel as graphics memory would hold it: 32-bit colour as it is; 16-bit colour cut to 5/5/5/1 bits (what the chip stores, so a picture that was
// blended in 8 bits reads as the chip would read it); then the usual convert() of the texture's own format (24-bit: texture alpha, 16-bit: expand16).
fn target_raw(t: Tex, x: i32, y: i32) -> u32 {
    let p = min(vec2<i32>(x, y), vec2<i32>(textureDimensions(src_tex)) - vec2<i32>(1, 1));  // only reached past the edge with PS2_GPU_HW_OVERSHOOT=1
    let c = vec4<u32>(clamp(textureLoad(src_tex, p, 0) * 255.0 + vec4<f32>(0.5), vec4<f32>(0.0), vec4<f32>(255.0)));
    if (t.tpsm == 0x02u || t.tpsm == 0x0Au) {
        return ((c.x >> 3u) & 31u) | (((c.y >> 3u) & 31u) << 5u) | (((c.z >> 3u) & 31u) << 10u) | ((c.w >> 7u) << 15u);
    }
    return c.x | (c.y << 8u) | (c.z << 16u) | (c.w << 24u);
}

fn target_texel(t: Tex, x: u32, y: u32) -> vec4<f32> {
    return unpack(convert(t, target_raw(t, i32(x), i32(y))));
}

fn texel_axes(t: Tex, ax: Axis, ay: Axis) -> vec4<f32> {
    return unpack(convert(t, raw_axes(t, ax, ay)));
}

// convert() for four raw texels at once: the palette reads of the four taps are issued together
fn convert4(t: Tex, r: vec4<u32>) -> vec4<u32> {
    switch t.tpsm {
        case 0x13u, 0x1Bu, 0x14u, 0x24u, 0x2Cu {
            var m = 255u;
            if (t.tpsm == 0x14u || t.tpsm == 0x24u || t.tpsm == 0x2Cu) {
                m = 15u;
            }
            let i = vec4<u32>(t.cbase) + (r & vec4<u32>(m));
            var e = vec4<u32>(clut[i.x], clut[i.y], clut[i.z], clut[i.w]);
            if (t.cpsm != 0u && t.cpsm != 1u) {
                e = vec4<u32>(expand16(e.x, t), expand16(e.y, t), expand16(e.z, t), expand16(e.w, t));
            }
            return e;
        }
        default {
            return vec4<u32>(convert(t, r.x), convert(t, r.y), convert(t, r.z), convert(t, r.w));
        }
    }
}

fn expand16(c: u32, t: Tex) -> u32 {
    let r = (c & 0x1Fu) << 3u;
    let g = ((c >> 5u) & 0x1Fu) << 3u;
    let b = ((c >> 10u) & 0x1Fu) << 3u;
    var a = t.ta0;
    if ((c & 0x8000u) != 0u) {
        a = t.ta1;
    } else if (t.aem && (c & 0x7FFFu) == 0u) {
        a = 0u;
    }
    return r | (g << 8u) | (b << 16u) | (a << 24u);
}

fn convert(t: Tex, raw: u32) -> u32 {
    switch t.tpsm {
        case 0x00u, 0x30u {
            return raw;
        }
        case 0x01u, 0x31u {
            var a = t.ta0;
            if (t.aem && (raw & 0xFFFFFFu) == 0u) {
                a = 0u;
            }
            return (raw & 0xFFFFFFu) | (a << 24u);
        }
        case 0x02u, 0x0Au, 0x32u, 0x3Au {
            return expand16(raw, t);
        }
        case 0x13u, 0x1Bu {
            let e = clut[t.cbase + (raw & 255u)];
            if (t.cpsm == 0u || t.cpsm == 1u) {
                return e;
            }
            return expand16(e, t);
        }
        case 0x14u, 0x24u, 0x2Cu {
            let e = clut[t.cbase + (raw & 15u)];
            if (t.cpsm == 0u || t.cpsm == 1u) {
                return e;
            }
            return expand16(e, t);
        }
        default {
            return raw;
        }
    }
}

// What the chip does to one pixel: colour (0..255 per channel, as floats), the raw alpha, and, for the dual-source draw, the card's blend factor
// (the shape alpha counted with 0x80 as 1.0, `factor`) and whether the alpha channel is written (`awrite`: 0 where a failed alpha test keeps the old alpha).
struct Shaded {
    rgb: vec3<f32>,
    alpha: f32,
    factor: f32,
    awrite: f32,
}

fn shade(in: VOut) -> Shaded {
    if (DEPTH_ONLY && PROBE == 3u) {  // measuring aid (wrong picture): the depth draw does nothing, so what is left is the cost of the triangles themselves
        var none: Shaded;
        return none;
    }
    let b = in.si * STRIDE;
    let flags = setups[b];
    let modes = setups[b + 7u];
    var r = i32(in.col.x + 0.001);
    var g = i32(in.col.y + 0.001);
    var bl = i32(in.col.z + 0.001);
    var a = i32(in.col.w + 0.001);
    if ((flags & F_TME) != 0u && PROBE != 4u) {  // PROBE 4 (wrong picture): no texture at all, the cost of the rest
        let tw = setups[b + 15u] & 0xFFu;
        let th = setups[b + 15u] >> 8u;
        var u: f32;
        var vv: f32;
        if ((flags & F_FST) != 0u) {
            u = in.st.x / 16.0;
            vv = in.st.y / 16.0;
        } else {
            var q = in.st.z;
            if (q == 0.0) {
                q = 1.0;
            }
            u = in.st.x / q * f32(1u << tw);
            vv = in.st.y / q * f32(1u << th);
        }
        let dec = setups[b + 24u];
        let mu = (modes >> 24u) & 3u;
        let mv = (modes >> 26u) & 3u;
        let minu = setups[b + 18u] & 0xFFFFu;
        let maxu = setups[b + 18u] >> 16u;
        let minv = setups[b + 19u] & 0xFFFFu;
        let maxv = setups[b + 19u] >> 16u;
        let sw = i32(1u << tw);
        let sh = i32(1u << th);
        var tex: Tex;
        if (DIRECT) {
            tex = load_tex(b);
        }
        let from_target = DIRECT && (setups[b + 41u] & 1u) != 0u;
        var t: vec4<f32>;
        if ((flags & F_BILINEAR) == 0u || PROBE == 1u) {
            let x = u32(wrap_coord(i32(floor(u)), sw, mu, minu, maxu));
            let y = u32(wrap_coord(i32(floor(vv)), sh, mv, minv, maxv));
            if (from_target) {
                t = target_texel(tex, x, y);
            } else if (DIRECT) {
                t = texel_axes(tex, axis_x(tex, x), axis_y(tex, y));
            } else {
                t = unpack(decoded[dec + (y << tw) + x]);
            }
        } else {
            let uu = u - 0.5;
            let vf = vv - 0.5;
            let x0 = i32(floor(uu));
            let y0 = i32(floor(vf));
            let xa = u32(wrap_coord(x0, sw, mu, minu, maxu));
            let xb = u32(wrap_coord(x0 + 1, sw, mu, minu, maxu));
            let ya = u32(wrap_coord(y0, sh, mv, minv, maxv));
            let yb = u32(wrap_coord(y0 + 1, sh, mv, minv, maxv));
            let fx = uu - f32(x0);
            let fy = vf - f32(y0);
            var t00: vec4<f32>;
            var t10: vec4<f32>;
            var t01: vec4<f32>;
            var t11: vec4<f32>;
            if (from_target) {
                t00 = target_texel(tex, xa, ya);
                t10 = target_texel(tex, xb, ya);
                t01 = target_texel(tex, xa, yb);
                t11 = target_texel(tex, xb, yb);
            } else if (DIRECT) {
                let axa = axis_x(tex, xa);
                let axb = axis_x(tex, xb);
                let aya = axis_y(tex, ya);
                let ayb = axis_y(tex, yb);
                let c = convert4(tex, vec4<u32>(raw_axes(tex, axa, aya), raw_axes(tex, axb, aya), raw_axes(tex, axa, ayb), raw_axes(tex, axb, ayb)));
                t00 = unpack(c.x);
                t10 = unpack(c.y);
                t01 = unpack(c.z);
                t11 = unpack(c.w);
            } else {
                t00 = unpack(decoded[dec + (ya << tw) + xa]);
                t10 = unpack(decoded[dec + (ya << tw) + xb]);
                t01 = unpack(decoded[dec + (yb << tw) + xa]);
                t11 = unpack(decoded[dec + (yb << tw) + xb]);
            }
            let top = mix(t00, t10, fx);
            let bottom = mix(t01, t11, fx);
            t = floor(clamp(mix(top, bottom, fy) + vec4<f32>(0.5), vec4<f32>(0.0), vec4<f32>(255.0)));
        }
        let tr = i32(t.x);
        let tg = i32(t.y);
        let tb = i32(t.z);
        let ta = i32(t.w);
        let tcc = (flags & F_TCC) != 0u;
        switch ((modes >> 20u) & 3u) {
            case 0u {
                r = clamp((tr * r) >> 7u, 0, 255);
                g = clamp((tg * g) >> 7u, 0, 255);
                bl = clamp((tb * bl) >> 7u, 0, 255);
                if (tcc) {
                    a = clamp((ta * a) >> 7u, 0, 255);
                }
            }
            case 1u {
                r = tr;
                g = tg;
                bl = tb;
                if (tcc) {
                    a = ta;
                }
            }
            case 2u {
                r = clamp(((tr * r) >> 7u) + a, 0, 255);
                g = clamp(((tg * g) >> 7u) + a, 0, 255);
                bl = clamp(((tb * bl) >> 7u) + a, 0, 255);
                if (tcc) {
                    a = clamp(ta + a, 0, 255);
                }
            }
            default {
                r = clamp(((tr * r) >> 7u) + a, 0, 255);
                g = clamp(((tg * g) >> 7u) + a, 0, 255);
                bl = clamp(((tb * bl) >> 7u) + a, 0, 255);
                if (tcc) {
                    a = ta;
                }
            }
        }
    }
    if ((flags & F_FGE) != 0u) {
        let f = i32(in.fog + 0.001);
        let fc = setups[b + 21u];
        r = (f * r + (255 - f) * i32(fc & 0xFFu)) >> 8u;
        g = (f * g + (255 - f) * i32((fc >> 8u) & 0xFFu)) >> 8u;
        bl = (f * bl + (255 - f) * i32((fc >> 16u) & 0xFFu)) >> 8u;
    }
    var keep_alpha = false;
    if ((flags & F_ATE) != 0u) {
        let av = u32(a);
        let aref = setups[b + 8u];
        var ok = false;
        switch (modes & 7u) {
            case 0u {
                ok = false;
            }
            case 1u {
                ok = true;
            }
            case 2u {
                ok = av < aref;
            }
            case 3u {
                ok = av <= aref;
            }
            case 4u {
                ok = av == aref;
            }
            case 5u {
                ok = av >= aref;
            }
            case 6u {
                ok = av > aref;
            }
            default {
                ok = av != aref;
            }
        }
        // what a failed test does is the chip's AFAIL: 0 keep (nothing written), 1 frame only, 2 depth only, 3 colour only.
        // The first draw (colour) drops the pixel for 0 and 2; the depth-only draw drops it for everything but 2.
        let fail = (modes >> 4u) & 3u;
        if (!ok && fail == 3u) {
            keep_alpha = true;
        }
        if (!ok) {
            if (DEPTH_ONLY) {
                if (fail != 2u) {
                    discard;
                }
            } else if (ALPHA_ONLY) {
                if (fail != 1u) {
                    discard;
                }
            } else if (fail == 0u || (fail == 2u && (modes & 7u) != 0u)) {  // (a test that never passes with "depth only on failure" is one draw with the colour mask off: classify() in hwdraw.cpp, the depth must be written)
                discard;
            }
        }
    }
    var s: Shaded;
    var levels = vec3<f32>(f32(r & 255), f32(g & 255), f32(bl & 255));
    s.factor = f32(min(a, 128)) / 128.0;
    if (BMODE != 0u) {
        let cf = select(a, i32(setups[b + 9u]), BFAC == 1u);  // the chip's factor, 0x80 = 1.0
        if (BMODE >= 2u) {
            // The forms Cs * C added to or taken from the target: the product is computed here as the chip does ((Cs * C) >> 7, taken from below or above) and the card only
            // adds it (its factors are one), so nothing is rounded by the card. A product over 255 saturates the same way in the card.
            let m = vec3<i32>(r & 255, g & 255, bl & 255) * cf + vec3<i32>(select(0, 127, BMODE == 3u));
            levels = vec3<f32>(min(m >> vec3<u32>(7u), vec3<i32>(255)));
        } else if (cf > 0) {
            // The card's blender takes the colour and the factor in 8 bits and rounds to nearest; the chip cuts down. The blend adds Cs * F (F = n / 255), so moving the colour
            // down by RND / F levels (a whole number: the card keeps no fraction of it) moves the result down by about RND, just under half a step.
            let n = floor(f32(min(cf, 128)) * 255.0 / 128.0 + 0.5);
            let fq = select(n, 255.0 - n, BINV) / 255.0;
            if (fq > 0.0) {
                levels = clamp(floor(levels - RND / fq + 0.5), vec3<f32>(0.0), vec3<f32>(255.0));
            }
        }
    }
    s.rgb = levels / 255.0;
    s.alpha = f32((a & 255) | select(0, 128, (flags & F_FBA) != 0u)) / 255.0;  // FBA forces the stored alpha's top bit
    s.awrite = select(1.0, 0.0, keep_alpha);
    return s;
}

@fragment
fn fs(in: VOut) -> @location(0) vec4<f32> {
    // the output alpha: the card's blend factor when the draw blends with the shape's alpha (the chip counts 0x80 as 1.0), else the alpha itself
    let s = shade(in);
    var out_a = s.alpha;
    if ((setups[in.si * STRIDE + 40u] & 1u) != 0u && !ALPHA_ONLY) {
        out_a = s.factor;
    }
    return vec4<f32>(s.rgb, out_a);
}

//DUAL_BEGIN
// Dual-source blending: output 0 is colour and the RAW alpha, output 1 is the card's blend factor in its three colour channels and the
// "alpha is written" factor in its alpha channel (1 = new alpha, 0 = keep the old one: a pixel that failed an alpha test whose failure mode keeps the alpha).
// The pipeline blends colour with Src1 / OneMinusSrc1 and alpha with Src1Alpha / OneMinusSrc1Alpha, so one draw does what the fallback needs two for.
struct DualOut {
    @location(0) @blend_src(0) c: vec4<f32>,
    @location(0) @blend_src(1) f: vec4<f32>,
}

@fragment
fn fs_dual(in: VOut) -> DualOut {
    let s = shade(in);
    var o: DualOut;
    o.c = vec4<f32>(s.rgb, s.alpha);
    o.f = vec4<f32>(s.factor, s.factor, s.factor, s.awrite);
    return o;
}
//DUAL_END

// ---- the depth of a target loaded from graphics memory. A depth texture cannot be written by a compute shader, so a full-screen
// triangle (cut to the wanted rectangle by the scissor) writes the depth the chip's memory holds, 24 bits as the card's float (exact: 24 bits fit in a float).
// The format words of the depth buffer (base, pages per row, table, bits, mask) are in the setup record whose number is in view.z (a float, exact below 2^24).
@vertex
fn vs_full(@builtin(vertex_index) i: u32) -> @builtin(position) vec4<f32> {
    let p = vec2<f32>(f32((i << 1u) & 2u), f32(i & 2u));
    return vec4<f32>(p * 2.0 - vec2<f32>(1.0, 1.0), 0.0, 1.0);
}

@fragment
fn fs_load_z(@builtin(position) pos: vec4<f32>) -> @builtin(frag_depth) f32 {
    let b = u32(view.z + 0.5) * STRIDE;
    var t: Tex;
    t.tf = Fmt(setups[b + 35u], setups[b + 36u], setups[b + 37u], setups[b + 38u], setups[b + 39u]);
    let raw = raw_axes(t, axis_x(t, u32(pos.x)), axis_y(t, u32(pos.y)));
    return f32(raw & 0xFFFFFFu) / 16777216.0;
}
