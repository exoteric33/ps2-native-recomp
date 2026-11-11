#include "ps2/disc_bundle.hpp"

#include "ps2/disc.hpp"
#include "ps2/os.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace ps2::disc {

namespace {

struct AssetSlice {
    std::uint64_t bundle_offset = 0;
    std::uint64_t byte_length = 0;
};

struct BundleSpan {
    std::uint64_t disk_offset = 0;
    std::uint64_t length = 0;
    std::uint64_t bundle_offset = 0;
};

struct LoadedBundle {
    SelfBundleInfo info;
    std::unordered_map<std::string, AssetSlice> assets;
    std::vector<std::string> asset_order;
    std::vector<BundleSpan> spans;
    std::uint64_t toc_hash = 0;

    std::mutex io_mutex;
    std::FILE *fp = nullptr;

    ~LoadedBundle() {
        if (fp) {
            std::fclose(fp);
            fp = nullptr;
        }
    }

    bool seek_and_read(std::uint64_t offset, void *out, std::size_t bytes) {
        if (bytes == 0) return true;
        std::lock_guard<std::mutex> lock(io_mutex);
        if (!fp) {
            fp = os::open(info.exe_path, "rb");
            if (!fp) return false;
        }
#if defined(_WIN32)
        if (_fseeki64(fp, static_cast<long long>(offset), SEEK_SET) != 0) return false;
#else
        if (fseeko(fp, static_cast<off_t>(offset), SEEK_SET) != 0) return false;
#endif
        return std::fread(out, 1, bytes, fp) == bytes;
    }

