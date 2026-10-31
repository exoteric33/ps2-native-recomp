#include "stretch.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace ps2sound {

namespace {

constexpr int kHop = Stretcher::kHop;
constexpr int kDecimate = 4;
constexpr int kCoarseLen = kHop / kDecimate;
// A candidate far from the nominal position loses a little: when two stretches look alike, the one nearer the nominal position
// wins, which keeps the average tempo true and stops a loud sound from being repeated for nothing.
constexpr float kNearBias = 0.02f;

std::int16_t to_s16(float v) {
    const long r = std::lrintf(v);
    return static_cast<std::int16_t>(r > 32767 ? 32767 : r < -32768 ? -32768 : r);
}

}  // namespace

Stretcher::Stretcher() : ring_(static_cast<std::size_t>(kCapacity) * 2, 0) {
    mono_.resize(static_cast<std::size_t>(2 * kSeek + kHop + 8));
    ref_.resize(kHop);
    coarse_ref_.resize(kCoarseLen);
    coarse_cand_.resize(static_cast<std::size_t>((2 * kSeek + kHop) / kDecimate + 4));
    // raised cosine: 0 to 1 over a step; the sound of the step before fades out with 1 - fade (adjusted below)
    fade_in_.resize(kHop);
    for (int n = 0; n < kHop; ++n) {
        const double w = (n + 0.5) / kHop;
        fade_in_[static_cast<std::size_t>(n)] = static_cast<float>(0.5 - 0.5 * std::cos(3.14159265358979323846 * w));
    }
    std::memset(pend_, 0, sizeof pend_);
}

std::size_t Stretcher::push(const std::int16_t *lr, std::size_t frames) {
    const std::uint64_t head = head_.load(std::memory_order_relaxed);
    const std::uint64_t keep = keep_.load(std::memory_order_acquire);
    const std::uint64_t used = head - keep;
    const std::uint64_t room = used >= static_cast<std::uint64_t>(kCapacity) ? 0 : static_cast<std::uint64_t>(kCapacity) - used;
    const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(frames, room));
    for (std::size_t i = 0; i < n; ++i) {
        const std::size_t at = static_cast<std::size_t>((head + i) & static_cast<std::uint64_t>(kMask)) * 2;
        ring_[at] = lr[i * 2];
        ring_[at + 1] = lr[i * 2 + 1];
    }
    head_.store(head + n, std::memory_order_release);
    return n;
}

void Stretcher::publish_keep() {
    std::int64_t keep = read_;
    if (stretching_) keep = std::min<std::int64_t>(cont_, static_cast<std::int64_t>(std::floor(nom_)) - kSeek - 8);
    keep_.store(static_cast<std::uint64_t>(std::max<std::int64_t>(0, keep)), std::memory_order_release);
}

void Stretcher::flush() {
    read_ = static_cast<std::int64_t>(head());
    stretching_ = false;
    pend_pos_ = pend_len_ = 0;
    publish_keep();
}

std::size_t Stretcher::pull(std::int16_t *out, std::size_t frames, double tempo) {
    std::size_t done = 0;
    while (done < frames) {
        if (pend_pos_ < pend_len_) {
            const std::size_t n = std::min<std::size_t>(frames - done, static_cast<std::size_t>(pend_len_ - pend_pos_));
            std::memcpy(out + done * 2, pend_ + pend_pos_ * 2, n * 4);
            pend_pos_ += static_cast<int>(n);
            done += n;
            continue;
        }
        if (!stretching_) {
            if (tempo >= 1.0) {  // as made
                const std::int64_t waiting = static_cast<std::int64_t>(head()) - read_;
                const std::size_t n = static_cast<std::size_t>(std::min<std::int64_t>(waiting, static_cast<std::int64_t>(frames - done)));
                for (std::size_t i = 0; i < n; ++i) {
                    const std::size_t at = static_cast<std::size_t>((read_ + static_cast<std::int64_t>(i)) & kMask) * 2;
                    out[(done + i) * 2] = ring_[at];
                    out[(done + i) * 2 + 1] = ring_[at + 1];
                }
                read_ += static_cast<std::int64_t>(n);
                done += n;
                break;  // either full or out of input
            }
            stretching_ = true;  // start where the plain copy would go on
            cont_ = read_;
            nom_ = static_cast<double>(read_);
        } else if (tempo >= 1.0) {  // between steps: the sound goes on by itself from cont_, which is a plain copy from there
            stretching_ = false;
            read_ = cont_;
            continue;
        }
        if (!step(std::max(0.05, std::min(1.0, tempo)))) break;
    }
    publish_keep();
    return done;
}

