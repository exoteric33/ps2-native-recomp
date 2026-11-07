#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ps2::disc {

inline constexpr const char *kBundleSentinel = "@bundle";

#pragma pack(push, 1)
struct Ps2BundleFooter {
    char magic[8];               // "PS2PACK1"
    std::uint64_t bundle_start;  // byte offset where appended payload starts
    std::uint64_t toc_offset;    // byte offset where TOC starts
    std::uint64_t toc_size;      // byte length of TOC
    std::uint64_t total_sectors; // virtual disc total_sectors
    std::uint32_t layer1_start;  // virtual disc layer1_start
    std::uint32_t entry_count;   // number of TOC entries
    char game_id[16];            // null-terminated game identifier (e.g. "mhdos")
};

struct Ps2BundleTocEntryHeader {
    std::uint8_t kind;           // 0 = ASSET, 1 = DISC_SPAN
    std::uint8_t flags;          // 0 = raw/uncompressed
    std::uint16_t reserved;      // 0
    std::uint32_t path_len;      // UTF-8 byte length of virtual_path following header
    std::uint64_t disk_offset;   // for DISC_SPAN: start byte offset on virtual ISO (sector * 2048)
    std::uint64_t bundle_offset; // absolute byte offset inside the executable file
    std::uint64_t byte_length;   // payload size in bytes
};
#pragma pack(pop)

static_assert(sizeof(Ps2BundleFooter) == 64, "Ps2BundleFooter must be 64 bytes");
static_assert(sizeof(Ps2BundleTocEntryHeader) == 32, "Ps2BundleTocEntryHeader must be 32 bytes");

struct SelfBundleInfo {
    std::string exe_path;
    std::string game_id;
    std::string boot_elf_arg;       // e.g. "cdrom0:\\SLPM_662.80;1"
    std::uint64_t bundle_start = 0;
    std::uint64_t toc_offset = 0;
    std::uint64_t total_sectors = 0;
    std::uint32_t layer1_start = 0;
};

// Returns the absolute UTF-8 path of the currently running executable.
std::string current_executable_path();

// Checks whether `exe_path` (or the current executable if empty) contains an
// appended PS2PACK1 bundle. Thread-safe and cached after first probe.
bool detect_self_bundle(SelfBundleInfo *out_info = nullptr, const std::string &exe_path = {});

// Mounts the virtual disc directly from the PS2PACK1 bundle inside `exe_path`
// (or the current executable when `path == "@bundle"`).
bool open_bundle(const std::string &path);

// Reads a named virtual asset (e.g. "boot.elf", "translations/01_ui_system.json")
// from the active self-bundle into `out`.
bool read_bundle_asset(const std::string &virtual_path, std::vector<std::uint8_t> &out);

// Lists all asset virtual paths in the bundle starting with `prefix` (e.g. "translations/").
std::vector<std::string> list_bundle_assets(const std::string &prefix);

// Returns a writable user data directory for saves/logs/quick-states when running
// as a single-file bundle: prefers `<exe_dir>/saves` if writable, falling back to
// `$XDG_DATA_HOME/ps2recomp/<game_id>` on Linux or `%APPDATA%\ps2recomp\<game_id>` on Windows.
std::string bundle_writable_data_dir();

// Extracts bundled runtime shared libraries under `subdir` (e.g. "ee-live" or "vu1-live")
// into a persistent user cache directory if not already extracted.
// Returns the extracted directory path, or empty string if the bundle has no files in `subdir`.
std::string ensure_bundle_live_cache(const std::string &subdir);

}  // namespace ps2::disc
