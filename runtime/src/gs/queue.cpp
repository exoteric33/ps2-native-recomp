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
    std::atomic<std::uint64_t> posts{0}, bytes{0}, sleeps{0}, sleep_us{0}, notifies{0}, full_waits{0}, full_us{0}, empty_polls{0};
    std::uint64_t last_print = 0;
};
QStats g_qs[2];
bool qs_on() { static const bool on = std::getenv("PS2_QUEUE_STATS") != nullptr; return on; }
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
    buffer_ = std::make_unique<std::uint8_t[]>(capacity);
    capacity_ = capacity;
}

void Ring::post(Type type, std::uint32_t arg0, std::uint32_t arg1, const void *data, std::uint32_t bytes) {
    const std::uint64_t need = sizeof(Header) + align16(bytes);
    std::uint64_t head = head_.load(std::memory_order_relaxed);
    for (;;) {
        const std::uint64_t pos = head % capacity_;
        const std::uint64_t pad = pos + need > capacity_ ? capacity_ - pos : 0;
        const std::uint32_t seen = freed_.load(std::memory_order_acquire);
        if (head + pad + need - tail_.load(std::memory_order_acquire) <= capacity_) {
            if (pad) {
                const Header wrap{kWrap, 0, 0, 0};
                std::memcpy(buffer_.get() + pos, &wrap, sizeof wrap);
                head += pad;
            }
            break;
        }
        producer_waiting_.store(1);
        if (head + pad + need - tail_.load() > capacity_) {
            const std::uint64_t t0 = qs_on() ? now_us() : 0;
            wait(freed_, seen);
            if (qs_on()) { QStats &q = g_qs[this == &g_render ? 0 : 1]; q.full_waits.fetch_add(1); q.full_us.fetch_add(now_us() - t0); }
        }
    }
    std::uint8_t *at = buffer_.get() + head % capacity_;
    const Header h{type, bytes, arg0, arg1};
    std::memcpy(at, &h, sizeof h);
    if (bytes) std::memcpy(at + sizeof h, data, bytes);
    head_.store(head + need, std::memory_order_release);
    posted_.fetch_add(1);
    if (qs_on()) { QStats &q = g_qs[this == &g_render ? 0 : 1]; q.posts.fetch_add(1, std::memory_order_relaxed); q.bytes.fetch_add(need, std::memory_order_relaxed); }
    if (consumer_waiting_.exchange(0)) {
        if (qs_on()) g_qs[this == &g_render ? 0 : 1].notifies.fetch_add(1, std::memory_order_relaxed);
        notify(posted_);
    }
}

bool Ring::peek(Header &header, const std::uint8_t *&payload) {
    std::uint64_t tail = tail_.load(std::memory_order_relaxed);
    const std::uint64_t head = head_.load(std::memory_order_acquire);
    while (tail != head) {
        const std::uint64_t pos = tail % capacity_;
        std::memcpy(&header, buffer_.get() + pos, sizeof header);
        if (header.type != kWrap) {
            payload = buffer_.get() + pos + sizeof header;
            return true;
        }
        tail += capacity_ - pos;
        tail_.store(tail, std::memory_order_release);
    }
    return false;
}

void Ring::pop(const Header &header) {
    tail_.store(tail_.load(std::memory_order_relaxed) + sizeof(Header) + align16(header.bytes), std::memory_order_release);
    freed_.fetch_add(1);
    if (producer_waiting_.exchange(0)) notify(freed_);
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
            if (head_.load(std::memory_order_acquire) != tail_.load(std::memory_order_relaxed)) return;
            for (int i = 0; i < 16; ++i) {
#if defined(__x86_64__) || defined(_M_X64)
                __builtin_ia32_pause();
#endif
            }
        } while (std::chrono::steady_clock::now() < until);
    }
#endif
    const std::uint32_t seen = posted_.load();
    consumer_waiting_.store(1);
    if (head_.load() != tail_.load()) {
        consumer_waiting_.store(0);
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
            if (q.last_print) std::fprintf(stderr, "[queue %s] per second: posts %llu, MB %.1f, consumer sleeps %llu (%.0f ms asleep), wakes sent %llu, producer stalls %llu (%.0f ms)\n", this == &g_render ? "render" : "vu",
                (unsigned long long)(q.posts.exchange(0) / 2), q.bytes.exchange(0) / 2.0 / 1048576.0, (unsigned long long)(q.sleeps.exchange(0) / 2), q.sleep_us.exchange(0) / 2000.0, (unsigned long long)(q.notifies.exchange(0) / 2), (unsigned long long)(q.full_waits.exchange(0) / 2), q.full_us.exchange(0) / 2000.0);
            q.last_print = t1;
        }
    } else {
        futex_wait(posted_, seen, timeout_ms);
    }
#else
    (void)seen;
    std::this_thread::sleep_for(std::chrono::milliseconds(std::min(timeout_ms, 1)));
#endif
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
