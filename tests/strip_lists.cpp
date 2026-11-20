// The tile lists kept per strip of two pixel rows (runtime/src/gs/gpu/tiles.cpp with strips, PS2_GPU_STRIPS) against the lists per tile: the same
// tiles in the same order, and every strip's list is its tile's list without the shapes whose box misses the strip's two rows, in the same order (the
// raster program's box test would throw those out for every pixel of the strip). Also built in steps on the helper thread. No game needed:
//
//   ps2striplisttest                 random batches, scales 1 to 4, tiles of 8 and 4 pixels
//   ps2striplisttest <dump> [reps]   batches recorded from a game (a file of {u32 shapes, shapes * 28 words}, as ps2tilelisttest reads), checked, and
//                                    both builders timed
#include "gs/gpu/setup.hpp"
#include "gs/gpu/tiles.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

using ps2::gs::Vertex;
namespace tiles = ps2::gs::gpu::tiles;
namespace ps = ps2::gs::gpu::prim_setup;
using tiles::kPrimWords;

namespace {

int g_scale = 1, g_tile = 8;

std::vector<std::uint32_t> g_out;
std::uint32_t *room(void *, std::size_t words) {
    g_out.assign(words, 0xDEADBEEFu);  // the builder must write every word
    return g_out.data();
}

struct Built {
    std::vector<std::uint32_t> words;
    tiles::Lists lists;
};

Built build(const std::vector<std::uint32_t> &prims, bool strips) {
    tiles::configure(g_scale, g_tile, strips);
    Built b;
    b.lists = tiles::build_now(prims.data(), static_cast<std::uint32_t>(prims.size() / kPrimWords), room, nullptr);
    b.words = g_out;
    return b;
}

std::uint16_t entry(const Built &b, std::uint32_t at) {
    const std::uint32_t w = b.words[b.lists.table + at / 2];
    return static_cast<std::uint16_t>(w >> ((at & 1u) * 16u));
}

// Whether the box of shape `prim` reaches strip `g` (rows 2g and 2g + 1 of the upscaled tile) of tile row `ty`.
bool reaches(const std::vector<std::uint32_t> &prims, std::uint32_t prim, std::uint32_t ty, std::uint32_t g) {
    const int y0 = static_cast<int>(prims[prim * kPrimWords + 2] >> 16), y1 = static_cast<int>(prims[prim * kPrimWords + 3] >> 16);
    for (int r = 0; r < 2; ++r) {
        const int y = (static_cast<int>(ty) * g_tile + 2 * static_cast<int>(g) + r) / g_scale;
        if (y >= y0 && y <= y1) return true;
    }
    return false;
}

// The lists per strip against the lists per tile; returns the number of differences found (printing the first few).
std::size_t compare(const std::vector<std::uint32_t> &prims, const Built &tile, const Built &strip, const char *what, std::size_t batch) {
    const std::uint32_t strips = static_cast<std::uint32_t>(g_tile / 2), entry_words = strips + 2;
    std::size_t bad = 0;
    const auto fail = [&](const char *why, std::uint32_t at) {
        if (bad++ < 5) std::printf("  %s batch %zu: %s (tile %u)\n", what, batch, why, at);
    };
    if (strip.lists.tiles != tile.lists.tiles) {
        fail("another number of tiles", 0);
        return bad;
    }
    if (strip.lists.table != tile.lists.tiles * entry_words + 2 || strip.lists.words != strip.words.size()) fail("wrong table size", 0);
    std::uint32_t expect_at = 0, total = 0;
    for (std::uint32_t i = 0; i < tile.lists.tiles; ++i) {
        const std::uint32_t *const te = tile.words.data() + i * 2;
        const std::uint32_t *const se = strip.words.data() + i * entry_words;
        if (te[0] != se[0]) fail("another tile in this place", i);
        const std::uint32_t ty = te[0] >> 16;
        for (std::uint32_t g = 0; g < strips; ++g) {
            std::vector<std::uint16_t> want;
            for (std::uint32_t h = te[1]; h < te[3]; ++h)
                if (reaches(prims, entry(tile, h), ty, g)) want.push_back(entry(tile, h));
            if (se[1 + g] != expect_at) fail("a strip's list does not start where the one before ends", i);
            std::vector<std::uint16_t> got;
            for (std::uint32_t h = se[1 + g]; h < se[2 + g]; ++h) got.push_back(entry(strip, h));
            if (got != want) {
                if (bad < 2) std::printf("  strip %u of tile %08x: %zu wanted (first %d), %zu got (first %d)\n", g, te[0], want.size(), want.empty() ? -1 : want[0], got.size(), got.empty() ? -1 : got[0]);
                fail("a strip's list differs", i);
            }
            expect_at = se[2 + g];
            total += static_cast<std::uint32_t>(want.size());
        }
    }
    if (total != strip.lists.total) fail("wrong number of entries", 0);
    const std::uint32_t *const close = strip.words.data() + strip.lists.tiles * entry_words;
    if (close[0] != 0 || close[1] != total) fail("wrong closing entry", 0);
    return bad;
}

// One random batch: shapes of every kind, sizes from below a pixel to the whole screen, clustered like a model's mesh (as ps2tilelisttest makes them).
std::vector<std::uint32_t> random_batch(std::mt19937 &rng, std::uint32_t shapes) {
    std::vector<std::uint32_t> out;
    const std::uint32_t full = 2047u << 16;
    float cx = 320, cy = 224;
    for (std::uint32_t i = 0; i < shapes; ++i) {
        std::uint32_t rec[kPrimWords] = {};
        int box[4];
        const std::uint32_t pick = rng() % 100;
        if (pick < 3) cx = static_cast<float>(rng() % 1400) - 100.0f, cy = static_cast<float>(rng() % 1000) - 100.0f;
        const float size = pick < 40 ? 1.5f : pick < 70 ? 6.0f : pick < 90 ? 30.0f : pick < 98 ? 200.0f : 2500.0f;
        const auto coord = [&](float c) {
            std::uniform_real_distribution<float> d(-size, size);
            const float v = std::clamp(c + d(rng), -2000.0f, 3800.0f);
            return static_cast<std::uint32_t>(static_cast<int>(v * 16.0f) + 0x8000);
        };
        const std::uint32_t kind = rng() % 20;
        if (kind < 16) {
            Vertex v[3] = {};
            for (Vertex &p : v) p.x = coord(cx), p.y = coord(cy);
            if (rng() % 8 == 0) v[2].x = v[0].x;
            if (rng() % 8 == 0) v[1].y = v[0].y;
            if (!ps::triangle(rec, box, v, 0x8000, 0x8000, 1.0f, full, full, false, true)) continue;
        } else {
            rec[1] = kind < 18 ? 1u : kind < 19 ? 2u : 3u;
            const int x0 = static_cast<int>(coord(cx) - 0x8000) / 16, y0 = static_cast<int>(coord(cy) - 0x8000) / 16;
            const int x1 = static_cast<int>(coord(cx) - 0x8000) / 16, y1 = static_cast<int>(coord(cy) - 0x8000) / 16;
            box[0] = std::min(x0, x1), box[1] = std::min(y0, y1), box[2] = std::max(x0, x1), box[3] = std::max(y0, y1);
        }
        box[0] = std::max(box[0], 0), box[1] = std::max(box[1], 0), box[2] = std::min(box[2], 2047), box[3] = std::min(box[3], 2047);
        if (box[0] > box[2] || box[1] > box[3]) continue;
        rec[2] = static_cast<std::uint32_t>(box[0]) | (static_cast<std::uint32_t>(box[1]) << 16);
        rec[3] = static_cast<std::uint32_t>(box[2]) | (static_cast<std::uint32_t>(box[3]) << 16);
        out.insert(out.end(), rec, rec + kPrimWords);
    }
    return out;
}

std::size_t check(const std::vector<std::vector<std::uint32_t>> &batches, const char *what) {
    std::size_t bad = 0, entries_tile = 0, entries_strip = 0;
    for (std::size_t i = 0; i < batches.size(); ++i) {
        const Built t = build(batches[i], false), s = build(batches[i], true);
        entries_tile += t.lists.total;
        entries_strip += s.lists.total;
        bad += compare(batches[i], t, s, what, i);
        if (t.lists.strips || !s.lists.strips) bad += 1, std::printf("  %s batch %zu: wrong Lists::strips\n", what, i);
        // with strips on but the batch kept per tile (ratio 0): exactly the lists per tile
        tiles::configure(g_scale, g_tile, true, 0.0);
        const tiles::Lists kept = tiles::build_now(batches[i].data(), static_cast<std::uint32_t>(batches[i].size() / kPrimWords), room, nullptr);
        if (kept.strips || g_out != t.words || kept.table != t.lists.table) bad += 1, std::printf("  %s batch %zu: a batch kept per tile differs\n", what, i);
    }
    std::printf("%s: %zu batches, %zu entries per tile, %zu per strip (%.2f per tile entry), %zu differ\n", what, batches.size(), entries_tile, entries_strip,
                entries_tile ? static_cast<double>(entries_strip) / static_cast<double>(entries_tile) : 0.0, bad);
    return bad;
}

// Built in steps on the helper (open, feed, close) with strips: the same words as built at once.
std::size_t check_stream(const std::vector<std::vector<std::uint32_t>> &batches, const char *what) {
    std::mt19937 rng(77u);
    std::size_t bad = 0, streamed = 0;
    for (std::size_t i = 0; i < batches.size(); ++i) {
        const std::vector<std::uint32_t> &prims = batches[i];
        const std::uint32_t n = static_cast<std::uint32_t>(prims.size() / kPrimWords);
        if (n < 2) continue;
        const Built want = build(prims, true);
        std::uint32_t fed = 1 + rng() % (n - 1);
        tiles::Job *job = tiles::open(prims.data(), fed);
        if (!job) return bad;  // no helper in this build
        while (fed < n) {
            fed = std::min(n, fed + 1 + static_cast<std::uint32_t>(rng() % 2000));
            if (fed < n) tiles::feed(job, prims.data(), fed);
        }
        tiles::close(job, prims.data(), n);
        tiles::Job *const got_job = tiles::next_finished(true);
        if (got_job != job) {
            std::printf("  %s batch %zu: the helper returned another job\n", what, i);
            return bad + 1;
        }
        const std::vector<std::uint32_t> got(job->lists.data(), job->lists.data() + job->result.words);
        const bool same = got == want.words && job->result.table == want.lists.table && job->result.total == want.lists.total;
        tiles::release(job);
        ++streamed;
        if (!same) {
            if (bad < 5) std::printf("  %s batch %zu (%u shapes): the lists built in steps differ\n", what, i, n);
            ++bad;
        }
    }
    std::printf("%s: %zu batches built in steps on the helper, %zu differ\n", what, streamed, bad);
    return bad;
}

void bench(const std::vector<std::vector<std::uint32_t>> &batches, int reps) {
    std::size_t shapes = 0;
    for (const auto &b : batches) shapes += b.size() / kPrimWords;
    double best[2] = {1e30, 1e30};
    for (int k = 0; k < reps; ++k)
        for (int strips = 0; strips < 2; ++strips) {
            tiles::configure(g_scale, g_tile, strips != 0);
            const auto t0 = std::chrono::steady_clock::now();
            for (const auto &b : batches) tiles::build_now(b.data(), static_cast<std::uint32_t>(b.size() / kPrimWords), room, nullptr);
            const auto t1 = std::chrono::steady_clock::now();
            best[strips] = std::min(best[strips], std::chrono::duration<double, std::milli>(t1 - t0).count());
        }
    std::printf("bench: %zu batches, %zu shapes: per tile %.2f ms (%.1f ns a shape), per strip %.2f ms (%.1f ns a shape)\n", batches.size(), shapes, best[0],
                best[0] * 1e6 / static_cast<double>(shapes), best[1], best[1] * 1e6 / static_cast<double>(shapes));
}

}  // namespace

