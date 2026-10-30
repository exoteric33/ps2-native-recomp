#include "dmac.hpp"

#include "kernel.hpp"

#include "gs/gif.hpp"
#include "gs/gs.hpp"
#include "ipu/ipu.hpp"
#include "hw.hpp"
#include "ps2/memory.hpp"
#include "ps2/system.hpp"
#include "vif.hpp"
#include "vu_micro.hpp"

#include "ps2/state.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace ps2::dmac {

namespace {

struct Channel {
    std::uint32_t chcr = 0, madr = 0, qwc = 0, tadr = 0, asr[2] = {}, sadr = 0;
    bool paused = false, pause_end = false, tag_end = false;
};

Channel g_ch[10];
std::uint32_t g_ctrl = 0, g_pcr = 0, g_sqwc = 0, g_rbsr = 0, g_rbor = 0, g_stadr = 0, g_enable = 0;

constexpr std::uint32_t kBase[10] = {0x10008000u, 0x10009000u, 0x1000A000u, 0x1000B000u, 0x1000B400u,
                                     0x1000C000u, 0x1000C400u, 0x1000C800u, 0x1000D000u, 0x1000D400u};

std::uint64_t g_starts[10];
bool g_mfifo_waiting = false;

// MFIFO: with D_CTRL.MFD set to VIF1, fromSPR fills a ring buffer in main memory (D_RBOR,
// D_RBSR) and VIF1 runs a chain out of it, stalling while its tag address has caught up
// with fromSPR's write address.
bool mfifo_vif1() { return ((g_ctrl >> 2) & 3u) == 2u; }
std::uint32_t ring(std::uint32_t a) { return g_rbor | (a & g_rbsr); }
bool tag_data_in_ring(std::uint32_t chcr) {
    const std::uint32_t id = (chcr >> 28) & 7u;
    return id == 1 || id == 7;
}

// The vector units' memories sit on the bus at 0x11000000..0x1100FFFF (EE memory map: VU0 code 0x11000000, VU0 data
// 0x11004000, VU1 code 0x11008000, VU1 data 0x1100C000), and a DMA address there reaches them like one in main memory.
// Silent Hill 2 copies what VU0 leaves in its data memory to the scratchpad with toSPR from 0x11004000 (the face morphs);
// read as main memory that was 0x01004000, all zeros. Other addresses of the window fall through to main memory, as before.
std::uint8_t *vu_window(std::uint32_t address, bool write) {
    if ((address & 0x7FFF0000u) != 0x11000000u) return nullptr;
    const std::uint32_t area = (address >> 12) & 0xFu;
    if (area >= 0x8u) drain_vu_thread();  // VU1 runs on its own thread: what it was sent must have landed (memory.cpp does the same)
    switch (area) {
    case 0x0: if (write) vu::code_changed(0); return g_vu0_code + (address & 0xFF0u);
    case 0x4: return g_vu0_mem + (address & 0xFF0u);
    case 0x8: case 0x9: case 0xA: case 0xB: if (write) vu::code_changed(1); return g_vu1_code + (address & 0x3FF0u);
    case 0xC: case 0xD: case 0xE: case 0xF: return g_vu1_mem + (address & 0x3FF0u);
    default: return nullptr;
    }
}

const std::uint8_t *source(std::uint32_t address) {
    if (address & 0x80000000u) return g_spr + (address & (kScratchpadSize - 1));
    if (const std::uint8_t *vu = vu_window(address, false)) [[unlikely]] return vu;
    return g_ram + (address & (kRamSize - 16));
}

std::uint8_t *target(std::uint32_t address) {
    if (address & 0x80000000u) return g_spr + (address & (kScratchpadSize - 1));
    if (std::uint8_t *vu = vu_window(address, true)) [[unlikely]] return vu;
    return g_ram + (address & (kRamSize - 16));
}

void deliver(int ch, std::uint32_t address, std::uint32_t qwc) {
    if (!qwc) return;
    static std::vector<std::uint8_t> buffer;
    const std::size_t bytes = static_cast<std::size_t>(qwc) * 16;
    const std::size_t room = (address & 0x80000000u) ? kScratchpadSize - (address & (kScratchpadSize - 1)) : kRamSize - (address & (kRamSize - 16));
    const std::uint8_t *data = source(address);
    if (bytes > room || ch == 9 || ((address & 0x80000000u) == 0 && (address & 0x7FFF0000u) == 0x11000000u)) {  // a VU memory is small: copy by quadword
        buffer.resize(bytes);
        for (std::uint32_t i = 0; i < qwc; ++i) std::memcpy(buffer.data() + i * 16, source(address + i * 16), 16);
        data = buffer.data();
    }
    switch (ch) {
    case 0: vif::transfer(0, data, qwc); break;
    case 1: vif::transfer(1, data, qwc); break;
    case 2: gif::transfer(3, data, qwc); break;
    case 9:
        for (std::uint32_t i = 0; i < qwc; ++i)
            std::memcpy(g_spr + ((g_ch[9].sadr + i * 16) & (kScratchpadSize - 1)), data + i * 16, 16);
        g_ch[9].sadr = (g_ch[9].sadr + qwc * 16) & (kScratchpadSize - 1);
        break;
    default:
        std::fprintf(stderr, "[dma] channel %d data (%u qwords) not handled\n", ch, qwc);
        break;
    }
}

void deliver_ring(int ch, std::uint32_t address, std::uint32_t qwc) {
    while (qwc) {
        const std::uint32_t room = ((g_rbsr - (address & g_rbsr)) >> 4) + 1;
        const std::uint32_t n = std::min(qwc, room);
        deliver(ch, address, n);
        address = ring(address + n * 16);
        qwc -= n;
    }
}

bool vif_stalled(int ch) { return (ch == 0 || ch == 1) && vif::stalled(ch); }

void pause(int ch, bool end) {
    g_ch[ch].paused = true;
    g_ch[ch].pause_end = end;
}

void run_chain(int ch);

void finish(int ch) {
    Channel &c = g_ch[ch];
    static const bool trace = std::getenv("PS2_TRACE_DMA") != nullptr;
    if (trace && ch <= 4) std::fprintf(stderr, "[dma] ch%d done chcr 0x%08X tadr 0x%08X madr 0x%08X\n", ch, c.chcr, c.tadr, c.madr);
    c.chcr &= ~0x100u;
    if (ch == 1) gif::vif1_idle();
    hw::dmac_raise(static_cast<unsigned>(ch));
    if (ch == 8 && g_mfifo_waiting && (g_ch[1].chcr & 0x100u) && !g_ch[1].paused) {
        g_mfifo_waiting = false;
        run_chain(1);
    }
}

void spr_to_ram(Channel &c, std::uint32_t qwc) {
    const bool wrap = mfifo_vif1();
    for (std::uint32_t i = 0; i < qwc; ++i) {
        const std::uint32_t at = wrap ? ring(c.madr + i * 16) : c.madr + i * 16;
        std::memcpy(target(at), g_spr + ((c.sadr + i * 16) & (kScratchpadSize - 1)), 16);
        watch_span(at, 16, "dma:spr_to_ram");
    }
    c.sadr = (c.sadr + qwc * 16) & (kScratchpadSize - 1);
}

std::uint32_t spr_advance(std::uint32_t madr, std::uint32_t qwc) {
    return mfifo_vif1() ? ring(madr + qwc * 16) : madr + qwc * 16;
}

void run_from_spr_chain() {
    Channel &c = g_ch[8];
    const bool tie = c.chcr & 0x80u;
    bool end = false;
    if (c.qwc) {
        end = ((c.chcr >> 28) & 7u) == 7u || (tie && (c.chcr & 0x80000000u));
        spr_to_ram(c, c.qwc);
        c.madr = spr_advance(c.madr, c.qwc);
        c.qwc = 0;
    }
    for (int guard = 0; !end && guard < 1'000'000; ++guard) {
        std::uint64_t tag;
        std::memcpy(&tag, g_spr + (c.sadr & (kScratchpadSize - 1)), 8);
        c.sadr = (c.sadr + 16) & (kScratchpadSize - 1);
        c.chcr = (c.chcr & 0xFFFFu) | (static_cast<std::uint32_t>(tag >> 16) << 16);
        const std::uint32_t qwc = static_cast<std::uint32_t>(tag) & 0xFFFFu;
        c.madr = static_cast<std::uint32_t>(tag >> 32);
        spr_to_ram(c, qwc);
        c.madr = spr_advance(c.madr, qwc);
        end = ((tag >> 28) & 7u) == 7u || (tie && ((tag >> 31) & 1u));
    }
    finish(8);
}

// Interleave mode (CHCR.MOD == 2) on the scratchpad channels: D_SQWC.TQWC quadwords go through, then
// D_SQWC.SQWC quadwords of main memory are skipped. The scratchpad side stays contiguous (SADR only
// counts the transferred quadwords) and MADR also steps over the skip after the last block. A TQWC of
// 0 transfers everything in one block.
bool interleaved(const Channel &c) { return ((c.chcr >> 2) & 3u) == 2u; }

template <class Block>
void interleave(int ch, Block block) {
    Channel &c = g_ch[ch];
    const std::uint32_t tqwc = (g_sqwc >> 16) & 0xFFu, skip = g_sqwc & 0xFFu;
    for (std::uint32_t left = c.qwc; left;) {
        const std::uint32_t n = std::min(tqwc ? tqwc : left, left);
        block(n);
        c.madr = ch == 8 ? spr_advance(c.madr, n + skip) : c.madr + (n + skip) * 16;
        left -= n;
    }
}

void run_normal(int ch) {
    Channel &c = g_ch[ch];
    if (ch == 8) {
        if (interleaved(c)) {
            interleave(8, [&](std::uint32_t n) { spr_to_ram(c, n); });
        } else {
            spr_to_ram(c, c.qwc);
            c.madr = spr_advance(c.madr, c.qwc);
        }
        c.qwc = 0;
        finish(8);
        return;
    } else if (ch == 9 && interleaved(c)) {
        interleave(9, [&](std::uint32_t n) { deliver(9, c.madr, n); });
        c.qwc = 0;
        finish(9);
        return;
    } else if ((c.chcr & 1u) || ch == 2 || ch == 0 || ch == 9) {
        deliver(ch, c.madr, c.qwc);
    } else if (ch == 1) {
        for (std::uint32_t i = 0, n = 1; i < c.qwc; i += n, n = 1) {
            while (i + n < c.qwc && target(c.madr + (i + n) * 16) == target(c.madr + i * 16) + n * 16) ++n;
            gs::read_image(target(c.madr + i * 16), n);
            watch_span(c.madr + i * 16, n * 16, "dma:gs_read_image");
        }
    } else {
        std::fprintf(stderr, "[dma] channel %d reads from a device (not emulated)\n", ch);
    }
    c.madr += c.qwc * 16;
    c.qwc = 0;
    if (vif_stalled(ch)) {
        pause(ch, true);
        return;
    }
    finish(ch);
}

bool chain_ends_after_current(const Channel &c) {
    const std::uint32_t id = (c.chcr >> 28) & 7u;
    return id == 0 || id == 7 || ((c.chcr & 0x80u) && (c.chcr & 0x80000000u));
}

void run_chain(int ch) {
    Channel &c = g_ch[ch];
    const bool mfifo = ch == 1 && mfifo_vif1();
    if (c.qwc) {
        const bool end = chain_ends_after_current(c);
        if (mfifo && tag_data_in_ring(c.chcr)) {
            deliver_ring(ch, c.madr, c.qwc);
            c.madr = ring(c.madr + c.qwc * 16);
        } else {
            deliver(ch, c.madr, c.qwc);
            c.madr += c.qwc * 16;
        }
        c.qwc = 0;
        if (vif_stalled(ch)) {
            pause(ch, end);
            return;
        }
        if (end) {
            finish(ch);
            return;
        }
    }
    const bool tte = c.chcr & 0x40u, tie = c.chcr & 0x80u;
    for (int guard = 0; guard < 1'000'000; ++guard) {
        if (mfifo && ring(c.tadr) == ring(g_ch[8].madr)) {
            g_mfifo_waiting = true;
            return;
        }
        const std::uint8_t *tagp = source(c.tadr);
        std::uint64_t tag;
        std::memcpy(&tag, tagp, 8);
        c.chcr = (c.chcr & 0xFFFFu) | (static_cast<std::uint32_t>(tag >> 16) << 16);
        const std::uint32_t qwc = static_cast<std::uint32_t>(tag) & 0xFFFFu;
        const std::uint32_t id = static_cast<std::uint32_t>(tag >> 28) & 7u;
        const bool irq = (tag >> 31) & 1u;
        const std::uint32_t addr = static_cast<std::uint32_t>(tag >> 32) & 0x7FFFFFF0u;
        const std::uint32_t addr_full = addr | (static_cast<std::uint32_t>(tag >> 63) << 31);
        if (tte && (ch == 0 || ch == 1)) {
            // The upper 64 bits of the tag go to the unit as two words, nothing else. (Padding them to a quadword with two zero words
            // looked harmless, since a zero word is a NOP, but a command still waiting for its data takes those words as data:
            // Tekken 5 sends an UNPACK across a tag, and the two extra words pushed its last two data words out as commands.)
            std::uint32_t q[2];
            std::memcpy(q, tagp + 8, 8);
            vif::transfer_words(ch, q, 2);
        } else if (tte && ch == 9) {
            // toSPR with the tag transfer enabled copies the whole 16-byte tag into the scratchpad ahead of its data (the game
            // reads its list at SADR+16). The tag goes to the SPR address the transfer is at, which then moves on by one quadword.
            std::memcpy(g_spr + (c.sadr & (kScratchpadSize - 1)), tagp, 16);
            c.sadr = (c.sadr + 16) & (kScratchpadSize - 1);
        }
        bool end = false;
        const std::uint32_t tag_at = c.tadr;
        std::uint32_t data = c.tadr + 16;
        switch (id) {
        case 0: data = addr_full; c.tadr += 16; end = true; break;
        case 1: c.tadr = data + qwc * 16; break;
        case 2: c.tadr = addr_full; break;
        case 3: case 4: data = addr_full; c.tadr += 16; break;
        case 5: {
            const std::uint32_t asp = (c.chcr >> 4) & 3u;
            if (asp < 2) c.asr[asp] = data + qwc * 16;
            c.chcr = (c.chcr & ~0x30u) | (std::min(asp + 1, 2u) << 4);
            c.tadr = addr_full;
            break;
        }
        case 6: {
            const std::uint32_t asp = (c.chcr >> 4) & 3u;
            if (asp > 0) {
                c.tadr = c.asr[asp - 1];
                c.chcr = (c.chcr & ~0x30u) | ((asp - 1) << 4);
            } else {
                end = true;
            }
            break;
        }
        default: end = true; break;
        }
        if (mfifo) {
            if (id == 1 || id == 7) {
                data = ring(data);
                if (id == 1) c.tadr = data + qwc * 16;
            }
            c.tadr = ring(c.tadr);
        }
        c.madr = data;
        if (ch == 2 && g_starts[2] <= 2) std::fprintf(stderr, "[dma] gif tag id %u qwc %u addr 0x%08X data 0x%08X\n", id, qwc, addr_full, data);
        static const bool trace_tags = std::getenv("PS2_TRACE_DMA1") != nullptr;
        if (trace_tags && ch == 1) std::fprintf(stderr, "[dma] ch1 tag at 0x%08X id %u qwc %u irq %d addr 0x%08X next 0x%08X\n", tag_at, id, qwc, irq ? 1 : 0, addr_full, c.tadr);
        if (trace_tags && ch == 1 && qwc <= 12 && std::getenv("PS2_TRACE_DMA1")[0] == '2')  // PS2_TRACE_DMA1=2: the words of short packets too
            for (std::uint32_t i = 0; i < qwc; ++i) {
                float f[4];
                std::uint32_t w[4];
                std::memcpy(f, source(data + i * 16), 16);
                std::memcpy(w, f, 16);
                std::fprintf(stderr, "[dma1-data] %08X: %08X %08X %08X %08X | %g %g %g %g\n", data + i * 16, w[0], w[1], w[2], w[3], f[0], f[1], f[2], f[3]);
            }
        if (mfifo && (id == 1 || id == 7)) {
            deliver_ring(ch, data, qwc);
            c.madr = ring(data + qwc * 16);
        } else {
            deliver(ch, data, qwc);
            c.madr = data + qwc * 16;
        }
        if (vif_stalled(ch)) {
            pause(ch, end || (tie && irq));
            return;
        }
        if (end || (tie && irq)) break;
    }
    c.qwc = 0;
    finish(ch);
}

void start(int ch) {
    Channel &c = g_ch[ch];
    c.paused = false;
    c.tag_end = c.qwc && chain_ends_after_current(c);
    ++g_starts[ch];
    static const bool trace = std::getenv("PS2_TRACE_DMA") != nullptr;
    if (trace) std::fprintf(stderr, "[dma] ch%d chcr 0x%08X madr 0x%08X qwc %u tadr 0x%08X thread %d%s\n", ch, c.chcr, c.madr, c.qwc, c.tadr,
                            kernel::current_thread(), kernel::in_handler() ? " (handler)" : "");
    if (ch == 5 || ch == 6 || ch == 7) {
        finish(ch);
        return;
    }
    if (ch == 3 || ch == 4) {
        if (ch == 3 && !c.qwc) finish(ch);
        else ipu::dma_started(ch);
        return;
    }
    const std::uint32_t mode = (c.chcr >> 2) & 3u;
    if (mode == 1 && ch == 8) run_from_spr_chain();
    else if (mode == 1) run_chain(ch);
    else run_normal(ch);
}

}

