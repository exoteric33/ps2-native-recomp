#include "memory_profile.hpp"

#include "ps2/os.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

namespace recomp {

namespace {

constexpr std::uint32_t kStdRamSize = 32u * 1024u * 1024u;        // 0x02000000 (32 MB)
constexpr std::uint32_t kExpandedKernelTop = 96u * 1024u * 1024u; // 0x06000000 (96 MB contiguous kernel limit; 96..128 MB reserved for high_ram_alloc)

void simulate_crt0(const Image &image, MemoryProfile &profile) {
    std::uint32_t regs[32]{};
    std::uint32_t pc = image.entry();
    std::vector<std::uint32_t> call_stack;
    std::set<std::uint32_t> visited;

    auto exec_insn = [&](std::uint32_t word) {
        if (!word) return;
        const Insn insn = decode(word, pc);
        const std::uint32_t rs = insn.rs(), rt = insn.rt(), rd = insn.rd(), sa = insn.sa();
        switch (insn.op) {
        case Op::Lui:
            regs[rt] = insn.uimm() << 16;
            break;
        case Op::Addiu:
        case Op::Addi:
        case Op::Daddiu:
        case Op::Daddi:
            regs[rt] = regs[rs] + static_cast<std::uint32_t>(insn.simm());
            break;
        case Op::Ori:
            regs[rt] = regs[rs] | insn.uimm();
            break;
        case Op::Andi:
            regs[rt] = regs[rs] & insn.uimm();
            break;
        case Op::Xori:
            regs[rt] = regs[rs] ^ insn.uimm();
            break;
        case Op::Lw:
        case Op::Lwu:
        case Op::Ld:
        case Op::Lq: {
            const std::uint32_t addr = regs[rs] + static_cast<std::uint32_t>(insn.simm());
            regs[rt] = image.word(addr).value_or(0u);
            break;
        }
        case Op::Addu:
        case Op::Add:
        case Op::Daddu:
        case Op::Dadd:
            regs[rd] = regs[rs] + regs[rt];
            break;
        case Op::Subu:
        case Op::Sub:
        case Op::Dsubu:
        case Op::Dsub:
            regs[rd] = regs[rs] - regs[rt];
            break;
        case Op::Or:
        case Op::Por:
            regs[rd] = regs[rs] | regs[rt];
            break;
        case Op::And:
        case Op::Pand:
            regs[rd] = regs[rs] & regs[rt];
            break;
        case Op::Xor:
        case Op::Pxor:
            regs[rd] = regs[rs] ^ regs[rt];
            break;
        case Op::Nor:
            regs[rd] = ~(regs[rs] | regs[rt]);
            break;
        case Op::Sll:
        case Op::Dsll:
            regs[rd] = regs[rt] << sa;
            break;
        case Op::Srl:
        case Op::Dsrl:
            regs[rd] = regs[rt] >> sa;
            break;
        case Op::Syscall: {
            std::uint32_t num = regs[3] & 0xFFFFu;
            if (num & 0x8000u) num = (0x10000u - num) & 0xFFFFu;
            if (num == 0x3Cu && !profile.has_setup_thread) {
                profile.has_setup_thread = true;
                profile.gp = regs[4];
                profile.stack_base = regs[5];
                profile.stack_size = regs[6];
                const std::uint32_t top = (regs[5] == 0xFFFFFFFFu) ? (kStdRamSize - 0x1000u) : (regs[5] + regs[6]);
                regs[2] = top - 0x100u;
            } else if (num == 0x3Du && !profile.has_setup_heap) {
                profile.has_setup_heap = true;
                profile.heap_base = regs[4];
                profile.heap_size = regs[5];
                regs[2] = regs[4];
            } else if (num == 0x3Eu) {
                regs[2] = kStdRamSize - 0x1000u;
            } else if (num == 0x7Fu) {
                regs[2] = kStdRamSize;
            }
            break;
        }
        default:
            break;
        }
        regs[0] = 0;
    };

    for (int step = 0; step < 800; ++step) {
        const auto w_opt = image.word(pc);
        if (!w_opt) break;
        const Insn insn = decode(*w_opt, pc);
        if (insn.op == Op::Invalid) break;
        const Flow f = flow(insn.op);
        const std::uint32_t delay_word = image.word(pc + 4u).value_or(0u);

        if (insn.op == Op::Jal) {
            exec_insn(delay_word);
            regs[31] = pc + 8u;
            const std::uint32_t target = insn.jump_target();
            if (call_stack.size() < 2 && !visited.count(target)) {
                visited.insert(target);
                call_stack.push_back(pc + 8u);
                pc = target;
            } else {
                pc += 8u;
            }
        } else if (insn.op == Op::J) {
            exec_insn(delay_word);
            const std::uint32_t target = insn.jump_target();
            if (!visited.count(target)) {
                visited.insert(target);
                pc = target;
            } else {
                break;
            }
        } else if (insn.op == Op::Jr && insn.rs() == 31u) {
            exec_insn(delay_word);
            if (!call_stack.empty()) {
                pc = call_stack.back();
                call_stack.pop_back();
            } else {
                break;
            }
        } else if (f == Flow::Branch || f == Flow::BranchLikely) {
            exec_insn(delay_word);
            const std::uint32_t target = insn.branch_target_of();
            if (target <= pc) {
                // Skip backward loop (e.g. .bss clear loop in crt0)
                pc += 8u;
            } else if (!visited.count(target)) {
                visited.insert(target);
                pc = target;
            } else {
                pc += 8u;
            }
        } else {
            exec_insn(*w_opt);
            pc += 4u;
        }

        if (profile.has_setup_thread && profile.has_setup_heap) break;
    }
}

void scan_functions_for_allocators(const std::string &mod_name, const Image &image, const Program &program, MemoryProfile &profile) {
    std::set<std::uint32_t> func_set(program.functions.begin(), program.functions.end());
    // Also include 16-byte aligned tail-call (`j target`) destinations from short wrapper functions in functions or entries
    for (std::uint32_t fn : program.entries) {
        if ((fn & 0xFu) != 0u) continue;
        for (std::uint32_t pc = fn; pc < fn + 48u; pc += 4u) {
            if (!program.is_code(pc)) break;
            const auto w_opt = image.word(pc);
            if (!w_opt) break;
            const Insn insn = decode(*w_opt, pc);
            if (insn.op == Op::J) {
                const std::uint32_t target = insn.jump_target();
                if ((target & 0xFu) == 0u && program.is_code(target)) {
                    func_set.insert(target);
                }
                break;
            }
            if (insn.op == Op::Jr) break;
        }
    }
    // Also include 16-byte aligned entries that immediately follow a function return (`jr $ra`) or tail jump (`j`) + delay slot + optional nops
    for (std::uint32_t lbl : program.entries) {
        if ((lbl & 0xFu) != 0u || lbl < program.base + 8u) continue;
        for (std::uint32_t back = 8u; back <= 20u && lbl >= program.base + back; back += 4u) {
            const std::uint32_t cand = lbl - back;
            const auto w_opt = image.word(cand);
            if (!w_opt) break;
            const Insn insn = decode(*w_opt, cand);
            if ((insn.op == Op::Jr && insn.rs() == 31u) ||
                (insn.op == Op::J && (insn.jump_target() < lbl - 32u || insn.jump_target() > lbl + 32u))) {
                func_set.insert(lbl);
                break;
            }
            // Allow nop padding between `jr $ra / j + delay_slot` and the 16-byte aligned entry
            if (back > 8u && *w_opt != 0u) break;
        }
    }

    std::set<std::uint32_t> recorded_addrs;
    const std::vector<std::uint32_t> funcs(func_set.begin(), func_set.end());
    for (std::size_t idx = 0; idx < funcs.size(); ++idx) {
        const std::uint32_t start = funcs[idx];
        const std::uint32_t limit = (idx + 1 < funcs.size()) ? std::min(funcs[idx + 1], start + 900u) : std::min(program.end, start + 900u);

        std::uint32_t insn_count = 0;
        std::set<std::int32_t> a0_sw_offsets;
        std::set<std::int32_t> a0_lw_offsets;
        std::set<std::uint32_t> arena_lui_Consts;
        std::set<std::uint32_t> pool_size_lui_consts;
        std::set<std::uint32_t> a0_aliases{4u};
        std::set<std::uint32_t> a1_aliases{5u};
        bool has_minus_one_add = false;
        bool has_nor_zero = false;
        bool has_and = false;
        bool has_addu = false;
        bool has_sltu = false;
        bool has_shift4 = false;
        bool has_hdr_sub_from_a1 = false;
        std::uint32_t global_sw_count = 0;
        std::uint32_t gp_sw_count = 0;
        std::uint32_t calls_count = 0;

        for (std::uint32_t pc = start; pc < limit; pc += 4u) {
            if (!program.is_code(pc)) break;
            const auto w_opt = image.word(pc);
            if (!w_opt) break;
            const Insn insn = decode(*w_opt, pc);
            ++insn_count;

            if (insn.op == Op::Jal || insn.op == Op::Jalr) ++calls_count;

            // Track simple register moves of $a0 ($4) and $a1 ($5)
            if ((insn.op == Op::Addu || insn.op == Op::Daddu || insn.op == Op::Or) && insn.rt() == 0u && insn.rd() != 0u) {
                if (a0_aliases.count(insn.rs())) a0_aliases.insert(insn.rd());
                if (a1_aliases.count(insn.rs())) a1_aliases.insert(insn.rd());
            }

            if ((insn.op == Op::Sw || insn.op == Op::Sh) && a0_aliases.count(insn.rs()) && insn.simm() >= 0 && insn.simm() <= 36) {
                a0_sw_offsets.insert(insn.simm());
            }
            if (insn.op == Op::Lw && a0_aliases.count(insn.rs()) && insn.simm() >= 0 && insn.simm() <= 36) {
                a0_lw_offsets.insert(insn.simm());
            }
            if ((insn.op == Op::Addiu || insn.op == Op::Addi) && insn.simm() == -1) {
                has_minus_one_add = true;
            }
            if ((insn.op == Op::Addiu || insn.op == Op::Addi) && a1_aliases.count(insn.rs()) &&
                insn.simm() >= -64 && insn.simm() <= -8) {
                has_hdr_sub_from_a1 = true;
            }
            if (insn.op == Op::Nor && (insn.rs() == 0u || insn.rt() == 0u)) {
                has_nor_zero = true;
            }
            if (insn.op == Op::And) has_and = true;
            if (insn.op == Op::Addu || insn.op == Op::Daddu) has_addu = true;
            if (insn.op == Op::Sltu) has_sltu = true;
            if (insn.op == Op::Sll && insn.sa() == 4u) has_shift4 = true;

            if (insn.op == Op::Lui) {
                const std::uint32_t u = insn.uimm();
                if (u == 0x0200u && mod_name.empty()) {
                    profile.hardcoded_32mb_bounds.emplace_back(start, pc);
                }
                if (u >= 0x0080u && u <= 0x01F8u) {
                    arena_lui_Consts.insert(u);
                }
                if (u >= 0x0002u && u <= 0x0100u) {
                    pool_size_lui_consts.insert(u);
                }
            }
            if (insn.op == Op::Sw && insn.rs() != 29u && !a0_aliases.count(insn.rs())) {
                ++global_sw_count;
                if (insn.rs() == 28u) ++gp_sw_count;
            }

            if (insn.op == Op::Syscall && mod_name.empty() && pc > start) {
                // Check previous instructions for syscall number in $v1 ($3)
                for (std::uint32_t back = 4u; back <= 20u && pc >= start + back; back += 4u) {
                    const Insn prev = decode(image.word(pc - back).value_or(0u), pc - back);
                    if ((prev.op == Op::Addiu || prev.op == Op::Ori) && prev.rt() == 3u && prev.rs() == 0u) {
                        std::uint32_t sc = prev.uimm();
                        if (sc & 0x8000u) sc = (0x10000u - sc) & 0xFFFFu;
                        if (sc == 0x3Eu) ++profile.end_of_heap_Refer_count;
                        else if (sc == 0x7Fu) ++profile.get_memory_size_count;
                        else if (sc == 0x55u) ++profile.init_tlb_count;
                        break;
                    }
                }
            }
        }

        const bool has_align_round = has_minus_one_add && has_nor_zero && has_and;

        // Pattern 1A: Arena / Block-Heap Descriptor Initializer ((desc=$a0, base=$a1, size=$a2))
        // Writes >= 5 fields to 0..36($a0), performs power-of-two alignment rounding, <= 1 helper call.
        if (insn_count >= 12 && insn_count <= 70 && calls_count <= 1 && a0_sw_offsets.size() >= 5 &&
            has_align_round && has_addu) {
            const auto kind = (a0_sw_offsets.size() >= 7) ? AllocatorCandidate::Kind::kBlockHeapInit
                                                          : AllocatorCandidate::Kind::kBumpArenaInit;
            profile.allocators.push_back({kind, mod_name, start, insn_count});
            recorded_addrs.insert(start);
            continue;
        }

        // Pattern 1B: Linked Free-List Heap Initializer ((base=$a0, size=$a1), e.g. Snowblind Engine 0x00146118)
        // Writes >= 4 header fields into 0..36($a0_alias), subtracts header size from $a1_alias, stores globals.
        if (insn_count >= 15 && insn_count <= 65 && calls_count == 0 && a0_sw_offsets.size() >= 4 &&
            has_hdr_sub_from_a1 && global_sw_count >= 4) {
            profile.allocators.push_back({AllocatorCandidate::Kind::kLinkedHeapInit, mod_name, start, insn_count});
            recorded_addrs.insert(start);
            continue;
        }

        // Pattern 2: Double-Ended Bump Arena Allocator
        // Reads >= 3 fields from $a0 (cursors & align), updates cursor in $a0, rounds size to alignment, checks sltu.
        if (insn_count >= 16 && insn_count <= 75 && calls_count <= 1 && a0_lw_offsets.size() >= 3 &&
            !a0_sw_offsets.empty() && a0_sw_offsets.size() <= 3 && has_align_round && has_sltu) {
            profile.allocators.push_back({AllocatorCandidate::Kind::kBumpArenaAlloc, mod_name, start, insn_count});
            recorded_addrs.insert(start);
            continue;
        }

        // Pattern 3: Block-Table Heap Allocator
        // Uses power-of-two alignment rounding + 16-byte block entries (`sll 4`) + `sltu` bounds check.
        if (insn_count >= 45 && insn_count <= 220 && has_align_round && has_sltu && has_shift4) {
            profile.allocators.push_back({AllocatorCandidate::Kind::kBlockHeapAlloc, mod_name, start, insn_count});
            recorded_addrs.insert(start);
            continue;
        }

        // Pattern 4: Static Arena Partition Table Initializer (e.g. mhdos submain 0x002E45A0, King's Field IV 0x00209A28)
        if ((insn_count >= 20 && insn_count <= 160 && arena_lui_Consts.size() >= 4 && global_sw_count >= 6) ||
            (insn_count >= 15 && insn_count <= 120 && pool_size_lui_consts.size() >= 6 && gp_sw_count >= 6)) {
            profile.allocators.push_back({AllocatorCandidate::Kind::kStaticPartitionInit, mod_name, start, insn_count});
            recorded_addrs.insert(start);
        }
    }

    // Pattern 5: Indexed Zone Initializer called multiple times with (zone_id=$a0, base=$a1, size=$a2)
    // (e.g. Final Fantasy XII 0x002136A0 called 8 times from main with 64 KB-aligned base & size in 0..32 MB).
    std::map<std::uint32_t, std::uint32_t> indexed_zone_hits;
    for (std::uint32_t pc = program.base; pc < program.end; pc += 4u) {
        if (!program.is_code(pc)) continue;
        const auto w_opt = image.word(pc);
        if (!w_opt) continue;
        const Insn insn = decode(*w_opt, pc);
        if (insn.op != Op::Jal) continue;
        const std::uint32_t target = insn.jump_target();
        if (recorded_addrs.count(target)) continue;

        std::uint32_t regs[32]{};
        bool known[32]{};
        known[0] = true;
        const std::uint32_t win_start = (pc > program.base + 48u) ? (pc - 48u) : program.base;
        const std::uint32_t win_end = std::min(program.end, pc + 8u);
        for (std::uint32_t k = win_start; k < win_end; k += 4u) {
            if (k == pc) continue;
            const auto kw = image.word(k);
            if (!kw) continue;
            const Insn ki = decode(*kw, k);
            if (ki.op == Op::Lui && ki.rt() != 0u) {
                regs[ki.rt()] = ki.uimm() << 16;
                known[ki.rt()] = true;
            } else if ((ki.op == Op::Addiu || ki.op == Op::Ori) && ki.rt() != 0u && known[ki.rs()]) {
                regs[ki.rt()] = (ki.op == Op::Addiu)
                                    ? (regs[ki.rs()] + static_cast<std::uint32_t>(ki.simm()))
                                    : (regs[ki.rs()] | ki.uimm());
                known[ki.rt()] = true;
            }
        }
        if (known[4] && known[5] && known[6]) {
            const std::uint32_t id = regs[4], base = regs[5], size = regs[6];
            if (id < 32u && (base & 0xFFFFu) == 0u && (size & 0xFFFFu) == 0u &&
                base >= 0x00500000u && base < kStdRamSize &&
                size >= 0x00020000u && size <= 0x01800000u &&
                base != size && (base + size) <= kStdRamSize) {
                ++indexed_zone_hits[target];
            }
        }
    }
    for (const auto &[target, hits] : indexed_zone_hits) {
        if (hits >= 3u) {
            profile.allocators.push_back({AllocatorCandidate::Kind::kIndexedZoneInit, mod_name, target, hits});
            recorded_addrs.insert(target);
        }
    }
}

}  // namespace

const char *archetype_name(MemoryArchetype a) {
    switch (a) {
    case MemoryArchetype::kDynamicKernelAutoStack: return "Archetype1_DynamicKernelAutoStack";
    case MemoryArchetype::kDynamicKernelFixedStack: return "Archetype2_DynamicKernelFixedStack";
    case MemoryArchetype::kStaticBssSubAllocators: return "Archetype3_StaticBssSubAllocators";
    case MemoryArchetype::kLowHeapHighStreaming: return "Archetype4_LowHeapHighStreaming";
    case MemoryArchetype::kHighBootstrapperOrPacker: return "Archetype5_HighBootstrapperOrPacker";
    default: return "Unknown";
    }
}

const char *archetype_strategy(MemoryArchetype a) {
    switch (a) {
    case MemoryArchetype::kDynamicKernelAutoStack:
        return "Auto-expand SetupThread/SetupHeap top to 0x06000000 (~90 MB contiguous kernel heap)";
    case MemoryArchetype::kDynamicKernelFixedStack:
        return "Relocate fixed 32MB stack to 0x06000000 if no blocking high PT_LOAD exists; otherwise overflow into High-RAM";
    case MemoryArchetype::kStaticBssSubAllocators:
        return "Intercept detected ArenaInit/ArenaAlloc functions and redirect sub-arenas to High-RAM pool (0x06000000..0x07FFFFFF)";
    case MemoryArchetype::kLowHeapHighStreaming:
        return "Promote GetMemorySize(0x7F) to 0x06000000 and auto-redirect indexed zone arenas to High-RAM";
    case MemoryArchetype::kHighBootstrapperOrPacker:
        return "Preserve 32MB layout during high bootstrapper/unpacker execution; re-evaluate policy when target ELF calls SetupThread/SetupHeap";
    default:
        return "Standard 32MB compatibility mode";
    }
}

const char *candidate_kind_name(AllocatorCandidate::Kind k) {
    switch (k) {
    case AllocatorCandidate::Kind::kBumpArenaInit: return "BumpArenaInit";
    case AllocatorCandidate::Kind::kBlockHeapInit: return "BlockHeapInit";
    case AllocatorCandidate::Kind::kLinkedHeapInit: return "LinkedHeapInit";
    case AllocatorCandidate::Kind::kIndexedZoneInit: return "IndexedZoneInit";
    case AllocatorCandidate::Kind::kBumpArenaAlloc: return "BumpArenaAlloc";
    case AllocatorCandidate::Kind::kBlockHeapAlloc: return "BlockHeapAlloc";
    case AllocatorCandidate::Kind::kStaticPartitionInit: return "StaticPartitionInit";
    }
    return "Unknown";
}

MemoryProfile analyze_memory_profile(const Image &main_image, const Program &main_program,
                                     const std::vector<OverlayAnalysisView> &overlays) {
    MemoryProfile profile;
    profile.entry = main_image.entry();

    std::vector<ProgramHeader> sorted_headers = main_image.headers();
    if (sorted_headers.empty()) {
        for (const auto &seg : main_image.segments()) {
            sorted_headers.push_back(ProgramHeader{
                0, seg.address, seg.address, seg.initialized_size,
                static_cast<std::uint32_t>(seg.bytes.size()), seg.executable ? 1u : 0u, 0});
        }
    }
    std::sort(sorted_headers.begin(), sorted_headers.end(),
              [](const ProgramHeader &a, const ProgramHeader &b) { return a.vaddr < b.vaddr; });

    profile.pt_load_count = sorted_headers.size();
    bool has_lowest = false;

    std::uint32_t prev_low_end = 0;
    for (const auto &h : sorted_headers) {
        if (h.memsz == 0u && h.filesz == 0u) {
            continue;
        }
        if (!has_lowest) {
            profile.lowest_vaddr = h.vaddr;
            has_lowest = true;
        }
        if (h.vaddr >= 0x70000000u) {
            profile.has_scratchpad_segments = true;
            continue;
        }
        if (h.vaddr >= kStdRamSize) {
            profile.has_devkit_high_segments = true;
            continue;
        }
        profile.code_file_bytes += h.filesz;
        if (h.memsz > h.filesz) profile.bss_bytes += (h.memsz - h.filesz);

        const std::uint32_t seg_end = h.vaddr + h.memsz;
        if (prev_low_end > 0 && h.vaddr > prev_low_end) {
            const std::uint32_t gap = h.vaddr - prev_low_end;
            if (gap > profile.max_gap_bytes) {
                profile.max_gap_bytes = gap;
                profile.max_gap_start = prev_low_end;
                profile.max_gap_end = h.vaddr;
            }
        }
        prev_low_end = std::max(prev_low_end, seg_end);
        profile.highest_low_ram_end = std::max(profile.highest_low_ram_end, seg_end);
        profile.highest_Segment_vaddr = std::max(profile.highest_Segment_vaddr, h.vaddr);
    }

    simulate_crt0(main_image, profile);
    scan_functions_for_allocators("", main_image, main_program, profile);
    for (const auto &ovl : overlays) {
        if (ovl.image && ovl.program) {
            scan_functions_for_allocators(ovl.name, *ovl.image, *ovl.program, profile);
        }
    }

    // Check if any non-empty PT_LOAD exists in 0x01000000..0x01FFFFFF above heap_base
    if (profile.has_setup_heap) {
        for (const auto &h : sorted_headers) {
            if (h.vaddr < kStdRamSize && h.memsz > 0 && h.vaddr > profile.heap_base && h.vaddr >= 0x01000000u) {
                profile.has_blocking_high_segment = true;
                break;
            }
        }
    }

    // Compute native 32 MB kernel heap size
    const std::uint32_t native_stack_bottom =
        (!profile.has_setup_thread || profile.stack_base == 0xFFFFFFFFu)
            ? (kStdRamSize - 0x1000u - profile.stack_size)
            : profile.stack_base;
    if (profile.has_setup_heap) {
        if (profile.heap_size == 0xFFFFFFFFu) {
            profile.native_kernel_heap_bytes =
                (native_stack_bottom > profile.heap_base) ? (native_stack_bottom - profile.heap_base) : 0u;
        } else {
            profile.native_kernel_heap_bytes = profile.heap_size;
        }
    }

    // Classify into one of the 5 Universal PS2 Memory Archetypes
    const bool large_static_arena =
        profile.heap_base >= 0x01000000u &&
        ((profile.bss_bytes >= (10ull << 20)) || (profile.max_gap_bytes >= (8u << 20)));

    if (profile.entry >= 0x00800000u || (!profile.has_setup_thread && !profile.has_setup_heap)) {
        profile.archetype = MemoryArchetype::kHighBootstrapperOrPacker;
        profile.expanded_kernel_heap_bytes = profile.native_kernel_heap_bytes;
    } else if (large_static_arena && profile.native_kernel_heap_bytes <= (2u << 20)) {
        profile.archetype = MemoryArchetype::kStaticBssSubAllocators;
        if (profile.has_setup_heap && profile.heap_size == 0xFFFFFFFFu && !profile.has_blocking_high_segment) {
            const std::uint32_t exp_top = kExpandedKernelTop - profile.stack_size;
            profile.expanded_kernel_heap_bytes = (exp_top > profile.heap_base) ? (exp_top - profile.heap_base) : (profile.native_kernel_heap_bytes + (32u << 20));
        } else {
            profile.expanded_kernel_heap_bytes = profile.native_kernel_heap_bytes + (32u << 20);
        }
    } else if (profile.has_setup_heap && profile.heap_size == 0xFFFFFFFFu &&
               (!profile.has_setup_thread || profile.stack_base == 0xFFFFFFFFu)) {
        profile.archetype = MemoryArchetype::kDynamicKernelAutoStack;
        const std::uint32_t exp_top = profile.has_blocking_high_segment ? (profile.native_kernel_heap_bytes * 2u + profile.heap_base)
                                                                        : (kExpandedKernelTop - 0x1000u - profile.stack_size);
        profile.expanded_kernel_heap_bytes = (exp_top > profile.heap_base) ? (exp_top - profile.heap_base) : 0u;
    } else if (profile.has_setup_heap && profile.heap_size == 0xFFFFFFFFu) {
        profile.archetype = MemoryArchetype::kDynamicKernelFixedStack;
        const std::uint32_t exp_top = profile.has_blocking_high_segment ? (profile.native_kernel_heap_bytes * 2u + profile.heap_base)
                                                                        : (kExpandedKernelTop - profile.stack_size);
        profile.expanded_kernel_heap_bytes = (exp_top > profile.heap_base) ? (exp_top - profile.heap_base) : 0u;
    } else if (profile.has_setup_heap && (profile.heap_base + profile.heap_size) <= 0x01000000u) {
        profile.archetype = MemoryArchetype::kLowHeapHighStreaming;
        profile.expanded_kernel_heap_bytes = kExpandedKernelTop - (profile.heap_base + profile.heap_size);
    } else {
        profile.archetype = MemoryArchetype::kStaticBssSubAllocators;
        profile.expanded_kernel_heap_bytes = profile.native_kernel_heap_bytes + (32u << 20);
    }

    return profile;
}

std::string format_memory_profile_json(const MemoryProfile &p) {
    auto hex = [](std::uint32_t v) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "0x%08X", v);
        return std::string(buf);
    };

    std::ostringstream out;
    out << "{\n";
    out << "  \"archetype\": \"" << archetype_name(p.archetype) << "\",\n";
    out << "  \"strategy\": \"" << archetype_strategy(p.archetype) << "\",\n";
    out << "  \"elf\": {\n";
    out << "    \"entry\": \"" << hex(p.entry) << "\",\n";
    out << "    \"pt_load_count\": " << p.pt_load_count << ",\n";
    out << "    \"lowest_vaddr\": \"" << hex(p.lowest_vaddr) << "\",\n";
    out << "    \"highest_low_ram_end\": \"" << hex(p.highest_low_ram_end) << "\",\n";
    out << "    \"code_file_bytes\": " << p.code_file_bytes << ",\n";
    out << "    \"bss_bytes\": " << p.bss_bytes << ",\n";
    out << "    \"max_linker_gap_bytes\": " << p.max_gap_bytes << ",\n";
    out << "    \"max_linker_gap_range\": [\"" << hex(p.max_gap_start) << "\", \"" << hex(p.max_gap_end) << "\"],\n";
    out << "    \"has_blocking_high_segment\": " << (p.has_blocking_high_segment ? "true" : "false") << ",\n";
    out << "    \"has_devkit_high_segments\": " << (p.has_devkit_high_segments ? "true" : "false") << ",\n";
    out << "    \"has_scratchpad_segments\": " << (p.has_scratchpad_segments ? "true" : "false") << "\n";
    out << "  },\n";
    out << "  \"crt0\": {\n";
    out << "    \"has_setup_thread\": " << (p.has_setup_thread ? "true" : "false") << ",\n";
    out << "    \"gp\": \"" << hex(p.gp) << "\",\n";
    out << "    \"stack_base\": \"" << hex(p.stack_base) << "\",\n";
    out << "    \"stack_size\": \"" << hex(p.stack_size) << "\",\n";
    out << "    \"has_setup_heap\": " << (p.has_setup_heap ? "true" : "false") << ",\n";
    out << "    \"heap_base\": \"" << hex(p.heap_base) << "\",\n";
    out << "    \"heap_size\": \"" << hex(p.heap_size) << "\",\n";
    out << "    \"native_kernel_heap_bytes\": " << p.native_kernel_heap_bytes << ",\n";
    out << "    \"expanded_kernel_heap_bytes\": " << p.expanded_kernel_heap_bytes << "\n";
    out << "  },\n";
    out << "  \"syscalls\": {\n";
    out << "    \"end_of_heap_count\": " << p.end_of_heap_Refer_count << ",\n";
    out << "    \"get_memory_size_count\": " << p.get_memory_size_count << ",\n";
    out << "    \"init_tlb_count\": " << p.init_tlb_count << ",\n";
    out << "    \"hardcoded_32mb_bounds_count\": " << p.hardcoded_32mb_bounds.size() << "\n";
    out << "  },\n";
    out << "  \"allocators\": [\n";
    for (std::size_t i = 0; i < p.allocators.size(); ++i) {
        const auto &a = p.allocators[i];
        out << "    { \"kind\": \"" << candidate_kind_name(a.kind)
            << "\", \"module\": \"" << (a.module_name.empty() ? "main" : a.module_name)
            << "\", \"func_addr\": \"" << hex(a.func_addr)
            << "\", \"insn_count\": " << a.insn_count << " }"
            << (i + 1 < p.allocators.size() ? "," : "") << "\n";
    }
    out << "  ]\n";
    out << "}\n";
    return out.str();
}

