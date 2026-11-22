// The GS message ring (runtime/src/gs/queue.cpp) under two threads, no game needed: ninja ps2queuestresstest
//
// A producer posts numbered messages of random sizes into a small ring (so it wraps and fills all the time), sometimes in held
// batches, sometimes pausing so the consumer goes to sleep; a consumer checks every message (number, type, size, every payload
// byte), sometimes slowly so the producer waits for space. A third thread calls wake() now and then. Fails on any wrong message and
// on a lost wake-up: the producer never pauses for long (2 ms; about 16 ms on Windows, whose sleeps end on the 15.6 ms timer tick), so
// a consumer sleep (2 s limit) of over 100 ms means the producer published without waking it (the third thread's wake() every 250 ms
// only shortens such a sleep). Some runs hold long batches with a small wake amount (Ring::set_wake_bytes), so a sleeping consumer is
// often left asleep by a held publish and must be woken by the flush or the wait for space that follows; now and
// then the producer flushes and pauses, and the ring must be empty within 100 ms (a flush that forgot the wake it owed would leave the
// consumer asleep). lend_case below checks passing data on as
// pointers between two rings.
//
// Every time limit here assumes the test's threads get a processor when they are woken. When they do not (the test runs at a lower
// priority while builds or a game keep the processor busy), a woken thread can wait seconds before it runs, which looks like a lost
// wake-up although the ring did its part: on Windows at BelowNormal under load, even the ring from before the deferred wake and the
// lending failed every run so. So a ticker thread, at the test's own priority, notes when it was kept waiting;
// a limit that runs out while it was is not counted (the run says how many were let off), and on an idle machine nothing changes.
#include "gs/queue.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <thread>
#include <vector>

using namespace ps2::gs::queue;

