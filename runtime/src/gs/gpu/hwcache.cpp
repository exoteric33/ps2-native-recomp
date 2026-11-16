// The texture cache's bookkeeping. Page layouts come from the public GS memory descriptions (ps2tek) and from how
// gpu.cpp's region() reads them; nothing here is taken from another emulator. This file never touches the card in counting mode.
#include "hwcache.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <vector>

#include "../gs.hpp"

namespace ps2::gs {
void swizzle_tables(std::vector<std::uint32_t> &out);
}

namespace ps2::gs::gpu::hwcache {

bool g_on = false;

namespace {

using u32 = std::uint32_t;
using u64 = std::uint64_t;
using u8 = std::uint8_t;

bool g_count = false, g_cache = false, g_verify = false, g_lines = false;
u32 g_skip = 5;                 // frames left out of the means (the first frames are all cold)
u32 g_max_age = 90;             // frames an unused entry stays (tens of frames)
u32 g_stream_after = 3;         // consecutive rebuilds of one entry before it counts as streamed
bool g_allow_move = true;       // false in the replay: the plan has the offsets baked in
u64 g_seq = 1;                  // number of the latest write
u64 g_block_seq[16384];         // number of the latest write that touched each 256-byte block of memory (a page is 32 blocks)
u8 g_block_who[16384];          // who wrote it
u64 g_block_id[16384];          // what the block holds, by identity: the same number means the same bytes (an upload of bytes seen before gets the number it got then; every
                                // other writer gets a number nobody had). 0 = never written since the program began.
u64 g_page_seq[512];            // the newest of its 32 blocks: the quick test before the exact one
u64 g_frame = 0;
u64 g_epoch = 1;                // bumped by a loaded state: palettes before it are unknown

u64 mix(u64 h, u64 v) { return (h ^ (v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2))) * 0x100000001B3ull; }

void page_dims(u32 psm, u32 &pw, u32 &ph) {
    switch (psm) {
    case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S: pw = 64; ph = 64; break;
    case PSMT8: pw = 128; ph = 64; break;
    case PSMT4: pw = 128; ph = 128; break;
    default: pw = 64; ph = 32; break;  // 32 and 24 bit, the upper-byte index formats too
    }
}

bool indexed(u32 psm) { return psm == PSMT8 || psm == PSMT4 || psm == PSMT8H || psm == PSMT4HL || psm == PSMT4HH; }
bool eight_bit(u32 psm) { return psm == PSMT8 || psm == PSMT8H; }
bool alpha_expanding(u32 psm, u32 cpsm) { return psm == PSMCT24 || psm == PSMCT16 || psm == PSMCT16S || (indexed(psm) && (cpsm == 2 || cpsm == 0xA)); }

// ---- where things lie in memory: 256-byte blocks. The swizzle tables of gs.cpp (the ones the shaders use) say which block of its page each pixel is in.
struct Fmt {
    u32 pw, ph, bw, bh;  // page and block size in pixels
    std::vector<u8> idx; // block of the page for each block cell, row by row
};
Fmt g_fmt[8];
bool g_fmt_ready = false;

int fmt_class(u32 psm) {
    switch (psm) {
    case PSMZ32: case PSMZ24: return 1;
    case PSMCT16: return 2;
    case PSMCT16S: return 3;
    case PSMZ16: return 4;
    case PSMZ16S: return 5;
    case PSMT8: return 6;
    case PSMT4: return 7;
    default: return 0;  // 32 bit, 24 bit and the upper-byte index formats
    }
}

void build_formats() {
    std::vector<u32> t;
    ps2::gs::swizzle_tables(t);
    static const u32 off[8] = {0, 2048, 4096, 8192, 12288, 16384, 20480, 28672};
    static const u32 dims[8][4] = {{64, 32, 8, 8}, {64, 32, 8, 8}, {64, 64, 16, 8}, {64, 64, 16, 8}, {64, 64, 16, 8}, {64, 64, 16, 8}, {128, 64, 16, 16}, {128, 128, 32, 16}};
    for (int c = 0; c < 8; ++c) {
        Fmt &f = g_fmt[c];
        f.pw = dims[c][0], f.ph = dims[c][1], f.bw = dims[c][2], f.bh = dims[c][3];
        const u32 cols = f.pw / f.bw, rows = f.ph / f.bh;
        f.idx.assign(cols * rows, 0);
        for (u32 by = 0; by < rows; ++by)
            for (u32 bx = 0; bx < cols; ++bx) {
                const u32 v = t[off[c] + (by * f.bh) * f.pw + bx * f.bw];  // byte offset in the page (a nibble index for 4-bit)
                f.idx[by * cols + bx] = static_cast<u8>((c == 7 ? v / 2 : v) / 256);
            }
    }
    g_fmt_ready = true;
}

// A set of blocks (sorted, without repeats) and the pages they lie in.
struct Cover {
    std::vector<std::uint16_t> blocks;
    Pages pages;
    bool empty() const { return blocks.empty(); }
};

void cover_rect(u32 bp, u32 bw, u32 psm, int x0, int y0, int x1, int y1, Cover &out, std::vector<std::uint8_t> *full = nullptr) {
    if (!g_fmt_ready) build_formats();
    const Fmt &f = g_fmt[fmt_class(psm)];
    const u64 per_row = std::max<u32>(1, bw * 64 / f.pw);
    const u32 cols = f.pw / f.bw;
    static std::bitset<16384> seen;
    const int bx0 = std::max(x0, 0) / static_cast<int>(f.bw), bx1 = std::max(x1, 0) / static_cast<int>(f.bw);
    const int by0 = std::max(y0, 0) / static_cast<int>(f.bh), by1 = std::max(y1, 0) / static_cast<int>(f.bh);
    const std::size_t first = out.blocks.size();
    for (int by = by0; by <= by1; ++by)
        for (int bx = bx0; bx <= bx1; ++bx) {
            const u64 px = static_cast<u64>(bx) * f.bw / f.pw, py = static_cast<u64>(by) * f.bh / f.ph;
            const u32 inpage = f.idx[(static_cast<u32>(by) * f.bh % f.ph / f.bh) * cols + (static_cast<u32>(bx) % cols)];
            const u32 id = static_cast<u32>((bp + (px + py * per_row) * 32 + inpage) & 16383u);
            if (seen.test(id)) continue;
            seen.set(id);
            out.blocks.push_back(static_cast<std::uint16_t>(id));
            out.pages.set(id >> 5);
            if (full) full->push_back(bx * static_cast<int>(f.bw) >= x0 && (bx + 1) * static_cast<int>(f.bw) - 1 <= x1 && by * static_cast<int>(f.bh) >= y0 && (by + 1) * static_cast<int>(f.bh) - 1 <= y1);
        }
    for (std::size_t i = first; i < out.blocks.size(); ++i) seen.reset(out.blocks[i]);
}

// ---- palettes: per painter slot, 32 groups of 16 entries; each group holds the identity of the load that wrote it
struct Clut {
    u64 g[32];
};
std::vector<Clut> g_clut;

u64 palette_id(const Request &r) {
    if (!indexed(r.tpsm)) return 0;
    if (g_clut.empty()) g_clut.assign(2048, Clut{});
    const Clut &c = g_clut[r.clut_slot % g_clut.size()];
    u64 h = 0x1234;
    const u32 n = eight_bit(r.tpsm) ? 16 : 1;
    for (u32 i = 0; i < n; ++i) {
        const u64 id = c.g[(r.csa + i) & 31];
        h = mix(h, id ? id : mix(mix(g_epoch, r.clut_slot), (r.csa + i) & 31));
    }
    return h | 1;
}

// ---- the replay of the card path. The uploads and palette loads of the captured frame are remembered with the identities they gave their blocks, so that every replay can
// put the same identities back at the same places in its sequence of commands.
struct Rec {
    bool clut = false;
    u32 args[10] = {};
    std::vector<std::pair<std::uint16_t, u64>> blocks;
};
std::vector<Rec> g_pending;           // uploads and palette loads queued since the last round, in the order of the painter's list
std::map<u64, std::vector<Rec>> g_recorded;
bool g_replay = false;
u64 g_snap_id[16384];
std::vector<Clut> g_snap_clut;
bool g_snapped = false;

void push_clut_rec(const u32 *args) {
    Rec r;
    r.clut = true;
    std::memcpy(r.args, args, sizeof r.args);
    g_pending.push_back(std::move(r));
}
void push_upload_rec(const Cover &c) {
    Rec r;
    r.blocks.reserve(c.blocks.size());
    for (std::uint16_t b : c.blocks) r.blocks.emplace_back(b, g_block_id[b]);
    g_pending.push_back(std::move(r));
}

// ---- entries. A base is a texture as the game names it (address, size, format, palette mode); its versions are the decoded copies for the memory contents and palettes it
// has had. A version is valid while the memory it covers holds the same bytes by identity.
using BaseKey = std::array<u32, 8>;

struct Base {
    Cover cover;
    u32 words = 0;
    u64 checked = 0, identity = 0;  // write number at which `identity` was worked out; the fold of the covered blocks' identities
    std::vector<u32> versions;      // entry ids, alive ones
    u64 last_frame = ~0ull;
    u32 rebuilds = 0;               // consecutive frames that needed a new version
    bool picture = false, ever = false;
    u64 last_built = 0, last_identity = 0, last_pal = 0;  // of the newest version
    u64 idx_identity = 0;                                  // the index form's shadow
    bool stream = false;
    u32 pinned = ~0u;  // the replay: one fixed place in the decoded buffer, baked into the plan
};
std::map<BaseKey, Base> g_bases;

struct Entry {
    Base *base = nullptr;
    u64 identity = 0, pal = 0;
    u32 offset = 0;
    bool stream = false, alive = false, pinned = false;
    u64 last_frame = 0;
    bool alpha_known = false;
    u8 amin = 0, amax = 255;
};
std::vector<Entry> g_entries;

// has any page of the cover been written after write number `seq`? (the quick test; the identity fold is the exact one)
bool pages_newer(const Cover &c, u64 seq) {
    for (int p = 0; p < 512; ++p)
        if (c.pages.test(static_cast<std::size_t>(p)) && g_page_seq[p] > seq) return true;
    return false;
}

u64 fold_identity(const Cover &c) {
    u64 h = 0x51ED270B;
    for (std::uint16_t b : c.blocks) h = (h ^ g_block_id[b]) * 0x9FB21C651E98DF25ull, h ^= h >> 31;
    return h | 1;
}

// the space the decoded words live in (words of the caller's decoded buffer); a main area with first fit, and a ring for streamed textures
struct Span {
    u32 off, len;
};
std::vector<Span> g_free;
u32 g_cap = 16u << 20, g_stream = 4u << 20, g_ring = 0;  // counting mode: 64 MB of words, the last 16 MB for streamed textures
bool g_space_ready = false;

bool alloc_main(u32 words, u32 &off) {
    for (std::size_t i = 0; i < g_free.size(); ++i) {
        if (g_free[i].len < words) continue;
        off = g_free[i].off;
        g_free[i].off += words;
        g_free[i].len -= words;
        if (!g_free[i].len) g_free.erase(g_free.begin() + static_cast<std::ptrdiff_t>(i));
        return true;
    }
    return false;
}
void free_main(u32 off, u32 words) {
    auto it = std::lower_bound(g_free.begin(), g_free.end(), off, [](const Span &s, u32 o) { return s.off < o; });
    it = g_free.insert(it, {off, words});
    if (it + 1 != g_free.end() && it->off + it->len == (it + 1)->off) it->len += (it + 1)->len, g_free.erase(it + 1);
    if (it != g_free.begin() && (it - 1)->off + (it - 1)->len == it->off) (it - 1)->len += it->len, g_free.erase(it);
}
void ensure_space() {
    if (g_space_ready) return;
    g_space_ready = true;
    g_free.assign(1, {0, g_cap - g_stream});
    g_ring = g_cap - g_stream;
}

struct Counts {
    u64 n = 0, bytes = 0;
    void add(u64 b) { ++n, bytes += b; }
};
enum Cause { kHit = 0, kNew, kNewPalette, kEvicted, kByUpload, kByDraw, kByCopy, kByStore, kByState, kCauses };
const char *const kCauseName[kCauses] = {"unchanged", "first seen", "new palette", "evicted/aged", "upload", "draw", "copy", "write-back", "state load"};

struct Frame {
    Counts cell[2][kCauses];      // by (picture?, outcome) of the first request of each distinct texture in the frame
    Counts hit_prev[2];           // hits whose texture was also asked for in the previous frame ("unchanged from frame to frame")
    Counts cold[2];               // every decode that had to run (a texture decoded twice in a frame counts twice)
    Counts cold_pal;              // the decodes of palette formats (uploaded kind): 32-bit words per palette
    Counts idx_cold;              // the same for the index form of palette textures (1 byte a texel), pictures left out
    Counts idx_palettes;          // distinct new palette identities (1 KB each)
    u64 requests = 0, streamed = 0, refused = 0, evicted_hot = 0, pal_requests = 0;
};
Frame g_cur, g_sum;
u64 g_frames_counted = 0;
std::vector<u64> g_per_frame_cold;
std::set<u64> g_pal_seen_frame;

void add_frame(Frame &to, const Frame &f) {
    for (int p = 0; p < 2; ++p) {
        for (int c = 0; c < kCauses; ++c) to.cell[p][c].n += f.cell[p][c].n, to.cell[p][c].bytes += f.cell[p][c].bytes;
        to.hit_prev[p].n += f.hit_prev[p].n, to.hit_prev[p].bytes += f.hit_prev[p].bytes;
        to.cold[p].n += f.cold[p].n, to.cold[p].bytes += f.cold[p].bytes;
    }
    to.cold_pal.n += f.cold_pal.n, to.cold_pal.bytes += f.cold_pal.bytes;
    to.idx_cold.n += f.idx_cold.n, to.idx_cold.bytes += f.idx_cold.bytes;
    to.idx_palettes.n += f.idx_palettes.n, to.idx_palettes.bytes += f.idx_palettes.bytes;
    to.requests += f.requests, to.streamed += f.streamed, to.refused += f.refused, to.evicted_hot += f.evicted_hot, to.pal_requests += f.pal_requests;
}

void drop(u32 id) {
    Entry &e = g_entries[id];
    if (!e.alive) return;
    if (!e.stream && !e.pinned) free_main(e.offset, e.base->words);  // ring space is reused by position, a pinned place stays
    auto &v = e.base->versions;
    v.erase(std::remove(v.begin(), v.end(), id), v.end());
    e.alive = false;
}

// make room for `words` in the main area: drop versions nobody asked for in this frame, the longest unused first
bool make_room(u32 words, u32 &off) {
    if (alloc_main(words, off)) return true;
    std::vector<u32> order;
    for (u32 i = 0; i < g_entries.size(); ++i)
        if (g_entries[i].alive && !g_entries[i].stream && g_entries[i].last_frame != g_frame) order.push_back(i);
    std::sort(order.begin(), order.end(), [](u32 a, u32 b) { return g_entries[a].last_frame < g_entries[b].last_frame; });
    for (u32 id : order) {
        if (g_frame - g_entries[id].last_frame < 30) ++g_cur.evicted_hot;  // thrash: something used within the last 30 frames had to go
        drop(id);
        if (alloc_main(words, off)) return true;
    }
    return false;
}

bool alloc_ring(u32 words, u32 &off) {
    if (words > g_stream) return false;
    if (g_ring + words > g_cap) g_ring = g_cap - g_stream;
    off = g_ring;
    g_ring += words;
    for (u32 i = 0; i < g_entries.size(); ++i) {  // whatever lived in that stretch of the ring is gone
        const Entry &e = g_entries[i];
        if (e.alive && e.stream && e.offset < off + words && off < e.offset + e.base->words) drop(i);
    }
    return true;
}

// the memory behind a base has new contents: who wrote the newest of its blocks after the last version was made
Cause cause_of(const Base &b) {
    u64 best = 0;
    u8 who = kNone;
    for (std::uint16_t k : b.cover.blocks)
        if (g_block_seq[k] > b.last_built && g_block_seq[k] >= best) best = g_block_seq[k], who = g_block_who[k];
    switch (who) {
    case kDraw: return kByDraw;
    case kCopy: return kByCopy;
    case kStore: return kByStore;
    case kState: return kByState;
    default: return kByUpload;
    }
}

bool is_picture(const Cover &c) {
    for (std::uint16_t b : c.blocks)
        if (g_block_who[b] == kDraw || g_block_who[b] == kStore) return true;
    return false;
}

}  // namespace

