#include "ps2/unshackle.hpp"

#include "ps2/cpu.hpp"
#include "ps2/memory.hpp"
#include "ps2/system.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace ps2::unshackle {

namespace {

std::mutex g_alloc_mutex;
struct HighRamBlock {
    std::uint32_t addr = 0;
    std::uint32_t size = 0;
    bool free = true;
};
std::vector<HighRamBlock> g_high_blocks;
std::uint32_t g_high_used_bytes = 0;
bool g_lower_high_ram_unlocked = false;

struct ArenaSlotKey {
    std::uint32_t guest_base = 0;
    std::uint32_t slot_id = 0;
    bool operator==(const ArenaSlotKey &o) const {
        return guest_base == o.guest_base && slot_id == o.slot_id;
    }
};
struct ArenaSlotKeyHash {
    std::size_t operator()(const ArenaSlotKey &k) const {
        return (static_cast<std::size_t>(k.guest_base) << 16) ^ static_cast<std::size_t>(k.slot_id);
    }
};
struct ArenaBacking {
    std::uint32_t high_addr = 0;
    std::uint32_t size = 0;
};
std::unordered_map<ArenaSlotKey, ArenaBacking, ArenaSlotKeyHash> g_arena_slots;

struct ArenaExpansionRule {
    std::uint32_t min_size = 0;
    std::uint32_t max_size = 0;
    std::uint32_t scale_factor = 1;
};
std::vector<ArenaExpansionRule> g_arena_rules;
std::unordered_map<std::uint32_t, ArenaBacking> g_redirected_arenas;

std::atomic<KernelHeapMode> g_kernel_heap_mode{[] {
    if (const char *s = std::getenv("PS2_AUTO_EXPAND_RAM")) {
        if (std::atoi(s) != 0) return KernelHeapMode::kAutoExpandHighRam;
    }
    return KernelHeapMode::kCompat32MB;
}()};

std::uint32_t g_elf_entry = 0x00100008u;
std::uint32_t g_highest_low_seg_vaddr = 0u;
std::uint32_t g_highest_low_seg_end = 0u;
bool g_kernel_stack_relocated_high = false;
bool g_kernel_heap_expanded_high = false;

std::uint32_t explicit_pool_base_locked() {
    return (g_kernel_heap_mode.load(std::memory_order_relaxed) == KernelHeapMode::kAutoExpandHighRam)
               ? kExpandedKernelRamTop
               : kHighRamBase;
}

std::uint32_t explicit_pool_size_locked() {
    const std::uint32_t base = explicit_pool_base_locked();
    return kTotalRamSize - base;
}

void ensure_allocator_locked() {
    if (g_high_blocks.empty()) {
        g_lower_high_ram_unlocked = (explicit_pool_base_locked() == kHighRamBase);
        g_high_blocks.push_back({explicit_pool_base_locked(), explicit_pool_size_locked(), true});
    }
}

void unlock_lower_high_ram_if_safe_locked() {
    if (g_lower_high_ram_unlocked || g_kernel_heap_expanded_high) return;
    g_lower_high_ram_unlocked = true;
    const std::uint32_t lower_end = g_kernel_stack_relocated_high ? (kExpandedKernelRamTop - 0x00200000u) : kExpandedKernelRamTop;
    if (lower_end <= kHighRamBase) return;
    const std::uint32_t lower_size = lower_end - kHighRamBase;
    if (!g_high_blocks.empty() && g_high_blocks.front().free && g_high_blocks.front().addr == lower_end) {
        g_high_blocks.front().addr = kHighRamBase;
        g_high_blocks.front().size += lower_size;
    } else {
        g_high_blocks.insert(g_high_blocks.begin(), {kHighRamBase, lower_size, true});
    }
}

}  // namespace

// ============================================================================
// Extended High-RAM (0x02000000..0x07FFFFFF)
// ============================================================================