    std::size_t read_disc_bytes(std::uint64_t offset, void *out, std::size_t bytes) {
        const std::uint64_t total_bytes = info.total_sectors * kSector;
        if (offset >= total_bytes || bytes == 0) return 0;
        const std::size_t clamped = static_cast<std::size_t>(std::min<std::uint64_t>(bytes, total_bytes - offset));
        std::uint8_t *dst = static_cast<std::uint8_t *>(out);
        std::memset(dst, 0, clamped);

        const std::uint64_t req_start = offset;
        const std::uint64_t req_end = offset + clamped;

        auto it = std::upper_bound(spans.begin(), spans.end(), req_start, [](std::uint64_t pos, const BundleSpan &s) {
            return pos < s.disk_offset + s.length;
        });

        for (; it != spans.end() && it->disk_offset < req_end; ++it) {
            const std::uint64_t ov_start = std::max(req_start, it->disk_offset);
            const std::uint64_t ov_end = std::min(req_end, it->disk_offset + it->length);
            if (ov_end <= ov_start) continue;

            const std::uint64_t file_pos = it->bundle_offset + (ov_start - it->disk_offset);
            const std::size_t chunk_len = static_cast<std::size_t>(ov_end - ov_start);
            std::uint8_t *chunk_dst = dst + (ov_start - req_start);
            (void)seek_and_read(file_pos, chunk_dst, chunk_len);
        }
        return clamped;
    }
};

std::mutex g_bundle_mutex;
std::shared_ptr<LoadedBundle> g_active_bundle;
bool g_probed_self = false;

std::uint64_t fnv1a_bytes(const void *data, std::size_t len, std::uint64_t h = 0xCBF29CE484222325ull) {
    const auto *p = static_cast<const std::uint8_t *>(data);
    for (std::size_t i = 0; i < len; ++i) {
        h = (h ^ p[i]) * 0x100000001B3ull;
    }
    return h;
}

std::shared_ptr<LoadedBundle> parse_bundle_file(const std::string &exe_path) {
    if (exe_path.empty()) return nullptr;
    std::FILE *fp = os::open(exe_path, "rb");
    if (!fp) return nullptr;

#if defined(_WIN32)
    if (_fseeki64(fp, 0, SEEK_END) != 0) {
        std::fclose(fp);
        return nullptr;
    }
    const long long file_size_s = _ftelli64(fp);
#else
    if (fseeko(fp, 0, SEEK_END) != 0) {
        std::fclose(fp);
        return nullptr;
    }
    const long long file_size_s = static_cast<long long>(ftello(fp));
#endif
    if (file_size_s < static_cast<long long>(sizeof(Ps2BundleFooter))) {
        std::fclose(fp);
        return nullptr;
    }
    const std::uint64_t file_size = static_cast<std::uint64_t>(file_size_s);

#if defined(_WIN32)
    if (_fseeki64(fp, static_cast<long long>(file_size - sizeof(Ps2BundleFooter)), SEEK_SET) != 0) {
        std::fclose(fp);
        return nullptr;
    }
#else
    if (fseeko(fp, static_cast<off_t>(file_size - sizeof(Ps2BundleFooter)), SEEK_SET) != 0) {
        std::fclose(fp);
        return nullptr;
    }
#endif
    Ps2BundleFooter footer{};
    if (std::fread(&footer, 1, sizeof(footer), fp) != sizeof(footer)) {
        std::fclose(fp);
        return nullptr;
    }
    if (std::memcmp(footer.magic, "PS2PACK1", 8) != 0) {
        std::fclose(fp);
        return nullptr;
    }
    if (footer.toc_offset >= file_size ||
        footer.toc_size > file_size - sizeof(Ps2BundleFooter) - footer.toc_offset ||
        footer.toc_size > (64u << 20)) {
        std::fprintf(stderr, "[disc-bundle] damaged PS2PACK1 TOC bounds in %s\n", exe_path.c_str());
        std::fclose(fp);
        return nullptr;
    }

    std::vector<std::uint8_t> toc_buf(static_cast<std::size_t>(footer.toc_size));
#if defined(_WIN32)
    if (_fseeki64(fp, static_cast<long long>(footer.toc_offset), SEEK_SET) != 0) {
        std::fclose(fp);
        return nullptr;
    }
#else
    if (fseeko(fp, static_cast<off_t>(footer.toc_offset), SEEK_SET) != 0) {
        std::fclose(fp);
        return nullptr;
    }
#endif
    if (footer.toc_size > 0 && std::fread(toc_buf.data(), 1, toc_buf.size(), fp) != toc_buf.size()) {
        std::fclose(fp);
        return nullptr;
    }

    auto bundle = std::make_shared<LoadedBundle>();
    bundle->fp = fp;
    bundle->info.exe_path = exe_path;
    bundle->info.bundle_start = footer.bundle_start;
    bundle->info.toc_offset = footer.toc_offset;
    bundle->info.total_sectors = footer.total_sectors;
    bundle->info.layer1_start = footer.layer1_start;
    footer.game_id[sizeof(footer.game_id) - 1] = '\0';
    bundle->info.game_id = footer.game_id[0] ? std::string(footer.game_id) : std::string("ps2game");
    bundle->toc_hash = fnv1a_bytes(toc_buf.data(), toc_buf.size(), fnv1a_bytes(&footer, sizeof(footer)));

    std::size_t pos = 0;
    for (std::uint32_t i = 0; i < footer.entry_count; ++i) {
        if (pos + sizeof(Ps2BundleTocEntryHeader) > toc_buf.size()) break;
        Ps2BundleTocEntryHeader hdr{};
        std::memcpy(&hdr, toc_buf.data() + pos, sizeof(hdr));
        pos += sizeof(hdr);
        if (pos + hdr.path_len > toc_buf.size()) break;
        std::string vpath(reinterpret_cast<const char *>(toc_buf.data() + pos), hdr.path_len);
        pos += hdr.path_len;

        if (hdr.bundle_offset > file_size || hdr.byte_length > file_size - hdr.bundle_offset) {
            continue;
        }
        if (hdr.kind == 0) {
            bundle->assets[vpath] = AssetSlice{hdr.bundle_offset, hdr.byte_length};
            bundle->asset_order.push_back(vpath);
        } else if (hdr.kind == 1 && hdr.byte_length > 0) {
            bundle->spans.push_back(BundleSpan{hdr.disk_offset, hdr.byte_length, hdr.bundle_offset});
        }
    }

    std::sort(bundle->spans.begin(), bundle->spans.end(), [](const BundleSpan &a, const BundleSpan &b) {
        if (a.disk_offset != b.disk_offset) return a.disk_offset < b.disk_offset;
        return a.length > b.length;
    });

    std::vector<BundleSpan> unique_spans;
    unique_spans.reserve(bundle->spans.size());
    for (const BundleSpan &s : bundle->spans) {
        if (!unique_spans.empty()) {
            const BundleSpan &prev = unique_spans.back();
            if (s.disk_offset < prev.disk_offset + prev.length) continue;
        }
        unique_spans.push_back(s);
    }
    bundle->spans = std::move(unique_spans);

    // Read boot_name.txt if present
    auto it_boot = bundle->assets.find("boot_name.txt");
    if (it_boot != bundle->assets.end() && it_boot->second.byte_length > 0 && it_boot->second.byte_length < 256) {
        std::string boot_name(static_cast<std::size_t>(it_boot->second.byte_length), '\0');
        if (bundle->seek_and_read(it_boot->second.bundle_offset, boot_name.data(), boot_name.size())) {
            while (!boot_name.empty() && (boot_name.back() == '\n' || boot_name.back() == '\r' || boot_name.back() == ' ')) {
                boot_name.pop_back();
            }
            bundle->info.boot_elf_arg = boot_name;
        }
    }
    if (bundle->info.boot_elf_arg.empty()) {
        bundle->info.boot_elf_arg = "cdrom0:\\BOOT.ELF;1";
    }

    return bundle;
}

}  // namespace

