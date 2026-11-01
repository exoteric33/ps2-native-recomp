#include "ps2/vu_ops.hpp"
#include "vu_micro.hpp"
#include "gs/gif.hpp"
#include "gs/queue.hpp"
#include "ps2/system.hpp"
#include "hw.hpp"
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace ps2 {
alignas(64) std::uint8_t g_vu0_code[4096], g_vu0_mem[4096], g_vu1_code[16384], g_vu1_mem[16384];
Cpu g_cpu;
Options g_options;
std::uint64_t frame_count() { return 0; }
bool halted() { return false; }
namespace vif { std::uint32_t itop(int) { return 0; } }
namespace gif { void transfer(int, const std::uint8_t *, std::uint32_t) {} }
namespace gs::queue {
    Ring &vu() { static Ring ring; return ring; }
    Role &role() { static Role r = Role::Emulation; return r; }
    bool vu_active() { return false; }
    void Ring::post(Type, std::uint32_t, std::uint32_t, const void *, std::uint32_t) {}
}
namespace vu {
namespace jit {
    bool g_dry = false;
    std::uint64_t g_kicks = 0, g_escape = 0;
    ProgramFn compile(const std::uint8_t *, std::uint32_t) { return nullptr; }
    int mode() { return 0; }
}
namespace live {
    bool g_ready = false;
    bool active() { return false; }
    void note_call(std::uint64_t) {}
    ProgramFn unknown(std::uint64_t, std::uint32_t, const std::uint8_t *) { return nullptr; }
    bool ready() { return false; }
    std::vector<std::pair<std::uint64_t, ProgramFn>> take() { return {}; }
}
}
}

using namespace ps2;

