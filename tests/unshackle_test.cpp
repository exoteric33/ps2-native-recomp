#include "ps2/memory.hpp"
#include "ps2/unshackle.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

int g_passed = 0;
int g_failed = 0;

void check(bool cond, const char *msg) {
    if (cond) {
        ++g_passed;
    } else {
        ++g_failed;
        std::fprintf(stderr, "FAIL: %s\n", msg);
    }
}

}  // namespace

int main() {
    using namespace ps2;
    using namespace ps2::unshackle;

    // 1. Default 32 MB isolation: High-RAM disabled by default
    enable_high_ram(false);
    check(!high_ram_enabled(), "High-RAM is disabled by default");
    check(ee_ram_size() == kRamSize, "ee_ram_size() is 32 MiB when High-RAM is disabled");
    check(ee_ram_mask() == (kRamSize - 1u), "ee_ram_mask() is 0x01FFFFFF when High-RAM is disabled");

    // 2. Extended High-RAM (> 32 MB) allocation and zero-aliasing with base 32 MB RAM
    high_ram_reset();
    const std::uint32_t a1 = high_ram_alloc(4u << 20, 64u); // 4 MB stage slot 0
    const std::uint32_t a2 = high_ram_alloc(4u << 20, 64u); // 4 MB stage slot 1
    check(a1 == kHighRamBase, "first High-RAM allocation starts at 0x02000000");
    check(a2 == kHighRamBase + (4u << 20), "second High-RAM allocation is contiguous at 0x02400000");
    check(high_ram_enabled(), "high_ram_alloc automatically enables High-RAM mode");
    check(ee_ram_size() == kTotalRamSize, "ee_ram_size() is 128 MiB when High-RAM is enabled");
    check(ee_ram_mask() == (kTotalRamSize - 1u), "ee_ram_mask() is 0x07FFFFFF when High-RAM is enabled");

    // Verify writing to High-RAM (0x02000000) does NOT alias/overwrite base RAM (0x00000000)
    st32(0x00001000u, 0x11223344u);
    st32(a1 + 0x1000u, 0xAABBCCDDu);
    check(ld32(0x00001000u) == 0x11223344u, "base 32 MB RAM is untouched by High-RAM write");
    check(ld32(a1 + 0x1000u) == 0xAABBCCDDu, "High-RAM read returns exact value written");

    // Verify uncached mirror (0x20000000 + High-RAM offset) also resolves without 32 MB folding
    check(slow_ld32(0x20000000u + a1 + 0x1000u) == 0xAABBCCDDu, "uncached mirror 0x20000000+ reaches High-RAM");

    high_ram_free(a1);
    high_ram_free(a2);
    check(high_ram_used_bytes() == 0u, "High-RAM allocator coalesces freed blocks back to 0 used bytes");

    // 3. Multi-Context RAM Arena Swapping
    constexpr std::uint32_t kArenaBase = 0x00A00000u;
    constexpr std::uint32_t kArenaSize = 64u * 1024u;
    std::memset(g_ram + kArenaBase, 0x41, kArenaSize); // Slot 0 content ('A')
    check(save_arena_slot(kArenaBase, kArenaSize, 0u), "save_arena_slot slot 0 succeeds");

    std::memset(g_ram + kArenaBase, 0x42, kArenaSize); // Slot 1 content ('B')
    check(swap_arena_slot(kArenaBase, kArenaSize, 1u, 0u), "swap_arena_slot 1 -> 0 succeeds");
    check(ld8(kArenaBase) == 0x41u && ld8(kArenaBase + kArenaSize - 1u) == 0x41u, "arena restored to slot 0 ('A')");

    check(swap_arena_slot(kArenaBase, kArenaSize, 0u, 1u), "swap_arena_slot 0 -> 1 succeeds");
    check(ld8(kArenaBase) == 0x42u && ld8(kArenaBase + kArenaSize - 1u) == 0x42u, "arena restored to slot 1 ('B')");

    // 4. Generic Kernel Heap & Sub-Arena Auto-Expansion across all 5 PS2 Memory Archetypes
    set_kernel_heap_mode(KernelHeapMode::kCompat32MB);
    note_elf_loaded(0x00100008u, 0x00100000u, 0x00E00000u);
    check(resolve_memory_size() == kRamSize, "compat 32MB mode reports 32 MB for GetMemorySize");
    check(resolve_setup_thread_top(0x00100100u, 0xFFFFFFFFu, 0x80000u) == kRamSize - 0x1000u,
          "compat 32MB mode places auto-stack at 0x01FFF000");

    // Enable auto-expansion mode (0x02000000..0x05FFFFFF for contiguous kernel heap, 0x06000000..0x07FFFFFF for explicit pool)
    high_ram_reset();
    set_kernel_heap_mode(KernelHeapMode::kAutoExpandHighRam);
    check(resolve_memory_size() == kExpandedKernelRamTop, "Archetype 4 (FF12/RE4/MGS2): GetMemorySize promoted to 96 MB (0x06000000)");

    // Archetype 1 (MH1, MHG, GTA3, RE Outbreak, PSU): SetupThread(stack=-1) + SetupHeap(base, -1)
    note_elf_loaded(0x00100008u, 0x00100000u, 0x00E00000u);
    const std::uint32_t arch1_top = resolve_setup_thread_top(0x00100100u, 0xFFFFFFFFu, 0x80000u);
    const std::uint32_t arch1_stack_bottom = arch1_top - 0x80000u;
    const std::uint32_t arch1_heap_end = resolve_setup_heap_end(0x00100120u, 0x00E00000u, 0xFFFFFFFFu, arch1_stack_bottom);
    check(arch1_top == kExpandedKernelRamTop - 0x1000u, "Archetype 1 (MHG/GTA3): auto-stack top expanded to 0x05FFF000");
    check(arch1_heap_end - 0x00E00000u > (80u << 20), "Archetype 1 (MHG/GTA3): contiguous kernel heap expanded from 17 MB to >81 MB");

    // Archetype 2A (Dragon Quest VIII, Rogue Galaxy, Sims): SetupThread(fixed stack at 0x01F60000) + SetupHeap(base, -1), no high PT_LOAD
    note_elf_loaded(0x00100008u, 0x00100000u, 0x00486C80u);
    const std::uint32_t arch2a_top = resolve_setup_thread_top(0x00100100u, 0x01F60000u, 0xA0000u);
    const std::uint32_t arch2a_stack_bottom = arch2a_top - 0xA0000u;
    const std::uint32_t arch2a_heap_end = resolve_setup_heap_end(0x00100120u, 0x00486C80u, 0xFFFFFFFFu, arch2a_stack_bottom);
    check(arch2a_top == kExpandedKernelRamTop, "Archetype 2A (DQ8): fixed 32MB stack relocated to 0x06000000");
    check(arch2a_heap_end - 0x00486C80u > (90u << 20), "Archetype 2A (DQ8): contiguous kernel heap expanded from 26.8 MB to >90 MB");

    // Archetype 2B (Champions of Norrath): High PT_LOAD at 0x01E00000..0x01F18200 blocks contiguous stack relocation
    note_elf_loaded(0x00100008u, 0x01E00000u, 0x01F18200u);
    const std::uint32_t arch2b_top = resolve_setup_thread_top(0x00100100u, 0x01FC0000u, 0x40000u);
    check(arch2b_top == 0x02000000u, "Archetype 2B (Norrath): high PT_LOAD safeguard keeps stack at 0x01FC0000..0x02000000");

    // Archetype 5 (Gran Turismo 4 / 007 Nightfire / Rainbow Six 3): High bootstrapper caller_pc >= 0x00800000
    note_elf_loaded(0x01000008u, 0x0102DC80u, 0x010519ACu);
    const std::uint32_t arch5_top = resolve_setup_thread_top(0x01000100u, 0x01FF8000u, 0x8000u);
    check(arch5_top == 0x02000000u, "Archetype 5 (GT4 bootstrapper): caller_pc >= 8MB safeguard preserves 32MB layout");

    // Archetype 3 (MH2, King's Field IV, Onimusha, PES6): Sub-Arena Redirection & Overflow into explicit pool (0x06000000..0x07FFFFFF)
    clear_arena_expansion_rules();
    register_arena_expansion_rule(4u << 20, 16u << 20, 4u); // Scale 4..16 MB sub-arenas by 4x in High-RAM
    const ArenaRedirect redir1 = intercept_arena_init(0x00AC8000u, 0x0055E000u, 64u); // MH2 main sub-heap (~5.36 MB)
    check(redir1.redirected, "Archetype 3 (MH2): 5.36 MB sub-arena redirected to High-RAM");
    check(redir1.base == kExpandedKernelRamTop, "Archetype 3 (MH2): redirected arena starts in explicit pool at 0x06000000 (no collision with 0x02000000..0x05FFFFFF)");
    check(redir1.size == 0x0055E000u * 4u, "Archetype 3 (MH2): redirected arena scaled 4x to 21.46 MB");

    // Verify re-initializing the same arena on stage reload reuses the exact same High-RAM block without leaking
    const ArenaRedirect redir2 = intercept_arena_init(0x00AC8000u, 0x0055E000u, 64u);
    check(redir2.redirected && redir2.base == redir1.base && redir2.size == redir1.size,
          "Archetype 3 (MH2): stage reload reuses existing High-RAM block without memory leak");

    // Verify overflow fallback allocation
    const std::uint32_t ovf_addr = intercept_arena_alloc_overflow(0x006AE040u, 256u * 1024u, 64u);
    check(ovf_addr >= kExpandedKernelRamTop + redir1.size, "Archetype 3: arena overflow allocates cleanly after redirected sub-arena");

    clear_arena_expansion_rules();
    set_kernel_heap_mode(KernelHeapMode::kCompat32MB);
    high_ram_reset();

    std::printf("unshackle: %d of %d checks passed, %d failed\n", g_passed, g_passed + g_failed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
