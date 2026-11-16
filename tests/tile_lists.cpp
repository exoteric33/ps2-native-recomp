// The tile lists of a drawing batch (runtime/src/gs/gpu/tiles.cpp) against the way they were built before (a copy of that code below, `ref`),
// word for word: the table of touched tiles, its order, the offsets and every list. No game needed: ninja ps2tilelisttest
//
//   ps2tilelisttest                 random batches (triangles from the drawing thread's set-up, sprites, lines, points), scales 1 to 4
//   ps2tilelisttest <dump> [reps]   batches recorded from a game (a file of {u32 shapes, shapes * 28 words}), checked and timed both ways
//   ps2tilelisttest bench [reps]    the random batches timed both ways
#include "gs/gpu/setup.hpp"
#include "gs/gpu/tiles.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <random>
#include <vector>

using ps2::gs::Vertex;
namespace tiles = ps2::gs::gpu::tiles;
namespace ps = ps2::gs::gpu::prim_setup;
using tiles::kKindMask;
using tiles::kPrimWords;

namespace ref {

// ---- the earlier tile-list builder, kept as the reference ----

int g_scale = 1, g_tile = 8, g_per_row = 256;

float u2f(std::uint32_t u) {
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}

struct Span {
    std::int16_t ty, tx0, tx1;
    std::uint16_t prim;
};

struct Builder {
    std::vector<std::uint32_t> count, fill, touched;
    std::vector<Span> spans;

    void prepare() {
        const std::size_t n = static_cast<std::size_t>(g_per_row) * static_cast<std::size_t>(g_per_row) + 8;
        if (count.size() == n) return;
        count.assign(n, 0);
        fill.assign(n, 0);
        touched.assign(n + 8, 0);
    }

    void add_spans(const std::uint32_t *rec, std::uint32_t lo, std::uint32_t hi, std::uint16_t prim) {
        const int sc = g_scale, tile = g_tile;
        const int tx0 = static_cast<int>(lo & 0xFFFFu) * sc / tile, ty0 = static_cast<int>(lo >> 16) * sc / tile;
        const int tx1 = (static_cast<int>(hi & 0xFFFFu) * sc + sc - 1) / tile, ty1 = (static_cast<int>(hi >> 16) * sc + sc - 1) / tile;
        const auto whole = [&] {
            for (int ty = ty0; ty <= ty1; ++ty) spans.push_back({static_cast<std::int16_t>(ty), static_cast<std::int16_t>(tx0), static_cast<std::int16_t>(tx1), prim});
        };
        if ((rec[1] & kKindMask) != 0 || tx1 - tx0 < 2) {
            whole();
            return;
        }
        typedef double V4 __attribute__((vector_size(32)));
        typedef long long M4 __attribute__((vector_size(32)));
        const double px0 = u2f(rec[4]), px1 = u2f(rec[6]), px2 = u2f(rec[8]), py0 = u2f(rec[5]), py1 = u2f(rec[7]), py2 = u2f(rec[9]);
        const double inv = u2f(rec[10]);
        const V4 xa = {px1, px2, px0, px0}, xb = {px2, px0, px1, px0}, ya = {py1, py2, py0, py0}, yb = {py2, py0, py1, py0};
        const V4 ca = (ya - yb) * inv, cb = (xb - xa) * inv, cc = (xa * yb - xb * ya) * inv;
        const V4 limit = {1.7976931348623157e308, 1.7976931348623157e308, 1.7976931348623157e308, 1.7976931348623157e308};
        const M4 finite = (__builtin_elementwise_abs(ca) <= limit) & (__builtin_elementwise_abs(cb) <= limit) & (__builtin_elementwise_abs(cc) <= limit);
        if (!(finite[0] & finite[1] & finite[2])) {
            whole();
            return;
        }
        const V4 zero = {0.0, 0.0, 0.0, 0.0};
        const M4 positive = ca > zero, negative = ca < zero, slanted = positive | negative;
        const V4 divided_m = (-cb) / ca, divided_q = (-cc) / ca;
        const V4 vm = (V4)(((M4)divided_m & slanted) | ((M4)cb & ~slanted)), vq = (V4)(((M4)divided_q & slanted) | ((M4)cc & ~slanted));
        const M4 kind_v = negative - positive;
        const int kind[3] = {static_cast<int>(kind_v[0]), static_cast<int>(kind_v[1]), static_cast<int>(kind_v[2])};
        const double span = static_cast<double>(tile) / static_cast<double>(sc);
        const bool span_pow2 = tile % sc == 0 && ((tile / sc) & (tile / sc - 1)) == 0;
        const double span_inverse = 1.0 / span;
        for (int ty = ty0; ty <= ty1; ++ty) {
            const double y0 = ty * span - 1.0, y1 = (ty + 1) * span + 1.0;
            const V4 a0 = vm * y0, a1 = vm * y1;
            const M4 smaller = a1 < a0, larger = a0 < a1;
            const V4 low = (V4)(((M4)a1 & smaller) | ((M4)a0 & ~smaller)) + vq, high = (V4)(((M4)a1 & larger) | ((M4)a0 & ~larger)) + vq;
            double left = -1e30, right = 1e30;
            bool empty = false;
            for (int e = 0; e < 3; ++e) {
                if (kind[e] == 0) empty = empty || high[e] < 0.0;
                else if (kind[e] > 0) left = std::max(left, low[e]);
                else right = std::min(right, high[e]);
            }
            left -= 1.0;
            right += 1.0;
            if (empty || left > right) continue;
            const double lo_x = std::max(left, -1.0), hi_x = std::min(right, 4096.0);
            const int rx0 = std::max(tx0, static_cast<int>(std::floor(span_pow2 ? lo_x * span_inverse : lo_x / span)));
            const int rx1 = std::min(tx1, static_cast<int>(std::floor(span_pow2 ? hi_x * span_inverse : hi_x / span)));
            if (rx0 <= rx1) spans.push_back({static_cast<std::int16_t>(ty), static_cast<std::int16_t>(rx0), static_cast<std::int16_t>(rx1), prim});
        }
    }

