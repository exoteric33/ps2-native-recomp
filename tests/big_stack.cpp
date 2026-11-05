// BigStackThread (host/big_stack_thread.hpp) gives the emulation thread a stack of 64 MB. This test nests a function deeper than
// the 1 MB that Windows gives a normal thread (public issue 42, Tekken 5) and proves the thread has the stack it asked for.
// Usage: big_stack [megabytes of stack to use, default 32]. Built with PS2_SMALL_STACK defined (CMake option PS2_WIN_SMALL_STACK
// for ps2run; by hand for this test) it must crash on Windows (C00000FD), which shows the test can fail. No game needed.
#include "../host/big_stack_thread.hpp"

#include <cstdio>
#include <cstdlib>

namespace {

constexpr std::size_t kFrameBytes = 16 * 1024;

// One call level that really uses kFrameBytes of stack: it touches one byte per page, and the sum keeps the compiler from dropping it.
__attribute__((noinline)) unsigned long nest(unsigned long levels) {
    volatile char frame[kFrameBytes];
    for (std::size_t i = 0; i < kFrameBytes; i += 4096) frame[i] = static_cast<char>(levels);
    const unsigned long below = levels ? nest(levels - 1) : 0;
    return below + frame[0] + frame[kFrameBytes - 4096];
}

}  // namespace

int main(int argc, char **argv) {
    const std::size_t megabytes = argc > 1 ? std::strtoul(argv[1], nullptr, 10) : 32;
    const unsigned long levels = static_cast<unsigned long>(megabytes * 1024 * 1024 / kFrameBytes);
    int failed = 0;
    unsigned long sum = 0;
    {
        ps2::host::BigStackThread thread([&] { sum = nest(levels); });
        thread.join();
    }
    std::printf("nested %lu levels of %zu KB (about %zu MB of stack): sum %lu\n", levels, kFrameBytes >> 10, megabytes, sum);
    if (sum == 0 && levels > 1) ++failed;  // the frames were touched, so the sum cannot be 0
    std::printf("big stack: %d failed\n", failed);
    return failed ? 1 : 0;
}