std::string current_executable_path() {
#ifdef _WIN32
    std::wstring buf(4096, L'\0');
    DWORD len = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    if (len == 0 || len >= buf.size()) return {};
    buf.resize(len);
    return os::utf8(std::filesystem::path(buf));
#elif !defined(__EMSCRIPTEN__)
    std::error_code ec;
    std::filesystem::path p = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (!ec && !p.empty()) return os::utf8(p);
    return {};
#else
    return {};
#endif
}

bool detect_self_bundle(SelfBundleInfo *out_info, const std::string &exe_path) {
    std::lock_guard<std::mutex> lock(g_bundle_mutex);
    if (!exe_path.empty()) {
        auto custom = parse_bundle_file(exe_path);
        if (!custom) return false;
        g_active_bundle = custom;
        g_probed_self = true;
    } else if (!g_probed_self) {
        g_probed_self = true;
        g_active_bundle = parse_bundle_file(current_executable_path());
    }
    if (!g_active_bundle) return false;
    if (out_info) *out_info = g_active_bundle->info;
    return true;
}

bool open_bundle(const std::string &path) {
    const std::string target = (path == kBundleSentinel) ? std::string() : path;
    if (!detect_self_bundle(nullptr, target)) return false;

    std::shared_ptr<LoadedBundle> bundle;
    {
        std::lock_guard<std::mutex> lock(g_bundle_mutex);
        bundle = g_active_bundle;
    }
    if (!bundle || bundle->info.total_sectors == 0 || bundle->spans.empty()) return false;

    const std::uint64_t total_bytes = bundle->info.total_sectors * kSector;
    std::fprintf(stderr,
                 "[disc-bundle] mounted embedded PS2PACK1 virtual disc from %s (%zu spans, %llu virtual sectors)\n",
                 bundle->info.exe_path.c_str(),
                 bundle->spans.size(),
                 static_cast<unsigned long long>(bundle->info.total_sectors));

    return open(bundle->info.exe_path, total_bytes, [bundle](std::uint64_t offset, void *out, std::size_t bytes) -> std::size_t {
        return bundle->read_disc_bytes(offset, out, bytes);
    });
}

bool read_bundle_asset(const std::string &virtual_path, std::vector<std::uint8_t> &out) {
    if (!detect_self_bundle()) return false;
    std::shared_ptr<LoadedBundle> bundle;
    {
        std::lock_guard<std::mutex> lock(g_bundle_mutex);
        bundle = g_active_bundle;
    }
    if (!bundle) return false;
    auto it = bundle->assets.find(virtual_path);
    if (it == bundle->assets.end()) return false;
    out.resize(static_cast<std::size_t>(it->second.byte_length));
    if (out.empty()) return true;
    return bundle->seek_and_read(it->second.bundle_offset, out.data(), out.size());
}

std::vector<std::string> list_bundle_assets(const std::string &prefix) {
    std::vector<std::string> result;
    if (!detect_self_bundle()) return result;
    std::shared_ptr<LoadedBundle> bundle;
    {
        std::lock_guard<std::mutex> lock(g_bundle_mutex);
        bundle = g_active_bundle;
    }
    if (!bundle) return result;
    for (const std::string &k : bundle->asset_order) {
        if (k.rfind(prefix, 0) == 0) {
            result.push_back(k);
        }
    }
    return result;
}