int main() {
    printf("=== Test 1: VU1 Loop 0x31F8-0x3240 (vi7 decrement & IBGTZ) ===\n");
    const std::vector<std::array<std::uint32_t, 2>> loop_pairs = {
        {0x34082800u, 0x01C83B1Bu}, // 0x00: FMAND vi8, vi0
        {0x80082231u, 0x01C951ECu}, // 0x08: ISUB vi8, vi4, vi8
        {0x01F73002u, 0x00214218u}, // 0x10: LQ vf3, 2(vi3)
        {0x12073801u, 0x01CA526Bu}, // 0x18: ISUBIU vi7, vi7, 1
        {0x10063003u, 0x000002FFu}, // 0x20: IADDIU vi6, vi6, 3
        {0x80081234u, 0x01CC3AFEu}, // 0x28: IAND vi8, vi2, vi8
        {0x800818F5u, 0x01C7602Eu}, // 0x30: IOR vi3, vi3, vi8
        {0x0A2337F9u, 0x01CAB93Du}, // 0x38: ISW vi3, 0x37F9(vi0)
        {0x5A003FF7u, 0x000002FFu}, // 0x40: IBGTZ vi7, target=0x00
        {0x80635BFCu, 0x0037BAEBu}, // 0x48: MFIR vi4, vf0[w] (delay slot)
        {0x8000033Cu, 0x400002FFu}, // 0x50: NOP with E-bit
        {0x8000033Cu, 0x000002FFu}, // 0x58: NOP (delay slot of E-bit)
    };

    const std::uint16_t test_values[] = { 0x0000, 0x0001, 0x0002, 0x7FFF, 0x8000, 0xFFFF };
    int failures = 0;

    for (int unit = 0; unit < 2; ++unit) {
        printf("--- Unit %d (%s) ---\n", unit, unit ? "decoding vu1" : "exact vu0");
        std::uint8_t *code = unit ? g_vu1_code : g_vu0_code;
        std::uint8_t *data = unit ? g_vu1_mem : g_vu0_mem;

        std::memset(code, 0, unit ? 16384 : 4096);
        for (std::size_t i = 0; i < loop_pairs.size(); ++i) {
            std::memcpy(code + i * 8, &loop_pairs[i][0], 4);
            std::memcpy(code + i * 8 + 4, &loop_pairs[i][1], 4);
        }
        vu::code_changed(unit);

        for (std::uint16_t vi7_init : test_values) {
            std::memset(data, 0, unit ? 16384 : 4096);
            Vu0Regs &r = vu::regs(unit);
            std::memset(&r, 0, sizeof(r));
            r.vf[0].w[3] = 0x3F800000u;
            r.vi[7] = vi7_init;
            r.vi[6] = 0;

            vu::run(unit, 0);

            std::int16_t s_init = static_cast<std::int16_t>(vi7_init);
            std::uint16_t expected_vi6 = 0, expected_vi7 = 0;
            if (vi7_init == 0x8000) {
                // 0x8000 - 1 wraps to 0x7FFF (+32767 > 0), looping 32768 times
                expected_vi6 = static_cast<std::uint16_t>(32768u * 3u); // 0x8000
                expected_vi7 = 0;
            } else if (s_init <= 0) {
                expected_vi6 = 3;
                expected_vi7 = static_cast<std::uint16_t>(s_init - 1);
            } else {
                expected_vi6 = static_cast<std::uint16_t>(s_init * 3);
                expected_vi7 = 0;
            }

            const bool ok = (r.vi[7] == expected_vi7 && r.vi[6] == expected_vi6);
            printf("  Init vi7=0x%04X (signed %6d): final vi7=0x%04X, vi6=0x%04X -> %s\n",
                   vi7_init, s_init, r.vi[7], r.vi[6], ok ? "PASS" : "FAIL");
            if (!ok) failures++;
        }
    }

    printf("\n=== Test 2: Subroutine BAL/JR and E-bit ===\n");
    for (int unit = 0; unit < 2; ++unit) {
        printf("--- Unit %d subroutine return ---\n", unit);
        const std::vector<std::array<std::uint32_t, 2>> sub_pairs = {
            {0x42010003u, 0x000002FFu}, // 0x00: BAL vi1, target=0x20
            {0x8000033Cu, 0x000002FFu}, // 0x08: NOP (delay slot)
            {0x8000033Cu, 0x400002FFu}, // 0x10: [E=1] Return target (exit)
            {0x8000033Cu, 0x000002FFu}, // 0x18: Delay of E-bit
            {0x1002002Au, 0x000002FFu}, // 0x20: Subroutine: IADDIU vi2, vi0, 42
            {0x48000800u, 0x000002FFu}, // 0x28: JR vi1
            {0x8000033Cu, 0x000002FFu}, // 0x30: Delay slot of JR
        };

        std::uint8_t *code = unit ? g_vu1_code : g_vu0_code;
        std::uint8_t *data = unit ? g_vu1_mem : g_vu0_mem;
        std::memset(code, 0, unit ? 16384 : 4096);
        for (std::size_t i = 0; i < sub_pairs.size(); ++i) {
            std::memcpy(code + i * 8, &sub_pairs[i][0], 4);
            std::memcpy(code + i * 8 + 4, &sub_pairs[i][1], 4);
        }
        vu::code_changed(unit);
        std::memset(data, 0, unit ? 16384 : 4096);
        Vu0Regs &r = vu::regs(unit);
        std::memset(&r, 0, sizeof(r));
        r.vf[0].w[3] = 0x3F800000u;

        vu::run(unit, 0);

        const bool ok = (r.vi[2] == 42 && r.vi[1] == 2);
        printf("  Subroutine return: final vi2=%d, vi1=0x%04X -> %s\n", r.vi[2], r.vi[1], ok ? "PASS" : "FAIL");
        if (!ok) failures++;
    }

    printf("\n=== Test 3: R188 ILW branch-delay exemption (ILW.z -> IBEQ) ===\n");
    for (int unit = 0; unit < 2; ++unit) {
        printf("--- Unit %d ILW branch-delay test ---\n", unit);
        // Microprogram layout:
        // 0x00: ILW.z vi4, 0(vi0) -> loads word from memory (which is 0) into vi4.
        //       Upper: NOP
        // 0x08: IBEQ vi4, vi0, target=0x20 (imm11=2 -> 0x08+8+16 = 0x20)
        //       Upper: NOP
        // 0x10: IADDIU vi5, vi0, 999 (delay slot of IBEQ: executes regardless)
        // 0x18: IADDIU vi2, vi0, 1   (FALLTHROUGH: executed only if branch was NOT taken)
        // 0x20: [E=1] IADDIU vi3, vi0, 1 (BRANCH TARGET: executed when branch IS taken)
        // 0x28: NOP (delay slot of E-bit)
        //
        // Encoding:
        // ILW.z vi4, 0(vi0):
        //   w >> 25 = 0x04
        //   dest = 2 (z component: dest & (8 >> 2) = 2) at bits 24..21 -> (2u << 21)
        //   it = 4 (vi4) at bits 20..16 -> (4u << 16)
        //   is = 0 (vi0) at bits 15..11 -> (0u << 11)
        //   imm11 = 0 at bits 10..0
        const std::uint32_t ilw_lo = (0x04u << 25) | (2u << 21) | (4u << 16); // ILW.z vi4, 0(vi0)
        const std::uint32_t ibeq_lo = (0x28u << 25) | (4u << 16) | (0u << 21) | 0x0002u; // IBEQ vi4, vi0, offset +2 (0x20)
        const std::vector<std::array<std::uint32_t, 2>> ilw_pairs = {
            {ilw_lo,       0x000002FFu}, // 0x00: ILW.z vi4, 0(vi0)
            {ibeq_lo,      0x000002FFu}, // 0x08: IBEQ vi4, vi0, target 0x20
            {0x100503E7u,  0x000002FFu}, // 0x10: IADDIU vi5, vi0, 999 (delay slot)
            {0x10020001u,  0x000002FFu}, // 0x18: IADDIU vi2, vi0, 1 (fallthrough path)
            {0x10030001u,  0x400002FFu}, // 0x20: [E=1] IADDIU vi3, vi0, 1 (branch target)
            {0x8000033Cu,  0x000002FFu}, // 0x28: NOP (delay of E-bit)
        };

        std::uint8_t *code = unit ? g_vu1_code : g_vu0_code;
        std::uint8_t *data = unit ? g_vu1_mem : g_vu0_mem;
        std::memset(code, 0, unit ? 16384 : 4096);
        for (std::size_t i = 0; i < ilw_pairs.size(); ++i) {
            std::memcpy(code + i * 8, &ilw_pairs[i][0], 4);
            std::memcpy(code + i * 8 + 4, &ilw_pairs[i][1], 4);
        }
        vu::code_changed(unit);

        // Memory at 0 has 0.
        std::memset(data, 0, unit ? 16384 : 4096);
        Vu0Regs &r = vu::regs(unit);
        std::memset(&r, 0, sizeof(r));
        r.vf[0].w[3] = 0x3F800000u;
        // Seed vi4 with stale non-zero value 0x8080 (the exact value seen in MKA):
        r.vi[4] = 0x8080;
        r.vi[5] = 0;
        r.vi[2] = 0;
        r.vi[3] = 0;

        vu::run(unit, 0);

        // If ILW is exempt from branch VI delay:
        // ILW loads 0 into vi4.
        // IBEQ reads new vi4 (0 == vi0), branch IS TAKEN to 0x20!
        // Delay slot 0x10 executes: vi5 = 999.
        // Fallthrough 0x18 is SKIPPED: vi2 remains 0!
        // Target 0x20 executes: vi3 = 1!
        const bool branch_taken = (r.vi[4] == 0 && r.vi[2] == 0 && r.vi[3] == 1 && r.vi[5] == 999);
        printf("  ILW -> IBEQ result: vi4=0x%04X, vi2=%d (fallthrough), vi3=%d (branch target), vi5=%d -> %s\n",
               r.vi[4], r.vi[2], r.vi[3], r.vi[5], branch_taken ? "PASS (Branch Taken)" : "FAIL (Branch Missed)");
        if (!branch_taken) failures++;
    }

    printf("\n==================================================\n");
    printf("Total test failures: %d\n", failures);
    return failures ? 1 : 0;
}
