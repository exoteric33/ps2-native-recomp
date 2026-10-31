// Time stretching for the game's sound (48000 Hz, 16-bit, stereo, interleaved) when the game runs slower than real time.
//
// Stretcher: waveform-similarity overlap-add (WSOLA, Verhelst and Roelands, ICASSP 1993) written from that description. It plays
// the sound slower without lowering its pitch: the input is read at a nominal position that advances `tempo` input frames per
// output frame, and each step of kHop output frames cross-fades the natural continuation of the last step into the stretch of
// input near the nominal position that looks most like it (normalized cross-correlation, a coarse search on a mono sum with 4
// times fewer samples, then a fine one). At tempo 1 it copies the input untouched.
//
// SoundPipe: the Stretcher plus the part that decides how much to stretch. The emulation thread pushes what the sound chip makes,
// the device thread asks for what it plays. Nothing here knows SDL, so tests/stretch_test.cpp can run it in virtual time.
// docs/sound.md explains the choices.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace ps2sound {

constexpr int kRate = 48000;

class Stretcher {
public:
    static constexpr int kHop = 512;    // output frames per step (10.7 ms); a step cross-fades over all of them
    static constexpr int kSeek = 640;   // the search reaches 13 ms either side of the nominal position

    Stretcher();

    // Producer thread. Returns how many frames fitted (all of them unless the consumer stopped taking them).
    std::size_t push(const std::int16_t *lr, std::size_t frames);
    // Frames pushed so far (the position in the sound, counted in frames since the start).
    std::uint64_t head() const { return head_.load(std::memory_order_acquire); }

    // Consumer thread from here on.
    // Writes up to `frames` frames; fewer only when the input ran short. tempo is the input consumed per output frame: 1 (or more)
    // copies, below 1 stretches by 1 / tempo. Smallest tempo: 0.05.
    std::size_t pull(std::int16_t *out, std::size_t frames, double tempo);
    // Drops everything waiting and starts again at the newest frame.
    void flush();
    // Where the output stands in the input: the input frames from here to head() are what is waiting.
    double position() const { return stretching_ ? nom_ : static_cast<double>(read_); }
    double waiting() const { return static_cast<double>(head()) - position(); }
    bool stretching() const { return stretching_; }
    // Input frames that must lie beyond position() before every step can be made; with less, pull() may stop short while stretching.
    static constexpr int lookahead() { return kSeek + 2 * kHop; }

private:
    bool step(double tempo);
    void publish_keep();
    std::int16_t at(std::int64_t frame, int channel) const { return ring_[static_cast<std::size_t>(frame & kMask) * 2 + channel]; }

    static constexpr std::int64_t kCapacity = 1 << 17;  // 2.7 s
    static constexpr std::int64_t kMask = kCapacity - 1;
    std::vector<std::int16_t> ring_;
    std::atomic<std::uint64_t> head_{0}, keep_{0};  // written by the producer / the consumer: what the consumer may still read
    // consumer only
    std::int64_t read_ = 0;       // plain copying: the next frame to play
    bool stretching_ = false;
    double nom_ = 0.0;            // stretching: the nominal read position
    std::int64_t cont_ = 0;       // stretching: where the sound of the last step goes on by itself
    std::int16_t pend_[kHop * 2];
    int pend_pos_ = 0, pend_len_ = 0;
    std::vector<float> mono_, ref_, coarse_ref_, coarse_cand_, fade_in_;
};

struct PipeConfig {
    bool stretch = true;          // false: the sound passes as it is and the device plays silence where it ran short (the old way)
    double max_ratio = 5.0;       // slowest tempo is 1 / max_ratio; below it the queue runs dry like without stretching
    double target_ms = 60.0;      // the amount of waiting sound (game time) the controller steers to
    double pace_ms = 80.0;        // the emulation waits while more than this is waiting
    double pace_stretch_ms = 120.0;  // the same while the sound is stretched (it swings more then, and a wait would slow the game down)
    double restart_ms = 50.0;     // after running dry: play again once this much is waiting
};

