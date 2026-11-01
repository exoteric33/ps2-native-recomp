#include "vurecomp_gen.hpp"

#include "ps2/os.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#endif

namespace fs = std::filesystem;

int main(int argc, char **argv) {
#ifdef _WIN32
    // argv is in the ANSI code page and loses every character outside it: take the UTF-16 command line and use it as UTF-8
    std::vector<std::string> utf8_args;
    std::vector<char *> utf8_argv;
    int wide_argc = 0;
    if (wchar_t **wide = CommandLineToArgvW(GetCommandLineW(), &wide_argc)) {
        for (int i = 0; i < wide_argc; ++i) utf8_args.push_back(ps2::os::utf8(wide[i]));
        LocalFree(wide);
        for (std::string &a : utf8_args) utf8_argv.push_back(a.data());
        utf8_argv.push_back(nullptr);
        argc = wide_argc;
        argv = utf8_argv.data();
    }
#endif
    if ((argc != 3 && argc != 5) || (argc == 5 && std::string(argv[3]) != "--vu0-dir") ||
        (argc == 3 && std::string(argv[1]) == "-")) {
        std::fprintf(stderr, "usage: vurecomp <recorded VU1 dir | -> <out dir> [--vu0-dir <recorded VU0 dir>]\n");
        return 1;
    }
    const fs::path out_dir = ps2::os::path(argv[2]);
    std::vector<ps2::vurecomp::Input> inputs;
    const auto load = [&](const fs::path &dir, int unit) {
        std::error_code error;
        if (!fs::is_directory(dir, error)) {
            std::fprintf(stderr, "VU%d recording directory is unavailable: %s\n", unit, ps2::os::utf8(dir).c_str());
            return false;
        }
        for (const auto &e : fs::directory_iterator(dir)) {
            if (e.path().extension() != ".bin" || !e.is_regular_file()) continue;
            unsigned long long key = 0;
            unsigned entry = 0;
            int consumed = 0;
            const std::string stem = ps2::os::utf8(e.path().stem());
            const unsigned bytes = unit == 0 ? 4096u : 16384u;
            if (std::sscanf(stem.c_str(), "%llx_%x%n", &key, &entry, &consumed) != 2 || static_cast<std::size_t>(consumed) != stem.size() || entry >= bytes || (entry & 7u)) {
                std::fprintf(stderr, "VU%d invalid recording name: %s\n", unit, ps2::os::utf8(e.path()).c_str());
                continue;
            }
            ps2::vurecomp::Input in{key, entry, std::vector<std::uint8_t>(bytes), unit};
            std::ifstream f(e.path(), std::ios::binary);
            if (!f.read(reinterpret_cast<char *>(in.code.data()), bytes) || f.peek() != std::ifstream::traits_type::eof()) {
                std::fprintf(stderr, "VU%d recording must contain %u bytes: %s\n", unit, bytes, ps2::os::utf8(e.path()).c_str());
                continue;
            }
            inputs.push_back(std::move(in));
        }
        return true;
    };
    if (std::string(argv[1]) != "-" && !load(ps2::os::path(argv[1]), 1)) return 1;
    if (argc == 5 && !load(ps2::os::path(argv[4]), 0)) return 1;
    const ps2::vurecomp::Result result = ps2::vurecomp::generate_files(std::move(inputs), out_dir, 0);
    std::printf("%zu VU programs, %zu of them jump targets%s\n", result.programs, result.jump_targets, result.skipped ? ", the rest left to the interpreter" : "");
    return 0;
}
