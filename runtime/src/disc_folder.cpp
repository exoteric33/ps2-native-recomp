#include "disc_folder.hpp"

#include "ps2/disc.hpp"
#include "ps2/os.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace ps2::disc {

namespace {

struct Span {
    std::uint64_t disk_offset;  // start byte offset on the virtual ISO (sector * 2048)
    std::uint64_t length;       // valid byte length of this span
    std::uint64_t file_offset;  // byte offset inside the target host file
    std::uint32_t file_id;      // index into VirtualDisc::files
};

struct VirtualDisc {
    static constexpr std::size_t kCacheSlots = 16;

    struct CacheSlot {
        std::uint32_t file_id = UINT32_MAX;
        std::uint64_t last_tick = 0;
        std::FILE *fp = nullptr;
    };

    std::vector<std::string> files;
    std::vector<Span> spans;
    std::uint64_t total_sectors = 0;
    std::uint32_t layer1_sector = 0;

    std::mutex lock;
    std::array<CacheSlot, kCacheSlots> cache{};
    std::uint64_t tick = 0;

    ~VirtualDisc() {
        for (CacheSlot &slot : cache) {
            if (slot.fp) {
                std::fclose(slot.fp);
                slot.fp = nullptr;
            }
        }
    }

    std::FILE *acquire_file(std::uint32_t file_id) {
        if (file_id >= files.size()) return nullptr;
        ++tick;
        for (CacheSlot &slot : cache) {
            if (slot.file_id == file_id && slot.fp) {
                slot.last_tick = tick;
                return slot.fp;
            }
        }
        std::size_t victim = 0;
        for (std::size_t i = 0; i < kCacheSlots; ++i) {
            if (!cache[i].fp) {
                victim = i;
                break;
            }
            if (cache[i].last_tick < cache[victim].last_tick) victim = i;
        }
        if (cache[victim].fp) {
            std::fclose(cache[victim].fp);
            cache[victim].fp = nullptr;
            cache[victim].file_id = UINT32_MAX;
        }
        std::FILE *fp = os::open(files[file_id], "rb");
        if (!fp) {
            std::fprintf(stderr, "[disc-folder] cannot open extracted file %s\n", files[file_id].c_str());
            return nullptr;
        }
        cache[victim].file_id = file_id;
        cache[victim].last_tick = tick;
        cache[victim].fp = fp;
        return fp;
    }

    std::size_t read_bytes(std::uint64_t offset, void *out, std::size_t bytes) {
        const std::uint64_t total_bytes = total_sectors * kSector;
        if (offset >= total_bytes || bytes == 0) return 0;
        const std::size_t clamped = static_cast<std::size_t>(std::min<std::uint64_t>(bytes, total_bytes - offset));
        std::uint8_t *dst = static_cast<std::uint8_t *>(out);
        std::memset(dst, 0, clamped);

        const std::uint64_t req_start = offset;
        const std::uint64_t req_end = offset + clamped;

        // Binary search for the first span whose end (disk_offset + length) > req_start.
        auto it = std::upper_bound(spans.begin(), spans.end(), req_start, [](std::uint64_t pos, const Span &s) {
            return pos < s.disk_offset + s.length;
        });

        std::lock_guard<std::mutex> guard(lock);
        for (; it != spans.end() && it->disk_offset < req_end; ++it) {
            const std::uint64_t ov_start = std::max(req_start, it->disk_offset);
            const std::uint64_t ov_end = std::min(req_end, it->disk_offset + it->length);
            if (ov_end <= ov_start) continue;

            std::FILE *fp = acquire_file(it->file_id);
            if (!fp) continue;

            const std::uint64_t file_pos = it->file_offset + (ov_start - it->disk_offset);
            const std::size_t chunk_len = static_cast<std::size_t>(ov_end - ov_start);
            std::uint8_t *chunk_dst = dst + (ov_start - req_start);

#if defined(_WIN32)
            if (_fseeki64(fp, static_cast<long long>(file_pos), SEEK_SET) != 0) continue;
#else
            if (fseeko(fp, static_cast<off_t>(file_pos), SEEK_SET) != 0) continue;
#endif
            (void)std::fread(chunk_dst, 1, chunk_len, fp);
        }
        return clamped;
    }
};

}

