#include "spu2.hpp"

#include "hw.hpp"
#include "iop.hpp"
#include "kernel.hpp"
#include "../load_stats.hpp"
#include "ps2/os.hpp"

#include "ps2/state.hpp"

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <new>
#include <string>
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
        // The chip scales an exponential decrease with a shift, which rounds down. A division rounds to 0, which made the step 0 once
        // the step times the level fell under 0x8000, so a release stopped short (at 4095 for rates 44 and up, lower for faster ones) and the voice never
        // went off: ENVX never read 0 and Digital Devil Saga's sound driver stopped starting effects (a wall switch made no sound).
        if (exponential && decrease) delta = (delta * level) >> 15;
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
    // reads of STATX that still say "not ready" after the core was just enabled (not part of a save state: it ends within moments)
    std::uint32_t statx_settling = 0;
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

// ---- what the problem report (F9) says about the sound (summary() at the end of this file) ----
// A few relaxed atomics that only the emulation thread writes (the key on/off registers, the DMA and the sound tick all run there) and
// that the report may read from another thread. They are not part of a save state and do not change what the chip does. Events are
// counted per second of IOP time in a small ring, so "the last 5 seconds" needs no list of events and no clock of its own.
constexpr std::uint64_t kClockHz = 36864000;  // the IOP's clock, which kernel::now() counts
constexpr unsigned kBuckets = 8, kWindowSeconds = 5;
using Counter = std::atomic<std::uint32_t>;
using Counter64 = std::atomic<std::uint64_t>;

void bump(Counter &c, std::uint32_t n = 1) { c.store(c.load(std::memory_order_relaxed) + n, std::memory_order_relaxed); }
void bump(Counter64 &c, std::uint64_t n = 1) { c.store(c.load(std::memory_order_relaxed) + n, std::memory_order_relaxed); }
void raise(Counter &c, std::uint32_t v) {
    if (v > c.load(std::memory_order_relaxed)) c.store(v, std::memory_order_relaxed);
}

struct Bucket {
    Counter64 second;  // the IOP second this bucket counts, plus 1 (0 = never used)
    Counter kon[2], koff[2], blank_kon[2], upload_bytes[2], adma_bytes[2], peak_pre[2], peak_out;
};
Bucket g_buckets[kBuckets];

struct Totals {
    Counter64 kon[2], koff[2], blank_kon[2], upload_bytes[2], uploads[2], adma_bytes[2], adma_stops[2];
    Counter64 last_upload[2], last_adma[2], last_stop[2], last_kon[2], last_batch, batches;  // IOP time + 1 (0 = never)
    Counter peak_out, peak_pre[2];
};
Totals g_totals;

// A voice that is playing, as the summary shows it (refreshed about six times a second).
struct VoiceNote {
    Counter w[3];  // 0: valid, voice, phase, start address; 1: pitch and flags; 2: envelope level
};
struct Live {
    Counter attr[2], admas[2], input_frames[2], queued[2], playing[2], keyed_no_mixer[2];
    Counter mvol[2][2], bvol[2][2], evol[2][2];
    VoiceNote notes[2][3];
};
Live g_live;
constexpr unsigned kNoteFlagVoll = 1, kNoteFlagVolr = 2, kNoteFlagData = 4, kNoteFlagMixed = 8;

// A machine reset starts the counts over (the IOP clock they are stamped with starts over too).
void clear_summary() {
    for (Bucket &b : g_buckets) {
        b.second.store(0, std::memory_order_relaxed);
    }
    new (&g_totals) Totals();
    new (&g_live) Live();
}