struct PipeStats {
    std::uint64_t pushed = 0, dropped = 0;  // frames the game made / frames that did not fit
    std::uint64_t played = 0;               // frames of game sound the device got (stretched)
    std::uint64_t silence = 0;              // frames of silence between them, after the sound had begun
    std::uint64_t dry = 0;                  // times it ran dry: pieces of sound that ended because there was no more
    std::uint64_t stretched = 0;            // frames made while stretching
    std::uint64_t at_cap = 0;               // frames made at the slowest tempo allowed
    double consumed = 0;                    // game frames those made up
    double slowest = 1.0;                   // slowest tempo asked for
    double first = -1.0, last = -1.0;       // device time of the first sound and of the latest block
};

// A copy of what the device thread plays, for the dump files; the emulation thread takes it away.
class Tap {
public:
    void add(const std::int16_t *lr, std::size_t frames);
    void take(std::vector<std::int16_t> &out);  // appends everything collected so far to `out`
    bool on = false;

private:
    std::mutex mutex_;
    std::vector<std::int16_t> data_;
};

class SoundPipe {
public:
    explicit SoundPipe(const PipeConfig &config = {});

    // ---- emulation thread. `now` is seconds on one steady clock, the same one render() gets.
    void push(const std::int16_t *lr, std::size_t frames, double now);
    // Once per video frame, before the wait for the sound: how fast the game runs is measured between these.
    void frame(double now);
    // Seconds the emulation spent waiting for the sound (that is not slowness of the game).
    void note_wait(double seconds) { wait_since_ += seconds; }
    // Sound waiting, in milliseconds of game time (what the title bar shows).
    double queued_ms() const { return (static_cast<double>(stretcher_.head()) - position_.load(std::memory_order_relaxed)) / (kRate / 1000.0); }
    // The emulation waits while this is true.
    bool full() const { return queued_ms() > pace_limit_ms_; }
    // The tempo now: 1 plays as made, 0.5 twice as slow.
    double tempo() const { return tempo_.load(std::memory_order_relaxed); }
    // Drop what is waiting (a state was loaded); the device thread does it before its next block.
    void flush() { flush_.store(true, std::memory_order_relaxed); }

    // ---- device thread. Fills `frames` frames. `overlay` (may be null) is applied to the whole block, before the taps see it.
    void render(std::int16_t *out, std::size_t frames, double now, void (*overlay)(std::int16_t *, std::size_t) = nullptr);

    PipeStats stats() const;
    const PipeConfig &config() const { return config_; }
    // The sound the stretcher made (no silence added where it ran dry), and everything the speaker got (silence included).
    Tap device_tap, heard_tap;

private:
    double choose_tempo(double now, double waiting_ms);
    void fade(std::int16_t *lr, std::size_t frames, bool in);

    static constexpr double kStall = 0.25;    // a gap this long between two pushes of sound is not counted as running time (a long frame, a hold)
    static constexpr double kHeld = 0.4;      // no new sound for this long: the game is held, what is left plays out at the plain speed
    static constexpr double kWindow = 0.5;    // seconds of running time the speed of the game is averaged over
    static constexpr double kGapWindow = 0.6; // the longest frame within this much running time is what the controller plans for

    PipeConfig config_;
    Stretcher stretcher_;
    std::atomic<bool> flush_{false};
    std::atomic<double> position_{0.0}, tempo_{1.0}, last_push_{-1.0};
    std::atomic<double> rate_{1.0}, frame_gap_{0.0};  // from the emulation thread: speed of the game, longest recent frame (seconds)

    // emulation thread only
    struct Mark {
        double alive;
        std::uint64_t pushed;
    };
    std::vector<Mark> marks_;
    std::size_t marks_next_ = 0, marks_count_ = 0;
    double alive_ = 0.0, last_push_t_ = -1.0, wait_since_ = 0.0;
    double pace_limit_ms_, pace_frame_time_ = -1.0;
    std::uint64_t pushed_ = 0, dropped_ = 0;

    // device thread only
    double level_ms_ = -1.0, last_render_ = -1.0;
    bool starved_ = true, playing_ = false, fade_in_ = false, prev_complete_ = false, last_live_ = false;
    std::int16_t last_[2] = {0, 0};
    double first_ = -1.0;

    mutable std::mutex stats_mutex_;
    PipeStats stats_;
    std::vector<std::int16_t> silence_;
};

}  // namespace ps2sound