void enable_high_ram(bool enabled) {
    g_high_ram_enabled = enabled;
    if (!g_watch_on) g_store_ram_end = ee_ram_size();
}

bool high_ram_enabled() {
    return g_high_ram_enabled;
}

std::uint32_t high_ram_alloc(std::uint32_t size_bytes, std::uint32_t align) {
    if (size_bytes == 0) return 0;
    align = std::max<std::uint32_t>(align, 16u);
    if ((align & (align - 1u)) != 0) {
        std::uint32_t p = 16u;
        while (p < align) p <<= 1;
        align = p;
    }
    const std::uint32_t rounded_size = (size_bytes + 15u) & ~15u;

    std::lock_guard<std::mutex> lock(g_alloc_mutex);
    enable_high_ram(true);
    ensure_allocator_locked();

    for (int pass = 0; pass < 2; ++pass) {
        for (std::size_t i = 0; i < g_high_blocks.size(); ++i) {
            HighRamBlock b = g_high_blocks[i];
            if (!b.free) continue;
            const std::uint32_t aligned_addr = (b.addr + align - 1u) & ~(align - 1u);
            const std::uint32_t pad = aligned_addr - b.addr;
            if (b.size < pad + rounded_size) continue;

            const std::uint32_t tail_addr = aligned_addr + rounded_size;
            const std::uint32_t tail_size = b.size - pad - rounded_size;

            g_high_blocks[i] = {aligned_addr, rounded_size, false};
            if (pad > 0) {
                g_high_blocks.insert(g_high_blocks.begin() + static_cast<std::ptrdiff_t>(i), {b.addr, pad, true});
                ++i;
            }
            if (tail_size > 0) {
                g_high_blocks.insert(g_high_blocks.begin() + static_cast<std::ptrdiff_t>(i + 1), {tail_addr, tail_size, true});
            }
            g_high_used_bytes += rounded_size;
            if (trace_enabled()) {
                std::fprintf(stderr, "[unshackle] high_ram_alloc(%u bytes, align %u) -> 0x%08X (used: %.2f MB / %.2f MB)\n",
                             size_bytes, align, aligned_addr,
                             static_cast<double>(g_high_used_bytes) / (1024.0 * 1024.0),
                             static_cast<double>(kHighRamSize) / (1024.0 * 1024.0));
            }
            return aligned_addr;
        }
        if (pass == 0) {
            unlock_lower_high_ram_if_safe_locked();
        }
    }
    std::fprintf(stderr, "[unshackle] high_ram_alloc failed: out of High-RAM for %u bytes\n", size_bytes);
    return 0;
}

void high_ram_free(std::uint32_t guest_addr) {
    if (guest_addr < kHighRamBase || guest_addr >= kTotalRamSize) return;
    std::lock_guard<std::mutex> lock(g_alloc_mutex);
    for (std::size_t i = 0; i < g_high_blocks.size(); ++i) {
        if (g_high_blocks[i].addr == guest_addr && !g_high_blocks[i].free) {
            g_high_blocks[i].free = true;
            g_high_used_bytes -= std::min(g_high_used_bytes, g_high_blocks[i].size);
            if (i + 1 < g_high_blocks.size() && g_high_blocks[i + 1].free &&
                g_high_blocks[i].addr + g_high_blocks[i].size == g_high_blocks[i + 1].addr) {
                g_high_blocks[i].size += g_high_blocks[i + 1].size;
                g_high_blocks.erase(g_high_blocks.begin() + static_cast<std::ptrdiff_t>(i + 1));
            }
            if (i > 0 && g_high_blocks[i - 1].free &&
                g_high_blocks[i - 1].addr + g_high_blocks[i - 1].size == g_high_blocks[i].addr) {
                g_high_blocks[i - 1].size += g_high_blocks[i].size;
                g_high_blocks.erase(g_high_blocks.begin() + static_cast<std::ptrdiff_t>(i));
            }
            return;
        }
    }
}

