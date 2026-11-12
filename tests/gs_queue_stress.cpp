// The GS message ring (runtime/src/gs/queue.cpp) under two threads, no game needed: ninja ps2queuestresstest
//
// A producer posts numbered messages of random sizes into a small ring (so it wraps and fills all the time), sometimes in held
// batches, sometimes pausing so the consumer goes to sleep; a consumer checks every message (number, type, size, every payload
// byte), sometimes slowly so the producer waits for space. A third thread calls wake() now and then. Fails on any wrong message and
// on a lost wake-up: the producer never pauses for more than 2 ms, so a consumer sleep (2 s limit) of over 100 ms means the producer
// published without waking it (the third thread's wake() every 250 ms only shortens such a sleep).
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

int run(std::size_t capacity, std::uint32_t messages, std::uint32_t seed) {
    Ring ring;
    ring.start(capacity);
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
        if (rng() % 16 == 0) ring.flush();
        if (rng() % 1024 == 0) {
            ring.flush();
            std::this_thread::sleep_for(std::chrono::microseconds(rng() % 200));   // let the consumer go to sleep
        }
    }
    ring.flush();
    consumer.join();
    waker.join();
    std::printf("ring %zu KiB, %u messages (%u through the heap): %u wrong, %llu consumer sleeps, %llu lost wake-ups\n", capacity >> 10, messages, spilled, errors.load(),
                static_cast<unsigned long long>(sleeps.load()), static_cast<unsigned long long>(lost.load()));
    return errors.load() || lost.load() ? 1 : 0;
}

}  // namespace

int main() {
    int failed = 0;
    failed |= run(64u << 10, 2000000, 1);
    failed |= run(256u << 10, 2000000, 2);
    failed |= run(1u << 20, 1000000, 3);
    std::printf(failed ? "FAIL\n" : "PASS\n");
    return failed;
}