void resume(int ch) {
    Channel &c = g_ch[ch];
    if (!c.paused) return;
    c.paused = false;
    if (c.pause_end || ((c.chcr >> 2) & 3u) != 1) {
        c.qwc = 0;
        finish(ch);
        return;
    }
    run_chain(ch);
}

bool ipu_pull(std::uint8_t *qword) {
    Channel &c = g_ch[4];
    if (!(c.chcr & 0x100u)) return false;
    const bool chain = ((c.chcr >> 2) & 3u) == 1;
    while (!c.qwc) {
        if (!chain || c.tag_end) {
            finish(4);
            return false;
        }
        std::uint64_t tag;
        std::memcpy(&tag, source(c.tadr), 8);
        c.chcr = (c.chcr & 0xFFFFu) | (static_cast<std::uint32_t>(tag >> 16) << 16);
        const std::uint32_t qwc = static_cast<std::uint32_t>(tag) & 0xFFFFu;
        const std::uint32_t id = static_cast<std::uint32_t>(tag >> 28) & 7u;
        const std::uint32_t addr = (static_cast<std::uint32_t>(tag >> 32) & 0x7FFFFFF0u) | (static_cast<std::uint32_t>(tag >> 63) << 31);
        const bool irq = (tag >> 31) & 1u, tie = c.chcr & 0x80u;
        std::uint32_t data = c.tadr + 16;
        switch (id) {
        case 0: data = addr; c.tadr += 16; c.tag_end = true; break;
        case 1: c.tadr = data + qwc * 16; break;
        case 2: c.tadr = addr; break;
        case 3: case 4: data = addr; c.tadr += 16; break;
        default: c.tag_end = true; break;
        }
        if (tie && irq) c.tag_end = true;
        static const bool trace = std::getenv("PS2_TRACE_DMA") != nullptr;
        if (trace) std::fprintf(stderr, "[dma] ch4 tag id %u qwc %u addr 0x%08X data 0x%08X next 0x%08X end %d\n", id, qwc, addr, data, c.tadr, c.tag_end);
        c.madr = data;
        c.qwc = qwc;
        if (!qwc && c.tag_end) {
            finish(4);
            return false;
        }
    }
    std::memcpy(qword, source(c.madr), 16);
    c.madr += 16;
    if (!--c.qwc && (!chain || c.tag_end)) finish(4);
    return true;
}