int main(int argc, char **argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::size_t bad = 0;
    if (argc > 1) {
        std::FILE *f = std::fopen(argv[1], "rb");
        if (!f) {
            std::printf("cannot open %s\n", argv[1]);
            return 2;
        }
        std::vector<std::vector<std::uint32_t>> batches;
        std::uint32_t n;
        while (std::fread(&n, 4, 1, f) == 1) {
            std::vector<std::uint32_t> b(static_cast<std::size_t>(n) * kPrimWords);
            if (std::fread(b.data(), 4, b.size(), f) != b.size()) break;
            batches.push_back(std::move(b));
        }
        std::fclose(f);
        bad += check(batches, argv[1]);
        bad += check_stream(batches, argv[1]);
        bench(batches, argc > 2 ? std::atoi(argv[2]) : 5);
        std::printf(bad ? "FAIL\n" : "PASS\n");
        return bad ? 1 : 0;
    }
    for (const int tile : {8, 4})
        for (int scale = 1; scale <= 4; ++scale) {
            g_scale = scale;
            g_tile = tile;
            std::mt19937 rng(4321u + static_cast<unsigned>(scale * 16 + tile));
            std::vector<std::vector<std::uint32_t>> batches;
            for (int i = 0; i < 60; ++i) batches.push_back(random_batch(rng, i % 10 == 0 ? 3000u : 1u + rng() % 600u));
            batches.push_back({});
            char what[48];
            std::snprintf(what, sizeof what, "random, scale %d, tile %d", scale, tile);
            bad += check(batches, what);
            if (tile == 8) bad += check_stream(batches, what);
        }
    std::printf(bad ? "FAIL\n" : "PASS\n");
    return bad ? 1 : 0;
}