void high_ram_reset() {
    std::lock_guard<std::mutex> lock(g_alloc_mutex);
    g_high_blocks.clear();
    g_lower_high_ram_unlocked = (explicit_pool_base_locked() == kHighRamBase);
    g_high_blocks.push_back({explicit_pool_base_locked(), explicit_pool_size_locked(), true});
    g_high_used_bytes = 0;
    g_kernel_stack_relocated_high = false;
    g_kernel_heap_expanded_high = false;
    g_arena_slots.clear();
    g_redirected_arenas.clear();
    std::memset(g_ram + kHighRamBase, 0, kHighRamSize);
}

std::uint32_t high_ram_used_bytes() {
    std::lock_guard<std::mutex> lock(g_alloc_mutex);
    return g_high_used_bytes;
}

// ============================================================================
// PILLAR 2B: Generic Kernel Heap & Sub-Arena Auto-Expansion (Archetypes 1..5)
// ============================================================================

void set_kernel_heap_mode(KernelHeapMode mode) {
    std::lock_guard<std::mutex> lock(g_alloc_mutex);
    const KernelHeapMode prev = g_kernel_heap_mode.exchange(mode, std::memory_order_relaxed);
    if (mode == KernelHeapMode::kAutoExpandHighRam) {
        enable_high_ram(true);
    }
    if (prev != mode && g_high_used_bytes == 0) {
        g_high_blocks.clear();
        g_lower_high_ram_unlocked = (explicit_pool_base_locked() == kHighRamBase);
        g_high_blocks.push_back({explicit_pool_base_locked(), explicit_pool_size_locked(), true});
    }
}

KernelHeapMode kernel_heap_mode() {
    return g_kernel_heap_mode.load(std::memory_order_relaxed);
}

void note_elf_loaded(std::uint32_t entry, std::uint32_t highest_low_ram_seg_vaddr, std::uint32_t highest_low_ram_seg_end) {
    std::lock_guard<std::mutex> lock(g_alloc_mutex);
    g_elf_entry = entry;
    g_highest_low_seg_vaddr = highest_low_ram_seg_vaddr;
    g_highest_low_seg_end = highest_low_ram_seg_end;
    g_kernel_stack_relocated_high = false;
    g_kernel_heap_expanded_high = false;
    if (g_kernel_heap_mode.load(std::memory_order_relaxed) == KernelHeapMode::kAutoExpandHighRam) {
        enable_high_ram(true);
    }
}

std::uint32_t resolve_setup_thread_top(std::uint32_t caller_pc, std::uint32_t stack_arg, std::uint32_t size_arg) {
    const std::uint32_t std_top = (stack_arg == 0xFFFFFFFFu) ? (kRamSize - 0x1000u) : (stack_arg + size_arg);
    if (kernel_heap_mode() != KernelHeapMode::kAutoExpandHighRam) {
        return std_top;
    }
    enable_high_ram(true);

    // Archetype 5 safeguard: if the caller is a high-memory bootstrapper/unpacker (e.g. Gran Turismo 4 at 0x01000000,
    // Rainbow Six 3 at 0x01400000, 007 Nightfire launcher at 0x00A00000), preserve its native 32 MB stack so it can
    // safely unpack the target engine into low RAM.
    if (caller_pc >= 0x00800000u) {
        return std_top;
    }

    // Archetype 2B safeguard: if the ELF has a static PT_LOAD segment in upper low-RAM (e.g. Champions of Norrath at 0x01E00000),
    // do not move the stack above 32 MB, because a contiguous SetupHeap(base, -1) would grow across and overwrite that segment.
    if (g_highest_low_seg_vaddr >= 0x01000000u) {
        return std_top;
    }

    // Archetype 1: SetupThread(stack = -1) -> place stack at top of 96 MB contiguous kernel window (0x05FFF000)
    if (stack_arg == 0xFFFFFFFFu) {
        const std::uint32_t exp_top = kExpandedKernelRamTop - 0x1000u;
        g_kernel_stack_relocated_high = true;
        if (trace_enabled()) {
            std::fprintf(stderr, "[unshackle] SetupThread(stack=-1, sz=0x%X) -> expanded top 0x%08X (was 0x%08X)\n",
                         size_arg, exp_top, std_top);
        }
        return exp_top;
    }

    // Archetype 2A: SetupThread with fixed stack near top of 32 MB (e.g. Dragon Quest VIII, Rogue Galaxy, Sims, Yu-Gi-Oh CMC)
    if (std_top >= 0x01F00000u && std_top <= kRamSize && stack_arg >= g_highest_low_seg_end) {
        const std::uint32_t exp_top = kExpandedKernelRamTop;
        g_kernel_stack_relocated_high = true;
        if (trace_enabled()) {
            std::fprintf(stderr, "[unshackle] SetupThread(stack=0x%08X, sz=0x%X) -> relocated top 0x%08X (was 0x%08X)\n",
                         stack_arg, size_arg, exp_top, std_top);
        }
        return exp_top;
    }

    return std_top;
}