bool ipu_push(const std::uint8_t *qword) {
    Channel &c = g_ch[3];
    if (!(c.chcr & 0x100u) || !c.qwc) return false;
    std::memcpy(target(c.madr), qword, 16);
    watch_span(c.madr, 16, "dma:ipu_push");
    c.madr += 16;
    if (!--c.qwc) finish(3);
    return true;
}

void reset() {
    for (Channel &c : g_ch) c = Channel{};
    g_ctrl = g_pcr = g_sqwc = g_rbsr = g_rbor = g_stadr = g_enable = 0;
    g_mfifo_waiting = false;
}

void stats() {
    std::fprintf(stderr, "[dma] starts:");
    for (int ch = 0; ch < 10; ++ch) std::fprintf(stderr, " ch%d=%llu", ch, static_cast<unsigned long long>(g_starts[ch]));
    std::fprintf(stderr, "\n");
}

// CPCOND0, the condition COP0 branches (bc0t, bc0f) test: true while every channel selected in
// D_PCR.CPC has its D_STAT.CIS bit set (so it is true for an empty selection).
bool cpcond0() {
    const std::uint32_t cpc = g_pcr & 0x3FFu;
    return (hw::read32(0x1000E010u) & cpc) == cpc;
}

bool owns(std::uint32_t a) { return (a >= 0x10008000u && a < 0x1000E000u) || (a >= 0x1000E000u && a < 0x1000E070u); }

