#pragma once

// The tile lists of a drawing batch: which 8 x 8 screen tiles every shape touches, how many shapes each tile has, the table of the touched tiles and the
// per-tile lists of shape numbers. The raster program of gs.wgsl reads the table and the lists; gpu.cpp's flush_batch asks for them when a batch ends.
//
// The work is one function of the batch's shape records (`build`), so it can run on a helper thread: the window thread hands the records over, goes on
// with the rest of the flush, and gpu.cpp puts the finished lists into the arena before the frame is uploaded (finish_tiles in gpu.cpp). With
// PS2_ONE_THREAD=1, in the browser build, without a render ring, or unless PS2_TILE_THREAD=1 is set (it is off by default for now) the lists are built
// in place by the caller, as they always were.
//
// What the shape record holds that the lists need: word 1 (kind in the low byte: 0 triangle, 1 sprite, 2 line, 3 point), words 2 and 3 (the box,
// x | y << 16, first and last pixel), words 4 to 9 (the three corners) and word 10 (the reciprocal of twice the triangle's area), see draw() in gpu.cpp.

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>

namespace ps2::gs::gpu::tiles {

constexpr std::uint32_t kPrimWords = 28;  // 32-bit words of one shape record (gpu.cpp checks that it has the same number)
constexpr std::uint32_t kKindMask = 0xFFu;
constexpr std::size_t kMaxBacklog = 64;  // started jobs not yet placed; at this many the window thread builds the next batch's lists itself (the helper's queue holds 1024). 768 held
                                         // up to 1.2 GB of job buffers on the laptop
constexpr std::uint32_t kMinJob = 16;  // batches of fewer shapes are built in place by the window thread, never handed to the helper
// A batch that grows past kStreamStep shapes is handed to the helper while it is still being drawn (open/feed/close below), kStreamStep shapes at a
// time: the helper makes the spans and counts of those shapes while this thread draws the next ones, so at the flush only the table and the lists are
// left. The God of War ship fight ends a frame with one batch of about 45,000 shapes that an upload flushes and the submit right after needs.
constexpr std::uint32_t kStreamStep = 1024;
constexpr std::size_t kKeepWords = 1u << 20;  // a released job frees a buffer that grew past this (4 MB) instead of keeping it for the next job

// A growing array of 32-bit words. std::vector's insert costs more than the copy it makes (it runs once per shape, about 50,000 times a frame), and
// growing by hand does not zero the new words first.
struct Words {
    std::unique_ptr<std::uint32_t[]> words;
    std::size_t used = 0, capacity = 0;