void write_memory_profile_json(const MemoryProfile &profile, const std::string &path) {
    std::ofstream out(ps2::os::path(path), std::ios::binary);
    if (!out) throw std::runtime_error("cannot write " + path);
    const std::string json = format_memory_profile_json(profile);
    out.write(json.data(), static_cast<std::streamsize>(json.size()));
}

void print_memory_profile_summary(const MemoryProfile &p) {
    std::printf("[memory-profile] archetype=%s | entry=0x%08X | segments=%zu [0x%08X..0x%08X] | file=%.2f MiB | bss=%.2f MiB | gap=%.2f MiB\n",
                archetype_name(p.archetype), p.entry, p.pt_load_count, p.lowest_vaddr, p.highest_low_ram_end,
                static_cast<double>(p.code_file_bytes) / (1024.0 * 1024.0),
                static_cast<double>(p.bss_bytes) / (1024.0 * 1024.0),
                static_cast<double>(p.max_gap_bytes) / (1024.0 * 1024.0));
    std::printf("[memory-profile] crt0: SetupThread(stack=0x%08X, sz=0x%X) | SetupHeap(base=0x%08X, sz=0x%08X) | native_heap=%.2f MiB -> expanded=%.2f MiB\n",
                p.stack_base, p.stack_size, p.heap_base, p.heap_size,
                static_cast<double>(p.native_kernel_heap_bytes) / (1024.0 * 1024.0),
                static_cast<double>(p.expanded_kernel_heap_bytes) / (1024.0 * 1024.0));
    if (!p.allocators.empty()) {
        std::printf("[memory-profile] detected %zu custom allocator / partition routines:\n", p.allocators.size());
        for (const auto &a : p.allocators) {
            std::printf("  - %-20s at 0x%08X (%u insns, module=%s)\n",
                        candidate_kind_name(a.kind), a.func_addr, a.insn_count,
                        a.module_name.empty() ? "main" : a.module_name.c_str());
        }
    }
}

}  // namespace recomp