bool counting() { return g_count; }
bool verify_on() { return g_verify; }

void init() {
    static bool done = false;
    if (done) return;
    done = true;
    if (const char *t = std::getenv("PS2_GPU_TEXSTAT"); t && *t && *t != '0') {
        g_count = true;
        const int level = std::atoi(t);
        g_lines = level >= 2;
        g_verify = level >= 3;
    }
    if (const char *c = std::getenv("PS2_GPU_HW_CACHE"); c && *c == '1') g_cache = true, g_replay = true;
    if (const char *s = std::getenv("PS2_GPU_TEXSTAT_SKIP")) g_skip = static_cast<u32>(std::atoi(s));
    if (const char *s = std::getenv("PS2_GPU_TEXSTAT_MB")) {  // capacity of the cache in MB (a quarter of it is the ring for streamed textures)
        g_cap = static_cast<u32>(std::max(1, std::atoi(s))) << 18;
        g_stream = g_cap / 4;
    }
    if (const char *s = std::getenv("PS2_GPU_TEXSTAT_AGE")) g_max_age = static_cast<u32>(std::max(1, std::atoi(s)));
    g_on = g_count || g_cache;
}

void configure(u32 capacity_words, u32 stream_words, bool allow_move) {
    g_cap = capacity_words;
    g_stream = stream_words;
    g_allow_move = allow_move;
    g_space_ready = false;
    g_entries.clear();
    g_bases.clear();
}