std::uint32_t read(std::uint32_t a) {
    switch (a) {
    case 0x1000E000u: return g_ctrl;
    case 0x1000E020u: return g_pcr;
    case 0x1000E030u: return g_sqwc;
    case 0x1000E040u: return g_rbsr;
    case 0x1000E050u: return g_rbor;
    case 0x1000E060u: return g_stadr;
    default: break;
    }
    for (int ch = 0; ch < 10; ++ch) {
        if (a < kBase[ch] || a >= kBase[ch] + 0x100u) continue;
        const Channel &c = g_ch[ch];
        static const bool trace = std::getenv("PS2_TRACE_DMA") != nullptr;
        if (trace && ch == 4)
            std::fprintf(stderr, "[dma] ch4 read 0x%02X chcr 0x%08X madr 0x%08X qwc %u tadr 0x%08X\n", a - kBase[ch], c.chcr, c.madr, c.qwc, c.tadr);
        switch (a - kBase[ch]) {
        case 0x00: return c.chcr;
        case 0x10: return c.madr;
        case 0x20: return c.qwc;
        case 0x30: return c.tadr;
        case 0x40: return c.asr[0];
        case 0x50: return c.asr[1];
        case 0x80: return c.sadr;
        default: return 0;
        }
    }
    return 0;
}

