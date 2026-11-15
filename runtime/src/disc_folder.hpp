#pragma once

#include <string>

namespace ps2::disc {

// Opens an extracted disc directory containing disc_manifest.tsv (created by scripts/extract_disc.py).
// Returns false if `path` is not an extracted disc directory, allowing disc::open() to fall back to ISO mode.
bool open_folder(const std::string &path);

}