namespace {
// a writer whose result nobody can predict (a draw, a copy, a write-back, a state): its blocks get an identity of their own
void write_cover(const Cover &c, Writer who) {
    ++g_seq;
    const u64 uid = mix(0x5DEECE66Dull, g_seq) | 1;
    for (std::uint16_t b : c.blocks) g_block_seq[b] = g_seq, g_block_who[b] = who, g_block_id[b] = uid, g_page_seq[b >> 5] = g_seq;
}

u64 hash_bytes(const u8 *p, std::size_t n) {
    u64 h = 0x9E3779B97F4A7C15ull ^ n;
    std::size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        u64 w;
        std::memcpy(&w, p + i, 8);
        h = (h ^ w) * 0x9FB21C651E98DF25ull;
        h ^= h >> 29;
    }
    u64 tail = 0;
    std::memcpy(&tail, p + i, n - i);
    return (h ^ tail) * 0x9FB21C651E98DF25ull;
}
u64 g_uploads_seen = 0, g_uploads_full = 0;
}  // namespace

void write_pages(const Pages &pages, Writer who) {
    Cover c;
    for (int p = 0; p < 512; ++p)
        if (pages.test(static_cast<std::size_t>(p)))
            for (int k = 0; k < 32; ++k) c.blocks.push_back(static_cast<std::uint16_t>(p * 32 + k));
    write_cover(c, who);
}

