#include "queue.hpp"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <ctime>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <emscripten/threading.h>
#elif defined(__linux__)
#include <ctime>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#elif defined(_WIN32)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <chrono>
#include <thread>
#endif

namespace ps2::gs::queue {

namespace {

constexpr std::uint64_t align16(std::uint64_t n) { return (n + 15) & ~15ull; }

Ring g_render, g_vu;
thread_local Role g_role = Role::Emulation;

// PS2_QUEUE_STATS=1 prints, every 2 s per ring, how the hand-over behaves.
struct QStats {
    std::atomic<std::uint64_t> posts{0}, publishes{0}, bytes{0}, sleeps{0}, sleep_us{0}, notifies{0}, full_waits{0}, full_us{0}, empty_polls{0};
    std::uint64_t last_print = 0;
};
QStats g_qs[2];
bool qs_on() { static const bool on = std::getenv("PS2_QUEUE_STATS") != nullptr; return on; }
// PS2_QUEUE_MAX=1: prints each new largest message per ring once it is over 4 KiB (the last line of a run is the largest), to size the
// rings.
bool qmax_on() { static const bool on = std::getenv("PS2_QUEUE_MAX") != nullptr; return on; }
std::atomic<std::uint32_t> g_qmax[2];
void note_size(int ring, std::uint32_t type, std::uint32_t bytes) {
    std::uint32_t old = g_qmax[ring].load(std::memory_order_relaxed);
    while (bytes > old && !g_qmax[ring].compare_exchange_weak(old, bytes)) {}
    if (bytes > old && bytes > 4096) std::fprintf(stderr, "[queue %s] largest message so far: %u bytes (type %u)\n", ring ? "vu" : "render", bytes, type);
}
std::uint64_t now_us() { return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

#if defined(__linux__) && !defined(__EMSCRIPTEN__)
void futex_wait(std::atomic<std::uint32_t> &word, std::uint32_t seen, int timeout_ms) {
    timespec limit{timeout_ms / 1000, static_cast<long>(timeout_ms % 1000) * 1000000L};
    syscall(SYS_futex, reinterpret_cast<std::uint32_t *>(&word), FUTEX_WAIT_PRIVATE, seen, timeout_ms < 0 ? nullptr : &limit, nullptr, 0);
}

void futex_wake(std::atomic<std::uint32_t> &word) { syscall(SYS_futex, reinterpret_cast<std::uint32_t *>(&word), FUTEX_WAKE_PRIVATE, INT_MAX, nullptr, nullptr, 0); }
#elif defined(_WIN32)
void futex_wait(std::atomic<std::uint32_t> &word, std::uint32_t seen, int timeout_ms) {
    WaitOnAddress(&word, &seen, sizeof seen, timeout_ms < 0 ? INFINITE : static_cast<DWORD>(timeout_ms));
}

void futex_wake(std::atomic<std::uint32_t> &word) { WakeByAddressAll(&word); }
#endif

}

#ifdef __EMSCRIPTEN__
EM_ASYNC_JS(void, ps2_wait_async, (std::atomic<std::uint32_t> *word, std::uint32_t seen, int timeout_ms), {
    const result = Atomics.waitAsync(HEAP32, word >> 2, seen, timeout_ms);
    if (result.async) await result.value;
});
#endif

void wait(std::atomic<std::uint32_t> &value, std::uint32_t seen) {
#ifdef __EMSCRIPTEN__
    emscripten_futex_wait(&value, seen, INFINITY);
#elif defined(__linux__) || defined(_WIN32)
    futex_wait(value, seen, -1);
#else
    value.wait(seen);
#endif
}

void notify(std::atomic<std::uint32_t> &value) {
#ifdef __EMSCRIPTEN__
    emscripten_futex_wake(&value, INT_MAX);
#elif defined(__linux__) || defined(_WIN32)
    futex_wake(value);
#else
    value.notify_all();
#endif
}

Ring &render() { return g_render; }
Ring &vu() { return g_vu; }
Role &role() { return g_role; }

Ring &outbox() {
    if (g_role == Role::Emulation && g_vu.active()) return g_vu;
    return g_render;
}

bool active() { return g_render.active(); }
bool vu_active() { return g_vu.active(); }

void post(Type type, std::uint32_t arg0, std::uint32_t arg1, const void *data, std::uint32_t bytes) { outbox().post(type, arg0, arg1, data, bytes); }

void Ring::start(std::size_t capacity) {
    while (capacity & (capacity - 1)) capacity &= capacity - 1;   // a power of two (8 MiB for the drawing thread, 32 MiB for the vector-unit thread), so a position is a mask away
    buffer_ = std::make_unique<std::uint8_t[]>(capacity);
    capacity_ = capacity;
    mask_ = capacity - 1;
    spill_above_ = capacity / 4;
}

void Ring::post(Type type, std::uint32_t arg0, std::uint32_t arg1, const void *data, std::uint32_t bytes) {
    if (sizeof(Header) + align16(bytes) > spill_above_) [[unlikely]] post_spilled(type, arg0, arg1, data, bytes);
    else put(Header{type, bytes, arg0, arg1}, data, bytes);
    if (qs_on()) { QStats &q = g_qs[this == &g_render ? 0 : 1]; q.posts.fetch_add(1, std::memory_order_relaxed); q.bytes.fetch_add(sizeof(Header) + align16(bytes), std::memory_order_relaxed); }
    if (qmax_on()) [[unlikely]] note_size(this == &g_render ? 0 : 1, type, bytes);
}

// Writes one message (the header and ring_bytes of data) at the head; ring_bytes is never more than a quarter of the ring (post()).
void Ring::put(const Header &header, const void *data, std::uint32_t ring_bytes) {
    const std::uint64_t need = sizeof(Header) + align16(ring_bytes);
    std::uint64_t head = write_;
    const std::uint64_t pos = head & mask_;
    const std::uint64_t pad = pos + need > capacity_ ? capacity_ - pos : 0;
    // Room up to the tail read last time; only when that is not enough is the consumer's tail read again (a line the other core writes).
    // need <= capacity / 4, so the tail waited for is never past the head: with a wrap pos > capacity - need >= need.
    if (head + pad + need - tail_seen_ > capacity_) [[unlikely]] {
        tail_seen_ = tail_.load(std::memory_order_acquire);
        if (head + pad + need - tail_seen_ > capacity_) wait_for_space(head + pad + need - capacity_);
    }
    if (pad) {
        const Header wrap{kWrap, 0, 0, 0};
        std::memcpy(buffer_.get() + pos, &wrap, sizeof wrap);
        head += pad;
    }
    std::uint8_t *at = buffer_.get() + (head & mask_);
    std::memcpy(at, &header, sizeof header);
    if (ring_bytes) std::memcpy(at + sizeof header, data, ring_bytes);
    write_ = head + need;
    if (!holding_ || write_ - published_ >= kBatchBytes) publish();
}

// A message too big for the ring: its data goes to a heap copy, the ring carries the header (type marked kSpilled, the real size) and the
// pointer. Never seen in the games measured; it is here so no size can hang or overwrite the ring.
void Ring::post_spilled(Type type, std::uint32_t arg0, std::uint32_t arg1, const void *data, std::uint32_t bytes) {
    auto *copy = static_cast<std::uint8_t *>(std::malloc(bytes));
    if (!copy) {
        std::fprintf(stderr, "[queue] out of memory for a message of %u bytes\n", bytes);
        std::abort();
    }
    std::memcpy(copy, data, bytes);
    put(Header{static_cast<std::uint32_t>(type) | kSpilled, bytes, arg0, arg1}, &copy, sizeof copy);
}

// Makes everything written visible to the consumer and wakes it if it sleeps. The head store and the flag load are seq_cst, and so are
// the consumer's flag store and head load before it sleeps (wait_for_work): of two such pairs one always sees the other's store, so
// either this sees the flag or the consumer sees the new head and does not sleep. On x86 that is one locked instruction (the store);
// the load is a plain read of a line the consumer writes only when it goes to sleep.
void Ring::publish() {
    if (qs_on()) g_qs[this == &g_render ? 0 : 1].publishes.fetch_add(1, std::memory_order_relaxed);
    published_ = write_;
    head_.store(write_, std::memory_order_seq_cst);
    if (consumer_waiting_.load(std::memory_order_seq_cst)) [[unlikely]] {
        if (consumer_waiting_.exchange(0)) {
            posted_.fetch_add(1);
            if (qs_on()) g_qs[this == &g_render ? 0 : 1].notifies.fetch_add(1, std::memory_order_relaxed);
            notify(posted_);
        }
    }
}

// The ring is full: publish what is written (the consumer must see it to make room) and sleep until the tail reaches `until_tail`.
// The consumer checks producer_waiting_ after each pop with a plain load (no fence there), and with a seq_cst one whenever it finds the
// ring empty or goes to sleep; a wake missed between the two costs at most the 1 ms the sleep is bounded by, and needs the whole ring
// unread.
void Ring::wait_for_space(std::uint64_t until_tail) {
    if (write_ != published_) publish();
    const std::uint64_t t0 = qs_on() ? now_us() : 0;
    for (;;) {
        const std::uint32_t seen = freed_.load(std::memory_order_acquire);
        producer_waiting_.store(1, std::memory_order_seq_cst);
        tail_seen_ = tail_.load(std::memory_order_seq_cst);
        if (tail_seen_ >= until_tail) break;
#if defined(__EMSCRIPTEN__)
        emscripten_futex_wait(&freed_, seen, 1.0);
#elif defined(__linux__) || defined(_WIN32)
        futex_wait(freed_, seen, 1);
#else
        (void)seen;
        std::this_thread::sleep_for(std::chrono::microseconds(100));
#endif
    }
    producer_waiting_.store(0, std::memory_order_relaxed);
    if (qs_on()) { QStats &q = g_qs[this == &g_render ? 0 : 1]; q.full_waits.fetch_add(1); q.full_us.fetch_add(now_us() - t0); }
}

void Ring::wake_producer() {
    if (producer_waiting_.exchange(0)) {
        freed_.fetch_add(1);
        notify(freed_);
    }
}

bool Ring::peek(Header &header, const std::uint8_t *&payload) {
    for (;;) {
        // The producer's head is read again only when every message up to the one read last time is done.
        if (read_ == head_seen_) {
            head_seen_ = head_.load(std::memory_order_acquire);
            if (read_ == head_seen_) {
                if (producer_waiting_.load(std::memory_order_relaxed)) [[unlikely]] wake_producer();
                return false;
            }
        }
        const std::uint64_t pos = read_ & mask_;
        std::memcpy(&header, buffer_.get() + pos, sizeof header);
        if (header.type != kWrap) {
            payload = buffer_.get() + pos + sizeof header;
            spilled_ = nullptr;
            if (header.type & kSpilled) [[unlikely]] {
                header.type &= ~kSpilled;
                std::memcpy(&spilled_, payload, sizeof spilled_);
                payload = spilled_;
            }
            return true;
        }
        read_ += capacity_ - pos;
        tail_.store(read_, std::memory_order_release);
    }
}

// The space goes back with a plain store (release); the producer reads the tail only when its own copy says the ring is full.
void Ring::pop(const Header &header) {
    if (spilled_) [[unlikely]] {   // a heap copy: the ring held only its pointer
        read_ += sizeof(Header) + align16(sizeof spilled_);
        std::free(spilled_);
        spilled_ = nullptr;
    } else {
        read_ += sizeof(Header) + align16(header.bytes);
    }
    tail_.store(read_, std::memory_order_release);
    if (producer_waiting_.load(std::memory_order_relaxed)) [[unlikely]] wake_producer();
}

void Ring::wake() {
    posted_.fetch_add(1);
    if (consumer_waiting_.exchange(0)) notify(posted_);
}

// Looking for new work for a few microseconds before going to sleep: a sleep and the wake-up of the other thread are two system calls
// (several microseconds each, 6000 to 15000 times a second between the vector-unit thread and the window thread in a fight), and the
// next message is usually less than 20 microseconds away. PS2_SPIN_US=<n> sets how long. Measured on the laptop (docs/speed.md, lane cpu):
// it does not make the game faster (60 fps is reached without it) and costs 3 to 5 % more processor time, so the default is 0 (off).
static int spin_us() {
    static const int us = [] {
        const char *text = std::getenv("PS2_SPIN_US");
        return text ? std::atoi(text) : 0;
    }();
    return us;
}

void Ring::wait_for_work(int timeout_ms) {
#if (defined(__linux__) || defined(_WIN32)) && !defined(__EMSCRIPTEN__)
    if (const int us = spin_us(); us > 0) {
        const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(us);
        do {
            if (head_.load(std::memory_order_acquire) != read_) return;
            for (int i = 0; i < 16; ++i) {
#if defined(__x86_64__) || defined(_M_X64)
                __builtin_ia32_pause();
#endif
            }
        } while (std::chrono::steady_clock::now() < until);
    }
#endif
    // The other half of the pair in publish(): the flag store and the head load are seq_cst, so either the producer sees the flag
    // and wakes this thread, or this sees its new head and does not sleep.
    const std::uint32_t seen = posted_.load(std::memory_order_acquire);
    consumer_waiting_.store(1, std::memory_order_seq_cst);
    if (producer_waiting_.load(std::memory_order_seq_cst)) wake_producer();
    head_seen_ = head_.load(std::memory_order_seq_cst);
    if (head_seen_ != read_) {
        consumer_waiting_.store(0, std::memory_order_relaxed);
        return;
    }
#ifdef __EMSCRIPTEN__
    if (emscripten_is_main_browser_thread()) ps2_wait_async(&posted_, seen, timeout_ms);
    else emscripten_futex_wait(&posted_, seen, timeout_ms);
#elif defined(__linux__) || defined(_WIN32)
    if (qs_on()) {
        QStats &q = g_qs[this == &g_render ? 0 : 1];
        const std::uint64_t t0 = now_us();
        futex_wait(posted_, seen, timeout_ms);
        const std::uint64_t t1 = now_us();
        q.sleeps.fetch_add(1);
        q.sleep_us.fetch_add(t1 - t0);
        if (t1 - q.last_print > 2000000) {
            if (q.last_print) std::fprintf(stderr, "[queue %s] per second: posts %llu (published %llu times), MB %.1f, consumer sleeps %llu (%.0f ms asleep), wakes sent %llu, producer stalls %llu (%.0f ms)\n", this == &g_render ? "render" : "vu",
                (unsigned long long)(q.posts.exchange(0) / 2), (unsigned long long)(q.publishes.exchange(0) / 2), q.bytes.exchange(0) / 2.0 / 1048576.0, (unsigned long long)(q.sleeps.exchange(0) / 2), q.sleep_us.exchange(0) / 2000.0, (unsigned long long)(q.notifies.exchange(0) / 2), (unsigned long long)(q.full_waits.exchange(0) / 2), q.full_us.exchange(0) / 2000.0);
            q.last_print = t1;
        }
    } else {
        futex_wait(posted_, seen, timeout_ms);
    }
#else
    (void)seen;
    std::this_thread::sleep_for(std::chrono::milliseconds(std::min(timeout_ms, 1)));
#endif
    consumer_waiting_.store(0, std::memory_order_relaxed);   // awake (woken, timed out or never asleep): no wake needed until the next sleep
}

}

#include "ps2/hitch.hpp"

namespace ps2::hitch {

double thread_cpu_ms() {
#ifdef _WIN32
    FILETIME created, exited, kernel, user;
    if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) return 0.0;
    const auto ticks = [](const FILETIME &t) { return static_cast<double>((static_cast<unsigned long long>(t.dwHighDateTime) << 32) | t.dwLowDateTime); };
    return (ticks(kernel) + ticks(user)) / 1e4;
#elif defined(CLOCK_THREAD_CPUTIME_ID)
    timespec time{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &time) != 0) return 0.0;
    return static_cast<double>(time.tv_sec) * 1000.0 + static_cast<double>(time.tv_nsec) / 1e6;
#else
    return 0.0;
#endif
}

}  // namespace ps2::hitch
