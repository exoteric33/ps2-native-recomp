// No game data, no graphics: the default of PS2_GPU_TRUSTED per platform and graphics vendor.
#include "gs/gpu/trusted.hpp"

#include <cstdio>

using ps2::gs::gpu::trusted_shader;

static int failed = 0;
static void check(bool got, bool want, const char *what) {
    if (got != want) { std::printf("FAIL %s\n", what); ++failed; }
}

int main() {
    check(trusted_shader(0x8086, true, ""), true, "Intel on Windows defaults on");
    check(trusted_shader(0x8086, false, ""), false, "Intel on Linux defaults off");
    check(trusted_shader(0x10de, true, ""), false, "NVIDIA on Windows defaults off");
    check(trusted_shader(0x1002, true, ""), false, "AMD on Windows defaults off");
    check(trusted_shader(0, true, ""), false, "unknown vendor defaults off");
    check(trusted_shader(0x8086, true, "0"), false, "0 turns Intel on Windows off");
    check(trusted_shader(0x10de, false, "1"), true, "1 turns NVIDIA on Linux on");
    check(trusted_shader(0x8086, false, "1"), true, "1 turns Intel on Linux on");
    check(trusted_shader(0x8086, true, "yes"), true, "other text keeps the default (on)");
    check(trusted_shader(0x10de, true, "yes"), false, "other text keeps the default (off)");
    std::printf(failed ? "trusted_shader: %d failed\n" : "trusted_shader: all passed\n", failed);
    return failed != 0;
}
