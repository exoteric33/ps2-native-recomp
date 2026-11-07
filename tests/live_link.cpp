// The live compiler links a library of many objects, each named by its full path. Windows refuses a command line over 32767
// characters, so a library of a few hundred objects in a game folder with a long path used to fail with "cannot start <compiler>"
// (Final Fantasy X: 213 objects, over the limit from a path of about 100 characters). The objects now go in a response file past a size.
// No game and no disc: this makes 320 small sources in a deep folder, compiles and links them with the compiler on the PATH
// (clang++), loads the library and calls the first and the last function.
//
//   clang++ -std=c++20 -O1 -Iruntime/include -Iruntime/src tests/live_link.cpp runtime/src/live_build.cpp -o live_link_test [-ldl]
//   ./live_link_test [folder]      the folder defaults to the system's temporary one; the test makes a deep one inside it
//
// It prints the length the command line would have had without the response file, and exits 0 only when that length is over 32767
// (the test would otherwise prove nothing on Windows) and the library works.
#include "live_build.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace fs = std::filesystem;

int main(int argc, char **argv) {
    std::error_code error;
    const fs::path base = argc > 1 ? fs::path(argv[1]) : fs::temp_directory_path(error);
    // 60 characters of folder names on top of the base: a player's documents folder easily has this much
    const fs::path root = base / "ps2-live-link-test" / "a-rather-long-folder-name-like-a-players" / "PS2 Games" / "ffx" / "ee-live";
    fs::remove_all(base / "ps2-live-link-test", error);
    const fs::path work = root / "work-0123456789ab", src = work / "src";
    fs::create_directories(src, error);
    if (error) {
        std::fprintf(stderr, "cannot make %s\n", src.string().c_str());
        return 2;
    }
    constexpr int kFiles = 320;
    ps2::live::Job job;
    std::size_t line = 0;
    for (int i = 0; i < kFiles; ++i) {
        char name[96];
        std::snprintf(name, sizeof name, "m_main_00100000_6B68A31F_0123456789ab_unit_%04X.cpp", i);
        const fs::path path = src / name;
        std::ofstream(path) << "extern \"C\" int ps2_live_link_test_" << i << "() { return " << i + 1 << "; }\n";
        job.sources.push_back(path);
        fs::path object = path;
        object.replace_extension(".o");
        line += object.string().size() + 3;
    }
    std::printf("the objects alone would take %zu characters of a command line (Windows allows 32767)\n", line);
    if (line <= 32767) {
        std::fprintf(stderr, "too short to prove anything: %zu characters\n", line);
        return 3;
    }
    job.work = work;
    job.include_dir = work / "include";  // empty: "-I" with nothing after it would take the next argument, -c
    fs::create_directories(job.include_dir, error);
    job.library = work / ("library" + std::string(ps2::live::kLibraryExtension));
    job.compiler = "clang++";
    job.flags = ps2::live::compile_flags("-O0");
    job.jobs = 4;
    job.below_normal = false;
    job.say = [](const std::string &text) { std::fprintf(stderr, "%s\n", text.c_str()); };
    ps2::live::Timing timing;
    if (!ps2::live::compile_and_link(job, timing)) {
        std::fprintf(stderr, "FAIL: compile_and_link returned false\n");
        return 1;
    }
    int first = 0, last = 0;
#ifdef _WIN32
    HMODULE lib = LoadLibraryW(job.library.c_str());
    if (!lib) {
        std::fprintf(stderr, "FAIL: cannot load the library (error %lu)\n", GetLastError());
        return 1;
    }
    auto f = reinterpret_cast<int (*)()>(GetProcAddress(lib, "ps2_live_link_test_0"));
    auto l = reinterpret_cast<int (*)()>(GetProcAddress(lib, ("ps2_live_link_test_" + std::to_string(kFiles - 1)).c_str()));
#else
    void *lib = dlopen(job.library.c_str(), RTLD_NOW);
    if (!lib) {
        std::fprintf(stderr, "FAIL: cannot load the library: %s\n", dlerror());
        return 1;
    }
    auto f = reinterpret_cast<int (*)()>(dlsym(lib, "ps2_live_link_test_0"));
    auto l = reinterpret_cast<int (*)()>(dlsym(lib, ("ps2_live_link_test_" + std::to_string(kFiles - 1)).c_str()));
#endif
    if (!f || !l) {
        std::fprintf(stderr, "FAIL: the functions are not in the library\n");
        return 1;
    }
    first = f();
    last = l();
    std::printf("linked %d objects (%.1f s compile, %.1f s link); first returns %d, last returns %d\n", kFiles, timing.compile, timing.link, first, last);
    if (first != 1 || last != kFiles) {
        std::fprintf(stderr, "FAIL: wrong results\n");
        return 1;
    }
    fs::remove_all(base / "ps2-live-link-test", error);
    std::printf("PASS\n");
    return 0;
}