// One step: kHop frames of output.
bool Stretcher::step(double tempo) {
    const std::int64_t head = static_cast<std::int64_t>(this->head());
    const std::int64_t nom = static_cast<std::int64_t>(std::floor(nom_));
    const std::int64_t lo = std::max<std::int64_t>(0, nom - kSeek), hi = nom + kSeek;
    if (hi + kHop > head || cont_ + kHop > head) return false;

    // mono sums: the candidate region [lo, hi + kHop) and the reference [cont_, cont_ + kHop)
    const std::int64_t span = hi + kHop - lo;
    for (std::int64_t i = 0; i < span; ++i) mono_[static_cast<std::size_t>(i)] = 0.5f * (at(lo + i, 0) + at(lo + i, 1));
    for (int i = 0; i < kHop; ++i) ref_[static_cast<std::size_t>(i)] = 0.5f * (at(cont_ + i, 0) + at(cont_ + i, 1));

    double ref_energy = 0.0;
    for (int i = 0; i < kHop; ++i) ref_energy += static_cast<double>(ref_[static_cast<std::size_t>(i)]) * ref_[static_cast<std::size_t>(i)];

    std::int64_t best = std::min(std::max<std::int64_t>(nom, lo), hi);  // silence: stay near the nominal position
    float best_ncc = 0.0f;
    if (ref_energy > 1.0 * kHop) {  // louder than about one step of the least significant bit; otherwise every candidate looks alike
        // coarse search
        for (int i = 0; i < kCoarseLen; ++i) {
            float s = 0;
            for (int k = 0; k < kDecimate; ++k) s += ref_[static_cast<std::size_t>(i * kDecimate + k)];
            coarse_ref_[static_cast<std::size_t>(i)] = s;
        }
        const std::int64_t coarse_len = span / kDecimate;
        for (std::int64_t i = 0; i < coarse_len; ++i) {
            float s = 0;
            for (int k = 0; k < kDecimate; ++k) s += mono_[static_cast<std::size_t>(i * kDecimate + k)];
            coarse_cand_[static_cast<std::size_t>(i)] = s;
        }
        double ref_c = 0.0;
        for (int i = 0; i < kCoarseLen; ++i) ref_c += static_cast<double>(coarse_ref_[static_cast<std::size_t>(i)]) * coarse_ref_[static_cast<std::size_t>(i)];
        double cand_c = 0.0;
        for (int i = 0; i < kCoarseLen; ++i) cand_c += static_cast<double>(coarse_cand_[static_cast<std::size_t>(i)]) * coarse_cand_[static_cast<std::size_t>(i)];
        float best_score = -2.0f;
        std::int64_t best_coarse = 0;
        const std::int64_t positions = (hi - lo) / kDecimate + 1;
        for (std::int64_t p = 0; p < positions; ++p) {
            float dot = 0;
            for (int i = 0; i < kCoarseLen; ++i) dot += coarse_ref_[static_cast<std::size_t>(i)] * coarse_cand_[static_cast<std::size_t>(p + i)];
            const float ncc = static_cast<float>(dot / std::sqrt(ref_c * cand_c + 1e-9));
            const float score = ncc - kNearBias * static_cast<float>(std::abs(lo + p * kDecimate - nom)) / kSeek;
            if (score > best_score) {
                best_score = score;
                best_coarse = p;
            }
            if (p + 1 < positions) {  // slide the candidate energy window by one coarse position
                const float out_v = coarse_cand_[static_cast<std::size_t>(p)], in_v = coarse_cand_[static_cast<std::size_t>(p + kCoarseLen)];
                cand_c += static_cast<double>(in_v) * in_v - static_cast<double>(out_v) * out_v;
                if (cand_c < 0.0) cand_c = 0.0;
            }
        }
        // fine search around the coarse winner, and the natural continuation itself when it lies in range
        const std::int64_t centre = lo + best_coarse * kDecimate;
        auto score_at = [&](std::int64_t q, float &ncc_out) {
            double dot = 0.0, energy = 0.0;
            const float *cand = &mono_[static_cast<std::size_t>(q - lo)];
            for (int i = 0; i < kHop; ++i) {
                dot += static_cast<double>(ref_[static_cast<std::size_t>(i)]) * cand[i];
                energy += static_cast<double>(cand[i]) * cand[i];
            }
            ncc_out = static_cast<float>(dot / std::sqrt(ref_energy * energy + 1e-9));
            return ncc_out - kNearBias * static_cast<float>(std::abs(q - nom)) / kSeek;
        };
        float top = -2.0f;
        auto consider = [&](std::int64_t q) {
            if (q < lo || q > hi) return;
            float ncc;
            const float score = score_at(q, ncc);
            if (score > top) {
                top = score;
                best = q;
                best_ncc = ncc;
            }
        };
        for (std::int64_t q = centre - kDecimate + 1; q < centre + kDecimate; ++q) consider(q);
        consider(cont_);
    }

    // The output: the natural continuation fades out while the chosen stretch fades in. When they are the same sound (correlation
    // 1) the weights add to 1; when they are unrelated (0) the squares do, so noise does not dip in loudness in the middle.
    float *const unused = nullptr;
    (void)unused;
    if (best == cont_) {
        for (int n = 0; n < kHop; ++n) {
            pend_[n * 2] = at(cont_ + n, 0);
            pend_[n * 2 + 1] = at(cont_ + n, 1);
        }
    } else {
        const float r = std::max(0.0f, std::min(1.0f, best_ncc));
        for (int n = 0; n < kHop; ++n) {
            const float gi = fade_in_[static_cast<std::size_t>(n)];
            const float go = -r * gi + std::sqrt(r * r * gi * gi + 1.0f - gi * gi);
            for (int ch = 0; ch < 2; ++ch)
                pend_[n * 2 + ch] = to_s16(go * static_cast<float>(at(cont_ + n, ch)) + gi * static_cast<float>(at(best + n, ch)));
        }
    }
    pend_pos_ = 0;
    pend_len_ = kHop;
    cont_ = best + kHop;
    nom_ += kHop * tempo;
    return true;
}

}  // namespace ps2sound