void write(std::uint32_t a, std::uint32_t v) {
    switch (a) {
    case 0x1000E000u: g_ctrl = v; return;
    case 0x1000E020u: g_pcr = v; return;
    case 0x1000E030u: g_sqwc = v; return;
    case 0x1000E040u: g_rbsr = v; return;
    case 0x1000E050u: g_rbor = v; return;
    case 0x1000E060u: g_stadr = v; return;
    default: break;
    }
    for (int ch = 0; ch < 10; ++ch) {
        if (a < kBase[ch] || a >= kBase[ch] + 0x100u) continue;
        Channel &c = g_ch[ch];
        switch (a - kBase[ch]) {
        case 0x00: {
            const bool was = c.chcr & 0x100u;
            c.chcr = v;
            if (!(v & 0x100u)) {
                c.paused = false;
                if (ch == 1) g_mfifo_waiting = false;
            }
            if (!was && (v & 0x100u)) start(ch);
            break;
        }
        // The scratchpad channels have no SPR bit in MADR: it does not read back and the transfer uses main memory.
        case 0x10: c.madr = ch == 8 || ch == 9 ? v & 0x7FFFFFFFu : v; break;
        case 0x20: c.qwc = v & 0xFFFFu; break;
        case 0x30: c.tadr = v; break;
        case 0x40: c.asr[0] = v; break;
        case 0x50: c.asr[1] = v; break;
        case 0x80: c.sadr = v & (kScratchpadSize - 16); break;
        default: break;
        }
        return;
    }
}

}

namespace ps2::dmac {

void state(ps2::state::Stream &s) {
    s.tag("dmac");
    s.pod(g_ch);
    s.pod(g_ctrl);
    s.pod(g_pcr);
    s.pod(g_sqwc);
    s.pod(g_rbsr);
    s.pod(g_rbor);
    s.pod(g_stadr);
    s.pod(g_enable);
    if (s.loading()) {
        const Channel &v = g_ch[1];
        g_mfifo_waiting = mfifo_vif1() && (v.chcr & 0x100u) && !v.paused && !v.qwc && ring(v.tadr) == ring(g_ch[8].madr);
    }
}

}
