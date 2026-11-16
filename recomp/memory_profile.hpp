#pragma once

#include "analysis.hpp"
#include "elf.hpp"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace recomp {

enum class MemoryArchetype : std::uint8_t {
    kUnknown = 0,
    kDynamicKernelAutoStack = 1,   // Archetype 1: SetupThread(stack=-1) + SetupHeap(base, -1) (MH1, MHG, GTA3, RE Outbreak, PSU)
    kDynamicKernelFixedStack = 2,  // Archetype 2: SetupThread(fixed ~32MB) + SetupHeap(base, -1) (DQ8, Rogue Galaxy, Sims, Norrath)
    kStaticBssSubAllocators = 3,   // Archetype 3: Mega-.bss / Linker-Gap + custom sub-allocators (MH2, King's Field IV, Onimusha, PES6)
    kLowHeapHighStreaming = 4,     // Archetype 4: Small low SetupHeap + unmapped upper streaming arena (FF12, RE4, MGS2, DOA2)
    kHighBootstrapperOrPacker = 5, // Archetype 5: High bootstrapper (entry >= 8MB) or packed stub (GT4, 007 Nightfire, R63, WRC3)
};

const char *archetype_name(MemoryArchetype a);
const char *archetype_strategy(MemoryArchetype a);

struct AllocatorCandidate {
    enum class Kind : std::uint8_t {
        kBumpArenaInit,
        kBlockHeapInit,
        kLinkedHeapInit,
        kIndexedZoneInit,
        kBumpArenaAlloc,
        kBlockHeapAlloc,
        kStaticPartitionInit,
    };
    Kind kind{};
    std::string module_name;  // empty for main ELF, or overlay/module name
    std::uint32_t func_addr{};
    std::uint32_t insn_count{};
};

const char *candidate_kind_name(AllocatorCandidate::Kind k);

struct MemoryProfile {
    std::uint32_t entry{};
    std::size_t pt_load_count{};
    std::uint32_t lowest_vaddr{};
    std::uint32_t highest_low_ram_end{};
    std::uint64_t code_file_bytes{};
    std::uint64_t bss_bytes{};

    // Linker gap between PT_LOAD segments inside 0..32MB (e.g. MH2's 17.8 MB gap)
    std::uint32_t max_gap_start{};
    std::uint32_t max_gap_end{};
    std::uint32_t max_gap_bytes{};

    // Special PT_LOAD segment flags discovered across PS2 library
    bool has_blocking_high_segment{};      // Non-empty PT_LOAD in 0x01000000..0x01FFFFFF above heap_base (e.g. Norrath 0x01E00000)
    std::uint32_t highest_Segment_vaddr{}; // Highest vaddr of a non-empty PT_LOAD in 0..32MB
    bool has_devkit_high_segments{};       // PT_LOAD in 0x02000000..0x07FFFFFF (DTL-T10000 128MB DevKit sections: Onimusha, PES6)
    bool has_scratchpad_segments{};        // PT_LOAD at 0x70000000..0x70004000 (WRC3)

    // Simulated crt0 syscalls
    bool has_setup_thread{};
    std::uint32_t gp{};
    std::uint32_t stack_base{};
    std::uint32_t stack_size{};

    bool has_setup_heap{};
    std::uint32_t heap_base{};
    std::uint32_t heap_size{};

    // Static code scan counters
    std::uint32_t end_of_heap_Refer_count{};
    std::uint32_t get_memory_size_count{};
    std::uint32_t init_tlb_count{};
    std::vector<std::pair<std::uint32_t, std::uint32_t>> hardcoded_32mb_bounds; // (func_addr, insn_pc) for `lui $r, 0x0200`

    // Classification & estimated RAM unlocking
    MemoryArchetype archetype{MemoryArchetype::kUnknown};
    std::uint32_t native_kernel_heap_bytes{};
    std::uint32_t expanded_kernel_heap_bytes{};

    // Detected custom allocator functions and partition tables
    std::vector<AllocatorCandidate> allocators;
};

struct OverlayAnalysisView {
    std::string name;
    const Image *image{};
    const Program *program{};
};

MemoryProfile analyze_memory_profile(const Image &main_image, const Program &main_program,
                                     const std::vector<OverlayAnalysisView> &overlays = {});

std::string format_memory_profile_json(const MemoryProfile &profile);
void write_memory_profile_json(const MemoryProfile &profile, const std::string &path);
void print_memory_profile_summary(const MemoryProfile &profile);

}  // namespace recomp