// ======================================================================================================== Tap, SoundPipe

namespace ps2sound {

namespace {

constexpr double kTrimSeconds = 0.3;   // a level that is off is brought back over this long
constexpr double kLevelTau = 0.12;     // smoothing of the level the controller looks at (it climbs and falls by 13 ms with every burst)
constexpr double kFloorMs = Stretcher::lookahead() * 1000.0 / kRate + 4.0;  // below this waiting a step may not be possible
constexpr double kHealthy = 0.98;      // a game that makes this much sound per second of running time needs no stretching
constexpr double kEnter = 0.995;       // stretching starts only when it asks for less than this
constexpr double kRecoveredUp = 0.03;  // a healthy producer with enough buffered sound can recover promptly
constexpr double kSmoothUp = 0.15;     // the tempo may rise only this fast (seconds); it may fall at once
constexpr std::size_t kFade = 96;      // 2 ms: the fade at the edges of a stretch of sound, so a start or a cut does not click
constexpr double kDropAbove = 500.0 * (kRate / 1000.0);  // more than this waiting: the new sound is dropped (the device stopped)

}  // namespace

void Tap::add(const std::int16_t *lr, std::size_t frames) {
    if (!on) return;
    std::lock_guard<std::mutex> lock(mutex_);
    data_.insert(data_.end(), lr, lr + frames * 2);
}

void Tap::take(std::vector<std::int16_t> &out) {
    std::lock_guard<std::mutex> lock(mutex_);
    out.insert(out.end(), data_.begin(), data_.end());
    data_.clear();
}

SoundPipe::SoundPipe(const PipeConfig &config) : config_(config), marks_(512), pace_limit_ms_(config.pace_ms) {}

// ---- emulation thread

void SoundPipe::push(const std::int16_t *lr, std::size_t frames, double now) {
    if (last_push_t_ >= 0.0) {
        const double gap = now - last_push_t_;
        if (gap < kStall) alive_ += std::max(0.0, gap - wait_since_);  // a long silence is the game being held, not slow
    }
    wait_since_ = 0.0;
    last_push_t_ = now;
    last_push_.store(now, std::memory_order_relaxed);
    if (static_cast<double>(stretcher_.head()) - position_.load(std::memory_order_relaxed) > kDropAbove) {
        dropped_ += frames;
        return;
    }
    const std::size_t took = stretcher_.push(lr, frames);
    pushed_ += took;
    dropped_ += frames - took;
}

void SoundPipe::frame(double now) {
    const double dt = pace_frame_time_ < 0.0 ? 0.0 : std::max(0.0, now - pace_frame_time_);
    pace_frame_time_ = now;
    // Keep room to replenish audio while playback is stretched. Once tempo
    // recovers, lower the allowance gradually in wall time: dropping it by
    // 40 ms at once makes the producer wait and introduces another hitch.
    double target = config_.pace_ms;
    if (tempo() < kEnter) {
        // A recent gap must fit together with WSOLA lookahead and one device
        // block. Otherwise pacing can pin the queue below the gap guard's
        // unity-tempo threshold even after the producer has recovered.
        const double reserve = frame_gap_.load(std::memory_order_relaxed) * 1000.0 + kFloorMs + device_block_ms_.load(std::memory_order_relaxed);
        target = std::max(config_.pace_stretch_ms, reserve);
    }
    // Ageing out a long gap must not abruptly drain its extra headroom either.
    pace_limit_ms_ = std::max(target, pace_limit_ms_ - 100.0 * dt);
    double alive = alive_;
    if (last_push_t_ >= 0.0 && now - last_push_t_ < kStall) alive += std::max(0.0, now - last_push_t_ - wait_since_);
    const std::size_t size = marks_.size();
    marks_[marks_next_] = {alive, pushed_};
    marks_next_ = (marks_next_ + 1) % size;
    marks_count_ = std::min(marks_count_ + 1, size);
    const auto back = [&](std::size_t k) -> const Mark & { return marks_[(marks_next_ + size - 1 - k) % size]; };  // 0 is the newest
    const auto speed_over = [&](double window) {  // sound made per second of running time over the last `window`, or -1
        for (std::size_t k = 1; k < marks_count_; ++k)
            if (alive - back(k).alive >= window) return static_cast<double>(pushed_ - back(k).pushed) / (kRate * (alive - back(k).alive));
        return -1.0;
    };
    const double measured = speed_over(kWindow);
    const double rate = measured >= 0.0 ? measured : 1.0;  // unknown until a few frames have gone by
    rate_.store(std::max(0.02, std::min(8.0, rate)), std::memory_order_relaxed);
    double longest = 0.0;
    for (std::size_t k = 1; k < marks_count_ && alive - back(k).alive < kGapWindow; ++k) longest = std::max(longest, back(k - 1).alive - back(k).alive);
    frame_gap_.store(longest, std::memory_order_relaxed);
}

// ---- device thread

double SoundPipe::choose_tempo(double now, double waiting_ms) {
    const double rate = rate_.load(std::memory_order_relaxed), gap = frame_gap_.load(std::memory_order_relaxed);
    const double last = last_push_.load(std::memory_order_relaxed), s_min = 1.0 / config_.max_ratio;
    if (last >= 0.0 && now - last > kHeld) return 1.0;  // the game is held (a window dragged, a message box): what is left plays out at the plain speed
    double s = 1.0;
    if (rate < kHealthy) s = rate + (level_ms_ - config_.target_ms) / 1000.0 / kTrimSeconds;
    // do not plan to use more than what is waiting lasts for until the longest recent frame is done
    if (gap > 0.0) s = std::min(s, std::max(0.0, waiting_ms - kFloorMs) / 1000.0 / gap);
    s = std::max(s_min, std::min(1.0, s));
    if (!stretcher_.stretching() && s > kEnter) s = 1.0;
    return s;
}

void SoundPipe::fade(std::int16_t *lr, std::size_t frames, bool in) {
    const std::size_t n = std::min(frames, kFade);
    for (std::size_t i = 0; i < n; ++i) {
        const float g = (static_cast<float>(i) + 0.5f) / static_cast<float>(kFade);
        const std::size_t at = (in ? i : frames - 1 - i) * 2;
        lr[at] = static_cast<std::int16_t>(std::lrintf(static_cast<float>(lr[at]) * g));
        lr[at + 1] = static_cast<std::int16_t>(std::lrintf(static_cast<float>(lr[at + 1]) * g));
    }
}

void SoundPipe::render(std::int16_t *out, std::size_t frames, double now, void (*overlay)(std::int16_t *, std::size_t)) {
    device_block_ms_.store(frames * 1000.0 / kRate, std::memory_order_relaxed);
    if (flush_.exchange(false, std::memory_order_relaxed)) {
        stretcher_.flush();
        if (config_.stretch && playing_) {
            starved_ = true;
            fade_in_ = true;
        }
    }
    const double waiting_ms = stretcher_.waiting() / (kRate / 1000.0);
    const double dt = last_render_ >= 0.0 ? std::max(0.0, now - last_render_) : 0.0;
    last_render_ = now;
    level_ms_ = level_ms_ < 0.0 ? waiting_ms : level_ms_ + (waiting_ms - level_ms_) * (1.0 - std::exp(-dt / kLevelTau));

    std::size_t made = 0;
    double tempo = 1.0;
    bool capped = false;
    if (!config_.stretch) {
        made = stretcher_.pull(out, frames, 1.0);
    } else {
        if (starved_ && waiting_ms >= config_.restart_ms) {
            starved_ = false;
            if (playing_) fade_in_ = true;
        }
        if (!starved_) {
            const double before = tempo_.load(std::memory_order_relaxed);
            tempo = choose_tempo(now, waiting_ms);
            if (tempo > before && stretcher_.stretching()) {
                // After an isolated pause, the producer can already be healthy
                // and the queue refilled while the old slow tempo still paces
                // the emulation. Recover faster only when the gap guard permits
                // normal playback; sustained slow production keeps its smoothing.
                const bool recovered = tempo == 1.0 && rate_.load(std::memory_order_relaxed) >= kHealthy && waiting_ms >= config_.target_ms;
                const double tau = recovered ? kRecoveredUp : kSmoothUp;
                tempo = before + (tempo - before) * (1.0 - std::exp(-dt / tau));
            }
            if (tempo > kEnter) tempo = 1.0;
            capped = tempo <= 1.0 / config_.max_ratio + 1e-9;
            made = stretcher_.pull(out, frames, tempo);
            if (made < frames) {
                if (made > 0) fade(out, made, false);
                starved_ = true;
            }
        }
    }
    if (made > 0 && fade_in_) {
        fade(out, made, true);
        fade_in_ = false;
    }
    if (made < frames) std::memset(out + made * 2, 0, (frames - made) * 4);
    if (config_.stretch) {
        if (made == 0 && last_live_) {  // the sound just stopped (it ran dry or was cleared): let the last sample die away instead of a step to zero
            for (std::size_t i = 0; i < std::min(frames, kFade); ++i)
                for (int ch = 0; ch < 2; ++ch) out[i * 2 + ch] = static_cast<std::int16_t>(std::lrintf(static_cast<float>(last_[ch]) * (1.0f - (static_cast<float>(i) + 1.0f) / kFade)));
        }
        last_live_ = made == frames;  // a fade (above) already brought the sound to zero when it ended inside this block
        if (made > 0) {
            last_[0] = out[(made - 1) * 2];
            last_[1] = out[(made - 1) * 2 + 1];
        }
    }
    // "ran dry": a piece of sound that ended because there was no more (inside this block, or exactly at the end of the one before)
    std::uint64_t dry = 0;
    if (playing_ && ((made > 0 && made < frames) || (made == 0 && prev_complete_))) dry = 1;
    prev_complete_ = made == frames;
    const bool began = made > 0 && !playing_;
    if (began) {
        playing_ = true;
        first_ = now;
    }
    if (overlay) overlay(out, frames);
    if (made > 0) device_tap.add(out, made);
    if (playing_) heard_tap.add(out, frames);

    tempo_.store(tempo, std::memory_order_relaxed);
    position_.store(stretcher_.position(), std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(stats_mutex_);
    stats_.played += made;
    if (playing_) stats_.silence += frames - made;
    stats_.dry += dry;
    if (stretcher_.stretching() && made > 0) stats_.stretched += made;
    if (capped && made > 0) stats_.at_cap += made;
    if (made > 0) stats_.slowest = std::min(stats_.slowest, tempo);
    stats_.first = first_;
    stats_.last = now;
    stats_.consumed = stretcher_.position();
}

PipeStats SoundPipe::stats() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    PipeStats copy = stats_;
    copy.pushed = stretcher_.head();
    copy.dropped = dropped_;
    return copy;
}

}  // namespace ps2sound