void write_region(u32 bp, u32 bw, u32 psm, int x0, int y0, int x1, int y1, Writer who) {
    Cover c;
    cover_rect(bp, bw, psm, x0, y0, x1, y1, c);
    write_cover(c, who);
}

// An upload carries bytes the program holds, so what it leaves in memory is known: a block it covers completely holds a function of (where, which bytes) only. The same
// bytes sent to the same place again give the blocks the identity they had before, whatever happened in between. A block it covers only in part (the seam of two
// chunks of one picture, or an odd-sized rectangle) keeps the history: old identity and this chunk.
void write_upload(u32 bp, u32 bw, u32 psm, int x0, int y0, int x1, int y1, bool whole_rows, const u8 *bytes, std::size_t size) {
    // (recorded for the replay at the end)
    Cover c;
    std::vector<std::uint8_t> full;
    cover_rect(bp, bw, psm, x0, y0, x1, y1, c, &full);
    ++g_seq;
    const u64 chunk = mix(mix(mix(mix(mix(bp, bw), psm), static_cast<u64>(x0) << 32 | static_cast<u32>(y0)), static_cast<u64>(x1) << 32 | static_cast<u32>(y1)), hash_bytes(bytes, size));
    ++g_uploads_seen;
    for (std::size_t i = 0; i < c.blocks.size(); ++i) {
        const std::uint16_t b = c.blocks[i];
        const bool exact = whole_rows && full[i];
        g_uploads_full += exact;
        g_block_id[b] = (exact ? mix(chunk, i) : mix(mix(g_block_id[b], chunk), i)) | 1;
        g_block_seq[b] = g_seq, g_block_who[b] = kUpload, g_page_seq[b >> 5] = g_seq;
    }
    if (g_replay) push_upload_rec(c);
}

void state_loaded() {
    Cover all;
    for (u32 b = 0; b < 16384; ++b) all.blocks.push_back(static_cast<std::uint16_t>(b));
    write_cover(all, kState);
    ++g_epoch;
    if (!g_clut.empty()) std::memset(g_clut.data(), 0, g_clut.size() * sizeof(Clut));
}

static void clut_apply(u32 from, u32 to, u32 cbp, u32 cpsm, u32 csm, u32 csa, u32 is8, u32 cbw, u32 cou, u32 cov) {
    if (g_clut.empty()) g_clut.assign(2048, Clut{});
    from %= static_cast<u32>(g_clut.size());
    to %= static_cast<u32>(g_clut.size());
    Cover c;
    if (csm) cover_rect(cbp, cbw, cpsm, static_cast<int>(cou * 16), static_cast<int>(cov), static_cast<int>(cou * 16 + 255), static_cast<int>(cov), c);
    else cover_rect(cbp, 1, cpsm, 0, 0, 15, 15, c);
    u64 id = mix(mix(mix(mix(cbp, cpsm), csm * 1000003ull + cbw), cou * 4099ull + cov), fold_identity(c));  // the palette is what its memory holds
    id |= 1;
    Clut next = g_clut[from];
    if (is8) {
        for (u32 g = 0; g < 16; ++g) next.g[(csa + g) & 31] = mix(id, g);
    } else {
        next.g[csa & 31] = id;
    }
    g_clut[to] = next;
}

void clut_load(u32 from, u32 to, u32 cbp, u32 cpsm, u32 csm, u32 csa, u32 is8, u32 cbw, u32 cou, u32 cov) {
    clut_apply(from, to, cbp, cpsm, csm, csa, is8, cbw, cou, cov);
    if (g_replay) {
        const u32 args[10] = {from, to, cbp, cpsm, csm, csa, is8, cbw, cou, cov};
        push_clut_rec(args);
    }
}

Request request_of(const u32 *w) {
    Request r{};
    r.tbp = w[12], r.tbw = w[13], r.tpsm = w[14];
    r.tw = w[15] & 0xFFu, r.th = w[15] >> 8;
    r.cpsm = w[16], r.csa = w[17];
    r.texa = w[20], r.texa_on = (w[0] & 32768u) ? 1u : 0u;
    r.clut_slot = w[22];
    return r;
}

namespace {
BaseKey base_key(const Request &r) {
    const bool expand = alpha_expanding(r.tpsm, r.cpsm);
    return {r.tbp, r.tbw, r.tpsm, r.tw | (r.th << 8), indexed(r.tpsm) ? (r.cpsm | (r.csa << 8)) : 0u, expand ? r.texa : 0u, expand ? r.texa_on : 0u,
            g_replay && indexed(r.tpsm) ? r.clut_slot + 1 : 0u};  // the replay's plan has one place per texture and palette slot: two palettes of one texture in one batch need two
}
Base &base_of(const Request &r, const BaseKey &bk) {
    auto [it, fresh_base] = g_bases.try_emplace(bk);
    Base &B = it->second;
    if (fresh_base) {
        B.words = 1u << (r.tw + r.th);
        cover_rect(r.tbp, r.tbw, r.tpsm, 0, 0, (1 << r.tw) - 1, (1 << r.th) - 1, B.cover);
    }
    return B;
}
struct Totals {
    u64 lookups = 0, hits = 0, pictures = 0, misses = 0, miss_bytes = 0;
} g_tot;
}  // namespace