    std::vector<std::uint32_t> build(const std::uint32_t *prims, std::uint32_t shapes) {
        prepare();
        spans.clear();
        for (std::uint32_t i = 0; i < shapes; ++i) add_spans(prims + i * kPrimWords, prims[i * kPrimWords + 2], prims[i * kPrimWords + 3], static_cast<std::uint16_t>(i));
        std::uint32_t touched_n = 0;
        for (const Span &sp : spans)
            for (int tx = sp.tx0; tx <= sp.tx1; ++tx) {
                const std::uint32_t t = static_cast<std::uint32_t>(sp.ty * g_per_row + tx);
                if (count[t]++ == 0) touched[touched_n++] = t;
            }
        std::uint32_t total = 0;
        for (std::uint32_t n = 0; n < touched_n; ++n) total += count[touched[n]];
        const std::uint32_t list_words = (total + 1) / 2, words = touched_n * 2 + 2 + list_words;
        std::vector<std::uint32_t> out(words, 0);
        std::uint32_t cursor = 0;
        for (std::uint32_t i = 0; i < touched_n; ++i) {
            const std::uint32_t t = touched[i];
            out[i * 2] = (t % g_per_row) | ((t / g_per_row) << 16);
            out[i * 2 + 1] = cursor;
            fill[t] = cursor;
            cursor += count[t];
        }
        out[touched_n * 2] = 0;
        out[touched_n * 2 + 1] = cursor;
        auto *halves = reinterpret_cast<std::uint16_t *>(out.data() + touched_n * 2 + 2);
        for (const Span &sp : spans)
            for (int tx = sp.tx0; tx <= sp.tx1; ++tx) halves[fill[static_cast<std::uint32_t>(sp.ty * g_per_row + tx)]++] = sp.prim;
        for (std::uint32_t n = 0; n < touched_n; ++n) count[touched[n]] = 0;
        return out;
    }
};

}  // namespace ref