Bucket &bucket() {
    const std::uint64_t second = kernel::now() / kClockHz + 1;
    Bucket &b = g_buckets[second % kBuckets];
    if (b.second.load(std::memory_order_relaxed) != second) {
        for (int c = 0; c < 2; ++c) {
            b.kon[c].store(0, std::memory_order_relaxed);
            b.koff[c].store(0, std::memory_order_relaxed);
            b.blank_kon[c].store(0, std::memory_order_relaxed);
            b.upload_bytes[c].store(0, std::memory_order_relaxed);
            b.adma_bytes[c].store(0, std::memory_order_relaxed);
            b.peak_pre[c].store(0, std::memory_order_relaxed);
        }
        b.peak_out.store(0, std::memory_order_relaxed);
        b.second.store(second, std::memory_order_relaxed);
    }
    return b;
}
void stamp(Counter64 &t) { t.store(kernel::now() + 1, std::memory_order_relaxed); }

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
    v.next = (flags & 1u) ? v.lsax : ((at + 8) & kRamMask);
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

// The noise generator, one per core, as the public PlayStation hardware documentation (nocash's psx-spx, "SPU Noise Generator")
// describes the first PlayStation's: a 16-bit shift register that takes in one new bit (the inverted parity of bits 15, 12, 11 and 10)
// each time a timer runs out; the timer counts down by 4..7 each sample and is reloaded with 0x20000 >> shift. The rate comes from the
// core's attribute register, bits 8-9 (the step) and 10-13 (the shift). A voice whose bit is set in NON (0x184/0x186) plays the
// register's value instead of its sound data (the data still runs on, for its loop and end flags), through its envelope and volumes.
// GTA Vice City's sound driver times the chip at start-up by watching a quiet noise voice change the core's output ring (issue 100).
// Its state lives in two unused words of the core's register array (0x3D0, 0x3D2/0x3D4), so a saved state needs nothing new.
constexpr std::uint32_t kNoiseLevel = 0x3D0, kNoiseTimer = 0x3D2;
void run_noise(Core &k, std::int16_t *out) {
    const unsigned clock = (k.attr >> 8) & 0x3Fu, step = 4 + (clock & 3u), reload = 0x20000u >> (clock >> 2);
    std::uint16_t level = k.regs[kNoiseLevel >> 1];
    std::int32_t timer = static_cast<std::int32_t>(static_cast<std::uint32_t>(k.regs[kNoiseTimer >> 1]) | static_cast<std::uint32_t>(k.regs[(kNoiseTimer + 2) >> 1]) << 16);
    if (timer > static_cast<std::int32_t>(reload)) timer = static_cast<std::int32_t>(reload);  // the rate was just made faster
    for (unsigned n = 0; n < kBatch; ++n) {
        timer -= static_cast<std::int32_t>(step);
        if (timer < 0) {
            const unsigned parity = ((level >> 15) ^ (level >> 12) ^ (level >> 11) ^ (level >> 10) ^ 1u) & 1u;
            level = static_cast<std::uint16_t>(level << 1 | parity);
            timer += static_cast<std::int32_t>(reload);
            if (timer < 0) timer += static_cast<std::int32_t>(reload);
        }
        out[n] = static_cast<std::int16_t>(level);
    }
    k.regs[kNoiseLevel >> 1] = level;
    k.regs[kNoiseTimer >> 1] = static_cast<std::uint16_t>(timer);
    k.regs[(kNoiseTimer + 2) >> 1] = static_cast<std::uint16_t>(static_cast<std::uint32_t>(timer) >> 16);
}

