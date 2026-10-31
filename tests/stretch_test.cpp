// Offline test of the sound stretcher (host/stretch.cpp): no game, no sound device, no SDL.
//
//   clang++ -std=c++20 -O2 tests/stretch_test.cpp host/stretch.cpp -o /tmp/ps2-stretch-test
//   /tmp/ps2-stretch-test
//
// Part 1 feeds known signals through the Stretcher: at tempo 1 the samples come out bit for bit as they went in; at ratios 1.5, 2,
// 3 (and 5, the cap) the output is continuous (no click, no hole), has the right length and the same pitch.
// Part 2 (further down) runs the whole SoundPipe in virtual time against a pretend game that makes its sound in bursts.
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "../host/stretch.hpp"

using namespace ps2sound;

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace {

int g_failed = 0, g_checked = 0;

void check(bool ok, const char *what, const std::string &detail = "") {
    ++g_checked;
    if (!ok) ++g_failed;
    std::printf("  %s  %s%s%s\n", ok ? "PASS" : "FAIL", what, detail.empty() ? "" : "  ", detail.c_str());
}

std::string fmt(const char *format, double a = 0, double b = 0, double c = 0, double d = 0) {
    char buffer[256];
    std::snprintf(buffer, sizeof buffer, format, a, b, c, d);
    return buffer;
}

using Frames = std::vector<std::int16_t>;  // interleaved stereo

Frames sine(double hz, std::size_t frames, double amplitude = 9000.0) {
    Frames out(frames * 2);
    for (std::size_t n = 0; n < frames; ++n) {
        out[n * 2] = static_cast<std::int16_t>(std::lrint(amplitude * std::sin(2.0 * M_PI * hz * static_cast<double>(n) / kRate)));
        out[n * 2 + 1] = static_cast<std::int16_t>(std::lrint(0.8 * amplitude * std::sin(2.0 * M_PI * hz * static_cast<double>(n) / kRate + 0.7)));
    }
    return out;
}

// a held note with harmonics and a slow vibrato, a bit of music-like body
Frames note(double hz, std::size_t frames) {
    Frames out(frames * 2);
    double phase = 0;
    for (std::size_t n = 0; n < frames; ++n) {
        const double t = static_cast<double>(n) / kRate;
        const double f = hz * (1.0 + 0.002 * std::sin(2.0 * M_PI * 5.0 * t));
        phase += 2.0 * M_PI * f / kRate;
        double v = 0;
        for (int h = 1; h <= 6; ++h) v += std::sin(h * phase) / h;
        out[n * 2] = static_cast<std::int16_t>(std::lrint(5200.0 * v));
        out[n * 2 + 1] = static_cast<std::int16_t>(std::lrint(4400.0 * v));
    }
    return out;
}

Frames noise(std::size_t frames, unsigned seed, double amplitude = 6000.0) {
    std::mt19937 rng(seed);
    std::normal_distribution<double> d(0.0, amplitude);
    Frames out(frames * 2);
    for (auto &v : out) v = static_cast<std::int16_t>(std::max(-32768.0, std::min(32767.0, d(rng))));
    return out;
}

// the strongest frequency of the left channel in [from, from + len), by a windowed FFT and a parabola through the peak
double dominant_hz(const Frames &x, std::size_t from, std::size_t len = 65536) {
    std::vector<std::complex<double>> a(len);
    for (std::size_t i = 0; i < len; ++i) {
        const double w = 0.5 - 0.5 * std::cos(2.0 * M_PI * static_cast<double>(i) / static_cast<double>(len));
        a[i] = w * (from + i < x.size() / 2 ? x[(from + i) * 2] : 0);
    }
    for (std::size_t i = 1, j = 0; i < len; ++i) {  // bit reversal
        std::size_t bit = len >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (std::size_t size = 2; size <= len; size <<= 1) {
        const std::complex<double> wn = std::polar(1.0, -2.0 * M_PI / static_cast<double>(size));
        for (std::size_t i = 0; i < len; i += size) {
            std::complex<double> w = 1;
            for (std::size_t k = 0; k < size / 2; ++k, w *= wn) {
                const auto u = a[i + k], v = a[i + k + size / 2] * w;
                a[i + k] = u + v;
                a[i + k + size / 2] = u - v;
            }
        }
    }
    std::size_t best = 1;
    for (std::size_t k = 2; k < len / 2 - 1; ++k)
        if (std::abs(a[k]) > std::abs(a[best])) best = k;
    const double l = std::log(std::abs(a[best - 1]) + 1e-12), m = std::log(std::abs(a[best]) + 1e-12), r = std::log(std::abs(a[best + 1]) + 1e-12);
    const double shift = 0.5 * (l - r) / (l - 2 * m + r);
    return (static_cast<double>(best) + shift) * kRate / static_cast<double>(len);
}

int max_step(const Frames &x, std::size_t from, std::size_t to) {
    int worst = 0;
    for (std::size_t n = from + 1; n < to; ++n)
        for (int ch = 0; ch < 2; ++ch) worst = std::max(worst, std::abs(x[n * 2 + ch] - x[(n - 1) * 2 + ch]));
    return worst;
}

double rms(const Frames &x, std::size_t from, std::size_t len) {
    double s = 0;
    std::size_t n = 0;
    for (std::size_t i = from; i < from + len && i * 2 + 1 < x.size(); ++i, ++n) s += double(x[i * 2]) * x[i * 2] + double(x[i * 2 + 1]) * x[i * 2 + 1];
    return n ? std::sqrt(s / (2.0 * static_cast<double>(n))) : 0.0;
}

// Feeds all of `in` through a Stretcher the way the game does (blocks of 32 frames) and takes the output in device-sized blocks.
// Returns the output; `starved` counts blocks that came back short while input was still ahead by more than `lookahead`.
struct Run {
    Frames out;
    int starved = 0;
    double position = 0;
};
Run run_stretcher(const Frames &in, double tempo, std::size_t block = 1024) {
    Stretcher s;
    Run run;
    const std::size_t total = in.size() / 2;
    std::size_t pushed = 0;
    Frames buffer(block * 2);
    for (;;) {
        // keep 4000 frames ahead, like a queue
        while (pushed < total && s.waiting() < 4000) {
            const std::size_t n = std::min<std::size_t>(32, total - pushed);
            s.push(&in[pushed * 2], n);
            pushed += n;
        }
        const double waiting = s.waiting();
        const std::size_t got = s.pull(buffer.data(), block, tempo);
        run.out.insert(run.out.end(), buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(got * 2));
        if (got < block) {
            if (pushed < total && waiting > Stretcher::lookahead() + 2 * Stretcher::kHop) ++run.starved;
            if (pushed >= total) break;
        }
    }
    run.position = s.position();
    return run;
}

void test_identity() {
    std::printf("tempo 1: the samples pass bit for bit\n");
    Frames in = noise(300000, 7, 12000.0);
    const Frames tail = sine(1000, 100000);
    in.insert(in.end(), tail.begin(), tail.end());
    Stretcher s;
    Frames out;
    std::mt19937 rng(3);
    std::size_t pushed = 0;
    const std::size_t total = in.size() / 2;
    while (out.size() / 2 < total) {
        const std::size_t n = std::min<std::size_t>(1 + rng() % 70, total - pushed);
        if (n) s.push(&in[pushed * 2], n);
        pushed += n;
        const std::size_t want = 1 + rng() % 1500;
        Frames buffer(want * 2);
        const std::size_t got = s.pull(buffer.data(), want, 1.0);
        out.insert(out.end(), buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(got * 2));
    }
    check(out.size() == in.size() && std::memcmp(out.data(), in.data(), in.size() * 2) == 0, "400000 frames, random chunk sizes, every sample identical");
    check(!s.stretching(), "never started stretching");
}

void test_ratio(const char *name, const Frames &in, double hz, double ratio, bool pitch_check = true, bool smooth_check = true) {
    const double tempo = 1.0 / ratio;
    const Run run = run_stretcher(in, tempo);
    const double expected = (static_cast<double>(in.size() / 2) - (Stretcher::kSeek + Stretcher::kHop)) * ratio;  // the last step needs its search range in the input
    const double got = static_cast<double>(run.out.size() / 2);
    check(std::fabs(got - expected) < Stretcher::kHop + 1024, fmt("length: %.0f frames for %.0f expected (ratio %.1f)", got, expected, ratio).c_str(), name);
    check(run.starved == 0, "no block came back short while there was input", fmt("%.0f short blocks", run.starved));
    if (pitch_check) {
        const std::size_t from = run.out.size() / 4 / 2;  // somewhere in the middle
        const double f_in = dominant_hz(in, in.size() / 4 / 2), f_out = dominant_hz(run.out, from);
        check(std::fabs(f_out - f_in) < 0.005 * f_in, fmt("pitch: %.2f Hz in, %.2f Hz out", f_in, f_out).c_str(), name);
        (void)hz;
    }
    if (smooth_check) {
        const bool hiss = std::strcmp(name, "noise") == 0;
        if (!hiss) {
            // no click: the steepest step of the output stays near the input's (a break in a sine would be several times steeper)
            const int step_in = max_step(in, 0, in.size() / 2), step_out = max_step(run.out, 2048, run.out.size() / 2);
            check(step_out <= step_in * 1.25 + 8, fmt("continuous: steepest step %.0f in, %.0f out", step_in, step_out).c_str(), name);
        }
        // no hole: the loudness over 5 ms windows. Noise wobbles by itself over such short windows, so it gets a wider band.
        const double r_in = rms(in, 0, in.size() / 2);
        double lo = 1e9, hi = 0;
        for (std::size_t n = 4096; n + 240 < run.out.size() / 2; n += 240) {
            const double r = rms(run.out, n, 240);
            lo = std::min(lo, r);
            hi = std::max(hi, r);
        }
        check(lo > (hiss ? 0.75 : 0.9) * r_in && hi < (hiss ? 1.25 : 1.15) * r_in,
              fmt("loudness over 5 ms stays within %.2f to %.2f of the input", lo / r_in, hi / r_in).c_str(), name);
    }
}

void test_switching() {
    std::printf("switching: tempo 1, then 0.5, then 1 again, on a sine\n");
    const Frames in = sine(660, 400000);
    Stretcher s;
    std::size_t pushed = 0;
    Frames out, buffer(1024 * 2);
    const std::size_t total = in.size() / 2;
    int phase = 0;
    while (pushed < total || s.waiting() > 4000) {
        while (pushed < total && s.waiting() < 6000) {
            const std::size_t n = std::min<std::size_t>(32, total - pushed);
            s.push(&in[pushed * 2], n);
            pushed += n;
        }
        phase = out.size() / 2 < 60000 ? 0 : out.size() / 2 < 160000 ? 1 : 2;
        const std::size_t got = s.pull(buffer.data(), 1024, phase == 1 ? 0.5 : 1.0);
        out.insert(out.end(), buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(got * 2));
        if (got < 1024) break;
    }
    const int step_in = max_step(in, 0, total);
    check(max_step(out, 0, out.size() / 2) <= step_in * 1.25 + 8, "no click anywhere, also at the two switches", fmt("steepest step %.0f, input %.0f", max_step(out, 0, out.size() / 2), step_in).c_str());
    check(std::memcmp(out.data(), in.data(), 60000 * 4) == 0, "the first 60000 frames (tempo 1) are still bit for bit");
    check(!s.stretching(), "back to plain copying at the end");
}

}  // namespace


// stretch_test wav <in.wav> <out.wav> <ratio>: stretches a 48000 Hz 16-bit stereo WAV file (to listen to or measure the stretcher on real sound)
int wav_main(const char *in_path, const char *out_path, double ratio) {
    std::FILE *f = std::fopen(in_path, "rb");
    if (!f) return std::printf("cannot read %s\n", in_path), 1;
    std::vector<unsigned char> bytes;
    unsigned char chunk[65536];
    for (std::size_t n; (n = std::fread(chunk, 1, sizeof chunk, f)) > 0;) bytes.insert(bytes.end(), chunk, chunk + n);
    std::fclose(f);
    std::size_t pos = 12, data = 0, size = 0;
    while (pos + 8 <= bytes.size()) {
        std::uint32_t len;
        std::memcpy(&len, &bytes[pos + 4], 4);
        if (!std::memcmp(&bytes[pos], "data", 4)) {
            data = pos + 8;
            size = std::min<std::size_t>(len, bytes.size() - data);
            break;
        }
        pos += 8 + len + (len & 1);
    }
    if (!data) return std::printf("no data chunk in %s\n", in_path), 1;
    Frames in(size / 2);
    std::memcpy(in.data(), &bytes[data], in.size() * 2);
    const Run run = run_stretcher(in, 1.0 / ratio);
    f = std::fopen(out_path, "wb");
    if (!f) return std::printf("cannot write %s\n", out_path), 1;
    const std::uint32_t n = static_cast<std::uint32_t>(run.out.size() * 2);
    const std::uint32_t words[] = {0x46464952u, 36 + n, 0x45564157u, 0x20746D66u, 16, 0x00020001u, 48000, 48000 * 4, 0x00100004u, 0x61746164u, n};
    std::fwrite(words, 4, 11, f);
    std::fwrite(run.out.data(), 2, run.out.size(), f);
    std::fclose(f);
    std::printf("%.2f s in, %.2f s out (ratio %.2f)\n", static_cast<double>(in.size() / 2) / kRate, static_cast<double>(run.out.size() / 2) / kRate, ratio);
    return 0;
}


// ========================================================================================== part 2: the whole pipe in virtual time
//
// A pretend game makes 800 frames of sound per video frame in 25 pushes of 32 frames: a few spread over the frame and the
// rest in a burst (that is how the real game does it, measured with a log of the real push times), and waits for the sound
// like the host does (pace: while more than 80 ms are waiting). A pretend device asks for blocks of 1024 frames. All times
// are made up by the test, so it runs in a second and gives the same numbers every time.

namespace {

struct Phase {
    double seconds;      // wall seconds (0 for a stall: nothing is pushed for `seconds`)
    double frame_ms;     // how long one video frame takes the game to make, when it does not wait for the sound
    bool stall = false;  // the game is held
};

struct SimResult {
    Frames device, heard;       // what the stretcher made, and what the speaker got
    PipeStats stats;
    std::vector<double> tempo;  // one value per device block, with the time of the block
    std::vector<double> tempo_time, queued;
    Frames input;               // what the game made
    double duration = 0;
};

struct Sim {
    explicit Sim(const PipeConfig &c, std::size_t block = 1024, unsigned seed = 5) : pipe(c), block_(block), rng(seed) {
        pipe.device_tap.on = true;
        pipe.heard_tap.on = true;
        out.resize(block * 2);
    }
    SoundPipe pipe;
    std::size_t block_;
    std::mt19937 rng;
    double t = 0, next_block = 0;
    std::uint64_t made = 0;
    SimResult result;
    Frames out;
    double uniform(double a, double b) { return std::uniform_real_distribution<double>(a, b)(rng); }

    void device_until(double until) {
        while (next_block <= until) {
            pipe.render(out.data(), block_, next_block);
            result.tempo.push_back(pipe.tempo());
            result.tempo_time.push_back(next_block);
            result.queued.push_back(pipe.queued_ms());
            next_block += static_cast<double>(block_) / kRate;
        }
    }
    void push32(double at) {
        device_until(at);
        t = at;
        std::int16_t buf[64];
        for (int i = 0; i < 32; ++i, ++made) {
            const double n = static_cast<double>(made);
            const std::int16_t v = static_cast<std::int16_t>(std::lrint(9000 * std::sin(2 * M_PI * 440 * n / kRate) + 2700 * std::sin(2 * M_PI * 1320 * n / kRate)));
            buf[i * 2] = buf[i * 2 + 1] = v;
        }
        result.input.insert(result.input.end(), buf, buf + 64);
        pipe.push(buf, 32, at);
    }
    void run_frame(double frame_s, unsigned sound_frames = 800) {
        const double start = t, cluster = uniform(0.35, 0.85) * frame_s;
        std::vector<double> times(sound_frames / 32);
        for (int i = 0; i < 5; ++i) times[i] = uniform(0.0002, std::max(0.0003, cluster));
        std::sort(times.begin(), times.begin() + 5);
        for (std::size_t i = 5; i < times.size(); ++i) times[i] = cluster + 0.00005 * (i - 5);
        for (double off : times) push32(start + off);
        device_until(start + frame_s);
        t = start + frame_s;
        pipe.frame(t);
        // the host's pace(): wait while more than 80 ms is waiting
        for (int guard = 0; pipe.full() && guard < 1000; ++guard) {
            t += 0.001;
            device_until(t);
            pipe.note_wait(0.001);
        }
    }
    void run(const std::vector<Phase> &phases) {
        for (const Phase &p : phases) {
            const double end = t + p.seconds;
            if (p.stall) {
                t = end;
                device_until(t);
                continue;
            }
            while (t < end) run_frame(p.frame_ms / 1000.0 * uniform(0.93, 1.07));
        }
    }
    SimResult finish() {
        pipe.device_tap.take(result.device);
        pipe.heard_tap.take(result.heard);
        result.stats = pipe.stats();
        result.duration = t;
        return std::move(result);
    }
};

// how often the speaker got silence in a stretch of the run (counted on the heard stream: runs of exact zeros of 5 ms or more)
struct Silence {
    int gaps = 0;
    double seconds = 0;
};
Silence silence_between(const Frames &heard, double from_s, double to_s) {
    Silence s;
    const std::size_t a = static_cast<std::size_t>(from_s * kRate), b = std::min<std::size_t>(static_cast<std::size_t>(to_s * kRate), heard.size() / 2);
    std::size_t run = 0;
    for (std::size_t n = a; n <= b; ++n) {
        const bool zero = n < b && heard[n * 2] == 0 && heard[n * 2 + 1] == 0;
        if (zero) {
            ++run;
        } else {
            if (run >= 240) {
                ++s.gaps;
                s.seconds += static_cast<double>(run) / kRate;
            }
            run = 0;
        }
    }
    return s;
}

double mean_tempo(const SimResult &r, double from, double to) {
    double sum = 0;
    int n = 0;
    for (std::size_t i = 0; i < r.tempo.size(); ++i)
        if (r.tempo_time[i] >= from && r.tempo_time[i] < to) {
            sum += r.tempo[i];
            ++n;
        }
    return n ? sum / n : 0;
}
double extreme_queued(const SimResult &r, double from, double to, bool max) {
    double v = max ? 0 : 1e9;
    for (std::size_t i = 0; i < r.queued.size(); ++i)
        if (r.tempo_time[i] >= from && r.tempo_time[i] < to) v = max ? std::max(v, r.queued[i]) : std::min(v, r.queued[i]);
    return v;
}
// the heard stream has a start gap of silence before the first sound; the steepest step anywhere in it
int steepest(const SimResult &r) { return max_step(r.heard, 0, r.heard.size() / 2); }

void report(const char *name, const SimResult &r) {
    const PipeStats &s = r.stats;
    std::printf("  [%s] %.1f s: %.1f s of sound made, %.1f s played, %llu dry, %.2f s silence, %.1f s stretched (slowest tempo %.2f, %.2f s at the cap)\n", name,
                r.duration, s.pushed / 48000.0, s.played / 48000.0, static_cast<unsigned long long>(s.dry), s.silence / 48000.0, s.stretched / 48000.0, s.slowest,
                s.at_cap / 48000.0);
}

}  // namespace

int simulation_main() {
    std::printf("\nthe pipe in virtual time (a pretend game, a pretend device)\n");
    PipeConfig on;
    {
        std::printf("full speed (the game could make 125 frames a second and waits for the sound):\n");
        Sim sim(on);
        sim.run({{20, 8}});
        SimResult r = sim.finish();
        report("full speed", r);
        check(r.stats.dry == 0, "no dry run");
        check(r.stats.stretched == 0 && r.stats.slowest == 1.0, "never stretched, the tempo stays exactly 1");
        const std::size_t n = std::min(r.device.size(), r.input.size());
        check(n > 48000u * 2 * 15 && std::memcmp(r.device.data(), r.input.data(), n * 2) == 0, "the sound that went to the device is bit for bit what the game made",
              fmt("%.1f s compared", n / 2.0 / kRate).c_str());
        check(extreme_queued(r, 2, 20, false) > 50 && extreme_queued(r, 2, 20, true) <= 90, "the waiting sound stays between 50 and 90 ms",
              fmt("%.0f to %.0f ms", extreme_queued(r, 2, 20, false), extreme_queued(r, 2, 20, true)).c_str());
    }
    for (const std::size_t block : {1024u, 512u}) {
        std::printf("recovery to full speed, %zu-frame audio blocks:\n", block);
        Sim sim(on, block);
        sim.run({{3, 32}});
        double longest = 0;
        for (int frame = 0; frame < 600; ++frame) {
            const double start = sim.t, tempo = sim.pipe.tempo();
            sim.run_frame(0.008);  // ample CPU headroom after the slow scene
            // Early recovery legitimately paces to the still-slow audio tempo.
            // Near unity the controller must not inject a new 40 ms queue drain.
            if (tempo >= 0.95) longest = std::max(longest, (sim.t - start) * 1000.0);
        }
        check(longest < 30.0, "no pacing hitch near normal tempo after recovery", fmt("longest %.1f ms", longest).c_str());
        check(sim.pipe.stats().dry == 0, "recovery keeps audio supplied");
        check(sim.pipe.tempo() == 1.0, "recovery reaches normal tempo");
    }
    for (const unsigned sound_frames : {800u, 960u}) {
        std::printf("one 72 ms frame, then CPU headroom (%u audio frames per video frame):\n", sound_frames);
        Sim sim(on, 512);
        for (int i = 0; i < 300; ++i) sim.run_frame(0.011, sound_frames);
        const double begin = sim.t;
        sim.run_frame(0.072, sound_frames);
        double recovery = -1.0, longest_wait = 0.0;
        for (int i = 0; i < 120; ++i) {
            const double start = sim.t;
            sim.run_frame(0.011, sound_frames);
            longest_wait = std::max(longest_wait, (sim.t - start) * 1000.0 - 11.0);
            if (recovery < 0.0 && sim.t - begin > 0.1 && sim.pipe.tempo() >= 0.995) recovery = sim.t - begin;
        }
        check(recovery >= 0.0 && recovery < 1.0, "one long frame does not sustain slow playback", fmt("recovered in %.3f s", recovery).c_str());
        check(longest_wait < 40.0, "no large pacing drain after one long frame", fmt("longest wait %.1f ms", longest_wait).c_str());
        check(sim.pipe.stats().dry == 0, "single-frame recovery keeps audio supplied");
    }
    for (const double fps : {45.0, 33.0, 20.0}) {
        std::printf("steady %.0f frames a second:\n", fps);
        Sim sim(on);
        sim.run({{25, 1000.0 / fps}});
        SimResult r = sim.finish();
        report("steady", r);
        const Silence quiet = silence_between(r.heard, 4, 24);
        check(quiet.gaps == 0, "no silence between 4 s and 24 s", fmt("%.0f gaps, %.2f s", quiet.gaps, quiet.seconds).c_str());
        const double tempo = mean_tempo(r, 4, 24), want = fps / 59.94;
        check(std::fabs(tempo - want) < 0.06, fmt("the tempo follows the game: %.3f (game speed %.3f)", tempo, want).c_str());
        check(extreme_queued(r, 4, 24, true) < 110, fmt("the waiting sound stays small: %.0f to %.0f ms", extreme_queued(r, 4, 24, false), extreme_queued(r, 4, 24, true)).c_str());
        const std::size_t from = static_cast<std::size_t>(10.0 * kRate);
        check(r.heard.size() > from + 65536 * 2, "enough sound");
        const double f = dominant_hz(r.heard, from);
        check(std::fabs(f - 440.0) < 2.0, fmt("pitch %.2f Hz", f).c_str());
        check(steepest(r) < 1300, "no click", fmt("steepest step %.0f", steepest(r)).c_str());
    }
    {
        std::printf("the arena dip: 33 frames a second, then 13, then 33 again:\n");
        Sim sim(on);
        sim.run({{6, 30}, {7, 76}, {8, 30}});
        SimResult r = sim.finish();
        report("dip", r);
        const Silence before = silence_between(r.heard, 3, 6), during = silence_between(r.heard, 6, 13.5), after = silence_between(r.heard, 15, 21);
        std::printf("  silence: before the dip %d gaps (%.2f s), in the dip %d gaps (%.2f s), after %d gaps (%.2f s)\n", before.gaps, before.seconds, during.gaps, during.seconds,
                    after.gaps, after.seconds);
        check(before.gaps == 0 && after.gaps == 0, "no gap at 33 frames a second before and after the dip");
        check(during.seconds < 0.5, "at most half a second of silence in 7 seconds of the dip (the old way: about 5 s)", fmt("%.2f s", during.seconds).c_str());
        check(steepest(r) < 1300, "no click", fmt("steepest step %.0f", steepest(r)).c_str());
    }
    {
        std::printf("a hold of 3 seconds (a window dragged), at 33 frames a second:\n");
        Sim sim(on);
        sim.run({{8, 30}, {3, 0, true}, {8, 30}});
        SimResult r = sim.finish();
        report("hold", r);
        const Silence after = silence_between(r.heard, 13, 19);
        check(after.gaps == 0, "after the hold the sound is smooth again within two seconds", fmt("%.0f gaps", after.gaps).c_str());
        check(steepest(r) < 1300, "no click, no burst of garbage", fmt("steepest step %.0f", steepest(r)).c_str());
        const double tempo_after = mean_tempo(r, 13, 19), tempo_before = mean_tempo(r, 4, 8);
        check(std::fabs(tempo_after - tempo_before) < 0.08, fmt("no long stretched tail: tempo %.2f before, %.2f after", tempo_before, tempo_after).c_str());
    }
    {
        std::printf("beyond the cap (8 frames a second needs a stretch of 7.5, the cap is 5):\n");
        Sim sim(on);
        sim.run({{15, 125}});
        SimResult r = sim.finish();
        report("beyond", r);
        check(r.stats.slowest >= 0.19, "never slower than the cap");
        check(r.stats.at_cap > 0, "the cap was reached and counted");
        check(steepest(r) < 1300, "no click", fmt("steepest step %.0f", steepest(r)).c_str());
    }
    {
        std::printf("a state loaded (the queue is cleared) at 33 frames a second:\n");
        Sim sim(on);
        sim.run({{8, 30}});
        sim.pipe.flush();
        sim.run({{8, 30}});
        SimResult r = sim.finish();
        report("flush", r);
        check(silence_between(r.heard, 12, 16).gaps == 0, "smooth again 4 seconds after the load");
        check(steepest(r) < 1300, "no click at the load", fmt("steepest step %.0f", steepest(r)).c_str());
    }
    {
        std::printf("blocks of 480 frames instead of 1024, 33 frames a second:\n");
        Sim sim(on, 480);
        sim.run({{20, 30}});
        SimResult r = sim.finish();
        report("480", r);
        check(silence_between(r.heard, 4, 20).gaps == 0, "no silence");
    }
    {
        std::printf("the switch off (PS2_AUDIO_STRETCH=0), 33 frames a second: the old behaviour, for comparison:\n");
        PipeConfig off = on;
        off.stretch = false;
        Sim sim(off);
        sim.run({{20, 30}});
        SimResult r = sim.finish();
        report("off", r);
        check(r.stats.stretched == 0 && r.stats.slowest == 1.0, "never stretched");
        check(r.stats.silence / 48000.0 > 3.0, "the sound has silence in it", fmt("%.1f s", r.stats.silence / 48000.0).c_str());
    }
    return 0;
}


// stretch_test replay <push.txt>: the real producer. The log is made by the host with PS2_AUDIO_PUSHLOG (scripts/soundcheck.py): the time of
// every push of the sound chip and of every video frame of a real run. The pipe is fed the same times with a test tone and run against a
// pretend device, so the controller can be tuned on how the game really delivers its sound.
int replay_main(const char *path) {
    std::FILE *f = std::fopen(path, "r");
    if (!f) return std::printf("cannot read %s\n", path), 1;
    struct Event {
        double t;
        int kind;
    };
    std::vector<Event> events;
    double t, queued;
    int k, frame;
    while (std::fscanf(f, "%lf %d %lf %d", &t, &k, &queued, &frame) == 4) events.push_back({t, k >= 100000 ? 1 : 0});
    std::fclose(f);
    if (events.empty()) return 1;
    const double t0 = events.front().t;
    Sim sim{PipeConfig{}};
    for (const Event &e : events) {
        const double at = e.t - t0;
        if (e.kind == 0) {
            sim.push32(at);
        } else {
            sim.device_until(at);
            sim.pipe.frame(at);
        }
    }
    SimResult r = sim.finish();
    report("replay", r);
    const Silence quiet = silence_between(r.heard, 3, r.duration);
    std::printf("  silence after 3 s: %d gaps, %.2f s; steepest step %d; mean tempo %.3f; waiting %.0f to %.0f ms\n", quiet.gaps, quiet.seconds, steepest(r),
                mean_tempo(r, 3, 1e9), extreme_queued(r, 3, 1e9, false), extreme_queued(r, 3, 1e9, true));
    for (std::size_t i = 0; i < r.tempo.size(); i += 12) std::printf("%.2f %.3f %.0f\n", r.tempo_time[i], r.tempo[i], r.queued[i]);
    return 0;
}

// stretch_test trace <frame_ms> <seconds>: the tempo and the waiting sound of every 4th device block of a steady pretend game (for tuning)
int trace_main(double frame_ms, double seconds) {
    Sim sim{PipeConfig{}};
    sim.run({{seconds, frame_ms}});
    SimResult r = sim.finish();
    report("trace", r);
    for (std::size_t i = 0; i < r.tempo.size(); i += 4) std::printf("%.2f %.3f %.0f\n", r.tempo_time[i], r.tempo[i], r.queued[i]);
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 4 && std::string(argv[1]) == "trace") return trace_main(std::atof(argv[2]), std::atof(argv[3]));
    if (argc == 3 && std::string(argv[1]) == "replay") return replay_main(argv[2]);
    if (argc == 5 && std::string(argv[1]) == "wav") return wav_main(argv[2], argv[3], std::atof(argv[4]));
    const bool only_sim = argc > 1 && std::string(argv[1]) == "sim";
    if (!only_sim) {
        test_identity();
        const Frames tone = sine(440, 48000 * 12), melody = note(220, 48000 * 12), hiss = noise(48000 * 12, 11);
        for (const double ratio : {1.5, 2.0, 3.0, 5.0}) {
            std::printf("ratio %.1f\n", ratio);
            test_ratio("sine 440 Hz", tone, 440, ratio);
            test_ratio("note 220 Hz with harmonics and vibrato", melody, 220, ratio);
            test_ratio("noise", hiss, 0, ratio, false);
        }
        test_switching();
    }
    const int sim = simulation_main();
    std::printf("\n%d of %d checks passed\n", g_checked - g_failed, g_checked);
    return (g_failed || sim) ? 1 : 0;
}