namespace {

std::vector<std::uint32_t> g_out;
std::uint32_t *room(void *, std::size_t words) {
    g_out.assign(words, 0xDEADBEEFu);  // the builder must write every word
    return g_out.data();
}

std::vector<std::uint32_t> build_new(const std::uint32_t *prims, std::uint32_t shapes) {
    const tiles::Lists lists = tiles::build_now(prims, shapes, room, nullptr);
    if (lists.words != g_out.size()) std::printf("  words %u but room for %zu\n", lists.words, g_out.size());
    return g_out;
}

void configure(int scale) {
    tiles::configure(scale, 8);
    ref::g_scale = scale;
    ref::g_tile = 8;
    ref::g_per_row = (2048 * scale + 7) / 8;
}

// One random batch: shapes of every kind, sizes from below a pixel to the whole screen, clustered like a model's mesh.
std::vector<std::uint32_t> random_batch(std::mt19937 &rng, std::uint32_t shapes) {
    std::vector<std::uint32_t> out;
    out.reserve(static_cast<std::size_t>(shapes) * kPrimWords);
    const std::uint32_t full = 2047u << 16;
    float cx = 320, cy = 224;
    for (std::uint32_t i = 0; i < shapes; ++i) {
        std::uint32_t rec[kPrimWords] = {};
        int box[4];
        const std::uint32_t pick = rng() % 100;
        if (pick < 3) cx = static_cast<float>(rng() % 1400) - 100.0f, cy = static_cast<float>(rng() % 1000) - 100.0f;  // a new cluster
        const float size = pick < 40 ? 1.5f : pick < 70 ? 6.0f : pick < 90 ? 30.0f : pick < 98 ? 200.0f : 2500.0f;
        const auto coord = [&](float c) {
            std::uniform_real_distribution<float> d(-size, size);
            const float v = std::clamp(c + d(rng), -2000.0f, 3800.0f);
            return static_cast<std::uint32_t>(static_cast<int>(v * 16.0f) + 0x8000);
        };
        const std::uint32_t kind = rng() % 20;
        if (kind < 16) {  // triangle
            Vertex v[3] = {};
            for (Vertex &p : v) p.x = coord(cx), p.y = coord(cy);
            if (rng() % 8 == 0) v[2].x = v[0].x;  // a vertical edge
            if (rng() % 8 == 0) v[1].y = v[0].y;  // a horizontal edge
            if (rng() % 64 == 0) v[2] = v[1];      // no area
            if (!ps::triangle(rec, box, v, 0x8000, 0x8000, 1.0f, full, full, false, true)) continue;
        } else {  // sprite, line or point: the lists use only the box
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

struct Batch {
    std::vector<std::uint32_t> prims;
    std::uint32_t shapes() const { return static_cast<std::uint32_t>(prims.size() / kPrimWords); }
};

std::size_t check(const std::vector<Batch> &batches, const char *what) {
    static ref::Builder r;
    std::size_t bad = 0, entries = 0;
    for (std::size_t i = 0; i < batches.size(); ++i) {
        const Batch &b = batches[i];
        const std::vector<std::uint32_t> want = r.build(b.prims.data(), b.shapes());
        const std::vector<std::uint32_t> got = build_new(b.prims.data(), b.shapes());
        entries += want.size();
        if (want != got) {
            if (bad < 5) {
                std::size_t at = 0;
                while (at < std::min(want.size(), got.size()) && want[at] == got[at]) ++at;
                std::printf("  %s batch %zu (%u shapes): %zu words wanted, %zu got, first difference at word %zu\n", what, i, b.shapes(), want.size(), got.size(), at);
            }
            ++bad;
        }
    }
    std::printf("%s: %zu batches, %zu words, %zu differ\n", what, batches.size(), entries, bad);
    return bad;
}

// The lists of batches handed to the helper thread while they "grow" (tiles::open, feed, close): fed in random steps, the records moved to a new copy
// now and then (as when the shape array grows), every few batches one given up half way (tiles::cancel), which must not change the next one's lists.
std::size_t check_stream(const std::vector<Batch> &batches, const char *what) {
    static ref::Builder r;
    std::mt19937 rng(99u);
    std::size_t bad = 0, streamed = 0, cancelled = 0;
    for (std::size_t i = 0; i < batches.size(); ++i) {
        const Batch &b = batches[i];
        const std::uint32_t n = b.shapes();
        if (n < 2) continue;
        std::vector<std::vector<std::uint32_t>> copies;  // every place the records were, kept until the job is done
        copies.push_back(b.prims);
        const bool give_up = rng() % 5 == 0;
        std::uint32_t fed = 1 + rng() % (n - 1);
        tiles::Job *job = tiles::open(copies.back().data(), fed);
        while (fed < n) {
            if (give_up && rng() % 3 == 0) break;
            fed = std::min(n, fed + 1 + static_cast<std::uint32_t>(rng() % 2000));
            if (rng() % 4 == 0) copies.push_back(b.prims);
            if (fed < n) tiles::feed(job, copies.back().data(), fed);
        }
        if (give_up) {
            tiles::cancel(job);
            ++cancelled;
            continue;
        }
        tiles::close(job, copies.back().data(), n);
        tiles::Job *const got_job = tiles::next_finished(true);
        if (got_job != job) {
            std::printf("  %s batch %zu: the helper returned another job\n", what, i);
            return bad + 1;
        }
        const std::vector<std::uint32_t> got(job->lists.data(), job->lists.data() + job->result.words);
        tiles::release(job);
        ++streamed;
        if (got != r.build(b.prims.data(), n)) {
            if (bad < 5) std::printf("  %s batch %zu (%u shapes): the lists built in steps differ\n", what, i, n);
            ++bad;
        }
    }
    std::printf("%s: %zu batches built in steps on the helper (%zu given up half way), %zu differ\n", what, streamed, cancelled, bad);
    return bad;
}

void bench(const std::vector<Batch> &batches, int reps) {
    static ref::Builder r;
    std::size_t shapes = 0;
    for (const Batch &b : batches) shapes += b.shapes();
    double best_ref = 1e30, best_new = 1e30;
    for (int k = 0; k < reps; ++k) {  // alternating, best of reps (the machine may be busy)
        auto t0 = std::chrono::steady_clock::now();
        for (const Batch &b : batches) r.build(b.prims.data(), b.shapes());
        auto t1 = std::chrono::steady_clock::now();
        for (const Batch &b : batches) tiles::build_now(b.prims.data(), b.shapes(), room, nullptr);
        auto t2 = std::chrono::steady_clock::now();
        best_ref = std::min(best_ref, std::chrono::duration<double, std::milli>(t1 - t0).count());
        best_new = std::min(best_new, std::chrono::duration<double, std::milli>(t2 - t1).count());
    }
    std::printf("bench: %zu batches, %zu shapes: before %.2f ms (%.1f ns a shape), now %.2f ms (%.1f ns a shape)\n", batches.size(), shapes, best_ref,
                best_ref * 1e6 / static_cast<double>(shapes), best_new, best_new * 1e6 / static_cast<double>(shapes));
}

}  // namespace

int main(int argc, char **argv) {
    std::size_t bad = 0;
    if (argc > 1 && std::strcmp(argv[1], "bench") != 0) {
        std::FILE *f = std::fopen(argv[1], "rb");
        if (!f) {
            std::printf("cannot open %s\n", argv[1]);
            return 2;
        }
        std::vector<Batch> batches;
        std::uint32_t n;
        while (std::fread(&n, 4, 1, f) == 1) {
            Batch b;
            b.prims.resize(static_cast<std::size_t>(n) * kPrimWords);
            if (std::fread(b.prims.data(), 4, b.prims.size(), f) != b.prims.size()) break;
            batches.push_back(std::move(b));
        }
        std::fclose(f);
        configure(1);
        bad += check(batches, argv[1]);
        bad += check_stream(batches, argv[1]);
        bench(batches, argc > 2 ? std::atoi(argv[2]) : 5);
        return bad ? 1 : 0;
    }
    for (int scale = 1; scale <= 4; ++scale) {
        configure(scale);
        std::mt19937 rng(1234u + static_cast<unsigned>(scale));
        std::vector<Batch> batches;
        for (int i = 0; i < 300; ++i) batches.push_back({random_batch(rng, i % 10 == 0 ? 20000u : 1u + rng() % 600u)});
        batches.push_back({});  // an empty batch
        char what[32];
        std::snprintf(what, sizeof what, "random, scale %d", scale);
        bad += check(batches, what);
        bad += check_stream(batches, what);
        if (argc > 1 && scale == 1) bench(batches, argc > 2 ? std::atoi(argv[2]) : 5);
    }
    std::printf(bad ? "FAIL\n" : "PASS\n");
    return bad ? 1 : 0;
}