u32 reserve(const Request &r) {
    ensure_space();
    Base &B = base_of(r, base_key(r));
    if (B.pinned == ~0u) {
        u32 off;
        if (!alloc_main(B.words, off)) return ~0u;
        B.pinned = off;
    }
    return B.pinned;
}

void stats_reset() { g_tot = Totals(); }
void stats_print(const char *label) {
    std::fprintf(stderr, "[cache] %s: %llu lookups, %llu hits, %llu pictures (decoded again: the texture is memory the card wrote), %llu misses decoded (%.2f MB)\n", label, static_cast<unsigned long long>(g_tot.lookups),
                 static_cast<unsigned long long>(g_tot.hits), static_cast<unsigned long long>(g_tot.pictures), static_cast<unsigned long long>(g_tot.misses), static_cast<double>(g_tot.miss_bytes) / 1048576.0);
}

Lookup lookup(const Request &r) {
    Lookup out;
    ensure_space();
    const u64 pal = palette_id(r);
    const BaseKey bk = base_key(r);
    ++g_cur.requests;
    if (indexed(r.tpsm)) ++g_cur.pal_requests;
    Base &B = base_of(r, bk);
    if (!B.checked || pages_newer(B.cover, B.checked)) {  // something was written near this texture: work out what the memory holds now
        const u64 id = fold_identity(B.cover);
        if (id != B.identity) B.picture = is_picture(B.cover);
        B.identity = id;
    }
    B.checked = g_seq;
    // is there a decoded copy of exactly this memory and palette?
    u32 found = ~0u;
    for (u32 v : B.versions)
        if (g_entries[v].identity == B.identity && g_entries[v].pal == pal) found = v;
    const bool valid = found != ~0u;
    const bool first_in_frame = B.last_frame != g_frame;
    const u64 bytes = static_cast<u64>(B.words) * 4;
    Cause cause = kHit;
    if (!valid) {
        if (!B.ever) cause = kNew;
        else if (B.identity == B.last_identity) cause = B.last_pal != pal ? kNewPalette : kEvicted;
        else cause = cause_of(B);
    }
    const bool picture = B.picture;
    if (first_in_frame) {
        g_cur.cell[picture][cause].add(bytes);
        if (valid && B.last_frame + 1 == g_frame) g_cur.hit_prev[picture].add(bytes);
        B.rebuilds = valid ? 0 : (B.ever ? B.rebuilds + 1 : 0);
    }
    if (!valid) {
        g_cur.cold[picture].add(bytes);
        if (indexed(r.tpsm) && !picture) g_cur.cold_pal.add(bytes);
        if (picture) {  // a picture the game drew: it changes with every draw, the card path samples it directly; the cache keeps nothing of it
            B.last_frame = g_frame;
            out.picture = true;
            out.refused = true;
            while (B.pinned != ~0u && !B.versions.empty()) drop(B.versions.back());  // the caller decodes into the fixed place anyway: what was kept there is gone
            out.offset = B.pinned == ~0u ? 0 : B.pinned;
            out.words = B.words;
            ++g_tot.pictures;
            return out;
        }
        // a texture the game rewrites again and again is a stream: its copies go to the ring so that it takes nothing from the rest
        const bool to_ring = g_allow_move && !picture && B.rebuilds >= g_stream_after;
        u32 off = 0;
        bool ok = false;
        const bool pinned = B.pinned != ~0u;
        if (pinned) ok = true, off = B.pinned;
        else if (to_ring) ok = alloc_ring(B.words, off), g_cur.streamed += ok;
        if (!ok) ok = make_room(B.words, off);
        if (!ok) {
            ++g_cur.refused;
            out.refused = true;
            B.last_frame = g_frame;
            return out;
        }
        while (B.versions.size() >= (to_ring || pinned ? 1u : 4u)) {  // a few copies at most per texture: the oldest goes
            u32 oldest = B.versions.front();
            for (u32 v : B.versions)
                if (g_entries[v].last_frame < g_entries[oldest].last_frame) oldest = v;
            drop(oldest);
        }
        Entry e;
        e.base = &B;
        e.identity = B.identity, e.pal = pal;
        e.offset = off;
        e.stream = ok && to_ring && !pinned;
        e.pinned = pinned;
        e.alive = true;
        found = static_cast<u32>(g_entries.size());
        g_entries.push_back(e);
        B.versions.push_back(found);
        B.ever = true;
        B.last_built = g_seq, B.last_identity = B.identity, B.last_pal = pal;
        ++g_tot.misses, g_tot.miss_bytes += bytes;
    } else {
        ++g_tot.hits;
    }
    ++g_tot.lookups;
    Entry &e = g_entries[found];
    e.last_frame = g_frame;
    B.last_frame = g_frame;
    // the index form (one byte a texel, the palette apart) as a shadow: what it would decode
    if (indexed(r.tpsm) && !picture) {
        if (B.idx_identity != B.identity) g_cur.idx_cold.add(B.words), B.idx_identity = B.identity;
        if (g_pal_seen_frame.insert(pal).second) g_cur.idx_palettes.add(1024);
    }
    out.hit = valid;
    out.picture = picture;
    out.entry = found;
    out.offset = e.offset;
    out.words = B.words;
    out.alpha_known = e.alpha_known;
    out.alpha_min = e.amin, out.alpha_max = e.amax;
    return out;
}

void set_alpha(u32 entry, u8 lo, u8 hi) {
    if (entry >= g_entries.size() || !g_entries[entry].alive) return;
    g_entries[entry].alpha_known = true;
    g_entries[entry].amin = lo, g_entries[entry].amax = hi;
}

void batch_textures(const u32 *setups, u32 count, u32 stride) {
    for (u32 i = 0; i < count; ++i) {
        const u32 *w = setups + static_cast<std::size_t>(i) * stride;
        if (!(w[0] & 2u)) continue;
        lookup(request_of(w));
    }
}