std::uint32_t resolve_setup_heap_end(std::uint32_t caller_pc, std::uint32_t heap_base, std::uint32_t heap_size, std::uint32_t thread_stack_bottom) {
    const std::uint32_t std_end = (heap_size == 0xFFFFFFFFu) ? thread_stack_bottom : (heap_base + heap_size);
    if (kernel_heap_mode() != KernelHeapMode::kAutoExpandHighRam) {
        return std_end;
    }
    enable_high_ram(true);

    if (caller_pc >= 0x00800000u) {
        return std_end;
    }

    if (heap_size == 0xFFFFFFFFu) {
        std::uint32_t exp_end = thread_stack_bottom;
        if (g_highest_low_seg_vaddr >= 0x01000000u && g_highest_low_seg_vaddr > heap_base) {
            exp_end = std::min(std_end, kRamSize);
        } else if (thread_stack_bottom <= heap_base) {
            // If stack was placed below heap_base (or SetupHeap called before SetupThread relocation), expand to 96 MB limit
            exp_end = kExpandedKernelRamTop - 0x1000u;
        }
        if (exp_end > kRamSize) {
            g_kernel_heap_expanded_high = true;
        }
        if (trace_enabled() && exp_end > heap_base) {
            const std::uint32_t orig_top = (thread_stack_bottom > kRamSize) ? (kRamSize - (kExpandedKernelRamTop - thread_stack_bottom)) : std::min(std_end, kRamSize);
            const double new_mib = static_cast<double>(exp_end - heap_base) / (1024.0 * 1024.0);
            const double orig_mib = (orig_top > heap_base) ? static_cast<double>(orig_top - heap_base) / (1024.0 * 1024.0) : new_mib;
            std::fprintf(stderr, "[unshackle] SetupHeap(base=0x%08X, sz=-1) -> end 0x%08X (%.2f MiB, was %.2f MiB)\n",
                         heap_base, exp_end, new_mib, orig_mib);
        }
        return exp_end;
    }

    return std_end;
}

std::uint32_t hook_end_of_heap(std::uint32_t caller_pc, std::uint32_t current_thread_heap) {
    (void)caller_pc;
    return current_thread_heap;
}

std::uint32_t resolve_memory_size() {
    if (kernel_heap_mode() == KernelHeapMode::kAutoExpandHighRam) {
        enable_high_ram(true);
        if (trace_enabled()) {
            std::fprintf(stderr, "[unshackle] GetMemorySize() -> 0x%08X (%.1f MiB)\n",
                         kExpandedKernelRamTop, static_cast<double>(kExpandedKernelRamTop) / (1024.0 * 1024.0));
        }
        return kExpandedKernelRamTop;
    }
    return kRamSize;
}

