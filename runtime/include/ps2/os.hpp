#pragma once

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <string>
#include <system_error>
#include <thread>

#ifdef _WIN32
#include <io.h>
#elif !defined(__EMSCRIPTEN__)
#include <unistd.h>
#endif

// Paths and environment values are UTF-8 inside the runner. Windows reads a char* path in the ANSI code page, so a name with
// characters outside it (a Chinese folder name, say) breaks there; every file the runner opens for the user goes through these.
namespace ps2::os {

inline std::filesystem::path path(const std::string &text) {
    try {
        return std::filesystem::path(std::u8string(text.begin(), text.end()));
    } catch (const std::exception &) {
        return std::filesystem::path(text);  // not valid UTF-8: take it as the system's own encoding
    }
}

inline std::string utf8(const std::filesystem::path &p) {
    const std::u8string text = p.u8string();
    return std::string(text.begin(), text.end());
}

inline std::FILE *open(const std::string &text, const char *mode) {
#ifdef _WIN32
    return _wfopen(path(text).c_str(), std::wstring(mode, mode + std::strlen(mode)).c_str());
#else
    return std::fopen(text.c_str(), mode);
#endif
}

inline std::string env(const char *name) {
#ifdef _WIN32
    const wchar_t *value = _wgetenv(std::wstring(name, name + std::strlen(name)).c_str());
    return value ? utf8(value) : std::string();
#else
    const char *value = std::getenv(name);
    return value ? value : std::string();
#endif
}

// Sets an environment value for this process and the code it runs later; the value is UTF-8. env() reads back exactly what
// was set, also for names with characters outside the ANSI code page. Returns false when the system refused.
inline bool set_env(const char *name, const std::string &value) {
#ifdef _WIN32
    return _wputenv_s(std::wstring(name, name + std::strlen(name)).c_str(), path(value).c_str()) == 0;
#else
    return setenv(name, value.c_str(), 1) == 0;
#endif
}

// Replaces the file `target` (a UTF-8 path) with `size` bytes from `data` so that the file is always whole, the old one or the
// new one: the bytes go to "<target>.tmp" in the same folder, are forced to the disk, and the finished file then takes the old
// one's place in one step (a rename; on Windows std::filesystem::rename is MoveFileExW with MOVEFILE_REPLACE_EXISTING). A crash,
// a kill or a full disk in the middle leaves the old file as it was, plus a stray .tmp file that the next write overwrites.
// Returns false (and why, if asked) when the file was not replaced; the old file is then untouched.
// PS2_TEST_CUT_BEFORE_RENAME=1 ends the program right before the rename, as a crash there would (tests/save_files.cpp).
inline bool replace_file(const std::string &target, const void *data, std::size_t size, std::string *why = nullptr) {
    const std::string temp = target + ".tmp";
    std::FILE *f = open(temp, "wb");
    if (!f) {
        if (why) *why = "cannot create " + temp;
        return false;
    }
    bool ok = size == 0 || std::fwrite(data, 1, size, f) == size;
    ok = std::fflush(f) == 0 && ok;
#ifdef _WIN32
    ok = _commit(_fileno(f)) == 0 && ok;
#elif !defined(__EMSCRIPTEN__)
    ok = fsync(fileno(f)) == 0 && ok;
#endif
    ok = std::fclose(f) == 0 && ok;
    std::error_code error;
    if (!ok) {
        if (why) *why = "cannot write " + temp + " (is the disk full?)";
        std::filesystem::remove(path(temp), error);
        return false;
    }
    static const bool cut = !env("PS2_TEST_CUT_BEFORE_RENAME").empty();
    if (cut) std::_Exit(3);
    // On Windows a virus scanner often opens a file that was just written for a moment, and the rename then fails with "the file
    // is used by another process" (seen with a 45 MB save state); it is tried again for up to a second.
    for (int attempt = 0; attempt < 20; ++attempt) {
        if (attempt) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        std::filesystem::rename(path(temp), path(target), error);
        if (!error) break;
    }
    if (error) {
        if (why) *why = "cannot replace " + target + ": " + error.message();
        std::filesystem::remove(path(temp), error);
        return false;
    }
    return true;
}

}