// ---- the replay of the card path. The uploads and palette loads of the captured frame are remembered with the identities they gave their blocks, so that every replay can
// put the same identities back at the same places in its sequence of commands.
void capture_pre(u32 sub, u32 command, std::size_t first, std::size_t last) {
    if (!g_replay) return;
    auto &v = g_recorded[(static_cast<u64>(sub) << 32) | command];
    for (std::size_t i = first; i < last && i < g_pending.size(); ++i) v.push_back(g_pending[i]);
}

void pending_clear() { g_pending.clear(); }

void replay_snapshot() {
    if (!g_replay) return;
    std::memcpy(g_snap_id, g_block_id, sizeof g_snap_id);
    if (g_clut.empty()) g_clut.assign(2048, Clut{});
    g_snap_clut = g_clut;
    g_snapped = true;
    g_recorded.clear();
}

void replay_restore() {
    if (!g_snapped) return;
    ++g_seq;
    for (u32 b = 0; b < 16384; ++b)
        if (g_block_id[b] != g_snap_id[b]) g_block_id[b] = g_snap_id[b], g_block_seq[b] = g_seq, g_block_who[b] = kState, g_page_seq[b >> 5] = g_seq;
    g_clut = g_snap_clut;
}

void forget() {
    for (u32 i = 0; i < g_entries.size(); ++i) drop(i);
}

void replay_compute(u32 sub, u32 command) {
    const auto it = g_recorded.find((static_cast<u64>(sub) << 32) | command);
    if (it == g_recorded.end()) {  // a command nobody described: it may have written anything
        Pages all;
        all.set();
        write_pages(all, kCopy);
        return;
    }
    for (const Rec &r : it->second) {
        if (r.clut) {
            clut_apply(r.args[0], r.args[1], r.args[2], r.args[3], r.args[4], r.args[5], r.args[6], r.args[7], r.args[8], r.args[9]);
            continue;
        }
        ++g_seq;
        for (const auto &bi : r.blocks) g_block_id[bi.first] = bi.second, g_block_seq[bi.first] = g_seq, g_block_who[bi.first] = kUpload, g_page_seq[bi.first >> 5] = g_seq;
    }
}

// ---- the end of a frame, the checks and the report
namespace {
struct Verify {
    bool have = false;
    u64 hash[16384];
    u64 id[16384];
    u64 changed = 0, missed = 0, missed_textured = 0, false_textured = 0, false_inval[kWriters] = {}, true_change[kWriters] = {};
    u64 frames = 0;
    u32 first_missed_block = ~0u, first_missed_frame = 0;
    u8 first_missed_who = 0;
} g_ver;

double mb(u64 bytes) { return static_cast<double>(bytes) / 1048576.0; }
}  // namespace

void frame_end() {
    for (u32 i = 0; i < g_entries.size(); ++i)  // age out copies nobody asked for lately
        if (g_entries[i].alive && g_frame - g_entries[i].last_frame > g_max_age) drop(i);
    if (g_count) {
        const u64 cold = g_cur.cold[0].bytes;
        if (g_lines) {
            u64 hit[2] = {g_cur.cell[0][kHit].bytes, g_cur.cell[1][kHit].bytes}, all[2] = {0, 0};
            for (int p = 0; p < 2; ++p)
                for (int c = 0; c < kCauses; ++c) all[p] += g_cur.cell[p][c].bytes;
            std::fprintf(stderr, "[texstat] frame %llu: %llu requests; uploaded kind %.2f MB of which unchanged %.2f MB; pictures %.2f MB; cold decode %.2f MB (+%.2f MB pictures)\n",
                         static_cast<unsigned long long>(g_frame), static_cast<unsigned long long>(g_cur.requests), mb(all[0]), mb(hit[0]), mb(all[1]), mb(cold), mb(g_cur.cold[1].bytes));
        }
        if (g_frame >= g_skip) {
            add_frame(g_sum, g_cur);
            ++g_frames_counted;
            g_per_frame_cold.push_back(cold);
        }
    }
    g_cur = Frame();
    g_pal_seen_frame.clear();
    ++g_frame;
}

// The memory as the card holds it, against what the numbers say (PS2_GPU_TEXSTAT=3). A block that changed although its identity did not is a writer nobody told us about.
// A block whose identity changed although its bytes did not costs a decode that was not needed.
void verify(const u8 *memory) {
    Verify &v = g_ver;
    std::vector<char> textured(16384, 0);
    for (const auto &kv : g_bases)
        for (std::uint16_t b : kv.second.cover.blocks) textured[b] = 1;
    for (int b = 0; b < 16384; ++b) {
        u64 h = 1469598103934665603ull;
        const u64 *w = reinterpret_cast<const u64 *>(memory + static_cast<std::size_t>(b) * 256);
        for (int i = 0; i < 32; ++i) h = (h ^ w[i]) * 1099511628211ull;
        if (v.have) {
            const bool changed = h != v.hash[b], bumped = g_block_id[b] != v.id[b];
            if (changed) ++v.changed;
            if (changed && !bumped) {
                ++v.missed;
                v.missed_textured += textured[b];
                if (v.first_missed_block == ~0u) v.first_missed_block = static_cast<u32>(b), v.first_missed_frame = static_cast<u32>(g_frame), v.first_missed_who = g_block_who[b];
            }
            if (bumped && !changed) ++v.false_inval[g_block_who[b]], v.false_textured += textured[b];
            if (bumped && changed) ++v.true_change[g_block_who[b]];
        }
        v.hash[b] = h;
        v.id[b] = g_block_id[b];
    }
    v.have = true;
    ++v.frames;
}

