#include "ps2/disc.hpp"
#include "ps2/hitch.hpp"
#include "ps2/os.hpp"
#include "load_stats.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>

namespace ps2::disc {

namespace {

std::FILE *g_file = nullptr;
Reader g_reader;
Probe g_probe;
std::uint64_t g_size = 0;
std::uint32_t g_layer1 = 0;
std::map<std::string, Entry> g_index;
bool g_indexed = false;  // the reads while the file system is being read are not the game's

std::uint32_t u32(const std::uint8_t *p) {
    std::uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

std::string upper(std::string s) {
    for (char &c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

// The image is untrusted (a damaged rip, or a crafted file): a directory that cannot be right is left out with one line, never
// read past its buffer. No real directory is near this size (a record is at least 34 bytes: 16 MiB is half a million names).
constexpr std::uint32_t kMaxDirectory = 16u << 20;

void damaged(const std::string &prefix, std::uint32_t sector, const char *why) {
    static int lines = 0;
    if (lines++ < 20) std::fprintf(stderr, "[disc] damaged directory '%s' at sector %u: %s (left out)\n", prefix.c_str(), sector, why);
}

void walk(std::uint32_t base, std::uint32_t lba, std::uint32_t size, const std::string &prefix, int depth, std::set<std::uint64_t> &seen) {
    if (depth > 16) return;
    // A directory that points back at itself or at a parent would be walked again and again: each extent is walked once.
    if (!seen.insert(std::uint64_t{base} + lba).second) return;
    if (size > kMaxDirectory) return damaged(prefix, base + lba, "too large");
    std::vector<std::uint8_t> data((size + kSector - 1) / kSector * kSector);
    if (!read(base + lba, static_cast<std::uint32_t>(data.size() / kSector), data.data())) return;
    std::size_t off = 0;
    while (off < size) {
        const std::uint8_t length = data[off];
        if (!length) {
            off = (off / kSector + 1) * kSector;
            continue;
        }
        // ISO 9660 (ECMA-119 9.1): a directory record is 33 bytes plus the name (at least one byte), all inside the directory.
        if (length < 34 || off + length > data.size()) return damaged(prefix, base + lba, "a record runs past its end");
        const std::uint8_t *rec = &data[off];
        off += length;
        const std::uint8_t name_len = rec[32];
        if (33u + name_len > length) {
            damaged(prefix, base + lba, "a name runs past its record");
            continue;
        }
        std::string name(reinterpret_cast<const char *>(rec + 33), name_len);
        if (name_len == 1 && (name[0] == 0 || name[0] == 1)) continue;
        const auto semi = name.find(';');
        if (semi != std::string::npos) name = name.substr(0, semi);
        const std::string path = prefix + upper(name);
        const bool dir = rec[25] & 2;
        const Entry entry{base + u32(rec + 2), u32(rec + 10), dir};
        g_index[path] = entry;
        if (dir) walk(base, u32(rec + 2), u32(rec + 10), path + "/", depth + 1, seen);
    }
}

void index_layer(std::uint32_t pvd_sector, std::uint32_t base) {
    std::uint8_t pvd[kSector];
    if (!read(pvd_sector, 1, pvd) || std::memcmp(pvd, "\x01" "CD001", 6) != 0) return;
    std::set<std::uint64_t> seen;
    walk(base, u32(pvd + 158), u32(pvd + 166), "", 0, seen);
}

void self_check();

}

bool open(const std::string &path) {
    std::FILE *file = os::open(path, "rb");
    if (!file) return false;
#if defined(_WIN32)
    const long long end = _fseeki64(file, 0, SEEK_END) == 0 ? _ftelli64(file) : -1;
#else
    const long long end = fseeko(file, 0, SEEK_END) == 0 ? static_cast<long long>(ftello(file)) : -1;
#endif
    if (end < 0) {
        std::fprintf(stderr, "[disc] %s: cannot find the size of the image\n", path.c_str());
        std::fclose(file);
        return false;
    }
    // A second image (a disc swap) replaces the first: its file is closed once the reader no longer uses it, so it is neither
    // leaked nor kept locked.
    std::FILE *previous = g_file;
    g_file = file;
    const bool opened = open(path, static_cast<std::uint64_t>(end), [file](std::uint64_t offset, void *out, std::size_t bytes) -> std::size_t {
        // A failed seek reads nothing (read() then says "short read") instead of the sectors at the old position.
#if defined(_WIN32)
        if (_fseeki64(file, static_cast<long long>(offset), SEEK_SET) != 0) return 0;
#else
        if (fseeko(file, static_cast<off_t>(offset), SEEK_SET) != 0) return 0;
#endif
        return std::fread(out, 1, bytes, file);
    });
    if (previous) std::fclose(previous);
    return opened;
}

bool open(const std::string &name, std::uint64_t size, Reader reader, Probe probe) {
    g_reader = std::move(reader);
    g_probe = std::move(probe);
    g_size = size;
    g_layer1 = 0;  // a second image without a second layer must not keep the first one's
    g_index.clear();
    g_indexed = false;
    index_layer(16, 0);
    std::uint8_t pvd[kSector];
    read(16, 1, pvd);
    const std::uint32_t layer0 = u32(pvd + 80);
    for (std::uint32_t s = layer0; s < layer0 + 64 && static_cast<std::uint64_t>(s) * kSector < g_size; ++s) {
        std::uint8_t probe[kSector];
        if (read(s, 1, probe) && std::memcmp(probe, "\x01" "CD001", 6) == 0) {
            g_layer1 = s - 16;
            index_layer(s, g_layer1);
            break;
        }
    }
    std::fprintf(stderr, "[disc] %s: %zu files, %llu sectors, layer 1 at %u\n", name.c_str(), g_index.size(),
                 static_cast<unsigned long long>(g_size / kSector), g_layer1);
    g_indexed = true;
    if (!os::env("PS2_DISC_CHECK").empty()) self_check();
    return true;
}

bool is_open() { return static_cast<bool>(g_reader); }

bool read(std::uint32_t sector, std::uint32_t count, void *out) {
    if (!g_reader) return false;
    const cost::Timer time_it(cost::kDisc);
    const std::size_t want = static_cast<std::size_t>(count) * kSector;
    if (cost::g_on) cost::g_disc_bytes += want;
    const auto started = hitch::on() ? hitch::Clock::now() : hitch::Clock::time_point{};
    const std::size_t got = g_reader(static_cast<std::uint64_t>(sector) * kSector, out, want);
    if (hitch::on()) {  // PS2_HITCH_LOG=<ms>: a disc read that held the game up (one third of the limit, since a frame has other work too)
        const double ms = hitch::ms_since(started);
        if (ms > hitch::threshold_ms() / 3) std::fprintf(stderr, "[hitch] disc read of %u sectors at sector %u (%.1f MB) took %.1f ms\n", count, sector, want / 1048576.0, ms);
    }
    if (got < want) std::memset(static_cast<std::uint8_t *>(out) + got, 0, want - got);
    // A game on a two-layer disc reads the second layer with the same sector numbers, which simply continue: the layer
    // boundary is not a gap in the image. Say so once, and say it when a read comes back short (the rest is filled with
    // zeros, which would otherwise go unnoticed). PS2_TRACE_DISC=1 prints every read in layer 1 and past the 4 GiB line.
    static const bool trace = !os::env("PS2_TRACE_DISC").empty();
    static std::uint32_t short_reads = 0, layer1_reads = 0;
    if (g_indexed && g_layer1 && count && sector + count > g_layer1 && (!layer1_reads++ || trace))
        std::fprintf(stderr, "[disc] read in layer 1: sector %u count %u (read %u)\n", sector, count, layer1_reads);
    if (got < want && count && short_reads++ < 20)
        std::fprintf(stderr, "[disc] short read: sector %u count %u, %zu of %zu bytes (the rest is zero)\n", sector, count, got, want);
    return got > 0 || !count;
}

namespace {

// FNV-1a of one sector, to compare with the same sector read by another program (scripts/disc.py reads the image with
// plain Python). The 4 GiB line lies in the second layer of a two-layer disc, and a read there is the one a 32-bit offset
// would get wrong.
std::uint64_t checksum(std::uint32_t sector) {
    std::uint8_t data[kSector];
    read(sector, 1, data);
    std::uint64_t h = 0xCBF29CE484222325ull;
    for (const std::uint8_t b : data) h = (h ^ b) * 0x100000001B3ull;
    return h;
}

void self_check() {
    const std::uint32_t last = static_cast<std::uint32_t>(g_size / kSector ? g_size / kSector - 1 : 0);
    for (const std::uint32_t s : {0u, 16u, g_layer1, g_layer1 + 16u, (1u << 21) - 1u, 1u << 21, (1u << 21) + 1u, last})
        if (s <= last) std::fprintf(stderr, "[disc-check] sector %u fnv1a %016llx\n", s, static_cast<unsigned long long>(checksum(s)));
    for (const auto &[path, entry] : g_index)
        if (g_layer1 && !entry.directory && entry.sector >= g_layer1) {
            const std::uint32_t end = entry.sector + (entry.size ? (entry.size - 1) / kSector : 0);
            std::fprintf(stderr, "[disc-check] layer 1 file %s: sectors %u to %u, first %016llx, last %016llx\n", path.c_str(), entry.sector, end,
                         static_cast<unsigned long long>(checksum(entry.sector)), static_cast<unsigned long long>(checksum(end)));
        }
}

}

bool ready(std::uint32_t sector, std::uint32_t count) {
    return !g_probe || g_probe(static_cast<std::uint64_t>(sector) * kSector, static_cast<std::size_t>(count) * kSector);
}

std::optional<Entry> find(const std::string &raw) {
    std::string path = raw;
    const auto colon = path.find(':');
    if (colon != std::string::npos) path = path.substr(colon + 1);
    std::replace(path.begin(), path.end(), '\\', '/');
    const auto semi = path.find(';');
    if (semi != std::string::npos) path = path.substr(0, semi);
    while (!path.empty() && path.front() == '/') path.erase(path.begin());
    auto it = g_index.find(upper(path));
    if (it == g_index.end()) return std::nullopt;
    return it->second;
}

std::uint32_t layer1_start() { return g_layer1; }
std::uint64_t sector_count() { return g_size / kSector; }

}
