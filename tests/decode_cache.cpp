// The decode cache (runtime/src/gs/gpu/gpu.cpp) follows graphics memory in 256-byte blocks and finds the blocks of a
// rectangle by one texel per cell: 8 x 8 pixels for the 32-bit formats, 16 x 8 for the 16-bit ones, 16 x 16 for 8-bit, 32 x 16 for 4-bit
// (for_cells). That is right only if, in the address tables the drawing program uses (swizzle_tables), every texel of such a cell lies in the
// same block and the 32 cells of a page are 32 different blocks. This checks it for every format's table. No game needed: ninja ps2decodecachetest
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <vector>

namespace ps2::gs {
void init();
void swizzle_tables(std::vector<std::uint32_t> &out);
}

int main() {
#ifdef _WIN32
    _putenv_s("PS2_GS", "software");  // the tables only: no WebGPU device
#else
    setenv("PS2_GS", "software", 1);
#endif
    ps2::gs::init();
    std::vector<std::uint32_t> t;
    ps2::gs::swizzle_tables(t);
    struct Format {
        const char *name;
        std::uint32_t offset, pws, phs, nibble;
    };
    // table offsets and page sizes as fmt_words in gpu.cpp gives them
    const Format formats[] = {{"32", 0, 6, 5, 0},       {"Z32", 2048, 6, 5, 0},  {"16", 4096, 6, 6, 0}, {"16S", 8192, 6, 6, 0},
                              {"Z16", 12288, 6, 6, 0}, {"Z16S", 16384, 6, 6, 0}, {"8", 20480, 7, 6, 0}, {"4", 28672, 7, 7, 1}};
    int bad = 0;
    for (const Format &f : formats) {
        const std::uint32_t pw = 1u << f.pws, ph = 1u << f.phs;
        const std::uint32_t bw = f.pws == 7 ? (f.phs == 7 ? 32u : 16u) : (f.phs == 6 ? 16u : 8u), bh = f.pws == 7 ? 16u : 8u;  // as for_cells
        std::set<std::uint32_t> blocks;
        for (std::uint32_t cy = 0; cy < ph / bh; ++cy)
            for (std::uint32_t cx = 0; cx < pw / bw; ++cx) {
                const auto block = [&](std::uint32_t x, std::uint32_t y) { return (t[f.offset + y * pw + x] >> f.nibble) / 256; };
                const std::uint32_t first = block(cx * bw, cy * bh);
                for (std::uint32_t y = cy * bh; y < (cy + 1) * bh; ++y)
                    for (std::uint32_t x = cx * bw; x < (cx + 1) * bw; ++x)
                        if (block(x, y) != first) {
                            if (bad++ < 10) std::printf("format %s: texel %u,%u is in block %u, its cell's first texel in %u\n", f.name, x, y, block(x, y), first);
                        }
                blocks.insert(first);
            }
        if (blocks.size() != 32 || *blocks.rbegin() != 31) {
            ++bad;
            std::printf("format %s: the cells of a page are %zu different blocks, not 0 to 31\n", f.name, blocks.size());
        }
    }
    std::printf(bad ? "FAIL: %d\n" : "ok: every cell is one block, every page 32 blocks, in all 8 tables\n", bad);
    return bad ? 1 : 0;
}