void render_voice(Voice &v, unsigned voice, const Routing &route, std::int32_t (*dry)[2], std::int32_t (*wet)[2], std::int16_t *raw, const std::int16_t *noise) {
    if (v.key_on) key_on(v);
    const std::uint32_t bit = 1u << voice;
    const std::uint32_t step = std::min<std::uint32_t>(v.pitch, 0x3FFFu);
    const bool sweep_l = v.voll & 0x8000u, sweep_r = v.volr & 0x8000u;
    const std::int32_t fixed_l = static_cast<std::int16_t>(v.voll << 1), fixed_r = static_cast<std::int16_t>(v.volr << 1);
    std::uint32_t position = v.position;
    for (unsigned n = 0; n < kBatch; ++n) {
        if (v.phase == kOff) break;
        const std::uint32_t index = position >> 12, frac = position & 0xFFFu;
        const std::int32_t sample = noise ? noise[n] : (v.buffer[index] * static_cast<std::int32_t>(0x1000 - frac) + v.buffer[index + 1] * static_cast<std::int32_t>(frac)) >> 12;
        position += step;
        while ((position >> 12) >= 28) {
            position -= 28u << 12;
            std::memmove(v.buffer, v.buffer + 28, 28 * sizeof(std::int16_t));
            if (block_end(v)[0]) {
                v.end = true;
                if (!((g_ram[v.block[0] & kRamMask] >> 8) & 2u)) {
                    v.phase = kRelease;
                    v.env.level = 0;
                }
            }
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
        bump(bucket().adma_bytes[core], chunk);
        bump(g_totals.adma_bytes[core], chunk);
        stamp(g_totals.last_adma[core]);
        t.madr += chunk;
        t.bytes -= chunk;
        // The channel's address register moves on with each piece the chip takes in, as on the console, where the chip asks for the
        // next piece each time 256 samples of its input buffer have played. Drivers read it to see how far a looping stream has got
        // (libsd's BlockTransStatus); GTA Vice City's sound driver times the chip with it at start-up (issue 100). Only the
        // newest transfer shows there: an older one still queued ahead of it is not what the register holds.
        if (t.channel >= 0 && k.transfers.size() == 1) hw::dma_progress(static_cast<unsigned>(t.channel), t.madr);
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

// The sound summary's view of the cores, refreshed every 256 batches (about 0.17 s of sound): what is playing and where it goes.
void publish_live() {
    constexpr auto kRelaxed = std::memory_order_relaxed;
    for (int c = 0; c < 2; ++c) {
        const Core &k = g_cores[c];
        const Routing route = routing(k);
        g_live.attr[c].store(k.attr, kRelaxed);
        g_live.admas[c].store(k.admas, kRelaxed);
        g_live.input_frames[c].store(k.input_count, kRelaxed);
        g_live.queued[c].store(static_cast<std::uint32_t>(k.transfers.size()), kRelaxed);
        for (int i = 0; i < 2; ++i) {
            g_live.mvol[c][i].store(k.mvol[i], kRelaxed);
            g_live.bvol[c][i].store(k.bvol[i], kRelaxed);
            g_live.evol[c][i].store(k.evol[i], kRelaxed);
        }
        std::uint32_t playing = 0, shown = 0, no_mixer = 0;
        for (unsigned i = 0; i < 24; ++i) {
            const Voice &v = k.voices[i];
            if (v.phase == kOff && !v.key_on) continue;
            ++playing;
            const bool mixed = ((route.dry[0] | route.dry[1] | route.wet[0] | route.wet[1]) >> i) & 1u;
            if (!mixed) ++no_mixer;
            if (shown >= 3) continue;
            bool data = false;
            for (unsigned w = 0; w < 8; ++w) data |= g_ram[(v.ssa + w) & kRamMask] != 0;
            const std::uint32_t flags = ((v.voll & 0x7FFFu) ? kNoteFlagVoll : 0) | ((v.volr & 0x7FFFu) ? kNoteFlagVolr : 0) | (data ? kNoteFlagData : 0) | (mixed ? kNoteFlagMixed : 0);
            VoiceNote &n = g_live.notes[c][shown++];
            n.w[0].store(0x80000000u | i << 24 | static_cast<std::uint32_t>(v.phase) << 20 | (v.ssa & 0xFFFFFu), kRelaxed);
            n.w[1].store(v.pitch | flags << 16, kRelaxed);
            n.w[2].store(static_cast<std::uint32_t>(v.env.level), kRelaxed);
        }
        for (; shown < 3; ++shown) g_live.notes[c][shown].w[0].store(0, kRelaxed);
        g_live.playing[c].store(playing, kRelaxed);
        g_live.keyed_no_mixer[c].store(no_mixer, kRelaxed);
    }
}

void note_batch(const std::int32_t (&peak_pre)[2], std::int32_t peak_out) {
    Bucket &b = bucket();
    for (int c = 0; c < 2; ++c) {
        raise(b.peak_pre[c], static_cast<std::uint32_t>(peak_pre[c]));
        raise(g_totals.peak_pre[c], static_cast<std::uint32_t>(peak_pre[c]));
    }
    raise(b.peak_out, static_cast<std::uint32_t>(peak_out));
    raise(g_totals.peak_out, static_cast<std::uint32_t>(peak_out));
    stamp(g_totals.last_batch);
    if ((g_totals.batches.load(std::memory_order_relaxed) & 255u) == 0) publish_live();
    bump(g_totals.batches);
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
        std::int16_t noise[kBatch];
        run_noise(g_cores[c], noise);
        const std::uint32_t non = g_cores[c].regs[0x184 >> 1] | static_cast<std::uint32_t>(g_cores[c].regs[0x186 >> 1] & 0xFFu) << 16;
        for (int i = 0; i < 24; ++i) {
            Voice &v = g_cores[c].voices[i];
            if (!v.key_on && v.phase == kOff) continue;
            if (count) {
                ++active;
                ++phases[v.phase];
            }
            render_voice(v, static_cast<unsigned>(i), route, sum[c], wet[c], i == 1 ? rings[c][2] : i == 3 ? rings[c][3] : nullptr,
                         (non >> i) & 1u ? noise : nullptr);
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
    std::int32_t peak_pre[2] = {}, peak_out = 0;  // for the problem report: the loudest sample before and after the master volume
    for (unsigned n = 0; n < kBatch; ++n) {
        std::int32_t mix[2] = {};
        for (int c = 0; c < 2; ++c) {
            peak_pre[c] = std::max({peak_pre[c], std::abs(sum[c][n][0]), std::abs(sum[c][n][1])});
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
        peak_out = std::max({peak_out, std::abs(static_cast<std::int32_t>(out[n * 2])), std::abs(static_cast<std::int32_t>(out[n * 2 + 1]))});
    }
    note_batch(peak_pre, peak_out);
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

// PS2_TRACE_KON: key on/off events and the reads that see a voice's state before its key on took effect
bool trace_kon() {
    static const bool on = std::getenv("PS2_TRACE_KON") != nullptr;
    return on;
}
double trace_time() { return g_first_tick ? static_cast<double>(kernel::now() - g_first_tick) / 36864000.0 : 0.0; }

// The chip takes a key on within a sample: ENDX, ENVX and NAX of the voice start over at once. The sound driver may look at them
// right after the key on (Urban Reign's NUSOUND does), and a voice still marked as ended there is taken for a free one and given to
// the next sound, so the first sound (a fighter's cry when knocked out) never plays. PS2_SPU2_KON_LATE=1 keeps the old way (the key
// on waits for the next batch of 32 samples), to compare.
bool kon_late() {
    static const bool on = std::getenv("PS2_SPU2_KON_LATE") != nullptr;
    return on;
}

void key(Core &k, std::uint32_t mask, bool on) {
    for (int i = 0; i < 24; ++i) {
        if (!(mask & (1u << i))) continue;
        Voice &v = k.voices[i];
        if (trace_kon())
            std::fprintf(stderr, "[kon] %.4f s core %d voice %d %s (pending %d phase %d env %d end %d ssa 0x%05X)\n", trace_time(), static_cast<int>(&k - g_cores), i,
                         on ? "on" : "off", v.key_on ? 1 : 0, static_cast<int>(v.phase), v.env.level, v.end ? 1 : 0, v.ssa);
        if (on) {
            v.key_on = true;
            if (!kon_late()) key_on(v);
        } else if (v.key_on) v.key_on = false;
        else if (v.phase != kOff) v.phase = kRelease;
        // for the problem report
        Bucket &b = bucket();
        const int c = static_cast<int>(&k - g_cores);
        if (on) {
            bool data = false;  // does the sound data at the voice's start address hold anything (a block of zeros is silence)?
            for (unsigned w = 0; w < 8; ++w) data |= g_ram[(v.ssa + w) & kRamMask] != 0;
            bump(b.kon[c]);
            bump(g_totals.kon[c]);
            if (!data) {
                bump(b.blank_kon[c]);
                bump(g_totals.blank_kon[c]);
            }
            stamp(g_totals.last_kon[c]);
        } else {
            bump(b.koff[c]);
            bump(g_totals.koff[c]);
        }
    }
}

std::uint32_t read_core(int core, std::uint32_t off) {
    Core &k = g_cores[core];
    if (trace_kon() && off < 0x180u && (off & 0xFu) == 0xA && k.voices[off >> 4].key_on)
        std::fprintf(stderr, "[kon] %.4f s core %d voice %u: ENVX read (%d) while its key on waits\n", trace_time(), core, off >> 4, k.voices[off >> 4].env.level);
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
        if (trace_kon())
            for (int i = 0; i < 24; ++i)
                if (k.voices[i].key_on && k.voices[i].end)
                    std::fprintf(stderr, "[kon] %.4f s core %d voice %d: ENDX read says ended while its key on waits\n", trace_time(), core, i);
        return off == 0x340 ? endx & 0xFFFFu : endx >> 16;
    }
    case 0x344:
        // right after the core is enabled the ready bit stays clear for a few reads; the sound driver waits for it (follows upstream fe2a4ef)
        if (k.statx_settling) {
            --k.statx_settling;
            return 0;
        }
        return 0x80u;
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
        if ((value & 0x8000u) && !(k.attr & 0x8000u)) k.statx_settling = 16;
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
        bump(g_totals.upload_bytes[core], 2);
        stamp(g_totals.last_upload[core]);
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
    clear_summary();
    const std::string chip = os::env("PS2_DUMP_AUDIO"), out = os::env("PS2_AUDIO_DUMP");
    if (!chip.empty() && g_chip_wav.open(chip)) std::fprintf(stderr, "[spu2] writing the sound chip's output to %s\n", chip.c_str());
    // the sink is set before the machine resets (host/window.cpp opens the sound device first)
    if (!out.empty() && out != chip && !g_sink && g_out_wav.open(out)) std::fprintf(stderr, "[spu2] writing the sound to %s (no sound device)\n", out.c_str());
}

void restart_ticks() { g_tick_pending = false; }

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

namespace {

// A stream starts into an empty input buffer (the first block, or the first after the game turned ADMA off, which empties it). On the
// console the input buffer is a ring of 512 samples that the chip reads at the same position as it writes its output rings, and it asks
// for the next piece each time it has played half of it, so a piece only ever goes into the half that is not playing. The first piece
// therefore plays from the next half boundary on, and every later one comes in exactly as a half ends, in step with the output rings.
// Here the buffer is a queue: start it at the ring position the next batch plays, with silence up to that boundary. A driver that times
// the chip with the stream (GTA Vice City's sound driver at start-up, issue 100) sees the pieces come at the moments it expects.
static_assert(kInputFrames == kRingWords, "the input buffer runs in step with the output rings");
void align_input(Core &k) {
    const auto pos = static_cast<unsigned>(g_next_tick / (kBatch * kCyclesPerSample) % (kRingWords / kBatch) * kBatch);
    const unsigned silence = kInputFrames / 2 - pos % (kInputFrames / 2);
    k.input_read = pos;
    for (unsigned i = 0; i < silence; ++i) k.input[(pos + i) % kInputFrames][0] = k.input[(pos + i) % kInputFrames][1] = 0;
    k.input_count = silence;
}

}

void dma(int core, std::uint32_t madr, std::uint32_t words, bool to_spu, int channel) {
    keep_ticking();
    Core &k = g_cores[core];
    if (to_spu && (k.admas & (1u << core))) {
        static const bool trace_adma = std::getenv("PS2_TRACE_ADMA") != nullptr;  // porting work: when each ADMA block starts, and the blocks
        if (trace_adma) std::fprintf(stderr, "[adma] core %d dma of %u bytes from 0x%06X at %.4f s, %u frames already waiting, %zu transfers queued\n", core, words * 4, madr,
                                                        g_first_tick ? static_cast<double>(kernel::now() - g_first_tick) / 36864000.0 : 0.0, k.input_count, k.transfers.size());
        if (!k.input_count && k.transfers.empty()) align_input(k);
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
    if (to_spu) {
        bump(bucket().upload_bytes[core], words * 4);
        bump(g_totals.upload_bytes[core], words * 4);
        bump(g_totals.uploads[core]);
        stamp(g_totals.last_upload[core]);
    }
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
    bump(g_totals.adma_stops[core]);
    stamp(g_totals.last_stop[core]);
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

namespace ps2::iop::spu2 {

namespace {

struct Window {
    std::uint64_t kon[2] = {}, koff[2] = {}, blank_kon[2] = {}, upload[2] = {}, adma[2] = {};
    std::uint32_t peak_pre[2] = {}, peak_out = 0;
};

// the sums of the buckets of the last kWindowSeconds seconds of IOP time (4 to 5 seconds, since the newest one is still filling)
Window window(std::uint64_t now) {
    Window w;
    const std::uint64_t current = now / kClockHz + 1;
    for (const Bucket &b : g_buckets) {
        const std::uint64_t second = b.second.load(std::memory_order_relaxed);
        if (!second || second > current || current - second >= kWindowSeconds) continue;
        for (int c = 0; c < 2; ++c) {
            w.kon[c] += b.kon[c].load(std::memory_order_relaxed);
            w.koff[c] += b.koff[c].load(std::memory_order_relaxed);
            w.blank_kon[c] += b.blank_kon[c].load(std::memory_order_relaxed);
            w.upload[c] += b.upload_bytes[c].load(std::memory_order_relaxed);
            w.adma[c] += b.adma_bytes[c].load(std::memory_order_relaxed);
            w.peak_pre[c] = std::max(w.peak_pre[c], b.peak_pre[c].load(std::memory_order_relaxed));
        }
        w.peak_out = std::max(w.peak_out, b.peak_out.load(std::memory_order_relaxed));
    }
    return w;
}

std::string text(const char *format, ...) __attribute__((format(printf, 1, 2)));
std::string text(const char *format, ...) {
    char buffer[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof buffer, format, args);
    va_end(args);
    return buffer;
}

// "never" or "3.2 s ago" for a time stamp (IOP time plus 1, 0 = never)
std::string ago(std::uint64_t stamp, std::uint64_t now) {
    if (!stamp) return "never";
    const double seconds = stamp - 1 > now ? 0.0 : static_cast<double>(now - (stamp - 1)) / static_cast<double>(kClockHz);
    return text("%.1f s ago", seconds);
}

// a master or bus volume register is zero when it is not sweeping and its 15 value bits are 0
bool volume_set(std::uint32_t reg) { return (reg & 0x8000u) || (reg & 0x7FFFu); }

}  // namespace

// The sound part of the problem report: plain lines (indented four spaces) that tell from one report whether the game's sound driver
// starts voices, whether sound data reached the sound memory, whether the stream (auto-DMA) goes on and whether anything comes out.
std::string summary() {
    constexpr auto kRelaxed = std::memory_order_relaxed;
    const std::uint64_t now = kernel::now();
    const Window w = window(now);
    const Totals &t = g_totals;
    const auto sum2 = [](const Counter64 (&a)[2]) { return a[0].load(kRelaxed) + a[1].load(kRelaxed); };
    const auto sum2w = [](const std::uint64_t (&a)[2]) { return a[0] + a[1]; };
    const std::uint64_t batches = t.batches.load(kRelaxed);
    std::string out;
    if (!batches) return "    the game has not used the sound chip yet (no sound batch has been made since the runner started)\n";

    out += text("    sound chip:      running, %llu batches made since start, the last one %s\n", static_cast<unsigned long long>(batches), ago(t.last_batch.load(kRelaxed), now).c_str());
    out += "    (\"last 5 s\" below = the last 4 to 5 seconds of the game's own time)\n";
    for (int c = 0; c < 2; ++c)
        out += text("    voices started:  core %d: last 5 s %llu on / %llu off; since start %llu on / %llu off (last start %s)\n", c, static_cast<unsigned long long>(w.kon[c]),
                    static_cast<unsigned long long>(w.koff[c]), static_cast<unsigned long long>(t.kon[c].load(kRelaxed)), static_cast<unsigned long long>(t.koff[c].load(kRelaxed)),
                    ago(t.last_kon[c].load(kRelaxed), now).c_str());
    out += text("    start data:      %llu of the %llu voices started since start had only zeros at their start address (last 5 s: %llu of %llu)\n",
                static_cast<unsigned long long>(sum2(t.blank_kon)), static_cast<unsigned long long>(sum2(t.kon)), static_cast<unsigned long long>(sum2w(w.blank_kon)),
                static_cast<unsigned long long>(sum2w(w.kon)));
    for (int c = 0; c < 2; ++c) {
        out += text("    playing now:     core %d: %u voices (%u not sent to the mixer)\n", c, g_live.playing[c].load(kRelaxed), g_live.keyed_no_mixer[c].load(kRelaxed));
        for (const VoiceNote &n : g_live.notes[c]) {
            const std::uint32_t w0 = n.w[0].load(kRelaxed), w1 = n.w[1].load(kRelaxed);
            if (!(w0 & 0x80000000u)) continue;
            static const char *const kPhase[] = {"off", "attack", "decay", "sustain", "release"};
            const std::uint32_t flags = w1 >> 16, phase = (w0 >> 20) & 7u;
            out += text("                     voice %u: start 0x%05X, pitch 0x%04X (0x1000 = normal), volume left %s right %s, %s, %s, %s, envelope %u of 32767\n", (w0 >> 24) & 31u, w0 & 0xFFFFFu,
                        w1 & 0xFFFFu, (flags & kNoteFlagVoll) ? "set" : "ZERO", (flags & kNoteFlagVolr) ? "set" : "ZERO", (flags & kNoteFlagData) ? "data at start" : "NO DATA at start (zeros)",
                        (flags & kNoteFlagMixed) ? "sent to the mixer" : "NOT sent to the mixer", phase < 5 ? kPhase[phase] : "?", n.w[2].load(kRelaxed));
        }
    }
    for (int c = 0; c < 2; ++c)
        out += text("    sound data in:   core %d: last 5 s %llu bytes into sound memory; since start %llu bytes in %llu transfers (last %s)\n", c, static_cast<unsigned long long>(w.upload[c]),
                    static_cast<unsigned long long>(t.upload_bytes[c].load(kRelaxed)), static_cast<unsigned long long>(t.uploads[c].load(kRelaxed)), ago(t.last_upload[c].load(kRelaxed), now).c_str());
    bool stream_stopped = false, stream_flowing = false;
    for (int c = 0; c < 2; ++c) {
        const bool on = (g_live.admas[c].load(kRelaxed) & (1u << c)) != 0;
        out += text("    stream (ADMA):   core %d: %s, last 5 s %llu bytes moved, since start %llu bytes (last %s), %u blocks waiting, stopped by the game %llu times\n", c, on ? "ON" : "off",
                    static_cast<unsigned long long>(w.adma[c]), static_cast<unsigned long long>(t.adma_bytes[c].load(kRelaxed)), ago(t.last_adma[c].load(kRelaxed), now).c_str(),
                    g_live.queued[c].load(kRelaxed), static_cast<unsigned long long>(t.adma_stops[c].load(kRelaxed)));
        if (on && !w.adma[c] && t.adma_bytes[c].load(kRelaxed)) stream_stopped = true;
        if (w.adma[c]) stream_flowing = true;
    }
    for (int c = 0; c < 2; ++c) {
        const std::uint32_t l = g_live.mvol[c][0].load(kRelaxed), r = g_live.mvol[c][1].load(kRelaxed), bl = g_live.bvol[c][0].load(kRelaxed), br = g_live.bvol[c][1].load(kRelaxed);
        const bool set = volume_set(l) || volume_set(r);
        out += text("    volumes:         core %d: master %s (0x%04X 0x%04X), stream input %s (0x%04X 0x%04X), core %s\n", c, set ? "not zero" : "ZERO", l, r, (volume_set(bl) || volume_set(br)) ? "not zero" : "ZERO", bl, br,
                    (g_live.attr[c].load(kRelaxed) & 0x8000u) ? "enabled" : "NOT enabled");
    }
    out += text("    loudness:        last 5 s: loudest output sample %u of 32767 (core 0 before its master volume %u, core 1 %u); since start %u\n", w.peak_out, w.peak_pre[0], w.peak_pre[1], t.peak_out.load(kRelaxed));

    // the one-line reading, only where the numbers make it safe to say
    const bool playing_now = g_live.playing[0].load(kRelaxed) + g_live.playing[1].load(kRelaxed) > 0;
    const bool active = sum2w(w.kon) > 0 || playing_now || stream_flowing;
    const bool ever_started = sum2(t.kon) > 0 || sum2(t.adma_bytes) > 0;
    std::string meaning;
    if (!ever_started) {
        meaning = sum2(t.upload_bytes) ? "sound data was loaded, but no voice was ever started and no stream ever ran: the game's sound driver has not played anything yet."
                                       : "no sound data was ever loaded and no voice or stream was ever started: the game's sound driver is probably not running.";
    } else if (active && w.peak_out == 0) {
        int muted_core = -1, quiet_stream = -1;  // a core that makes sound before its master volume but has it at zero; a stream whose input volume is zero
        for (int c = 0; c < 2; ++c) {
            if (w.peak_pre[c] && !volume_set(g_live.mvol[c][0].load(kRelaxed)) && !volume_set(g_live.mvol[c][1].load(kRelaxed))) muted_core = c;
            if (w.adma[c] && !volume_set(g_live.bvol[c][0].load(kRelaxed)) && !volume_set(g_live.bvol[c][1].load(kRelaxed))) quiet_stream = c;
        }
        if (muted_core >= 0) meaning = text("core %d makes sound but its master volume is zero, so nothing comes out.", muted_core);
        else if (quiet_stream >= 0) meaning = text("the stream of core %d is moving but its input volume is zero, so nothing comes out.", quiet_stream);
        else if (sum2w(w.kon) && sum2w(w.blank_kon) == sum2w(w.kon)) meaning = "voices are started but silent: the sound data at all their start addresses is zeros, so the sound bank probably did not land correctly in sound memory.";
        else if (!w.peak_pre[0] && !w.peak_pre[1]) meaning = "voices or the stream are active but silent before the master volume (voice volumes, envelopes or the mixer routing are zero).";
        else meaning = "sound is made before the master volume but nothing comes out of the chip.";
    } else if (!active) {
        meaning = stream_stopped ? "nothing plays right now and the stream is switched on but no data moved in the last 5 s: the streaming transfer has stopped (or the game paused it)."
                                 : "nothing plays right now (no voice started and no stream data in the last 5 s); sounds that should be heard now are not being started.";
    } else {
        meaning = "sound is coming out of the chip; if it is not heard, look after the chip (the sound device or the window's volume).";
    }
    out += "    reading:         " + meaning + "\n";
    return out;
}

}