void report() {
    if (!g_count || !g_frames_counted) return;
    static const char *const kWho[kWriters] = {"-", "upload", "draw", "copy", "write-back", "state"};
    const double n = static_cast<double>(g_frames_counted);
    const Frame &s = g_sum;
    std::fprintf(stderr, "[texstat] ---- %llu frames counted (the first %u left out), means per frame; sizes are the declared size of the texture decoded to 32-bit words\n",
                 static_cast<unsigned long long>(g_frames_counted), g_skip);
    for (int p = 0; p < 2; ++p) {
        u64 all_n = 0, all_b = 0;
        for (int c = 0; c < kCauses; ++c) all_n += s.cell[p][c].n, all_b += s.cell[p][c].bytes;
        std::fprintf(stderr, "[texstat] %s: %.1f distinct textures, %.2f MB; unchanged %.1f%% of the bytes (%.1f%% were also asked for in the previous frame); cold decode %.2f MB a frame\n",
                     p ? "pictures the game drew (direct sampling serves them)" : "uploaded textures (what the cache is for)", static_cast<double>(all_n) / n, mb(all_b) / n,
                     all_b ? 100.0 * static_cast<double>(s.cell[p][kHit].bytes) / static_cast<double>(all_b) : 0.0,
                     all_b ? 100.0 * static_cast<double>(s.hit_prev[p].bytes) / static_cast<double>(all_b) : 0.0, mb(s.cold[p].bytes) / n);
        std::fprintf(stderr, "[texstat]   outcome:");
        for (int c = 0; c < kCauses; ++c)
            if (s.cell[p][c].n) std::fprintf(stderr, " %s %.1f (%.2f MB)", kCauseName[c], static_cast<double>(s.cell[p][c].n) / n, mb(s.cell[p][c].bytes) / n);
        std::fprintf(stderr, "\n");
    }
    u64 tb = 0, th = 0;
    for (int p = 0; p < 2; ++p)
        for (int c = 0; c < kCauses; ++c) tb += s.cell[p][c].bytes, th += c == kHit ? s.cell[p][c].bytes : 0;
    std::fprintf(stderr, "[texstat] host uploads: %llu in all, %llu blocks written whole (their identity comes from the bytes alone)\n", static_cast<unsigned long long>(g_uploads_seen),
                 static_cast<unsigned long long>(g_uploads_full));
    std::fprintf(stderr, "[texstat] all textures together: unchanged %.1f%% of the bytes; requests %.1f a frame (%.1f for palette formats); refused %.2f, streamed %.2f, hot evictions %.2f a frame\n",
                 tb ? 100.0 * static_cast<double>(th) / static_cast<double>(tb) : 0.0, static_cast<double>(s.requests) / n, static_cast<double>(s.pal_requests) / n,
                 static_cast<double>(s.refused) / n, static_cast<double>(s.streamed) / n, static_cast<double>(s.evicted_hot) / n);
    std::vector<u64> sorted = g_per_frame_cold;
    std::sort(sorted.begin(), sorted.end());
    std::fprintf(stderr, "[texstat] cold decode of uploaded textures per frame: mean %.2f MB, median %.2f, 95th %.2f, worst %.2f MB (the gate: mean at most about 30 MB)\n",
                 mb(s.cold[0].bytes) / n, mb(sorted[sorted.size() / 2]), mb(sorted[std::min(sorted.size() - 1, sorted.size() * 95 / 100)]), mb(sorted.back()));
    std::fprintf(stderr, "[texstat] palette textures, uploaded kind (step 4): decoded to 32 bit per palette %.2f MB a frame; index form (1 byte a texel, palette apart) %.2f MB + %.1f new palettes (%.3f MB) a frame\n",
                 mb(s.cold_pal.bytes) / n, mb(s.idx_cold.bytes) / n, static_cast<double>(s.idx_palettes.n) / n, mb(s.idx_palettes.bytes) / n);
    if (g_verify) {
        const Verify &v = g_ver;
        std::fprintf(stderr, "[texstat] verify (%llu frames, memory compared block by block, 256 bytes): %llu blocks changed; changed with the same identity (a missed writer): %llu (%llu in blocks a texture covers)",
                     static_cast<unsigned long long>(v.frames), static_cast<unsigned long long>(v.changed), static_cast<unsigned long long>(v.missed), static_cast<unsigned long long>(v.missed_textured));
        if (v.missed) std::fprintf(stderr, ", first: block %u in frame %u, last known writer %s", v.first_missed_block, v.first_missed_frame, kWho[v.first_missed_who]);
        std::fprintf(stderr, "\n[texstat]   identity changed but bytes equal (a decode that was not needed) by writer:");
        for (int w = 1; w < kWriters; ++w) std::fprintf(stderr, " %s %llu", kWho[w], static_cast<unsigned long long>(v.false_inval[w]));
        std::fprintf(stderr, " (%llu in blocks a texture covers); identity and bytes changed:", static_cast<unsigned long long>(v.false_textured));
        for (int w = 1; w < kWriters; ++w) std::fprintf(stderr, " %s %llu", kWho[w], static_cast<unsigned long long>(v.true_change[w]));
        std::fprintf(stderr, "\n");
    }
}