void register_arena_expansion_rule(std::uint32_t min_size_bytes, std::uint32_t max_size_bytes, std::uint32_t scale_factor) {
    if (scale_factor == 0) return;
    std::lock_guard<std::mutex> lock(g_alloc_mutex);
    g_arena_rules.push_back({min_size_bytes, max_size_bytes, scale_factor});
}

void clear_arena_expansion_rules() {
    std::lock_guard<std::mutex> lock(g_alloc_mutex);
    g_arena_rules.clear();
    g_redirected_arenas.clear();
}

ArenaRedirect intercept_arena_init(std::uint32_t orig_base, std::uint32_t orig_size, std::uint32_t align) {
    if (orig_size == 0 || orig_base >= kRamSize) {
        return {orig_base, orig_size, false};
    }

    std::uint32_t scale = 0;
    {
        std::lock_guard<std::mutex> lock(g_alloc_mutex);
        auto existing = g_redirected_arenas.find(orig_base);
        if (existing != g_redirected_arenas.end() && existing->second.size >= orig_size) {
            std::memset(g_ram + existing->second.high_addr, 0, existing->second.size);
            return {existing->second.high_addr, existing->second.size, true};
        }
        for (const auto &rule : g_arena_rules) {
            if (orig_size >= rule.min_size && orig_size <= rule.max_size) {
                scale = rule.scale_factor;
                break;
            }
        }
        if (scale == 0 && g_arena_rules.empty() &&
            g_kernel_heap_mode.load(std::memory_order_relaxed) == KernelHeapMode::kAutoExpandHighRam &&
            orig_base >= 0x00400000u && orig_size >= (512u << 10) && orig_size <= (28u << 20)) {
            scale = 2u;
        }
    }

    if (scale == 0) {
        return {orig_base, orig_size, false};
    }

    const std::uint64_t scaled64 = static_cast<std::uint64_t>(orig_size) * scale;
    const std::uint32_t new_size = static_cast<std::uint32_t>(std::min<std::uint64_t>(scaled64, 64ull << 20));
    const std::uint32_t high_addr = high_ram_alloc(new_size, std::max<std::uint32_t>(align, 64u));
    if (!high_addr) {
        return {orig_base, orig_size, false};
    }

    std::memset(g_ram + high_addr, 0, new_size);
    {
        std::lock_guard<std::mutex> lock(g_alloc_mutex);
        g_redirected_arenas[orig_base] = {high_addr, new_size};
    }
    if (trace_enabled()) {
        std::fprintf(stderr, "[unshackle] intercept_arena_init(0x%08X, %.2f MiB) -> High-RAM 0x%08X (%.2f MiB, %ux)\n",
                     orig_base, static_cast<double>(orig_size) / (1024.0 * 1024.0),
                     high_addr, static_cast<double>(new_size) / (1024.0 * 1024.0), scale);
    }
    return {high_addr, new_size, true};
}

std::uint32_t intercept_arena_alloc_overflow(std::uint32_t desc_addr, std::uint32_t size_bytes, std::uint32_t align) {
    if (size_bytes == 0) return 0;
    const std::uint32_t addr = high_ram_alloc(size_bytes, std::max<std::uint32_t>(align, 64u));
    if (addr && trace_enabled()) {
        std::fprintf(stderr, "[unshackle] arena overflow (desc=0x%08X, req=%u bytes) satisfied in High-RAM at 0x%08X\n",
                     desc_addr, size_bytes, addr);
    }
    return addr;
}

