#include "spu2.hpp"

#include "hw.hpp"
#include "iop.hpp"
#include "kernel.hpp"
#include "../load_stats.hpp"
#include "ps2/os.hpp"

#include "ps2/state.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <vector>

namespace ps2::iop {
// libs.cpp: a guest address that reaches past the end of memory is said there (rate-limited).
void bad_span(const char *what, std::uint32_t address, std::uint64_t size, const char *outcome);
}

namespace ps2::iop::spu2 {

namespace {

constexpr std::uint32_t kRamWords = 1u << 20;
constexpr std::uint32_t kRamMask = kRamWords - 1;
constexpr std::uint64_t kCyclesPerSample = 768;
constexpr unsigned kBatch = 32;
constexpr unsigned kInputFrames = 512;
constexpr std::uint32_t kInputBase[2] = {0x2000u, 0x2400u};
constexpr unsigned kSpuLine = 9;
constexpr std::uint32_t kRingWords = 0x200;
static_assert(kRingWords % kBatch == 0, "a batch must not straddle the ring end");

enum Phase { kOff, kAttack, kDecay, kSustain, kRelease };

struct Envelope {
    int counter = 0;
    std::int32_t level = 0;

    void step(int rate, bool decrease, bool exponential) {
        const int shift = rate >> 2;
        int cycles = 1 << std::max(0, shift - 11);
        if (exponential && !decrease && level > 0x6000) cycles *= 4;
        if (++counter < cycles) return;
        counter = 0;
        int delta = (decrease ? -8 + (rate & 3) : 7 - (rate & 3)) << std::max(0, 11 - shift);
        if (exponential && decrease) delta = delta * level / 0x8000;
        level = std::clamp(level + delta, 0, 0x7FFF);
    }
};

struct Voice {
    std::uint16_t voll = 0, volr = 0, pitch = 0, adsr1 = 0, adsr2 = 0;
    Envelope vol[2];
    std::uint32_t ssa = 0, lsax = 0, next = 0, block[2] = {};
    bool lsax_written = false, key_on = false, end = false;
    Phase phase = kOff;
    Envelope env;
    std::int16_t buffer[56] = {};
    std::int32_t s1 = 0, s2 = 0;
    std::uint32_t position = 0;
};

struct Transfer {
    std::uint32_t madr = 0, bytes = 0;
    int channel = -1;
};

struct Core {
    Voice voices[24];
    std::uint16_t attr = 0, admas = 0;
    std::uint32_t irqa = 0x800u, tsa = 0;
    std::uint16_t mvol[2] = {0x3FFF, 0x3FFF}, bvol[2] = {0x7FFF, 0x7FFF}, avol[2] = {0x7FFF, 0x7FFF}, evol[2] = {};
    bool irq = false;
    std::int16_t input[kInputFrames][2] = {};
    unsigned input_read = 0, input_count = 0;
    std::deque<Transfer> transfers;
    std::uint16_t regs[0x200] = {};
};

std::vector<std::uint16_t> g_ram(kRamWords);
Core g_cores[2];
bool g_block_end[2][24][2];

bool *block_end(const Voice &v) {
    const int c = &v >= g_cores[1].voices ? 1 : 0;
    return g_block_end[c][&v - g_cores[c].voices];
}
std::uint16_t g_spdif_out = 0, g_spdif_mode = 0, g_spdif_media = 0;
std::function<void(const std::int16_t *, std::size_t)> g_sink;
std::uint64_t g_next_tick = 0;
bool g_reverb_off = false;  // PS2_SPU2_REVERB=0 leaves the reverb out (to compare with the sound from before it existed)
bool g_tick_pending = false;
std::uint64_t g_first_tick = 0, g_last_tick = 0;

// A 48000 Hz 16-bit stereo WAV file written while the sound is made, so a long run needs no memory for it. The header is
// brought up to date about once a second, so a run that is killed still leaves a file that plays up to that point.
class WavFile {
public:
    ~WavFile() { close(); }
    bool open(const std::string &path) {
        if (file_) return false;
        file_ = os::open(path, "wb");
        if (!file_) {
            std::fprintf(stderr, "[spu2] cannot write %s\n", path.c_str());
            return false;
        }
        path_ = path;
        frames_ = synced_ = 0;
        header();
        return true;
    }
    explicit operator bool() const { return file_ != nullptr; }
    void write(const std::int16_t *samples, std::size_t frames) {
        if (!file_ || frames_ + frames > kMaxFrames) return;
        std::fwrite(samples, 4, frames, file_);
        frames_ += frames;
        if (frames_ - synced_ >= 48000) header();
    }
    // closes the file and says how much went in: the chip makes 48000 samples per second of emulated IOP time from its first
    // sample on, which the second figure checks
    void close() {
        if (!file_) return;
        header();
        std::fclose(file_);
        file_ = nullptr;
        const double emulated = g_first_tick ? static_cast<double>(g_last_tick - g_first_tick) / 36864000.0 + kBatch / 48000.0 : 0.0;
        std::fprintf(stderr, "[spu2] wrote %llu samples (%.2f s) to %s; the chip ran %.2f s of emulated time\n",
                     static_cast<unsigned long long>(frames_), static_cast<double>(frames_) / 48000.0, path_.c_str(), emulated);
    }

private:
    static constexpr std::uint64_t kMaxFrames = (0xFFFFFFFFull - 36) / 4;
    void header() {
        const std::uint32_t bytes = static_cast<std::uint32_t>(frames_ * 4);
        const std::uint32_t words[] = {0x46464952u, 36 + bytes, 0x45564157u, 0x20746D66u, 16,         0x00020001u,
                                       48000,       48000 * 4,  0x00100004u, 0x61746164u, bytes};
        std::fseek(file_, 0, SEEK_SET);
        std::fwrite(words, 4, sizeof words / 4, file_);
        std::fseek(file_, 0, SEEK_END);
        std::fflush(file_);
        synced_ = frames_;
    }
    std::FILE *file_ = nullptr;
    std::string path_;
    std::uint64_t frames_ = 0, synced_ = 0;
};

// PS2_DUMP_AUDIO: everything the sound chip puts out, before the host's sound queue. PS2_AUDIO_DUMP is the sound that goes to
// the sound device: with a device (a window run) the host writes that file itself (host/window.cpp), without one there is no
// queue in between and it is the same chip output, written here.
WavFile g_chip_wav, g_out_wav;

void check_irq(std::uint32_t address, std::uint32_t words) {
    for (Core &k : g_cores) {
        if (!(k.attr & 0x40u) || k.irq) continue;
        if (((k.irqa - address) & kRamMask) >= words) continue;
        k.irq = true;
        static const bool trace = std::getenv("PS2_TRACE_SPU2") != nullptr;
        if (trace) std::fprintf(stderr, "[spu2] core %d irq at 0x%05X (access 0x%05X+%u) time %llu\n", static_cast<int>(&k - g_cores), k.irqa, address, words,
                                static_cast<unsigned long long>(kernel::now()));
        hw::raise_interrupt(kSpuLine);
    }
}

void decode_block(Voice &v, std::int16_t *out) {
    static const int kFilter[5][2] = {{0, 0}, {60, 0}, {115, -52}, {98, -55}, {122, -60}};
    const std::uint32_t at = v.next & kRamMask;
    const std::uint16_t header = g_ram[at];
    const int shift = header & 0xF, filter = std::min((header >> 4) & 0xF, 4);
    const unsigned flags = header >> 8;
    for (int i = 0; i < 28; ++i) {
        const std::uint16_t word = g_ram[(at + 1 + i / 4) & kRamMask];
        std::int32_t sample = static_cast<std::int16_t>(((word >> ((i & 3) * 4)) & 0xF) << 12) >> shift;
        sample += (v.s1 * kFilter[filter][0] + v.s2 * kFilter[filter][1] + 32) >> 6;
        sample = std::clamp(sample, -32768, 32767);
        v.s2 = v.s1;
        v.s1 = sample;
        out[i] = static_cast<std::int16_t>(sample);
    }
    v.block[out == v.buffer ? 0 : 1] = at;
    block_end(v)[out == v.buffer ? 0 : 1] = (flags & 1u) != 0;
    if ((flags & 4u) && !v.lsax_written) v.lsax = at;
    v.next = (at + 8) & kRamMask;
    if (flags & 1u) {
        v.next = v.lsax;
        if (!(flags & 2u)) {
            v.phase = kRelease;
            v.env.level = 0;
        }
    }
}

void key_on(Voice &v) {
    v.key_on = false;
    v.lsax_written = false;
    v.end = false;
    v.next = v.ssa;
    v.s1 = v.s2 = 0;
    v.position = 0;
    v.phase = kAttack;
    v.env = Envelope{};
    decode_block(v, v.buffer);
    decode_block(v, v.buffer + 28);
    check_irq(v.block[0], 8);
}

void update_envelope(Voice &v) {
    Envelope &e = v.env;
    switch (v.phase) {
    case kAttack:
        e.step((v.adsr1 >> 8) & 0x7F, false, v.adsr1 & 0x8000u);
        if (e.level >= 0x7FFF) v.phase = kDecay;
        break;
    case kDecay:
        e.step(((v.adsr1 >> 4) & 0xF) << 2, true, true);
        if (e.level <= ((v.adsr1 & 0xF) + 1) * 0x800) v.phase = kSustain;
        break;
    case kSustain:
        e.step((v.adsr2 >> 6) & 0x7F, v.adsr2 & 0x4000u, v.adsr2 & 0x8000u);
        break;
    case kRelease:
        e.step((v.adsr2 & 0x1F) << 2, true, v.adsr2 & 0x20u);
        if (e.level <= 0) v.phase = kOff;
        break;
    default:
        break;
    }
}

std::int32_t volume(std::uint16_t reg, Envelope &e) {
    if (!(reg & 0x8000u)) return static_cast<std::int16_t>(reg << 1);
    e.step(reg & 0x7F, reg & 0x2000u, reg & 0x4000u);
    return e.level;
}

std::int16_t clamp16(std::int32_t v) { return static_cast<std::int16_t>(std::clamp(v, -32768, 32767)); }

// Where one core sends what it makes (registers VMIXL/VMIXR = dry left/right, VMIXEL/VMIXER = into the reverb, one bit per voice, and
// MMIX for the mixer as a whole: bits 9 and 8 let the voices into the reverb left and right, bits 5 and 4 do the same for the sound
// that comes in by DMA (ADMA)). The reverb is only on while bit 7 of the core's attribute register is set.
struct Routing {
    std::uint32_t dry[2] = {}, wet[2] = {};
    bool input_wet[2] = {};
    bool reverb = false;
};

Routing routing(const Core &k) {
    const auto pair = [&k](std::uint32_t off) { return static_cast<std::uint32_t>(k.regs[off >> 1]) | static_cast<std::uint32_t>(k.regs[(off + 2) >> 1] & 0xFFu) << 16; };
    const std::uint16_t mmix = k.regs[0x198 >> 1];
    Routing r;
    r.reverb = (k.attr & 0x80u) != 0 && !g_reverb_off;
    r.dry[0] = pair(0x188);
    r.dry[1] = pair(0x190);
    if (r.reverb) {
        r.wet[0] = (mmix & 0x200u) ? pair(0x18C) : 0;
        r.wet[1] = (mmix & 0x100u) ? pair(0x194) : 0;
        r.input_wet[0] = (mmix & 0x20u) != 0;
        r.input_wet[1] = (mmix & 0x10u) != 0;
    }
    return r;
}

void render_voice(Voice &v, unsigned voice, const Routing &route, std::int32_t (*dry)[2], std::int32_t (*wet)[2], std::int16_t *raw) {
    if (v.key_on) key_on(v);
    const std::uint32_t bit = 1u << voice;
    const std::uint32_t step = std::min<std::uint32_t>(v.pitch, 0x3FFFu);
    const bool sweep_l = v.voll & 0x8000u, sweep_r = v.volr & 0x8000u;
    const std::int32_t fixed_l = static_cast<std::int16_t>(v.voll << 1), fixed_r = static_cast<std::int16_t>(v.volr << 1);
    std::uint32_t position = v.position;
    for (unsigned n = 0; n < kBatch; ++n) {
        if (v.phase == kOff) break;
        const std::uint32_t index = position >> 12, frac = position & 0xFFFu;
        const std::int32_t sample = (v.buffer[index] * static_cast<std::int32_t>(0x1000 - frac) + v.buffer[index + 1] * static_cast<std::int32_t>(frac)) >> 12;
        position += step;
        while ((position >> 12) >= 28) {
            position -= 28u << 12;
            std::memmove(v.buffer, v.buffer + 28, 28 * sizeof(std::int16_t));
            v.end |= block_end(v)[0];
            v.block[0] = v.block[1];
            block_end(v)[0] = block_end(v)[1];
            v.position = position;
            check_irq(v.block[0], 8);
            decode_block(v, v.buffer + 28);
        }
        update_envelope(v);
        const std::int32_t out = sample * v.env.level >> 15;
        if (raw) raw[n] = clamp16(out);
        // the volume sweeps step once per sample whatever the voice is sent to
        const std::int32_t l = out * (sweep_l ? volume(v.voll, v.vol[0]) : fixed_l) >> 15;
        const std::int32_t r = out * (sweep_r ? volume(v.volr, v.vol[1]) : fixed_r) >> 15;
        if (route.dry[0] & bit) dry[n][0] += l;
        if (route.dry[1] & bit) dry[n][1] += r;
        if (route.wet[0] & bit) wet[n][0] += l;
        if (route.wet[1] & bit) wet[n][1] += r;
    }
    v.position = position;
}

void feed_input(int core) {
    Core &k = g_cores[core];
    while (!k.transfers.empty() && k.input_count + 256 <= kInputFrames) {
        Transfer &t = k.transfers.front();
        const std::uint32_t chunk = std::min<std::uint32_t>(t.bytes, 1024);
        const std::uint32_t frames = std::max<std::uint32_t>(chunk / 4, 1);
        const std::uint8_t *src = ram_ptr(t.madr);
        // A block that runs over the end of the IOP's memory (only a bad address does that): read round to the start, as the
        // mirrored memory would, instead of up to 1 KiB past the array (its padding is 16 bytes).
        std::uint8_t wrapped[1024];
        if ((phys(t.madr) & (iop::kRamSize - 1)) + chunk > iop::kRamSize) {
            bad_span("ADMA sound input", t.madr, chunk, "read round to the start");
            for (std::uint32_t i = 0; i < chunk; ++i) wrapped[i] = *ram_ptr(t.madr + i);
            src = wrapped;
        }
        static const char *const trace_pieces = std::getenv("PS2_TRACE_ADMA");  // "2": every piece the chip takes in, with the time (see dma())
        if (trace_pieces && trace_pieces[0] == '2')
            std::fprintf(stderr, "[adma-piece] %.5f s core %d reads 0x%06X (%u bytes), %u bytes of the block left, %u frames waiting\n",
                         static_cast<double>(kernel::now()) / 36864000.0, core, t.madr, chunk, t.bytes, k.input_count);
        // PS2_DUMP_ADMA=file (porting work): the sound exactly as the chip takes it in, piece by piece, at the moment it takes it
        static const char *const dump_adma = std::getenv("PS2_DUMP_ADMA");
        if (dump_adma) {
            if (std::FILE *f = std::fopen(dump_adma, "ab")) {
                std::fwrite(src, 1, chunk, f);
                std::fclose(f);
            }
        }
        for (std::uint32_t i = 0; i < frames; ++i) {
            std::int16_t l, r;
            std::memcpy(&l, src + i * 2, 2);
            std::memcpy(&r, src + chunk / 2 + i * 2, 2);
            const unsigned slot = (k.input_read + k.input_count + i) % kInputFrames;
            k.input[slot][0] = l;
            k.input[slot][1] = r;
            g_ram[kInputBase[core] + slot] = static_cast<std::uint16_t>(l);
            g_ram[kInputBase[core] + 0x200 + slot] = static_cast<std::uint16_t>(r);
        }
        k.input_count += frames;
        t.madr += chunk;
        t.bytes -= chunk;
        if (!t.bytes) {
            const int channel = t.channel;
            k.transfers.pop_front();
            if (channel >= 0) kernel::schedule(kernel::now() + 32, kernel::Job{kernel::kJobDmaDone, static_cast<std::uint32_t>(channel)});
        }
    }
}

void render_input(int core, const Routing &route, std::int32_t *dry, std::int32_t *wet, std::int16_t &raw_l, std::int16_t &raw_r) {
    Core &k = g_cores[core];
    if (!k.input_count) return;
    const unsigned slot = k.input_read;
    check_irq(kInputBase[core] + slot, 1);
    raw_l = k.input[slot][0];
    raw_r = k.input[slot][1];
    const std::int32_t l = k.input[slot][0] * static_cast<std::int16_t>(k.bvol[0]) >> 15, r = k.input[slot][1] * static_cast<std::int16_t>(k.bvol[1]) >> 15;
    dry[0] += l;
    dry[1] += r;
    if (route.input_wet[0]) wet[0] += l;
    if (route.input_wet[1]) wet[1] += r;
    k.input_read = (k.input_read + 1) % kInputFrames;
    --k.input_count;
}

std::int32_t master(std::uint16_t reg) { return reg & 0x8000u ? 0x7FFF : static_cast<std::int16_t>(reg << 1); }

// The reverb. The chip's reverb is the one of the first PlayStation's sound chip, written about in the public PlayStation hardware
// documentation (nocash's psx-spx, "SPU Reverb"): a short ring of memory (from ESA to the end given by EEA), two same-side and two
// cross-side reflections with a damping filter, four early echoes (combs) and two all-pass filters, once for the left and once for the
// right side. This version runs one step for every two output samples (24000 steps a second; the register values of the game are
// those of the first chip, counted in 16-bit words) and reads the sum of everything sent to it (the wet side of the mixer). Where
// each register sits (the 22 addresses from 0x2E4, the 10 volumes in the master block from 0x10) is read from what libsd writes:
// the values of its "Studio Large" preset (dAPF1 0x38C = 0xE3 * 4, volumes 6F60 4FA8 BCE0 4510 BEF0 A680 5680 52C0 8000 8000, which sit
// in the master block at 0x14 to 0x26 of each core; 0x10 and 0x12 are two more words nobody here uses).
// The master block from 0x10 on is kept in the unused end of the core's register array (0x3E0 on), so a saved state needs nothing new
// (a state saved before this existed has no volumes in it: its reverb stays silent until the game sets the reverb again).
enum { kRevAddress = 0x2E4, kRevVolume = 0x3E4 };
enum { kApf1Size, kApf2Size, kSameLDst, kSameRDst, kComb1L, kComb1R, kComb2L, kComb2R, kSameLSrc, kSameRSrc, kDiffLDst, kDiffRDst, kComb3L, kComb3R,
       kComb4L, kComb4R, kDiffLSrc, kDiffRSrc, kApf1LDst, kApf1RDst, kApf2LDst, kApf2RDst };
enum { kVolIir, kVolComb1, kVolComb2, kVolComb3, kVolComb4, kVolWall, kVolApf1, kVolApf2, kVolInL, kVolInR };

std::int32_t mul15(std::int32_t sample, std::uint16_t volume) { return sample * static_cast<std::int16_t>(volume) >> 15; }

// The 'out' of 32 samples for the 32 samples of the wet side 'in'. step_base counts the reverb steps from the start of the run.
void run_reverb(Core &k, std::uint64_t step_base, const std::int32_t (*in)[2], std::int32_t (*out)[2], std::int32_t (&last)[2]) {
    const auto reg32 = [&k](std::uint32_t off) { return static_cast<std::uint32_t>(k.regs[off >> 1]) << 16 | k.regs[(off + 2) >> 1]; };
    const std::uint32_t start = reg32(0x2E0) & kRamMask, end = ((static_cast<std::uint32_t>(k.regs[0x33C >> 1]) << 16) | 0xFFFFu) & kRamMask;
    const std::uint64_t size = end >= start ? end - start + 1u : 0u;
    if (size < 64) {
        for (unsigned n = 0; n < kBatch; ++n) out[n][0] = out[n][1] = 0;
        return;
    }
    std::uint32_t address[22];
    for (unsigned i = 0; i < 22; ++i) address[i] = reg32(kRevAddress + i * 4);
    const std::uint16_t *vol = &k.regs[kRevVolume >> 1];
    for (unsigned t = 0; t < kBatch / 2; ++t) {
        const std::uint64_t at = (step_base + t) % size;
        // a cell of the ring, 'offset' words away from where this step is (a step further on, the same cell is one nearer to the writer)
        const auto cell = [&](std::uint32_t offset, int adjust = 0) -> std::uint16_t & { return g_ram[start + (at + offset + size + adjust) % size]; };
        const std::int32_t in_l = mul15((in[t * 2][0] + in[t * 2 + 1][0]) / 2, vol[kVolInL]), in_r = mul15((in[t * 2][1] + in[t * 2 + 1][1]) / 2, vol[kVolInR]);
        const auto reflect = [&](std::int32_t input, std::uint32_t source, std::uint32_t target) {
            const std::int32_t before = static_cast<std::int16_t>(cell(target, -1));
            const std::int32_t sum = input + mul15(static_cast<std::int16_t>(cell(source)), vol[kVolWall]) - before;
            cell(target) = static_cast<std::uint16_t>(clamp16(mul15(sum, vol[kVolIir]) + before));
        };
        reflect(in_l, address[kSameLSrc], address[kSameLDst]);
        reflect(in_r, address[kSameRSrc], address[kSameRDst]);
        reflect(in_l, address[kDiffRSrc], address[kDiffLDst]);
        reflect(in_r, address[kDiffLSrc], address[kDiffRDst]);
        const auto side = [&](unsigned c1, unsigned c2, unsigned c3, unsigned c4, unsigned apf1, unsigned apf2) {
            std::int32_t o = mul15(static_cast<std::int16_t>(cell(address[c1])), vol[kVolComb1]) + mul15(static_cast<std::int16_t>(cell(address[c2])), vol[kVolComb2]) +
                             mul15(static_cast<std::int16_t>(cell(address[c3])), vol[kVolComb3]) + mul15(static_cast<std::int16_t>(cell(address[c4])), vol[kVolComb4]);
            o = clamp16(o);
            const std::uint32_t back1 = address[kApf1Size], back2 = address[kApf2Size];
            const std::int32_t d1 = static_cast<std::int16_t>(cell(address[apf1] + size - back1 % size));
            o = clamp16(o - mul15(d1, vol[kVolApf1]));
            cell(address[apf1]) = static_cast<std::uint16_t>(o);
            o = clamp16(mul15(o, vol[kVolApf1]) + d1);
            const std::int32_t d2 = static_cast<std::int16_t>(cell(address[apf2] + size - back2 % size));
            o = clamp16(o - mul15(d2, vol[kVolApf2]));
            cell(address[apf2]) = static_cast<std::uint16_t>(o);
            return clamp16(mul15(o, vol[kVolApf2]) + d2);
        };
        const std::int32_t left = side(kComb1L, kComb2L, kComb3L, kComb4L, kApf1LDst, kApf2LDst);
        const std::int32_t right = side(kComb1R, kComb2R, kComb3R, kComb4R, kApf1RDst, kApf2RDst);
        // two output samples for one step: halfway from the last step's result, then the new one
        out[t * 2][0] = (last[0] + left) / 2;
        out[t * 2][1] = (last[1] + right) / 2;
        out[t * 2 + 1][0] = left;
        out[t * 2 + 1][1] = right;
        last[0] = left;
        last[1] = right;
    }
}

// Each core streams every sample into fixed rings in SPU RAM, per core c at c*0x800: input L/R (core 1's input is core 0's
// output), voice 1, voice 3; at 0x1000+c*0x800: dry L/R, wet L/R. Those writes trip IRQA, and some games (Burnout 3:
// IRQA 0x400) use that as their audio clock. One range check per ring per batch keeps it cheap.
void write_rings(int core, const std::int16_t (*rings)[kBatch]) {
    const auto pos = static_cast<std::uint32_t>(g_next_tick / (kBatch * kCyclesPerSample) % (kRingWords / kBatch) * kBatch);
    for (std::uint32_t r = 0; r < 8; ++r) {
        const std::uint32_t at = (r < 4 ? 0u : 0x1000u) + core * 0x800u + (r & 3u) * kRingWords + pos;
        for (unsigned n = 0; n < kBatch; ++n) g_ram[at + n] = static_cast<std::uint16_t>(rings[r][n]);
        check_irq(at, kBatch);
    }
}

void tick() {
    const cost::Timer time_it(cost::kSpuTick);
    std::int16_t out[kBatch * 2];
    std::int32_t sum[2][kBatch][2] = {}, wet[2][kBatch][2] = {}, echo[2][kBatch][2] = {};
    std::int16_t rings[2][8][kBatch] = {};
    static std::int32_t last_echo[2][2] = {};
    static const bool count = std::getenv("PS2_SPU2_COUNT") != nullptr;
    static std::uint64_t ticks = 0, active = 0, phases[5] = {};
    for (int c = 0; c < 2; ++c) {
        const Routing route = routing(g_cores[c]);
        for (int i = 0; i < 24; ++i) {
            Voice &v = g_cores[c].voices[i];
            if (!v.key_on && v.phase == kOff) continue;
            if (count) {
                ++active;
                ++phases[v.phase];
            }
            render_voice(v, static_cast<unsigned>(i), route, sum[c], wet[c], i == 1 ? rings[c][2] : i == 3 ? rings[c][3] : nullptr);
        }
        for (unsigned n = 0; n < kBatch; ++n) {
            rings[c][4][n] = clamp16(sum[c][n][0]);
            rings[c][5][n] = clamp16(sum[c][n][1]);
        }
        std::int16_t unused[2];
        for (unsigned n = 0; n < kBatch; ++n)
            render_input(c, route, sum[c][n], wet[c][n], c ? unused[0] : rings[0][0][n], c ? unused[1] : rings[0][1][n]);
        if (route.reverb) {
            run_reverb(g_cores[c], g_next_tick / (2 * kCyclesPerSample), wet[c], echo[c], last_echo[c]);
            for (unsigned n = 0; n < kBatch; ++n) {
                rings[c][6][n] = clamp16(wet[c][n][0]);
                rings[c][7][n] = clamp16(wet[c][n][1]);
                sum[c][n][0] += echo[c][n][0] * master(g_cores[c].evol[0]) >> 15;
                sum[c][n][1] += echo[c][n][1] * master(g_cores[c].evol[1]) >> 15;
            }
        }
    }
    for (unsigned n = 0; n < kBatch; ++n) {
        std::int32_t mix[2] = {};
        for (int c = 0; c < 2; ++c) {
            const std::int32_t l = sum[c][n][0] * master(g_cores[c].mvol[0]) >> 15, r = sum[c][n][1] * master(g_cores[c].mvol[1]) >> 15;
            if (!c) {
                rings[1][0][n] = clamp16(l);
                rings[1][1][n] = clamp16(r);
            }
            mix[0] += l;
            mix[1] += r;
        }
        out[n * 2] = clamp16(mix[0]);
        out[n * 2 + 1] = clamp16(mix[1]);
    }
    write_rings(0, rings[0]);
    write_rings(1, rings[1]);
    if (count && ++ticks % 15000 == 0)
        std::fprintf(stderr, "[spu2] %.1f voices per batch (attack %.1f decay %.1f sustain %.1f release %.1f)\n", static_cast<double>(active) / ticks,
                     static_cast<double>(phases[1]) / ticks, static_cast<double>(phases[2]) / ticks, static_cast<double>(phases[3]) / ticks, static_cast<double>(phases[4]) / ticks);
    if (!g_first_tick) g_first_tick = kernel::now();
    g_last_tick = kernel::now();
    g_chip_wav.write(out, kBatch);
    g_out_wav.write(out, kBatch);
    if (g_sink) g_sink(out, kBatch);
    feed_input(0);
    feed_input(1);
    g_next_tick += kBatch * kCyclesPerSample;
    kernel::schedule(g_next_tick, kernel::Job{kernel::kJobSpuTick});
}

void keep_ticking() {
    if (g_tick_pending) return;
    g_tick_pending = true;
    g_next_tick = kernel::now() + kBatch * kCyclesPerSample;
    kernel::schedule(g_next_tick, kernel::Job{kernel::kJobSpuTick});
}

std::uint32_t set_hi(std::uint32_t address, std::uint32_t v) { return ((v & 0xFu) << 16) | (address & 0xFFFFu); }
std::uint32_t set_lo(std::uint32_t address, std::uint32_t v) { return (address & 0xF0000u) | (v & 0xFFFFu); }

std::uint32_t read_voice(Core &k, std::uint32_t off) {
    if (off < 0x180u) {
        const Voice &v = k.voices[off >> 4];
        switch (off & 0xFu) {
        case 0x0: return v.voll;
        case 0x2: return v.volr;
        case 0x4: return v.pitch;
        case 0x6: return v.adsr1;
        case 0x8: return v.adsr2;
        case 0xA: return static_cast<std::uint32_t>(v.env.level);
        case 0xC: return static_cast<std::uint32_t>(v.vol[0].level);
        default: return static_cast<std::uint32_t>(v.vol[1].level);
        }
    }
    const Voice &v = k.voices[(off - 0x1C0u) / 12];
    const std::uint32_t nax = (v.block[0] + 1 + (v.position >> 14)) & kRamMask;
    switch ((off - 0x1C0u) % 12) {
    case 0: return v.ssa >> 16;
    case 2: return v.ssa & 0xFFFFu;
    case 4: return v.lsax >> 16;
    case 6: return v.lsax & 0xFFFFu;
    case 8: return nax >> 16;
    default: return nax & 0xFFFFu;
    }
}

// PS2_TRACE_SPU2 also reports the first few voice volumes written in sweep mode (bit 15), which fades a voice instead of setting it
bool trace_sweep(std::uint32_t value) {
    static const bool trace = std::getenv("PS2_TRACE_SPU2") != nullptr;
    static int reported = 0;
    return trace && (value & 0x8000u) && reported++ < 40;
}

void write_voice(Core &k, std::uint32_t off, std::uint32_t value) {
    if (off < 0x180u) {
        Voice &v = k.voices[off >> 4];
        switch (off & 0xFu) {
        case 0x0:
            if (trace_sweep(value)) std::fprintf(stderr, "[spu2] voice volume sweep L 0x%04X (voice %u)\n", value, off >> 4);
            v.voll = static_cast<std::uint16_t>(value);
            if (!(value & 0x8000u)) v.vol[0].level = static_cast<std::int16_t>(value << 1);
            break;
        case 0x2:
            if (trace_sweep(value)) std::fprintf(stderr, "[spu2] voice volume sweep R 0x%04X (voice %u)\n", value, off >> 4);
            v.volr = static_cast<std::uint16_t>(value);
            if (!(value & 0x8000u)) v.vol[1].level = static_cast<std::int16_t>(value << 1);
            break;
        case 0x4: v.pitch = static_cast<std::uint16_t>(value); break;
        case 0x6: v.adsr1 = static_cast<std::uint16_t>(value); break;
        case 0x8: v.adsr2 = static_cast<std::uint16_t>(value); break;
        case 0xA: v.env.level = static_cast<std::int16_t>(value); break;
        default: break;
        }
        return;
    }
    Voice &v = k.voices[(off - 0x1C0u) / 12];
    switch ((off - 0x1C0u) % 12) {
    case 0: v.ssa = set_hi(v.ssa, value); break;
    case 2: v.ssa = set_lo(v.ssa, value); break;
    case 4: v.lsax = set_hi(v.lsax, value); v.lsax_written = true; break;
    case 6: v.lsax = set_lo(v.lsax, value); v.lsax_written = true; break;
    case 8: v.next = set_hi(v.next, value); break;
    default: v.next = set_lo(v.next, value); break;
    }
}

void key(Core &k, std::uint32_t mask, bool on) {
    for (int i = 0; i < 24; ++i) {
        if (!(mask & (1u << i))) continue;
        Voice &v = k.voices[i];
        if (on) v.key_on = true;
        else if (v.key_on) v.key_on = false;
        else if (v.phase != kOff) v.phase = kRelease;
    }
}

std::uint32_t read_core(int core, std::uint32_t off) {
    Core &k = g_cores[core];
    if (off < 0x180u || (off >= 0x1C0u && off < 0x2E0u)) return read_voice(k, off);
    switch (off) {
    case 0x19A: return k.attr;
    case 0x19C: return k.irqa >> 16;
    case 0x19E: return k.irqa & 0xFFFFu;
    case 0x1A8: return k.tsa >> 16;
    case 0x1AA: return k.tsa & 0xFFFFu;
    case 0x1B0: return k.admas;
    case 0x340:
    case 0x342: {
        std::uint32_t endx = 0;
        for (int i = 0; i < 24; ++i)
            if (k.voices[i].end) endx |= 1u << i;
        return off == 0x340 ? endx & 0xFFFFu : endx >> 16;
    }
    case 0x344: return 0x80u;
    default: return k.regs[off >> 1];
    }
}

void write_core(int core, std::uint32_t off, std::uint32_t value) {
    Core &k = g_cores[core];
    static const bool trace = std::getenv("PS2_TRACE_SPU2") != nullptr;
    if (trace && off >= 0x180u && !(off >= 0x1C0u && off < 0x2E0u) && off != 0x1AC)
        std::fprintf(stderr, "[spu2] %.3f s core %d reg 0x%03X = 0x%04X\n", g_first_tick ? static_cast<double>(kernel::now() - g_first_tick) / 36864000.0 : 0.0, core, off,
                     value);
    if (off < 0x180u || (off >= 0x1C0u && off < 0x2E0u)) {
        write_voice(k, off, value);
        return;
    }
    k.regs[off >> 1] = static_cast<std::uint16_t>(value);
    switch (off) {
    case 0x19A:
        k.attr = static_cast<std::uint16_t>(value);
        if (!(value & 0x40u)) k.irq = false;
        break;
    case 0x19C: k.irqa = set_hi(k.irqa, value); break;
    case 0x19E: k.irqa = set_lo(k.irqa, value); break;
    case 0x1A0: key(k, value, true); break;
    case 0x1A2: key(k, value << 16, true); break;
    case 0x1A4: key(k, value, false); break;
    case 0x1A6: key(k, value << 16, false); break;
    case 0x1A8: k.tsa = set_hi(k.tsa, value); break;
    case 0x1AA: k.tsa = set_lo(k.tsa, value); break;
    case 0x1AC:
        check_irq(k.tsa, 1);
        g_ram[k.tsa] = static_cast<std::uint16_t>(value);
        k.tsa = (k.tsa + 1) & kRamMask;
        break;
    case 0x1B0:
        k.admas = static_cast<std::uint16_t>(value);
        if (!value) k.input_count = k.input_read = 0;
        break;
    case 0x340:
        for (int i = 0; i < 16; ++i) k.voices[i].end = false;
        break;
    case 0x342:
        for (int i = 16; i < 24; ++i) k.voices[i].end = false;
        break;
    default: break;
    }
}

std::uint16_t *master_reg(int core, std::uint32_t off) {
    Core &k = g_cores[core];
    switch (off) {
    case 0x0: return &k.mvol[0];
    case 0x2: return &k.mvol[1];
    case 0x4: return &k.evol[0];
    case 0x6: return &k.evol[1];
    case 0x8: return &k.avol[0];
    case 0xA: return &k.avol[1];
    case 0xC: return &k.bvol[0];
    case 0xE: return &k.bvol[1];
    case 0x10: case 0x12: case 0x14: case 0x16: case 0x18: case 0x1A: case 0x1C: case 0x1E: case 0x20: case 0x22: case 0x24: case 0x26:
        return &k.regs[(0x3E0 >> 1) + (off - 0x10) / 2];
    default: return nullptr;
    }
}

std::uint32_t read_global(std::uint32_t off) {
    switch (off) {
    case 0x7C0: return g_spdif_out;
    case 0x7C2: return (g_cores[0].irq ? 4u : 0u) | (g_cores[1].irq ? 8u : 0u);
    case 0x7C6: return g_spdif_mode;
    case 0x7C8: return g_spdif_media;
    default: return 0;
    }
}

void write_global(std::uint32_t off, std::uint32_t value) {
    switch (off) {
    case 0x7C0: g_spdif_out = static_cast<std::uint16_t>(value); break;
    case 0x7C6: g_spdif_mode = static_cast<std::uint16_t>(value); break;
    case 0x7C8: g_spdif_media = static_cast<std::uint16_t>(value); break;
    default: break;
    }
}

}

void reset() {
    kernel::register_job(kernel::kJobSpuTick, [](const kernel::Job &) { tick(); });
    std::fill(g_ram.begin(), g_ram.end(), 0);
    for (Core &k : g_cores) k = Core{};
    std::memset(g_block_end, 0, sizeof g_block_end);
    g_spdif_out = g_spdif_mode = g_spdif_media = 0;
    g_next_tick = 0;
    g_reverb_off = os::env("PS2_SPU2_REVERB") == "0";
    g_tick_pending = false;
    g_first_tick = g_last_tick = 0;
    const std::string chip = os::env("PS2_DUMP_AUDIO"), out = os::env("PS2_AUDIO_DUMP");
    if (!chip.empty() && g_chip_wav.open(chip)) std::fprintf(stderr, "[spu2] writing the sound chip's output to %s\n", chip.c_str());
    // the sink is set before the machine resets (host/window.cpp opens the sound device first)
    if (!out.empty() && out != chip && !g_sink && g_out_wav.open(out)) std::fprintf(stderr, "[spu2] writing the sound to %s (no sound device)\n", out.c_str());
}

std::uint16_t *ram() { return g_ram.data(); }

std::uint32_t read(std::uint32_t a, unsigned size) {
    keep_ticking();
    const std::uint32_t off = a & 0x7FEu;
    std::uint32_t value = 0;
    if (off < 0x760u) value = read_core((off & 0x400u) ? 1 : 0, off & 0x3FEu);
    else if (off < 0x7B0u) {
        if (const std::uint16_t *reg = master_reg((off - 0x760u) / 0x28, (off - 0x760u) % 0x28)) value = *reg;
    } else value = read_global(off);
    if (size == 4) value |= read(a + 2, 2) << 16;
    return value;
}

void write(std::uint32_t a, std::uint32_t v, unsigned size) {
    keep_ticking();
    if (size == 4) {
        write(a, v & 0xFFFFu, 2);
        write(a + 2, v >> 16, 2);
        return;
    }
    const std::uint32_t off = a & 0x7FEu;
    v &= 0xFFFFu;
    if (off < 0x760u) write_core((off & 0x400u) ? 1 : 0, off & 0x3FEu, v);
    else if (off < 0x7B0u) {
        if (std::uint16_t *reg = master_reg((off - 0x760u) / 0x28, (off - 0x760u) % 0x28)) *reg = static_cast<std::uint16_t>(v);
        static const bool trace = std::getenv("PS2_TRACE_SPU2") != nullptr;
        if (trace)
            std::fprintf(stderr, "[spu2] %.3f s core %u master reg 0x%02X = 0x%04X\n", g_first_tick ? static_cast<double>(kernel::now() - g_first_tick) / 36864000.0 : 0.0,
                         (off - 0x760u) / 0x28, (off - 0x760u) % 0x28, v);
    } else write_global(off, v);
}

void dma(int core, std::uint32_t madr, std::uint32_t words, bool to_spu, int channel) {
    keep_ticking();
    Core &k = g_cores[core];
    if (to_spu && (k.admas & (1u << core))) {
        static const bool trace_adma = std::getenv("PS2_TRACE_ADMA") != nullptr;  // porting work: when each ADMA block starts, and the blocks
        if (trace_adma) std::fprintf(stderr, "[adma] core %d dma of %u bytes from 0x%06X at %.4f s, %u frames already waiting, %zu transfers queued\n", core, words * 4, madr,
                                                        g_first_tick ? static_cast<double>(kernel::now() - g_first_tick) / 36864000.0 : 0.0, k.input_count, k.transfers.size());
        k.transfers.push_back(Transfer{madr, words * 4, channel});
        feed_input(core);
        return;
    }
    const std::uint32_t halfwords = words * 2;
    if (to_spu) check_irq(k.tsa, halfwords);
    for (std::uint32_t i = 0; i < halfwords; ++i) {
        std::uint16_t *spu = &g_ram[(k.tsa + i) & kRamMask];
        std::uint8_t *iop = ram_ptr(madr + i * 2);
        if (to_spu) std::memcpy(spu, iop, 2);
        else std::memcpy(iop, spu, 2);
    }
    k.tsa = (k.tsa + halfwords) & kRamMask;
    if (channel >= 0) kernel::schedule(kernel::now() + 64 + words / 4, kernel::Job{kernel::kJobDmaDone, static_cast<std::uint32_t>(channel)});
}

// The game stopped the DMA channel of the core (the start bit written as 0): the auto-DMA blocks that were still waiting to go into the
// input buffer are dropped, as the stopped channel would not have sent them. Without this the old blocks stay in the queue, play ahead of
// the blocks of the next transfer and keep the stream one block off for as long as the game streams.
void dma_stop(int core) {
    Core &k = g_cores[core];
    static const bool trace_adma = std::getenv("PS2_TRACE_ADMA") != nullptr;
    if (trace_adma && !k.transfers.empty())
        std::fprintf(stderr, "[adma] core %d dma stopped at %.4f s, %zu transfers dropped, %u frames waiting\n", core,
                     g_first_tick ? static_cast<double>(kernel::now() - g_first_tick) / 36864000.0 : 0.0, k.transfers.size(), k.input_count);
    k.transfers.clear();
}

void set_sink(std::function<void(const std::int16_t *, std::size_t)> sink) { g_sink = std::move(sink); }

void dump_voices() {
    for (int c = 0; c < 2; ++c) {
        const Core &k = g_cores[c];
        std::fprintf(stderr, "[spu2] core %d master volume %04X %04X dry %04X %04X wet %04X %04X input %04X %04X attr %04X admas %04X\n", c, k.mvol[0], k.mvol[1], k.avol[0], k.avol[1],
                     k.evol[0], k.evol[1], k.bvol[0], k.bvol[1], k.attr, k.admas);
        std::fprintf(stderr, "[spu2] core %d registers from 0x180:", c);
        for (std::uint32_t off = 0x180; off < 0x400; off += 2)
            if (!(off >= 0x1C0 && off < 0x2E0) && k.regs[off >> 1]) std::fprintf(stderr, " %03X=%04X", off, k.regs[off >> 1]);
        std::fprintf(stderr, "\n");
    }
    for (int c = 0; c < 2; ++c)
        for (int i = 0; i < 24; ++i) {
            const Voice &v = g_cores[c].voices[i];
            if (v.phase == kOff && !v.end) continue;
            const std::uint16_t header = g_ram[v.block[0] & kRamMask];
            std::fprintf(stderr, "[spu2] core %d voice %d phase %d ssa 0x%05X lsax 0x%05X block 0x%05X flags %X next 0x%05X pitch 0x%04X env %d end %d\n", c, i,
                         static_cast<int>(v.phase), v.ssa, v.lsax, v.block[0], header >> 8, v.next, v.pitch, v.env.level, v.end ? 1 : 0);
        }
}

// Called when the run ends (with PS2_DUMP_AUDIO set): closes the files the sound went into while it ran, the one path names
// included. A file left open is closed when the program exits normally, or keeps its last header update.
void dump_audio(const char *) {
    g_chip_wav.close();
    g_out_wav.close();
}

}

namespace ps2::iop::spu2 {

void state(ps2::state::Stream &s) {
    s.tag("spu2");
    s.vec(g_ram);
    for (Core &k : g_cores) {
        s.pod(k.voices);
        if (s.loading())
            for (const Voice &v : k.voices)
                for (int i = 0; i < 2; ++i) block_end(v)[i] = (g_ram[v.block[i] & kRamMask] >> 8) & 1u;
        s.pod(k.attr);
        s.pod(k.admas);
        s.pod(k.irqa);
        s.pod(k.tsa);
        s.pod(k.mvol);
        s.pod(k.bvol);
        s.pod(k.avol);
        s.pod(k.evol);
        s.pod(k.irq);
        s.pod(k.input);
        s.pod(k.input_read);
        s.pod(k.input_count);
        s.deque(k.transfers, [&s](Transfer &t) {
            s.pod(t.madr);
            s.pod(t.bytes);
            s.pod(t.channel);
        });
        s.pod(k.regs);
    }
    s.pod(g_spdif_out);
    s.pod(g_spdif_mode);
    s.pod(g_spdif_media);
    s.pod(g_next_tick);
    s.pod(g_tick_pending);
}

}