#ifndef __EMSCRIPTEN__
}  // namespace ps2::gs::gpu::hwcache
#include <webgpu/wgpu.h>
extern const char kCacheShader[];
namespace ps2::gs::gpu::hwcache {

namespace {
WGPUStringView sv(const char *t) { return WGPUStringView{t, std::strlen(t)}; }
WGPUBuffer make(WGPUDevice d, u64 size, WGPUBufferUsage usage) {
    WGPUBufferDescriptor bd = WGPU_BUFFER_DESCRIPTOR_INIT;
    bd.size = std::max<u64>(size, 16);
    bd.usage = usage;
    return wgpuDeviceCreateBuffer(d, &bd);
}
bool read_buffer(WGPUDevice dev, WGPUQueue q, WGPUBuffer src, u64 bytes, std::vector<u32> &out) {
    WGPUBuffer rb = make(dev, bytes, WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst);
    WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(dev, nullptr);
    wgpuCommandEncoderCopyBufferToBuffer(enc, src, 0, rb, 0, bytes);
    WGPUCommandBuffer cb = wgpuCommandEncoderFinish(enc, nullptr);
    wgpuQueueSubmit(q, 1, &cb);
    wgpuCommandBufferRelease(cb);
    wgpuCommandEncoderRelease(enc);
    bool done = false;
    WGPUBufferMapCallbackInfo mi = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
    mi.mode = WGPUCallbackMode_AllowSpontaneous;
    mi.callback = [](WGPUMapAsyncStatus, WGPUStringView, void *flag, void *) { *static_cast<bool *>(flag) = true; };
    mi.userdata1 = &done;
    wgpuBufferMapAsync(rb, WGPUMapMode_Read, 0, bytes, mi);
    while (!done) wgpuDevicePoll(dev, true, nullptr);
    const void *p = wgpuBufferGetConstMappedRange(rb, 0, bytes);
    out.resize(bytes / 4);
    if (p) std::memcpy(out.data(), p, bytes);
    wgpuBufferUnmap(rb);
    wgpuBufferRelease(rb);
    return p != nullptr;
}
std::vector<u32> g_alpha_ids;
}  // namespace

std::size_t alpha_ranges(WGPUDevice dev, WGPUQueue q, WGPUBuffer decoded, u64 decoded_bytes) {
    std::vector<u32> table;
    g_alpha_ids.clear();
    for (u32 i = 0; i < g_entries.size(); ++i)
        if (g_entries[i].alive) table.push_back(g_entries[i].offset), table.push_back(g_entries[i].base->words), g_alpha_ids.push_back(i);
    if (g_alpha_ids.empty()) return 0;
    const u64 n = g_alpha_ids.size();
    WGPUBuffer tb = make(dev, table.size() * 4, WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst);
    WGPUBuffer ob = make(dev, n * 8, WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst | WGPUBufferUsage_CopySrc);
    wgpuQueueWriteBuffer(q, tb, 0, table.data(), table.size() * 4);
    std::vector<u32> init(n * 2);
    for (u64 i = 0; i < n; ++i) init[i * 2] = 255, init[i * 2 + 1] = 0;
    wgpuQueueWriteBuffer(q, ob, 0, init.data(), init.size() * 4);
    WGPUShaderSourceWGSL ws = WGPU_SHADER_SOURCE_WGSL_INIT;
    ws.code = sv(kCacheShader);
    WGPUShaderModuleDescriptor md = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
    md.nextInChain = &ws.chain;
    WGPUShaderModule mod = wgpuDeviceCreateShaderModule(dev, &md);
    WGPUComputePipelineDescriptor pd = WGPU_COMPUTE_PIPELINE_DESCRIPTOR_INIT;
    pd.compute.module = mod;
    pd.compute.entryPoint = sv("alpha_range");
    WGPUComputePipeline pipe = wgpuDeviceCreateComputePipeline(dev, &pd);
    if (!pipe) return 0;
    WGPUBindGroupEntry e[3] = {WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT};
    e[0].binding = 0, e[0].buffer = decoded, e[0].size = decoded_bytes;
    e[1].binding = 1, e[1].buffer = tb, e[1].size = std::max<u64>(table.size() * 4, 16);
    e[2].binding = 2, e[2].buffer = ob, e[2].size = std::max<u64>(n * 8, 16);
    WGPUBindGroupDescriptor gd = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    gd.layout = wgpuComputePipelineGetBindGroupLayout(pipe, 0);
    gd.entryCount = 3;
    gd.entries = e;
    WGPUBindGroup group = wgpuDeviceCreateBindGroup(dev, &gd);
    WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(dev, nullptr);
    WGPUComputePassEncoder cp = wgpuCommandEncoderBeginComputePass(enc, nullptr);
    wgpuComputePassEncoderSetPipeline(cp, pipe);
    wgpuComputePassEncoderSetBindGroup(cp, 0, group, 0, nullptr);
    wgpuComputePassEncoderDispatchWorkgroups(cp, 64, static_cast<u32>(n), 1);
    wgpuComputePassEncoderEnd(cp);
    wgpuComputePassEncoderRelease(cp);
    WGPUCommandBuffer cb = wgpuCommandEncoderFinish(enc, nullptr);
    wgpuQueueSubmit(q, 1, &cb);
    wgpuCommandBufferRelease(cb);
    wgpuCommandEncoderRelease(enc);
    std::vector<u32> res;
    if (!read_buffer(dev, q, ob, n * 8, res)) return 0;
    for (u64 i = 0; i < n; ++i) set_alpha(g_alpha_ids[i], static_cast<u8>(res[i * 2]), static_cast<u8>(res[i * 2 + 1]));
    if (const char *chk = std::getenv("PS2_GPU_HW_CACHE_ALPHA_CHECK"); chk && *chk == '1') {  // the same numbers counted on the processor from the decoded words
        std::vector<u32> words;
        read_buffer(dev, q, decoded, decoded_bytes, words);
        u64 bad = 0;
        for (u64 i = 0; i < n; ++i) {
            const Entry &en = g_entries[g_alpha_ids[i]];
            u32 lo = 255, hi = 0;
            for (u32 k = 0; k < en.base->words; ++k) lo = std::min(lo, words[en.offset + k] >> 24), hi = std::max(hi, words[en.offset + k] >> 24);
            bad += lo != res[i * 2] || hi != res[i * 2 + 1];
        }
        std::fprintf(stderr, "[cache] alpha ranges checked against a count on the processor: %llu of %llu differ\n", static_cast<unsigned long long>(bad), static_cast<unsigned long long>(n));
    }
    wgpuBindGroupRelease(group);
    wgpuComputePipelineRelease(pipe);
    wgpuShaderModuleRelease(mod);
    wgpuBufferRelease(tb);
    wgpuBufferRelease(ob);
    return static_cast<std::size_t>(n);
}

void alpha_summary(std::size_t measured) {
    u64 constant = 0, never = 0, opaque = 0, wide = 0;
    for (u32 id : g_alpha_ids) {
        const Entry &e = g_entries[id];
        if (!e.alpha_known) continue;
        constant += e.amin == e.amax;
        never += e.amax == 0;
        opaque += e.amin >= 0x80;
        wide += e.amin == 0 && e.amax > 0;
    }
    std::fprintf(stderr, "[cache] alpha ranges of %zu decoded copies (units of the decode pass, 0x80 = 1.0): constant %llu, never visible (largest 0) %llu, fully opaque (smallest >= 0x80) %llu, some zero and some visible %llu\n", measured,
                 static_cast<unsigned long long>(constant), static_cast<unsigned long long>(never), static_cast<unsigned long long>(opaque), static_cast<unsigned long long>(wide));
}
#endif

}  // namespace ps2::gs::gpu::hwcache
