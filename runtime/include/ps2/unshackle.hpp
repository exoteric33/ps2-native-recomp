#pragma once

#include <cstdint>

namespace ps2 {
struct Cpu;
}

namespace ps2::unshackle {

// ============================================================================
// Extended High-RAM (0x02000000..0x07FFFFFF, +96 MB above retail 32 MB)
// ============================================================================

// Enables or queries Extended High-RAM addressing for CPU, DMA, and IOP SIF/CDVD.
// Automatically enabled on the first call to high_ram_alloc() or when PS2_HIGH_RAM=1 / PS2_AUTO_EXPAND_RAM=1 is set.
void enable_high_ram(bool enabled = true);
bool high_ram_enabled();

// Allocates a block in Extended High-RAM (>= 0x02000000), aligned to `align` bytes (minimum 16 for DMA).
// Returns 0 on out-of-memory.
std::uint32_t high_ram_alloc(std::uint32_t size_bytes, std::uint32_t align = 64);

// Frees a previously allocated High-RAM block (or resets the allocator).
void high_ram_free(std::uint32_t guest_addr);
void high_ram_reset();
std::uint32_t high_ram_used_bytes();

// ============================================================================
// Generic Kernel Heap & Sub-Arena Auto-Expansion (Archetypes 1..5)
// ============================================================================

inline constexpr std::uint32_t kExpandedKernelRamTop = 0x06000000u; // 96 MB contiguous kernel limit; 0x06000000..0x07FFFFFF reserved for high_ram_alloc

enum class KernelHeapMode : std::uint8_t {
    kCompat32MB = 0,       // Strict 32 MB console behavior (default)
    kAutoExpandHighRam = 1 // Automatically expand SetupThread/SetupHeap/EndOfHeap/GetMemorySize and sub-arenas into High-RAM
};

void set_kernel_heap_mode(KernelHeapMode mode);
KernelHeapMode kernel_heap_mode();

// Called by load_elf() (system.cpp) whenever a boot or sub-ELF is loaded into guest RAM.
// Records the ELF entry point and the highest non-empty PT_LOAD segment in 0..32 MB so
// resolve_setup_thread_top() / resolve_setup_heap_end() can apply Archetype 2 & Archetype 5 safeguards.
void note_elf_loaded(std::uint32_t entry, std::uint32_t highest_low_ram_seg_vaddr, std::uint32_t highest_low_ram_seg_end);

// Resolves the top-of-stack address for SetupThread (syscall 0x3C).
std::uint32_t resolve_setup_thread_top(std::uint32_t caller_pc, std::uint32_t stack_arg, std::uint32_t size_arg);

// Resolves the end-of-heap address for SetupHeap (syscall 0x3D).
std::uint32_t resolve_setup_heap_end(std::uint32_t caller_pc, std::uint32_t heap_base, std::uint32_t heap_size, std::uint32_t thread_stack_bottom);

// Resolves the reported end-of-heap for EndOfHeap (syscall 0x3E).
std::uint32_t hook_end_of_heap(std::uint32_t caller_pc, std::uint32_t current_thread_heap);

// Resolves the reported RAM size for GetMemorySize (syscall 0x7F).
std::uint32_t resolve_memory_size();

struct ArenaRedirect {
    std::uint32_t base = 0;
    std::uint32_t size = 0;
    bool redirected = false;
};

// Registers a generic rule to redirect/scale sub-arenas initialized in base 32 MB RAM (.bss / Linker-Gap)
// whose size lies in [min_size_bytes, max_size_bytes] into High-RAM with `scale_factor`x capacity.
void register_arena_expansion_rule(std::uint32_t min_size_bytes, std::uint32_t max_size_bytes, std::uint32_t scale_factor);
void clear_arena_expansion_rules();

// Intercepts an engine's ArenaInit(desc, base, size, align) call.
// If (orig_base, orig_size) matches a registered expansion rule in 0..32 MB, allocates (orig_size * scale_factor)
// in High-RAM (reusing the same High-RAM block if the same orig_base is re-initialized on stage reload).
ArenaRedirect intercept_arena_init(std::uint32_t orig_base, std::uint32_t orig_size, std::uint32_t align = 64);

// Intercepts an engine's ArenaAlloc / BlockHeapAlloc when the native 32 MB arena returns 0 (Out-Of-Memory),
// allocating a fallback block in High-RAM.
std::uint32_t intercept_arena_alloc_overflow(std::uint32_t desc_addr, std::uint32_t size_bytes, std::uint32_t align = 64);

// Automatic entry hooks emitted by eerecomp for statically detected custom arena initializers when PS2_AUTO_EXPAND_RAM=1:
// - auto_expand_arena_a1_a2: (desc/id=$a0, base=$a1, size=$a2) -> BlockHeapInit, LinkedHeapInit, IndexedZoneInit
// - auto_expand_arena_a0_a1: (base=$a0, size=$a1)              -> BumpArenaInit
void auto_expand_arena_a1_a2(Cpu &c, std::uint32_t func_pc = 0);
void auto_expand_arena_a0_a1(Cpu &c, std::uint32_t func_pc = 0);

// ============================================================================
// Multi-Context RAM Arena Swapping
// ============================================================================

// Saves or restores a fixed guest RAM arena (in 0x00000000..0x01FFFFFF)
// into a named High-RAM backing slot in O(1) host memcpy time.
bool save_arena_slot(std::uint32_t guest_base, std::uint32_t byte_size, std::uint32_t slot_id);
bool load_arena_slot(std::uint32_t guest_base, std::uint32_t byte_size, std::uint32_t slot_id);
bool swap_arena_slot(std::uint32_t guest_base, std::uint32_t byte_size, std::uint32_t old_slot_id, std::uint32_t new_slot_id);

// ============================================================================
// Telemetry & Diagnostics (PS2_UNSHACKLE_TRACE=1)
// ============================================================================
bool trace_enabled();

}  // namespace ps2::unshackle