namespace {

std::uint8_t pattern(std::uint32_t n, std::uint32_t i) { return static_cast<std::uint8_t>(n * 131u + i * 7u + (i >> 8)); }

std::int64_t now_ms() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

// The ticker sleeps 1 ms at a time (about 16 ms on Windows without a finer timer) and keeps the end of its last wait that took over
// kStallMs. stalled_since(t) says whether the test's threads were kept off the processor at some point after t (or are right now).
class Ticker {
public:
    static constexpr std::int64_t kStallMs = 50;
    void start() {
        last_.store(now_ms());
        thread_ = std::thread([this] {
            while (!stop_.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                const std::int64_t t = now_ms();
                if (t - last_.load() > kStallMs) stall_end_.store(t);
                last_.store(t);
            }
        });
    }
    void stop() {
        stop_.store(true);
        thread_.join();
    }
    bool stalled_since(std::int64_t t) const { return stall_end_.load() >= t || now_ms() - last_.load() > kStallMs; }

private:
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<std::int64_t> last_{0}, stall_end_{-1};
};
Ticker g_ticker;
std::atomic<std::uint64_t> g_let_off{0};   // limits that ran out while the ticker was kept waiting: not counted

int run(std::size_t capacity, std::uint32_t messages, std::uint32_t seed, std::uint64_t wake_bytes = Ring::kWakeBytes, std::uint32_t flush_every = 16) {
    Ring ring;
    ring.start(capacity);
    ring.set_wake_bytes(wake_bytes);
    std::atomic<bool> done{false};
    std::atomic<std::uint32_t> errors{0};
    std::atomic<std::uint64_t> lost{0}, sleeps{0};

    std::thread consumer([&] {
        std::mt19937 rng(seed * 7 + 1);
        std::uint32_t expect = 0;
        while (expect < messages) {
            Header h;
            const std::uint8_t *payload = nullptr;
            if (!ring.peek(h, payload)) {
                const std::int64_t t0 = now_ms();
                ring.wait_for_work(2000);
                sleeps.fetch_add(1);
                if (now_ms() - t0 > 100) {
                    if (g_ticker.stalled_since(t0)) g_let_off.fetch_add(1);
                    else if (lost.fetch_add(1) == 3) {
                        std::printf("FAIL: the consumer slept over 100 ms with work posted 4 times (lost wake-ups), at message %u\n", expect);
                        std::fflush(stdout);
                        std::_Exit(1);
                    }
                }
                continue;
            }
            const std::uint32_t n = h.arg0, bytes = h.bytes;
            bool ok = n == expect && h.type == static_cast<std::uint32_t>(kGif) + (n % 3) && h.arg1 == (n ^ 0x5A5A5A5Au);
            for (std::uint32_t i = 0; ok && i < bytes; ++i) ok = payload[i] == pattern(n, i);
            if (!ok && errors.fetch_add(1) < 5) std::fprintf(stderr, "wrong message: expected %u, got %u (%u bytes, type %u)\n", expect, n, bytes, h.type);
            ring.pop(h);
            ++expect;
            if (rng() % 4096 == 0) std::this_thread::sleep_for(std::chrono::microseconds(rng() % 3000));   // let the ring fill up
        }
        done.store(true);
    });

    std::thread waker([&] {
        while (!done.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            ring.wake();
        }
    });

    std::mt19937 rng(seed);
    // Now and then a message bigger than a quarter of the ring, up to twice the ring: it must go through the heap copy (Ring::kSpilled)
    // and arrive in order like any other.
    std::vector<std::uint8_t> data(capacity * 2);
    std::uint32_t spilled = 0;
    for (std::uint32_t n = 0; n < messages; ++n) {
        if (rng() % 64 == 0) ring.hold();
        const std::uint32_t r = rng() % 100;
        const bool huge = r == 99 && rng() % 8 == 0;
        const std::uint32_t bytes = r < 10 ? 0 : r < 80 ? rng() % 512 : r < 98 ? rng() % 4096 : huge ? static_cast<std::uint32_t>(capacity / 4 + rng() % (capacity * 7 / 4))
                                                                                      : rng() % static_cast<std::uint32_t>(capacity / 4);
        spilled += 16 + ((bytes + 15) & ~15u) > capacity / 4;
        for (std::uint32_t i = 0; i < bytes; ++i) data[i] = pattern(n, i);
        ring.post(static_cast<Type>(kGif + n % 3), n, n ^ 0x5A5A5A5Au, data.data(), bytes);
        if (rng() % flush_every == 0) ring.flush();
        if (rng() % 1024 == 0) {
            ring.flush();
            std::this_thread::sleep_for(std::chrono::microseconds(rng() % 200));   // let the consumer go to sleep
        }
        if (rng() % 32768 == 0) {   // the producer runs out of work: after its flush the consumer must finish everything without more posts
            ring.flush();
            const std::int64_t t0 = now_ms();
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            while (!ring.empty() && now_ms() - t0 < 100) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            if (!ring.empty()) {
                if (g_ticker.stalled_since(t0)) g_let_off.fetch_add(1);
                else if (lost.fetch_add(1) < 3) std::printf("a flush left the consumer asleep with work published, at message %u\n", n);
            }
        }
    }
    ring.flush();
    consumer.join();
    waker.join();
    std::printf("ring %zu KiB, wake after %llu KiB, %u messages (%u through the heap): %u wrong, %llu consumer sleeps, %llu lost wake-ups\n", capacity >> 10, static_cast<unsigned long long>(wake_bytes >> 10), messages, spilled, errors.load(),
                static_cast<unsigned long long>(sleeps.load()), static_cast<unsigned long long>(lost.load()));
    return errors.load() || lost.load() ? 1 : 0;
}

// The case the deferred wake must get right, made on purpose: the consumer is asleep, a held batch reaches Ring::kBatchBytes and is
// published without a wake (less than the wake amount), then the producer flushes with nothing new written and stops. The flush must
// wake the consumer: the ring has to be empty within 100 ms (the consumer's own sleep limit is 2 s).
int deferred_wake_case() {
    Ring ring;
    ring.start(1u << 20);
    constexpr std::uint32_t kRounds = 200;
    std::atomic<std::uint32_t> got{0};
    std::thread consumer([&] {
        while (got.load() < kRounds) {
            Header h;
            const std::uint8_t *payload = nullptr;
            if (!ring.peek(h, payload)) {
                ring.wait_for_work(2000);
                continue;
            }
            ring.pop(h);
            got.fetch_add(1);
        }
    });
    std::vector<std::uint8_t> data(Ring::kBatchBytes + 4096, 0x5A);
    std::uint32_t late = 0;
    for (std::uint32_t n = 0; n < kRounds; ++n) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));   // the consumer goes to sleep
        ring.hold();
        ring.post(kGif, n, 0, data.data(), static_cast<std::uint32_t>(data.size()));   // over kBatchBytes: published at once, held
        ring.flush();                                                                   // nothing new: only the owed wake is left
        const std::int64_t t0 = now_ms();
        while (!ring.empty() && now_ms() - t0 < 100) std::this_thread::yield();
        if (!ring.empty()) {
            if (g_ticker.stalled_since(t0)) g_let_off.fetch_add(1);
            else ++late;
        }
    }
    consumer.join();
    std::printf("deferred wake: %u rounds, %u left asleep after the flush\n", kRounds, late);
    return late ? 1 : 0;
}

}  // namespace