void auto_expand_arena_a1_a2(Cpu &c, std::uint32_t func_pc) {
    (void)func_pc;
    if (kernel_heap_mode() != KernelHeapMode::kAutoExpandHighRam) return;
    const std::uint32_t orig_base = c.r[5].w[0];
    const std::uint32_t orig_size = c.r[6].w[0];
    if (orig_base < 0x00400000u || orig_base >= kRamSize || orig_size < (512u << 10) || orig_size > (28u << 20)) return;
    if (static_cast<std::uint64_t>(orig_base) + orig_size > kRamSize) return;
    const ArenaRedirect redir = intercept_arena_init(orig_base, orig_size, 65536u);
    if (redir.redirected) {
        c.r[5].sd[0] = static_cast<std::int32_t>(redir.base);
        c.r[6].sd[0] = static_cast<std::int32_t>(redir.size);
    }
}

void auto_expand_arena_a0_a1(Cpu &c, std::uint32_t func_pc) {
    (void)func_pc;
    if (kernel_heap_mode() != KernelHeapMode::kAutoExpandHighRam) return;
    const std::uint32_t orig_base = c.r[4].w[0];
    const std::uint32_t orig_size = c.r[5].w[0];
    if (orig_base < 0x00400000u || orig_base >= kRamSize || orig_size < (512u << 10) || orig_size > (28u << 20)) return;
    if (static_cast<std::uint64_t>(orig_base) + orig_size > kRamSize) return;
    const ArenaRedirect redir = intercept_arena_init(orig_base, orig_size, 65536u);
    if (redir.redirected) {
        c.r[4].sd[0] = static_cast<std::int32_t>(redir.base);
        c.r[5].sd[0] = static_cast<std::int32_t>(redir.size);
    }
}

// ============================================================================
// Multi-Context RAM Arena Swapping
// ============================================================================

bool save_arena_slot(std::uint32_t guest_base, std::uint32_t byte_size, std::uint32_t slot_id) {
    if (byte_size == 0 || static_cast<std::uint64_t>(guest_base) + byte_size > kRamSize) return false;
    std::uint32_t high_addr = 0;
    {
        std::lock_guard<std::mutex> lock(g_alloc_mutex);
        auto it = g_arena_slots.find({guest_base, slot_id});
        if (it != g_arena_slots.end() && it->second.size >= byte_size) {
            high_addr = it->second.high_addr;
        }
    }
    if (!high_addr) {
        high_addr = high_ram_alloc(byte_size, 64);
        if (!high_addr) return false;
        std::lock_guard<std::mutex> lock(g_alloc_mutex);
        g_arena_slots[{guest_base, slot_id}] = {high_addr, byte_size};
    }
    std::memcpy(g_ram + high_addr, g_ram + guest_base, byte_size);
    return true;
}

bool load_arena_slot(std::uint32_t guest_base, std::uint32_t byte_size, std::uint32_t slot_id) {
    if (byte_size == 0 || static_cast<std::uint64_t>(guest_base) + byte_size > kRamSize) return false;
    std::uint32_t high_addr = 0;
    {
        std::lock_guard<std::mutex> lock(g_alloc_mutex);
        auto it = g_arena_slots.find({guest_base, slot_id});
        if (it == g_arena_slots.end() || it->second.size < byte_size) return false;
        high_addr = it->second.high_addr;
    }
    std::memcpy(g_ram + guest_base, g_ram + high_addr, byte_size);
    return true;
}

bool swap_arena_slot(std::uint32_t guest_base, std::uint32_t byte_size, std::uint32_t old_slot_id, std::uint32_t new_slot_id) {
    if (old_slot_id == new_slot_id) return true;
    if (!save_arena_slot(guest_base, byte_size, old_slot_id)) return false;
    return load_arena_slot(guest_base, byte_size, new_slot_id);
}

// ============================================================================
// Telemetry & Diagnostics (PS2_UNSHACKLE_TRACE=1)
// ============================================================================

bool trace_enabled() {
    static const bool on = std::getenv("PS2_UNSHACKLE_TRACE") != nullptr && std::atoi(std::getenv("PS2_UNSHACKLE_TRACE")) != 0;
    return on;
}

}  // namespace ps2::unshackle
