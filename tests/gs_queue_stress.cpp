// The GS message ring (runtime/src/gs/queue.cpp) under two threads, no game needed: ninja ps2queuestresstest
//
// A producer posts numbered messages of random sizes into a small ring (so it wraps and fills all the time), sometimes in held
// batches, sometimes pausing so the consumer goes to sleep; a consumer checks every message (number, type, size, every payload
// byte), sometimes slowly so the producer waits for space. A third thread calls wake() now and then. Fails on any wrong message and
// on a lost wake-up: the producer never pauses for more than 2 ms, so a consumer sleep (2 s limit) of over 100 ms means the producer
// published without waking it (the third thread's wake() every 250 ms only shortens such a sleep). Some runs hold long batches with a
// small wake amount (Ring::set_wake_bytes), so a sleeping consumer is often left asleep by a held publish and must be woken by the
// flush or the wait for space that follows; now and then the producer flushes and pauses 30 ms, and the ring must be
// empty by then (a flush that forgot the wake it owed would leave the consumer asleep).
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
                const auto t0 = std::chrono::steady_clock::now();
                ring.wait_for_work(2000);
                sleeps.fetch_add(1);
                if (std::chrono::steady_clock::now() - t0 > std::chrono::milliseconds(100) && lost.fetch_add(1) == 3) {
                    std::printf("FAIL: the consumer slept over 100 ms with work posted 4 times (lost wake-ups), at message %u\n", expect);
                    std::fflush(stdout);
                    std::_Exit(1);
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
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            if (!ring.empty() && lost.fetch_add(1) < 3) std::printf("a flush left the consumer asleep with work published, at message %u\n", n);
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
// wake the consumer: the ring has to be empty within 50 ms (the consumer's own sleep limit is 2 s).
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
        const auto t0 = std::chrono::steady_clock::now();
        while (!ring.empty() && std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(50)) std::this_thread::yield();
        late += !ring.empty();
    }
    consumer.join();
    std::printf("deferred wake: %u rounds, %u left asleep after the flush\n", kRounds, late);
    return late ? 1 : 0;
}

}  // namespace

int main() {
    int failed = deferred_wake_case();
    failed |= run(64u << 10, 2000000, 1);
    failed |= run(256u << 10, 2000000, 2);
    failed |= run(1u << 20, 1000000, 3);
    failed |= run(64u << 10, 2000000, 4, 4u << 10, 256);   // held batches that find the consumer asleep and do not wake it
    failed |= run(256u << 10, 2000000, 5, 1u << 10, 256);
    failed |= run(1u << 20, 1000000, 6, 0, 256);            // 0: every held publish wakes, as before
    std::printf(failed ? "FAIL\n" : "PASS\n");
    return failed;
}
