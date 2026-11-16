#include "iop.hpp"
#include "kernel.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <array>
#include <iterator>
#include <vector>

namespace ps2::iop {

alignas(64) std::uint8_t g_ram[kRamSize + 16];
Cpu g_cpu;

namespace {

inline std::int32_t simm(std::uint32_t w) { return static_cast<std::int16_t>(w & 0xFFFFu); }

std::vector<std::uint8_t> g_probes;

bool load_probes() {
    const char *spec = std::getenv("PS2_IOP_PROBE");
    if (!spec) return false;
    g_probes.assign(kRamSize / 32, 0);
    for (const char *p = spec; *p;) {
        char *end = nullptr;
        const std::uint32_t a = static_cast<std::uint32_t>(std::strtoul(p, &end, 16)) & (kRamSize - 1);
        g_probes[a >> 5] |= static_cast<std::uint8_t>(1u << ((a >> 2) & 7u));
        p = *end ? end + 1 : end;
    }
    return true;
}

void probe(const Cpu &c, std::uint32_t pc) {
    const kernel::Thread *t = kernel::current();
    std::fprintf(stderr, "[iop-probe] 0x%08X a0 %08X a1 %08X a2 %08X a3 %08X v0 %08X v1 %08X s0 %08X s1 %08X ra %08X thread %d time %llu\n", pc, c.r[4], c.r[5], c.r[6],
                 c.r[7], c.r[2], c.r[3], c.r[16], c.r[17], c.r[31], t ? t->id : 0, static_cast<unsigned long long>(kernel::now()));
    static const char *reg = std::getenv("PS2_IOP_PROBE_MEM");
    if (reg) {
        const std::uint32_t base = phys(c.r[std::atoi(reg) & 31]);
        if (base + 64 <= kRamSize) {
            std::fprintf(stderr, "[iop-probe]   r%s:", reg);
            for (std::uint32_t i = 0; i < 16; ++i) std::fprintf(stderr, " %08X", ram32(base + i * 4));
            std::fprintf(stderr, "\n");
        }
    }
}

void bad(Cpu &c, std::uint32_t pc, std::uint32_t w) {
    std::fprintf(stderr, "[iop] unknown instruction %08X at 0x%08X (ra 0x%08X)\n", w, pc, c.r[31]);
    kernel::fault("unknown instruction");
}

}

std::uint32_t fetch(std::uint32_t pc) {
    const std::uint32_t p = phys(pc);
    if (p < 0x00800000u) return ram32(p);
    return kernel::fetch_special(pc);
}

namespace {

// Decode once, run from a table. Every word of IOP RAM has one entry: the word as it was decoded and what the switch below needs
// from it (the operation and its three register numbers; the immediate, the shift amount and the jump target are cheap to take from
// the word again). Before an entry is used its word is compared with the word in RAM now, and decoded again when they differ, so
// every way code reaches RAM (modules loaded and relocated, the HLE copies, DMA, a save state loaded, a store of the IOP itself) is
// seen without a hook in any of them: a changed instruction is never run from an old decode. The zeroed table is the decode of
// word 0 (sll r0, r0, 0 = nop), so it needs no filling. Timing is untouched: one instruction is still one cycle of the budget, and
// the switch is asked after every instruction as before.
enum Op : std::uint8_t {
    kNop = 0,  // also every register operation whose result goes to r0
    kSll, kSrl, kSra, kSllv, kSrlv, kSrav, kJr, kJalr, kSyscall, kBreak, kMfhi, kMthi, kMflo, kMtlo, kMult, kMultu, kDiv, kDivu,
    kAddu, kSubu, kAnd, kOr, kXor, kNor, kSlt, kSltu,
    kRegimm, kJ, kJal, kBeq, kBne, kBlez, kBgtz, kAddiu, kSlti, kSltiu, kAndi, kOri, kXori, kLui, kCop0,
    kLb, kLh, kLwl, kLw, kLbu, kLhu, kLwr, kSb, kSh, kSwl, kSw, kSwr, kHle, kBad,
};

struct Decoded {
    std::uint32_t word;
    std::uint8_t op, rs, rt, rd;
};
static_assert(sizeof(Decoded) == 8);

Decoded g_decoded[kRamSize / 4];

Decoded decode(std::uint32_t w) {
    const auto rs = static_cast<std::uint8_t>((w >> 21) & 31u), rt = static_cast<std::uint8_t>((w >> 16) & 31u),
               rd = static_cast<std::uint8_t>((w >> 11) & 31u);
    auto op = kBad;
    switch (w >> 26) {
    case 0x00: {
        static constexpr Op kSpecial[64] = {
            kSll, kBad, kSrl, kSra, kSllv, kBad, kSrlv, kSrav, kJr, kJalr, kBad, kBad, kSyscall, kBreak, kBad, kNop,
            kMfhi, kMthi, kMflo, kMtlo, kBad, kBad, kBad, kBad, kMult, kMultu, kDiv, kDivu, kBad, kBad, kBad, kBad,
            kAddu, kAddu, kSubu, kSubu, kAnd, kOr, kXor, kNor, kBad, kBad, kSlt, kSltu, kBad, kBad, kBad, kBad,
            kBad, kBad, kBad, kBad, kBad, kBad, kBad, kBad, kBad, kBad, kBad, kBad, kBad, kBad, kBad, kBad};
        op = kSpecial[w & 63u];
        const bool to_rd = (op >= kSll && op <= kSrav) || op == kMfhi || op == kMflo || (op >= kAddu && op <= kSltu);
        if (to_rd && rd == 0) op = kNop;
        break;
    }
    case 0x01: op = kRegimm; break;
    case 0x02: op = kJ; break;
    case 0x03: op = kJal; break;
    case 0x04: op = kBeq; break;
    case 0x05: op = kBne; break;
    case 0x06: op = kBlez; break;
    case 0x07: op = kBgtz; break;
    case 0x08: case 0x09: op = rt ? kAddiu : kNop; break;
    case 0x0A: op = rt ? kSlti : kNop; break;
    case 0x0B: op = rt ? kSltiu : kNop; break;
    case 0x0C: op = rt ? kAndi : kNop; break;
    case 0x0D: op = rt ? kOri : kNop; break;
    case 0x0E: op = rt ? kXori : kNop; break;
    case 0x0F: op = rt ? kLui : kNop; break;
    case 0x10: op = kCop0; break;
    case 0x20: op = kLb; break;
    case 0x21: op = kLh; break;
    case 0x22: op = kLwl; break;
    case 0x23: op = kLw; break;
    case 0x24: op = kLbu; break;
    case 0x25: op = kLhu; break;
    case 0x26: op = kLwr; break;
    case 0x28: op = kSb; break;
    case 0x29: op = kSh; break;
    case 0x2A: op = kSwl; break;
    case 0x2B: op = kSw; break;
    case 0x2E: op = kSwr; break;
    case 0x2F: op = kNop; break;
    case kHleOpcode: op = kHle; break;
    default: break;
    }
    return Decoded{w, static_cast<std::uint8_t>(op), rs, rt, rd};
}

}

// Blocks: a run of instructions that ends with a branch or jump and its delay slot (or before an instruction
// that calls out of the interpreter: system call, HLE function, COP0, break) is decoded once into a list of operations with
// everything they need taken out of the word already (the immediate sign-extended, the branch target as an offset from the block's
// start, the shift amount), and is run as a whole: one check of its words against RAM instead of one per instruction, one look at
// the budget, no program counter kept per instruction and no "switch wanted" question after each one (only a call out of the
// interpreter can raise it, and none happens inside a block). A block is keyed by its physical start, so a mirror or a kseg
// address runs the same block; the addresses it needs come from the virtual pc it was entered at.
//
// Exactness: a block runs only when the budget covers all its instructions (otherwise the instructions run one by one as before),
// so the budget is decremented exactly as before. Whatever a block cannot do by itself (a load or store that is not plain RAM, any
// store while PS2_WATCH_IOP is on, a store into the block's own words) makes that instruction give the block up before it does
// anything: pc, npc and the budget are set to what the one-by-one loop has at that instruction, and the one-by-one loop runs it,
// asks the switch after it as always and goes on. A block is entered only where the one-by-one loop has just asked the switch (or
// another block ended, which cannot change it), never in a delay slot.
namespace {

// 8 bytes, so a block touches no more memory than the one-by-one loop's table: the word itself (also what is checked against RAM;
// the operation takes its fields from it), and where its code is
struct BlockOp {
    std::uint32_t w;
    std::int16_t code;   // the operation's block label, as an offset from the block code's first label
    std::uint8_t idx;    // the instruction's place in the block
    std::uint8_t unused;
};
static_assert(sizeof(BlockOp) == 8);

// A block in the arena: this head (four BlockOp places), then its n operations and its end (whose word is the first word for an
// empty block, so every block has at least one word to check)
struct BlockHead {
    std::uint64_t epoch;         // g_epoch when its words were last found equal to RAM
    std::uint32_t p;             // physical byte offset in RAM of the first word
    std::uint16_t n;             // instructions (0: the first one cannot be in a block, it runs one by one)
    std::uint16_t words;         // words kept to check (n, or 1 for an empty block)
    std::uint32_t succ_pc[2];    // the last block that followed it: on falling through [0] or on a jump [1], by virtual pc ...
    std::uint32_t succ_at[2];    // ... and its arena index (0: none)
};
constexpr std::uint32_t kHeadOps = 4;
static_assert(sizeof(BlockHead) == kHeadOps * sizeof(BlockOp));

constexpr unsigned kMaxBlock = 64;
constexpr std::size_t kMaxArena = 1u << 22;  // 32 MB; then every block is dropped and built again when next run

std::uint32_t g_block_at[kRamSize / 4];  // arena index of the block that starts at that word, 0 = none yet
std::vector<BlockOp> g_arena(1);         // index 0 is never a block
std::uint32_t g_generation = 0;          // goes up when the arena is dropped
// A block's words are compared with RAM only when RAM may have changed since the last time: g_epoch goes up when the interpreter is
// entered, after every call out of it (whatever that call wrote), and when one of its own stores writes into a 64-byte line that
// holds a block (g_code_lines, set when a block is built). While g_epoch stays the same, nothing has written a block's words.
std::uint64_t g_epoch = 1;
std::uint64_t g_code_lines[kRamSize / 64 / 64];
inline bool code_line(std::uint32_t q) { return (g_code_lines[q >> 12] >> ((q >> 6) & 63u)) & 1u; }

inline BlockHead &head(std::uint32_t at) { return *reinterpret_cast<BlockHead *>(g_arena.data() + at); }

bool in_block(std::uint8_t op) { return op != kSyscall && op != kBreak && op != kCop0 && op != kHle && op != kBad; }
bool ends_block(std::uint8_t op) { return (op >= kJr && op <= kJalr) || (op >= kRegimm && op <= kBgtz); }

void drop_blocks() {
    std::memset(g_block_at, 0, sizeof g_block_at);
    std::memset(g_code_lines, 0, sizeof g_code_lines);
    g_arena.resize(1);
    ++g_generation;
}

// builds the block that starts at the physical byte offset p (below kRamSize, word aligned) and returns its arena index; `code` maps
// an Op to its block label and kBad to the block's end, as offsets from the first label
std::uint32_t build_block(std::uint32_t p, const std::int16_t *code) {
    if (g_arena.size() > kMaxArena) drop_blocks();
    const std::uint32_t first = p >> 2, last = kRamSize / 4;
    unsigned n = 0;
    while (n < kMaxBlock && first + n < last) {
        const Decoded d = decode(ram32(p + n * 4));
        if (!in_block(d.op)) break;
        if (ends_block(d.op)) {
            // the branch and its delay slot, if that is an ordinary instruction (no branch, nothing that calls out)
            if (n + 1 >= kMaxBlock || first + n + 1 >= last) break;
            const Decoded slot = decode(ram32(p + (n + 1) * 4));
            if (in_block(slot.op) && !ends_block(slot.op)) n += 2;
            break;
        }
        ++n;
    }
    const auto at = static_cast<std::uint32_t>(g_arena.size());
    g_arena.resize(at + kHeadOps + n + 1);
    head(at) = BlockHead{g_epoch, p, static_cast<std::uint16_t>(n), static_cast<std::uint16_t>(n ? n : 1), {0, 0}, {0, 0}};
    BlockOp *ops = g_arena.data() + at + kHeadOps;
    for (unsigned i = 0; i < n; ++i) {
        const std::uint32_t w = ram32(p + i * 4);
        ops[i] = BlockOp{w, code[decode(w).op], static_cast<std::uint8_t>(i), 0};
    }
    ops[n] = BlockOp{n ? 0u : ram32(p), code[kBad], static_cast<std::uint8_t>(n), 0};
    for (std::uint32_t q = p; q < p + (n ? n : 1) * 4; q += 4) g_code_lines[q >> 12] |= std::uint64_t{1} << ((q >> 6) & 63u);
    g_block_at[first] = at;
    return at;
}

// the block's words are still the words in RAM
inline bool block_valid(std::uint32_t at) {
    const BlockHead &h = head(at);
    const BlockOp *want = g_arena.data() + at + kHeadOps;
    const std::uint8_t *have = g_ram + h.p;
    std::uint32_t diff = 0;
    for (unsigned i = 0; i < h.words; ++i) {
        std::uint32_t x;
        std::memcpy(&x, have + i * 4, 4);
        diff |= x ^ want[i].w;
    }
    return diff == 0;
}

}

// The one-by-one loop is threaded: every operation ends in its own copy of "next instruction" (DISPATCH) and jumps straight to the
// next operation's code, which the processor's branch prediction follows far better than one shared switch. The program counter,
// the next one and the budget live in locals; they are written back to the Cpu before every call that could look at them or change
// them (system call, HLE function, COP0 status, hardware load or store, a fault) and read back after it, and on every way out.
void execute(Cpu &c, std::int64_t &budget_out) {
    static const bool probing = load_probes();
    // PS2_IOP_BLOCKS=0: every instruction one by one, as before the blocks (to compare); PS2_IOP_PROBE needs that too
    static const bool blocks = !probing && !(std::getenv("PS2_IOP_BLOCKS") && std::getenv("PS2_IOP_BLOCKS")[0] == '0');
    static void *const kCode[] = {
        &&op_nop, &&op_sll, &&op_srl, &&op_sra, &&op_sllv, &&op_srlv, &&op_srav, &&op_jr, &&op_jalr, &&op_syscall, &&op_break,
        &&op_mfhi, &&op_mthi, &&op_mflo, &&op_mtlo, &&op_mult, &&op_multu, &&op_div, &&op_divu,
        &&op_addu, &&op_subu, &&op_and, &&op_or, &&op_xor, &&op_nor, &&op_slt, &&op_sltu,
        &&op_regimm, &&op_j, &&op_jal, &&op_beq, &&op_bne, &&op_blez, &&op_bgtz, &&op_addiu, &&op_slti, &&op_sltiu, &&op_andi,
        &&op_ori, &&op_xori, &&op_lui, &&op_cop0,
        &&op_lb, &&op_lh, &&op_lwl, &&op_lw, &&op_lbu, &&op_lhu, &&op_lwr, &&op_sb, &&op_sh, &&op_swl, &&op_sw, &&op_swr, &&op_hle,
        &&op_bad};
    static_assert(std::size(kCode) == kBad + 1);
    // the block code of each operation; the ones that never are in a block lead nowhere (kBad is the block's end)
    static const void *const kBlockCode[] = {
        &&b_nop, &&b_sll, &&b_srl, &&b_sra, &&b_sllv, &&b_srlv, &&b_srav, &&b_jr, &&b_jalr, nullptr, nullptr,
        &&b_mfhi, &&b_mthi, &&b_mflo, &&b_mtlo, &&b_mult, &&b_multu, &&b_div, &&b_divu,
        &&b_addu, &&b_subu, &&b_and, &&b_or, &&b_xor, &&b_nor, &&b_slt, &&b_sltu,
        &&b_regimm, &&b_j, &&b_jal, &&b_beq, &&b_bne, &&b_blez, &&b_bgtz, &&b_addiu, &&b_slti, &&b_sltiu, &&b_andi,
        &&b_ori, &&b_xori, &&b_lui, nullptr,
        &&b_lb, &&b_lh, &&b_lwl, &&b_lw, &&b_lbu, &&b_lhu, &&b_lwr, &&b_sb, &&b_sh, &&b_swl, &&b_sw, &&b_swr, nullptr,
        &&b_end};
    static_assert(std::size(kBlockCode) == kBad + 1);
    static const std::array<std::int16_t, kBad + 1> kBlockOff = [] {
        std::array<std::int16_t, kBad + 1> off{};
        for (unsigned i = 0; i <= kBad; ++i) {
            const std::ptrdiff_t d = kBlockCode[i] ? static_cast<const char *>(kBlockCode[i]) - static_cast<const char *>(kBlockCode[0]) : 0;
            if (d < INT16_MIN || d > INT16_MAX) std::abort();  // the block code is a few kilobytes
            off[i] = static_cast<std::int16_t>(d);
        }
        return off;
    }();
    const char *const block_code = static_cast<const char *>(kBlockCode[0]);
    ++g_epoch;  // anything may have written RAM since the last call
    std::int64_t budget = budget_out;
    std::uint32_t next = c.pc, after = c.npc;  // c.pc and c.npc
    std::uint32_t *const r = c.r;
    std::uint32_t pc, w, rs, rt, rd;
    Decoded d;
    // the running block: its virtual start, the pc after it (set by its branch), its physical bytes, its size and its operation
    std::uint32_t base = 0, bnext = 0, bp = 0, bbytes = 0, bn = 0;
    const BlockOp *e = nullptr;
    // the block that just ended and which of its two successors it went to, so that one is found without the table next time
    std::uint32_t cur_at = 0, from_at = 0, from_slot = 0, from_generation = 0, at = 0;

#define SAVE() (c.pc = next, c.npc = after)
#define LOAD() (next = c.pc, after = c.npc, ++g_epoch)  // after every call out of the interpreter
#define LEAVE()                                                                                                                  \
    do {                                                                                                                         \
        SAVE();                                                                                                                  \
        budget_out = budget;                                                                                                     \
        return;                                                                                                                  \
    } while (0)
#define DISPATCH()                                                                                                               \
    do {                                                                                                                         \
        if (blocks && after == next + 4) {                                                                                       \
            from_at = 0;                                                                                                         \
            goto block;                                                                                                          \
        }                                                                                                                        \
        goto step;                                                                                                               \
    } while (0)
#define STEP()                                                                                                                   \
    do {                                                                                                                         \
        if (budget-- <= 0) LEAVE();                                                                                              \
        pc = next;                                                                                                               \
        if (probing) {                                                                                                           \
            const std::uint32_t a_ = pc & (kRamSize - 1);                                                                        \
            if (g_probes[a_ >> 5] & (1u << ((a_ >> 2) & 7u))) probe(c, pc);                                                      \
        }                                                                                                                        \
        const std::uint32_t p_ = phys(pc);                                                                                       \
        if (p_ < 0x00800000u) [[likely]] {                                                                                       \
            Decoded &e_ = g_decoded[(p_ & (kRamSize - 1)) >> 2];                                                                 \
            const std::uint32_t w_ = ram32(p_);                                                                                  \
            if (e_.word != w_) [[unlikely]] e_ = decode(w_);                                                                     \
            d = e_;                                                                                                              \
        } else {                                                                                                                 \
            SAVE();                                                                                                              \
            d = decode(kernel::fetch_special(pc));                                                                               \
            LOAD();                                                                                                              \
        }                                                                                                                        \
        next = after;                                                                                                            \
        after += 4;                                                                                                              \
        w = d.word, rs = d.rs, rt = d.rt, rd = d.rd;                                                                             \
        goto *kCode[d.op];                                                                                                       \
    } while (0)
#define NEXT()                                                                                                                   \
    do {                                                                                                                         \
        r[0] = 0;                                                                                                                \
        if (kernel::switch_pending()) LEAVE();                                                                                   \
        DISPATCH();                                                                                                              \
    } while (0)
// RAM (and its mirrors below 8 MB) inline, as read8..write32 in hw.cpp do it; everything else, and every store while
// PS2_WATCH_IOP is on, through them
#define LOADS(out, a, type, slow)                                                                                                \
    do {                                                                                                                         \
        const std::uint32_t a_ = (a), p_ = phys(a_);                                                                             \
        if (p_ < 0x00800000u) [[likely]] {                                                                                       \
            type v_;                                                                                                             \
            std::memcpy(&v_, g_ram + (p_ & (kRamSize - 1)), sizeof(type));                                                       \
            out = v_;                                                                                                            \
        } else {                                                                                                                 \
            SAVE();                                                                                                              \
            out = slow(a_);                                                                                                      \
            LOAD();                                                                                                              \
        }                                                                                                                        \
    } while (0)
#define STORES(a, v, type, slow)                                                                                                 \
    do {                                                                                                                         \
        const std::uint32_t a_ = (a), p_ = phys(a_);                                                                             \
        const type v_ = static_cast<type>(v);                                                                                    \
        if (p_ < 0x00800000u && !g_watch_hi) [[likely]] {                                                                        \
            std::memcpy(g_ram + (p_ & (kRamSize - 1)), &v_, sizeof(type));                                                       \
            if (code_line(p_ & (kRamSize - 1)) || code_line((p_ + sizeof(type) - 1) & (kRamSize - 1))) ++g_epoch;                \
        } else {                                                                                                                 \
            SAVE();                                                                                                              \
            slow(a_, v_);                                                                                                        \
            LOAD();                                                                                                              \
        }                                                                                                                        \
    } while (0)
#define SIMM (static_cast<std::uint32_t>(simm(w)))
#define BRANCH_IF(cond)                                                                                                          \
    do {                                                                                                                         \
        if (cond) after = pc + 4 + (SIMM << 2);                                                                                  \
        NEXT();                                                                                                                  \
    } while (0)

    // the first instruction runs one by one: the switch may be wanted already, and the loop leaves after one instruction then
    goto step;

block: {
    const std::uint32_t p = phys(next);
    if (p >= 0x00800000u) [[unlikely]] goto step;
    const std::uint32_t q = p & (kRamSize - 1);
    at = g_block_at[q >> 2];
    if (at) [[likely]] {
        BlockHead &h = head(at);
        if (h.epoch != g_epoch) [[unlikely]] {
            if (block_valid(at)) h.epoch = g_epoch;
            else at = build_block(q, kBlockOff.data());
        }
    } else {
        if (budget < static_cast<std::int64_t>(kMaxBlock)) goto step;  // the end of a slice: no new block for every pc there
        at = build_block(q, kBlockOff.data());
    }
    if (from_at && from_generation == g_generation) {
        BlockHead &f = head(from_at);
        f.succ_pc[from_slot] = next;
        f.succ_at[from_slot] = at;
    }
}
enter: {
    const BlockHead &h = head(at);
    if (h.n == 0 || budget < h.n) goto step;
    budget -= h.n;
    bn = h.n;
    base = next;
    bnext = next + bn * 4;
    bp = h.p;
    bbytes = bn * 4;
    cur_at = at;
    e = g_arena.data() + at + kHeadOps;
    goto *(block_code + e->code);
}
step:
    STEP();

// fields of the running block operation's word
#define RS ((e->w >> 21) & 31u)
#define RT ((e->w >> 16) & 31u)
#define RD ((e->w >> 11) & 31u)
#define SA ((e->w >> 6) & 31u)
#define SIMM_E (static_cast<std::uint32_t>(simm(e->w)))
// block code: every operation ends with BNEXT; GIVE_UP hands the instruction to the one-by-one loop (see above)
// (the empty asm differs in every copy, so the compiler cannot fold the copies into one shared jump, which predicts worse)
#define BNEXT()                                                                                                                  \
    do {                                                                                                                         \
        ++e;                                                                                                                     \
        asm volatile("" ::"i"(__COUNTER__));                                                                                     \
        goto *(block_code + e->code);                                                                                            \
    } while (0)
#define GIVE_UP(op)                                                                                                              \
    do {                                                                                                                         \
        pc = base + e->idx * 4u;                                                                                                 \
        next = e->idx + 1u == bn ? bnext : pc + 4;                                                                               \
        after = next + 4;                                                                                                        \
        budget += bn - (e->idx + 1u);                                                                                            \
        w = e->w, rs = (w >> 21) & 31u, rt = (w >> 16) & 31u, rd = (w >> 11) & 31u;                                             \
        goto op;                                                                                                                 \
    } while (0)
#define BLOAD(out, type, op)                                                                                                     \
    do {                                                                                                                         \
        const std::uint32_t p_ = phys(r[RS] + SIMM_E);                                                                        \
        if (p_ >= 0x00800000u) [[unlikely]] GIVE_UP(op);                                                                         \
        type v_;                                                                                                                 \
        std::memcpy(&v_, g_ram + (p_ & (kRamSize - 1)), sizeof(type));                                                           \
        out = v_;                                                                                                                \
    } while (0)
// a store into the block's own words (any of its bytes) gives up too, so the instruction it changes is run as it is now
#define BSTORE_OK(p_, size) ((p_) < 0x00800000u && !g_watch_hi && (((p_) & (kRamSize - 1)) - bp + ((size) - 1u)) >= bbytes + ((size) - 1u))
#define BSTORE(v, type, op)                                                                                                      \
    do {                                                                                                                         \
        const std::uint32_t p_ = phys(r[RS] + SIMM_E);                                                                        \
        if (!BSTORE_OK(p_, sizeof(type))) [[unlikely]] GIVE_UP(op);                                                              \
        const type v_ = static_cast<type>(v);                                                                                    \
        std::memcpy(g_ram + (p_ & (kRamSize - 1)), &v_, sizeof(type));                                                           \
        if (code_line(p_ & (kRamSize - 1)) || code_line((p_ + sizeof(type) - 1) & (kRamSize - 1))) ++g_epoch;                    \
    } while (0)
#define BBRANCH_IF(cond)                                                                                                         \
    do {                                                                                                                         \
        if (cond) bnext = base + e->idx * 4u + 4u + (SIMM_E << 2);                                                                                         \
        BNEXT();                                                                                                                 \
    } while (0)

b_nop: BNEXT();
b_sll: r[RD] = r[RT] << SA; BNEXT();
b_srl: r[RD] = r[RT] >> SA; BNEXT();
b_sra: r[RD] = static_cast<std::uint32_t>(static_cast<std::int32_t>(r[RT]) >> SA); BNEXT();
b_sllv: r[RD] = r[RT] << (r[RS] & 31u); BNEXT();
b_srlv: r[RD] = r[RT] >> (r[RS] & 31u); BNEXT();
b_srav: r[RD] = static_cast<std::uint32_t>(static_cast<std::int32_t>(r[RT]) >> (r[RS] & 31u)); BNEXT();
b_jr: bnext = r[RS]; BNEXT();
b_jalr: { const std::uint32_t t = r[RS]; r[RD] = base + e->idx * 4u + 8u; r[0] = 0; bnext = t; BNEXT(); }
b_mfhi: r[RD] = c.hi; BNEXT();
b_mthi: c.hi = r[RS]; BNEXT();
b_mflo: r[RD] = c.lo; BNEXT();
b_mtlo: c.lo = r[RS]; BNEXT();
b_mult: {
    const std::int64_t m = static_cast<std::int64_t>(static_cast<std::int32_t>(r[RS])) * static_cast<std::int32_t>(r[RT]);
    c.lo = static_cast<std::uint32_t>(m);
    c.hi = static_cast<std::uint32_t>(static_cast<std::uint64_t>(m) >> 32);
    BNEXT();
}
b_multu: {
    const std::uint64_t m = static_cast<std::uint64_t>(r[RS]) * r[RT];
    c.lo = static_cast<std::uint32_t>(m);
    c.hi = static_cast<std::uint32_t>(m >> 32);
    BNEXT();
}
b_div: {
    const auto n = static_cast<std::int32_t>(r[RS]), q = static_cast<std::int32_t>(r[RT]);
    if (q == 0) { c.lo = n < 0 ? 1u : 0xFFFFFFFFu; c.hi = static_cast<std::uint32_t>(n); }
    else if (n == INT32_MIN && q == -1) { c.lo = static_cast<std::uint32_t>(INT32_MIN); c.hi = 0; }
    else { c.lo = static_cast<std::uint32_t>(n / q); c.hi = static_cast<std::uint32_t>(n % q); }
    BNEXT();
}
b_divu: {
    const std::uint32_t n = r[RS], q = r[RT];
    if (q == 0) { c.lo = 0xFFFFFFFFu; c.hi = n; }
    else { c.lo = n / q; c.hi = n % q; }
    BNEXT();
}
b_addu: r[RD] = r[RS] + r[RT]; BNEXT();
b_subu: r[RD] = r[RS] - r[RT]; BNEXT();
b_and: r[RD] = r[RS] & r[RT]; BNEXT();
b_or: r[RD] = r[RS] | r[RT]; BNEXT();
b_xor: r[RD] = r[RS] ^ r[RT]; BNEXT();
b_nor: r[RD] = ~(r[RS] | r[RT]); BNEXT();
b_slt: r[RD] = static_cast<std::int32_t>(r[RS]) < static_cast<std::int32_t>(r[RT]); BNEXT();
b_sltu: r[RD] = r[RS] < r[RT]; BNEXT();
b_regimm: {
    const std::uint32_t t = RT;
    const bool taken = (t & 1u) ? static_cast<std::int32_t>(r[RS]) >= 0 : static_cast<std::int32_t>(r[RS]) < 0;
    if ((t & 0x1Eu) == 0x10u) r[31] = base + e->idx * 4u + 8u;
    BBRANCH_IF(taken);
}
b_j: bnext = (base & 0xF0000000u) | ((e->w & 0x03FFFFFFu) << 2); BNEXT();
b_jal: r[31] = base + e->idx * 4u + 8u; bnext = (base & 0xF0000000u) | ((e->w & 0x03FFFFFFu) << 2); BNEXT();
b_beq: BBRANCH_IF(r[RS] == r[RT]);
b_bne: BBRANCH_IF(r[RS] != r[RT]);
b_blez: BBRANCH_IF(static_cast<std::int32_t>(r[RS]) <= 0);
b_bgtz: BBRANCH_IF(static_cast<std::int32_t>(r[RS]) > 0);
b_addiu: r[RT] = r[RS] + SIMM_E; BNEXT();
b_slti: r[RT] = static_cast<std::int32_t>(r[RS]) < static_cast<std::int32_t>(SIMM_E); BNEXT();
b_sltiu: r[RT] = r[RS] < SIMM_E; BNEXT();
b_andi: r[RT] = r[RS] & (e->w & 0xFFFFu); BNEXT();
b_ori: r[RT] = r[RS] | (e->w & 0xFFFFu); BNEXT();
b_xori: r[RT] = r[RS] ^ (e->w & 0xFFFFu); BNEXT();
b_lui: r[RT] = (e->w << 16); BNEXT();
b_lb: { std::uint8_t v; BLOAD(v, std::uint8_t, op_lb); r[RT] = static_cast<std::uint32_t>(static_cast<std::int8_t>(v)); r[0] = 0; BNEXT(); }
b_lh: { std::uint16_t v; BLOAD(v, std::uint16_t, op_lh); r[RT] = static_cast<std::uint32_t>(static_cast<std::int16_t>(v)); r[0] = 0; BNEXT(); }
b_lw: { std::uint32_t v; BLOAD(v, std::uint32_t, op_lw); r[RT] = v; r[0] = 0; BNEXT(); }
b_lbu: { std::uint8_t v; BLOAD(v, std::uint8_t, op_lbu); r[RT] = v; r[0] = 0; BNEXT(); }
b_lhu: { std::uint16_t v; BLOAD(v, std::uint16_t, op_lhu); r[RT] = v; r[0] = 0; BNEXT(); }
b_lwl: {
    const std::uint32_t a = r[RS] + SIMM_E, shift = (3u - (a & 3u)) * 8u, p_ = phys(a & ~3u);
    if (p_ >= 0x00800000u) [[unlikely]] GIVE_UP(op_lwl);
    const std::uint32_t mem = ram32(p_);
    r[RT] = (mem << shift) | (shift ? (r[RT] & ((1u << shift) - 1u)) : 0u);
    r[0] = 0;
    BNEXT();
}
b_lwr: {
    const std::uint32_t a = r[RS] + SIMM_E, shift = (a & 3u) * 8u, p_ = phys(a & ~3u);
    if (p_ >= 0x00800000u) [[unlikely]] GIVE_UP(op_lwr);
    const std::uint32_t mem = ram32(p_);
    r[RT] = (mem >> shift) | (shift ? (r[RT] & ~(0xFFFFFFFFu >> shift)) : 0u);
    r[0] = 0;
    BNEXT();
}
b_sb: BSTORE(r[RT], std::uint8_t, op_sb); BNEXT();
b_sh: BSTORE(r[RT], std::uint16_t, op_sh); BNEXT();
b_sw: BSTORE(r[RT], std::uint32_t, op_sw); BNEXT();
b_swl: {
    const std::uint32_t a = r[RS] + SIMM_E, shift = (3u - (a & 3u)) * 8u, p_ = phys(a & ~3u);
    if (!BSTORE_OK(p_, 4u)) [[unlikely]] GIVE_UP(op_swl);
    set_ram32(p_, (r[RT] >> shift) | (shift ? (ram32(p_) & ~(0xFFFFFFFFu >> shift)) : 0u));
    if (code_line(p_ & (kRamSize - 1))) ++g_epoch;
    BNEXT();
}
b_swr: {
    const std::uint32_t a = r[RS] + SIMM_E, shift = (a & 3u) * 8u, p_ = phys(a & ~3u);
    if (!BSTORE_OK(p_, 4u)) [[unlikely]] GIVE_UP(op_swr);
    set_ram32(p_, (r[RT] << shift) | (shift ? (ram32(p_) & ((1u << shift) - 1u)) : 0u));
    if (code_line(p_ & (kRamSize - 1))) ++g_epoch;
    BNEXT();
}
b_end: {
    next = bnext;
    after = bnext + 4;
    const std::uint32_t slot = bnext != base + bn * 4;
    BlockHead &h = head(cur_at);
    if (h.succ_pc[slot] == bnext && h.succ_at[slot]) {
        at = h.succ_at[slot];
        BlockHead &s = head(at);
        if (s.epoch == g_epoch) [[likely]] goto enter;
        if (block_valid(at)) {
            s.epoch = g_epoch;
            goto enter;
        }
    }
    from_at = cur_at, from_slot = slot, from_generation = g_generation;
    goto block;
}

op_nop: NEXT();
op_sll: r[rd] = r[rt] << ((w >> 6) & 31u); NEXT();
op_srl: r[rd] = r[rt] >> ((w >> 6) & 31u); NEXT();
op_sra: r[rd] = static_cast<std::uint32_t>(static_cast<std::int32_t>(r[rt]) >> ((w >> 6) & 31u)); NEXT();
op_sllv: r[rd] = r[rt] << (r[rs] & 31u); NEXT();
op_srlv: r[rd] = r[rt] >> (r[rs] & 31u); NEXT();
op_srav: r[rd] = static_cast<std::uint32_t>(static_cast<std::int32_t>(r[rt]) >> (r[rs] & 31u)); NEXT();
op_jr: after = r[rs]; NEXT();
op_jalr: { const std::uint32_t t = r[rs]; r[rd] = pc + 8; after = t; NEXT(); }
op_syscall: SAVE(); kernel::syscall(c, pc); LOAD(); NEXT();
op_break: std::fprintf(stderr, "[iop] break at 0x%08X\n", pc); NEXT();
op_mfhi: r[rd] = c.hi; NEXT();
op_mthi: c.hi = r[rs]; NEXT();
op_mflo: r[rd] = c.lo; NEXT();
op_mtlo: c.lo = r[rs]; NEXT();
op_mult: {
    const std::int64_t m = static_cast<std::int64_t>(static_cast<std::int32_t>(r[rs])) * static_cast<std::int32_t>(r[rt]);
    c.lo = static_cast<std::uint32_t>(m);
    c.hi = static_cast<std::uint32_t>(static_cast<std::uint64_t>(m) >> 32);
    NEXT();
}
op_multu: {
    const std::uint64_t m = static_cast<std::uint64_t>(r[rs]) * r[rt];
    c.lo = static_cast<std::uint32_t>(m);
    c.hi = static_cast<std::uint32_t>(m >> 32);
    NEXT();
}
op_div: {
    const auto n = static_cast<std::int32_t>(r[rs]), q = static_cast<std::int32_t>(r[rt]);
    if (q == 0) { c.lo = n < 0 ? 1u : 0xFFFFFFFFu; c.hi = static_cast<std::uint32_t>(n); }
    else if (n == INT32_MIN && q == -1) { c.lo = static_cast<std::uint32_t>(INT32_MIN); c.hi = 0; }
    else { c.lo = static_cast<std::uint32_t>(n / q); c.hi = static_cast<std::uint32_t>(n % q); }
    NEXT();
}
op_divu:
    if (r[rt] == 0) { c.lo = 0xFFFFFFFFu; c.hi = r[rs]; }
    else { c.lo = r[rs] / r[rt]; c.hi = r[rs] % r[rt]; }
    NEXT();
op_addu: r[rd] = r[rs] + r[rt]; NEXT();
op_subu: r[rd] = r[rs] - r[rt]; NEXT();
op_and: r[rd] = r[rs] & r[rt]; NEXT();
op_or: r[rd] = r[rs] | r[rt]; NEXT();
op_xor: r[rd] = r[rs] ^ r[rt]; NEXT();
op_nor: r[rd] = ~(r[rs] | r[rt]); NEXT();
op_slt: r[rd] = static_cast<std::int32_t>(r[rs]) < static_cast<std::int32_t>(r[rt]); NEXT();
op_sltu: r[rd] = r[rs] < r[rt]; NEXT();
op_regimm: {
    const bool link = (rt & 0x1Eu) == 0x10u;
    const bool taken = (rt & 1u) ? static_cast<std::int32_t>(r[rs]) >= 0 : static_cast<std::int32_t>(r[rs]) < 0;
    if (link) r[31] = pc + 8;
    BRANCH_IF(taken);
}
op_j: after = (pc & 0xF0000000u) | ((w & 0x03FFFFFFu) << 2); NEXT();
op_jal: r[31] = pc + 8; after = (pc & 0xF0000000u) | ((w & 0x03FFFFFFu) << 2); NEXT();
op_beq: BRANCH_IF(r[rs] == r[rt]);
op_bne: BRANCH_IF(r[rs] != r[rt]);
op_blez: BRANCH_IF(static_cast<std::int32_t>(r[rs]) <= 0);
op_bgtz: BRANCH_IF(static_cast<std::int32_t>(r[rs]) > 0);
op_addiu: r[rt] = r[rs] + SIMM; NEXT();
op_slti: r[rt] = static_cast<std::int32_t>(r[rs]) < simm(w); NEXT();
op_sltiu: r[rt] = r[rs] < SIMM; NEXT();
op_andi: r[rt] = r[rs] & (w & 0xFFFFu); NEXT();
op_ori: r[rt] = r[rs] | (w & 0xFFFFu); NEXT();
op_xori: r[rt] = r[rs] ^ (w & 0xFFFFu); NEXT();
op_lui: r[rt] = w << 16; NEXT();
op_cop0:
    if (rs == 0) r[rt] = rd == 12 ? c.status : rd == 13 ? c.cause : rd == 14 ? c.epc : rd == 15 ? 0x1Fu : 0u;
    else if (rs == 4) {
        if (rd == 12) { SAVE(); kernel::set_status(c, r[rt]); LOAD(); }
        else if (rd == 13) c.cause = r[rt];
        else if (rd == 14) c.epc = r[rt];
    } else if ((w & 63u) == 0x10) {
        c.status = (c.status & ~0xFu) | ((c.status >> 2) & 0xFu);
    }
    NEXT();
op_lb: { std::uint8_t v; LOADS(v, r[rs] + SIMM, std::uint8_t, read8); r[rt] = static_cast<std::uint32_t>(static_cast<std::int8_t>(v)); NEXT(); }
op_lh: { std::uint16_t v; LOADS(v, r[rs] + SIMM, std::uint16_t, read16); r[rt] = static_cast<std::uint32_t>(static_cast<std::int16_t>(v)); NEXT(); }
op_lwl: {
    const std::uint32_t a = r[rs] + SIMM, shift = (3u - (a & 3u)) * 8u;
    std::uint32_t mem;
    LOADS(mem, a & ~3u, std::uint32_t, read32);
    r[rt] = (mem << shift) | (shift ? (r[rt] & ((1u << shift) - 1u)) : 0u);
    NEXT();
}
op_lw: { std::uint32_t v; LOADS(v, r[rs] + SIMM, std::uint32_t, read32); r[rt] = v; NEXT(); }
op_lbu: { std::uint8_t v; LOADS(v, r[rs] + SIMM, std::uint8_t, read8); r[rt] = v; NEXT(); }
op_lhu: { std::uint16_t v; LOADS(v, r[rs] + SIMM, std::uint16_t, read16); r[rt] = v; NEXT(); }
op_lwr: {
    const std::uint32_t a = r[rs] + SIMM, shift = (a & 3u) * 8u;
    std::uint32_t mem;
    LOADS(mem, a & ~3u, std::uint32_t, read32);
    r[rt] = (mem >> shift) | (shift ? (r[rt] & ~(0xFFFFFFFFu >> shift)) : 0u);
    NEXT();
}
op_sb: STORES(r[rs] + SIMM, r[rt], std::uint8_t, write8); NEXT();
op_sh: STORES(r[rs] + SIMM, r[rt], std::uint16_t, write16); NEXT();
op_swl: {
    const std::uint32_t a = r[rs] + SIMM, shift = (3u - (a & 3u)) * 8u;
    std::uint32_t mem;
    LOADS(mem, a & ~3u, std::uint32_t, read32);
    STORES(a & ~3u, (r[rt] >> shift) | (shift ? (mem & ~(0xFFFFFFFFu >> shift)) : 0u), std::uint32_t, write32);
    NEXT();
}
op_sw: STORES(r[rs] + SIMM, r[rt], std::uint32_t, write32); NEXT();
op_swr: {
    const std::uint32_t a = r[rs] + SIMM, shift = (a & 3u) * 8u;
    std::uint32_t mem;
    LOADS(mem, a & ~3u, std::uint32_t, read32);
    STORES(a & ~3u, (r[rt] << shift) | (shift ? (mem & ((1u << shift) - 1u)) : 0u), std::uint32_t, write32);
    NEXT();
}
op_hle:
    next = r[31];
    after = next + 4;
    SAVE();
    r[0] = 0;
    call_hle(c, w & 0x03FFFFFFu);
    LOAD();
    r[0] = 0;
    if (kernel::switch_pending()) LEAVE();
    DISPATCH();
op_bad:
    SAVE();
    budget_out = budget;
    bad(c, pc, w);
    return;

#undef SAVE
#undef LOAD
#undef LEAVE
#undef DISPATCH
#undef NEXT
#undef STEP
#undef BNEXT
#undef GIVE_UP
#undef BLOAD
#undef BSTORE_OK
#undef BSTORE
#undef BBRANCH_IF
#undef RS
#undef RT
#undef RD
#undef SA
#undef SIMM_E
#undef LOADS
#undef STORES
#undef SIMM
#undef BRANCH_IF
}

}