// Lending (Ring::lend_into) with three threads, as the vector-unit thread uses it: a producer fills ring A, a middle
// thread reads A and passes each message on into ring B (whole, in pieces, as a copy from its own buffer, or only partly), holding
// batches as that thread does, and a consumer checks every message of B. The pieces that lie in A's message go as pointers, so A must
// keep that space until the consumer has popped them: the producer writes new patterns as soon as A gives space back, so a message
// given back too early shows as wrong bytes. The consumer is sometimes slow (A then keeps more than a quarter of itself and the middle
// thread must wait for the consumer) and the producer sometimes pauses (the middle thread sleeps with messages still kept). Fails on any
// wrong message or when nothing moves for 1 s (a hang, or a sleep that only its time limit ended).
int lend_case(std::size_t cap_a, std::size_t cap_b, std::uint32_t messages, std::uint32_t seed) {
    Ring a, b;
    a.start(cap_a);
    b.start(cap_b);
    std::atomic<std::uint32_t> errors{0}, got{0};
    std::atomic<bool> done{false};
    std::atomic<std::uint64_t> lent{0}, copied{0};

    std::thread middle([&] {
        a.lend_into(b);
        std::mt19937 rng(seed * 3 + 2);
        std::vector<std::uint8_t> own(cap_a);
        std::uint32_t seen = 0;
        while (seen < messages) {
            Header h;
            const std::uint8_t *payload = nullptr;
            b.hold();
            while (a.peek(h, payload)) {
                const std::uint32_t n = h.arg0, bytes = h.bytes, way = rng() % 8;
                // each message of A becomes one or more messages of B: (number, offset) in the arguments, the bytes in the payload
                if (way == 0 && bytes) {   // through a copy of its own (must be copied)
                    std::memcpy(own.data(), payload, bytes);
                    b.post(kGif, n, 0, own.data(), bytes);
                    copied.fetch_add(1, std::memory_order_relaxed);
                } else if (way == 1 && bytes > 64) {   // in three pieces
                    const std::uint32_t c1 = bytes / 3 & ~15u, c2 = bytes * 2 / 3 & ~15u;
                    b.post(kGif, n, 0, payload, c1);
                    b.post(kGif, n, c1, payload + c1, c2 - c1);
                    b.post(kGif, n, c2, payload + c2, bytes - c2);
                } else if (way == 2 && bytes > 32) {   // only its middle
                    b.post(kGif, n, 16, payload + 16, bytes - 32);
                } else {
                    b.post(kGif, n, 0, payload, bytes);
                }
                if (bytes >= Ring::kLendMin && way != 0) lent.fetch_add(1, std::memory_order_relaxed);
                a.pop(h);
                ++seen;
                if (rng() % 16 == 0) {
                    b.flush();
                    b.hold();
                }
            }
            b.flush();
            if (seen < messages) a.wait_for_work(2000);
        }
        b.post(kVblank, ~0u, 0, nullptr, 0);   // the end
        b.flush();
    });

    std::thread consumer([&] {
        std::mt19937 rng(seed * 5 + 3);
        for (;;) {
            Header h;
            const std::uint8_t *payload = nullptr;
            if (!b.peek(h, payload)) {
                b.wait_for_work(2000);
                continue;
            }
            if (h.type == kVblank) break;
            const std::uint32_t n = h.arg0, off = h.arg1;
            bool ok = h.type == kGif;
            for (std::uint32_t i = 0; ok && i < h.bytes; ++i) ok = payload[i] == pattern(n, off + i);
            if (!ok && errors.fetch_add(1) < 5) std::fprintf(stderr, "lend: wrong bytes in message %u (offset %u, %u bytes)\n", n, off, h.bytes);
            b.pop(h);
            got.store(n, std::memory_order_relaxed);
            if (rng() % 2048 == 0) std::this_thread::sleep_for(std::chrono::microseconds(rng() % 4000));   // let A keep a lot
        }
        done.store(true);
    });

    std::thread watchdog([&] {
        std::uint32_t last = ~0u;
        std::int64_t moved = now_ms();   // when the consumer was last seen to move on
        for (int still = 0; !done.load(); std::this_thread::sleep_for(std::chrono::milliseconds(100))) {
            const std::uint32_t now = got.load();
            still = now == last ? still + 1 : 0;
            if (!still) moved = now_ms();
            last = now;
            if (still == 10 && g_ticker.stalled_since(moved)) {   // the test's threads were kept off the processor: say nothing yet
                g_let_off.fetch_add(1);
                still = 0;
                moved = now_ms();
            }
            if (still == 10) {   // the middle thread sleeps at most 2 s at a time: a stall of 1 s means it slept with work to do
                std::printf("FAIL: lending stood still for 1 s at message %u\n", now);
                std::fflush(stdout);
                std::_Exit(1);
            }
        }
    });

    std::mt19937 rng(seed);
    std::vector<std::uint8_t> data(cap_a / 4);
    for (std::uint32_t n = 0; n < messages; ++n) {
        const std::uint32_t r = rng() % 100;
        const std::uint32_t bytes = r < 10 ? 0 : r < 60 ? rng() % 1024 : r < 95 ? rng() % 8192 : rng() % static_cast<std::uint32_t>(cap_a / 4 - 32);
        for (std::uint32_t i = 0; i < bytes; ++i) data[i] = pattern(n, i);
        a.post(kGif, n, 0, data.data(), bytes);
        if (rng() % 2048 == 0) std::this_thread::sleep_for(std::chrono::microseconds(rng() % 500));   // the middle thread sleeps
    }
    middle.join();
    consumer.join();
    watchdog.join();
    std::printf("lend: rings %zu and %zu KiB, %u messages (%llu passed on as pointers, %llu copied first): %u wrong\n", cap_a >> 10, cap_b >> 10, messages,
                static_cast<unsigned long long>(lent.load()), static_cast<unsigned long long>(copied.load()), errors.load());
    return errors.load() ? 1 : 0;
}

int main() {
    g_ticker.start();
    int failed = deferred_wake_case();
    failed |= lend_case(256u << 10, 64u << 10, 400000, 7);
    failed |= lend_case(1u << 20, 256u << 10, 400000, 8);
    failed |= lend_case(256u << 10, 1u << 20, 400000, 9);
    failed |= run(64u << 10, 2000000, 1);
    failed |= run(256u << 10, 2000000, 2);
    failed |= run(1u << 20, 1000000, 3);
    failed |= run(64u << 10, 2000000, 4, 4u << 10, 256);   // held batches that find the consumer asleep and do not wake it
    failed |= run(256u << 10, 2000000, 5, 1u << 10, 256);
    failed |= run(1u << 20, 1000000, 6, 0, 256);            // 0: every held publish wakes, as before
    g_ticker.stop();
    if (g_let_off.load())
        std::printf("%llu time limits ran out while the test's threads were kept off the processor (not counted)\n", static_cast<unsigned long long>(g_let_off.load()));
    std::printf(failed ? "FAIL\n" : "PASS\n");
    return failed;
}