    void append(const std::uint32_t *from, std::size_t count) {
        if (used + count > capacity) grow(used + count);
        std::memcpy(words.get() + used, from, count * 4);
        used += count;
    }
    void grow(std::size_t need) {
        std::size_t next = std::max<std::size_t>(capacity * 2, 1u << 16);
        while (next < need) next *= 2;
        auto bigger = std::make_unique_for_overwrite<std::uint32_t[]>(next);
        if (used) std::memcpy(bigger.get(), words.get(), used * 4);
        words = std::move(bigger);
        capacity = next;
    }
    // Room for `count` words, content not kept (the lists of a batch are written from the start).
    std::uint32_t *discard_and_reserve(std::size_t count) {
        if (count > capacity) {
            std::size_t next = std::max<std::size_t>(capacity * 2, 1u << 14);
            while (next < count) next *= 2;
            words = std::make_unique_for_overwrite<std::uint32_t[]>(next);
            capacity = next;
        }
        used = count;
        return words.get();
    }
    void swap(Words &other) {
        words.swap(other.words);
        std::swap(used, other.used);
        std::swap(capacity, other.capacity);
    }
    std::size_t size() const { return used; }
    bool empty() const { return !used; }
    void clear() { used = 0; }
    void free_memory() {
        words.reset();
        used = capacity = 0;
    }
    std::uint32_t *data() { return words.get(); }
    const std::uint32_t *data() const { return words.get(); }
    std::uint32_t &operator[](std::size_t i) { return words[i]; }
    const std::uint32_t &operator[](std::size_t i) const { return words[i]; }
};

// Called once from init(): the resolution scale and the tile size in pixels (8: one workgroup of the raster program).
void configure(int scale, int tile);

// The lists of one batch: a table of `tiles` entries (tile x | y << 16, then the offset of its list), a closing entry, then the lists, all in one
// array of `words` words. The table's offsets count from the start of the lists, so the whole array can sit anywhere in the arena.
struct Lists {
    std::uint32_t tiles = 0, total = 0, words = 0;  // total: entries in all the lists
};

// Where build_now writes: the callback returns room for that many words (the arena, in gpu.cpp).
using Room = std::uint32_t *(*)(void *context, std::size_t words);

// Builds the lists now, on the calling thread (the window thread's own scratch).
Lists build_now(const std::uint32_t *prims, std::uint32_t count, Room room, void *context);

// ---- the helper thread --------------------------------------------------------------------------------------------------------------------

struct Job {
    // set by start() (or open, feed and close)
    std::atomic<const std::uint32_t *> prims{nullptr};  // the batch's shape records, read by the helper until `done`; an open batch's may move (the
                                                        // array grew), the new place holds the same records
    std::atomic<std::uint32_t> count{0};                // shapes handed over so far (all of them once `state` is kClosed)
    std::atomic<std::uint32_t> state{0};                // kClosed, kOpen (more shapes may come) or kCancelled (the lists are not wanted)
    std::atomic<std::uint32_t> done{0};
    std::uint64_t ticket = 0;                    // its place in the helper's queue
    std::atomic<std::uint64_t> claim{~0ull};     // `ticket` while nobody builds it; kTaken once the helper or the window thread (build_waiting) took it
    // written by the helper, read by the window thread after `done`
    Words lists;  // the table and the lists
    Lists result;
    // for the window thread (finish_tiles in gpu.cpp)
    std::uint32_t param = 0, command = 0;  // the parameter record and the command that wait for the numbers
    std::uint64_t held = 0;                 // words of its list buffer when it was last released (PS2_TILE_LOG's memory line)
};

// True when batches go to the helper (PS2_TILE_THREAD=1 in a threaded run, 2 in any run; off when the variable is not set).
bool threaded();

// Hands the records of a batch to the helper. The records must stay where they are, unchanged, until the job is released (gpu.cpp keeps them in the
// frame's shape array, g_shapes, and an old block of it until the submit that waits for every job).
Job *start(const std::uint32_t *prims, std::uint32_t count);
// True on the thread that starts and places the jobs, or when there are none. Others (the emulation thread at the end of a run, while the window thread
// may still be drawing) must not place them.
bool owner();
// Jobs started and not yet released.
std::size_t backlog();
// The window thread, before it waits for all jobs: builds the newest jobs the helper has not taken yet itself (instead of waiting while the helper works
// through them one by one), until it meets one the helper has taken.
void build_waiting();
// A batch still being drawn (see kStreamStep): open hands over its first `count` records, feed the records drawn since (`prims` is where the batch's
// records are now: the same records, maybe moved), close the last ones when the batch ends; from then on it is a job like those of start(). A job is
// open only while it is the newest. cancel drops an open job (its batch's records are dropped or move at a submit): it waits until the helper lets go
// of it and releases it; the batch's lists are then built in another way.
Job *open(const std::uint32_t *prims, std::uint32_t count);
void feed(Job *job, const std::uint32_t *prims, std::uint32_t count);
void close(Job *job, const std::uint32_t *prims, std::uint32_t count);
void cancel(Job *job);
// The oldest job when its lists are built; when `wait`, waits until they are. nullptr when there is none (or, without `wait`, it is not ready).
Job *next_finished(bool wait);
// The lists have been copied away: the job and its buffers can be used again.
void release(Job *job);

// PS2_TILE_LOG=1: a fingerprint of every batch's lists (the same for the same lists wherever they were built) and the helper's work, printed when the program ends.
bool logging();
void note_lists(const std::uint32_t *words, const Lists &lists);
void report();  // prints the lines (gpu::stats at the end of a run)

}
