#pragma once
// Whether the built-in draw program is created without wgpu's run-time checks (see gpu.cpp, PS2_GPU_TRUSTED).
// Default: on for Intel graphics (PCI vendor id 0x8086) in a Windows build, off everywhere else. The variable overrides
// on any platform: "1" on, "0" off, anything else (or not set) keeps the default.
#include <cstdint>
#include <string>

namespace ps2::gs::gpu {

inline bool trusted_shader(std::uint32_t vendor_id, bool is_windows, const std::string &env_value, const char **why = nullptr) {
    auto say = [&](const char *text) { if (why) *why = text; };
    if (env_value == "1") { say("PS2_GPU_TRUSTED=1"); return true; }
    if (env_value == "0") { say("PS2_GPU_TRUSTED=0"); return false; }
    if (is_windows && vendor_id == 0x8086) { say("Intel graphics on Windows; PS2_GPU_TRUSTED=0 turns it off"); return true; }
    say("");
    return false;
}

}  // namespace ps2::gs::gpu