std::string bundle_writable_data_dir() {
    SelfBundleInfo info;
    if (!detect_self_bundle(&info)) return "saves";

    std::error_code ec;
    const std::filesystem::path exe_dir = os::path(info.exe_path).parent_path();
    const std::filesystem::path portable_dir = exe_dir / "saves";
    std::filesystem::create_directories(portable_dir, ec);
    if (!ec) {
        const std::string probe = os::utf8(portable_dir / ".write_probe");
        if (std::FILE *f = os::open(probe, "wb")) {
            std::fclose(f);
            std::filesystem::remove(os::path(probe), ec);
            return os::utf8(portable_dir);
        }
    }

    // Fallback to OS user data directory if executable directory is read-only
    std::filesystem::path fallback;
#ifdef _WIN32
    const std::string appdata = os::env("APPDATA");
    if (!appdata.empty()) fallback = os::path(appdata) / "ps2recomp" / info.game_id;
#else
    const std::string xdg = os::env("XDG_DATA_HOME");
    if (!xdg.empty()) {
        fallback = os::path(xdg) / "ps2recomp" / info.game_id;
    } else {
        const std::string home = os::env("HOME");
        if (!home.empty()) fallback = os::path(home) / ".local" / "share" / "ps2recomp" / info.game_id;
    }
#endif
    if (fallback.empty()) fallback = std::filesystem::temp_directory_path(ec) / ("ps2recomp_" + info.game_id);
    std::filesystem::create_directories(fallback, ec);
    return os::utf8(fallback);
}

std::string ensure_bundle_live_cache(const std::string &subdir) {
    SelfBundleInfo info;
    if (!detect_self_bundle(&info)) return {};

    const std::string prefix = subdir + "/";
    const std::vector<std::string> keys = list_bundle_assets(prefix);
    if (keys.empty()) return {};

    std::uint64_t hash = 0;
    {
        std::lock_guard<std::mutex> lock(g_bundle_mutex);
        if (g_active_bundle) hash = g_active_bundle->toc_hash;
    }
    char hash_hex[20];
    std::snprintf(hash_hex, sizeof(hash_hex), "%016llx", static_cast<unsigned long long>(hash));

    std::error_code ec;
    std::filesystem::path cache_base;
    const std::string saves_dir = bundle_writable_data_dir();
    if (!saves_dir.empty()) {
        cache_base = os::path(saves_dir) / "cache";
    } else {
        cache_base = std::filesystem::temp_directory_path(ec) / "ps2recomp_cache";
    }

    const std::filesystem::path target_dir = cache_base / (info.game_id + "-" + hash_hex) / subdir;
    const std::filesystem::path marker = target_dir / ".extracted_ok";
    if (std::filesystem::is_regular_file(marker, ec)) {
        return os::utf8(target_dir);
    }

    std::filesystem::create_directories(target_dir, ec);
    std::vector<std::uint8_t> buf;
    std::size_t extracted = 0;
    for (const std::string &k : keys) {
        const std::string rel = k.substr(prefix.size());
        if (rel.empty() || rel.find("..") != std::string::npos) continue;
        if (!read_bundle_asset(k, buf)) continue;
        const std::filesystem::path dst = target_dir / os::path(rel);
        std::filesystem::create_directories(dst.parent_path(), ec);
        if (std::FILE *out = os::open(os::utf8(dst), "wb")) {
            if (!buf.empty()) (void)std::fwrite(buf.data(), 1, buf.size(), out);
            std::fclose(out);
            ++extracted;
        }
    }

    if (extracted > 0) {
        if (std::FILE *m = os::open(os::utf8(marker), "wb")) {
            std::fputs("ok\n", m);
            std::fclose(m);
        }
        std::fprintf(stderr, "[disc-bundle] extracted %zu bundled %s file(s) to %s\n",
                     extracted, subdir.c_str(), os::utf8(target_dir).c_str());
        return os::utf8(target_dir);
    }
    return {};
}

}  // namespace ps2::disc