bool open_folder(const std::string &path) {
    std::error_code ec;
    const std::filesystem::path fs_path = os::path(path);
    std::filesystem::path base_dir;
    std::filesystem::path manifest_path;

    if (std::filesystem::is_directory(fs_path, ec)) {
        base_dir = fs_path;
        manifest_path = base_dir / "disc_manifest.tsv";
    } else if (fs_path.filename() == "disc_manifest.tsv" && std::filesystem::is_regular_file(fs_path, ec)) {
        manifest_path = fs_path;
        base_dir = fs_path.parent_path();
    } else {
        return false;
    }

    if (!std::filesystem::is_regular_file(manifest_path, ec)) return false;

    std::ifstream in(manifest_path);
    if (!in) return false;

    auto vdisc = std::make_shared<VirtualDisc>();
    // File ID 0 is always disc_meta.bin (system/PVD/directory metadata sectors).
    vdisc->files.push_back((base_dir / "disc_meta.bin").string());

    std::string line;
    std::size_t extracted_files = 0;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;

        std::istringstream ss(line);
        std::string kind;
        if (!(ss >> kind)) continue;

        if (kind == "TOTAL_SECTORS") {
            std::string l1_tag;
            ss >> vdisc->total_sectors >> l1_tag >> vdisc->layer1_sector;
        } else if (kind == "META") {
            std::uint64_t start_sector = 0, sector_count = 0, meta_offset = 0;
            if (ss >> start_sector >> sector_count >> meta_offset) {
                vdisc->spans.push_back(Span{
                    start_sector * kSector,
                    sector_count * kSector,
                    meta_offset,
                    0u
                });
            }
        } else if (kind == "FILE") {
            std::uint32_t layer = 0;
            std::uint64_t start_sector = 0, size_bytes = 0;
            std::string iso_path, local_rel;
            if (ss >> layer >> start_sector >> size_bytes >> iso_path >> local_rel) {
                const std::uint32_t file_id = static_cast<std::uint32_t>(vdisc->files.size());
                vdisc->files.push_back((base_dir / local_rel).string());
                if (size_bytes > 0) {
                    vdisc->spans.push_back(Span{
                        start_sector * kSector,
                        size_bytes,
                        0u,
                        file_id
                    });
                }
                ++extracted_files;
            }
        }
    }

    if (vdisc->total_sectors == 0 || vdisc->spans.empty()) {
        std::fprintf(stderr, "[disc-folder] %s: invalid or empty disc_manifest.tsv\n", manifest_path.string().c_str());
        return false;
    }

    std::sort(vdisc->spans.begin(), vdisc->spans.end(), [](const Span &a, const Span &b) {
        if (a.disk_offset != b.disk_offset) return a.disk_offset < b.disk_offset;
        return a.length > b.length;
    });

    // Deduplicate any aliased ISO file extents that start at the exact same sector.
    std::vector<Span> unique_spans;
    unique_spans.reserve(vdisc->spans.size());
    for (const Span &s : vdisc->spans) {
        if (!unique_spans.empty()) {
            const Span &prev = unique_spans.back();
            if (s.disk_offset < prev.disk_offset + prev.length) continue;
        }
        unique_spans.push_back(s);
    }
    vdisc->spans = std::move(unique_spans);

    const std::uint64_t total_bytes = vdisc->total_sectors * kSector;
    std::fprintf(stderr, "[disc-folder] mounted extracted disc directory %s (%zu extracted files, %llu virtual sectors)\n",
                 base_dir.string().c_str(), extracted_files, static_cast<unsigned long long>(vdisc->total_sectors));

    return open(path, total_bytes, [vdisc](std::uint64_t offset, void *out, std::size_t bytes) -> std::size_t {
        return vdisc->read_bytes(offset, out, bytes);
    });
}

}
