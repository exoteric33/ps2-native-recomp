// BigStackThread (host/big_stack_thread.hpp) gives the emulation thread a stack of 64 MB. This test nests a function deeper than
// the 1 MB that Windows gives a normal thread (public issue 42, Tekken 5) and proves the thread has the stack it asked for.
// Usage: big_stack [megabytes of stack to use, default 32]. No game needed.
// It can fail, which is what makes it a proof: asked for more than the thread has (ps2bigstacktest 80), it crashes with a
// stack overflow (exception C00000FD on Windows, SIGSEGV elsewhere), and so does ps2bigstacktest_small, the same test built with
// PS2_SMALL_STACK (the thread gets the system's default size, 1 MB on Windows, 8 MB on Linux), at the default 32 MB.
#include "../host/big_stack_thread.hpp"

#include <cstdio>
#include <cstdlib>

namespace {

constexpr std::size_t kFrameBytes = 16 * 1024;

// Every level's frame is published here, so it lives in memory the compiler cannot see through: each level needs storage of its own.
char *volatile g_frame;

// One call level that really uses kFrameBytes of stack: it touches one byte per page and hands the frame out through g_frame. Since the
// frame has escaped, the call below may change it, so the bytes are read again after the call: there is work after the call (no tail
// call), and the compiler cannot turn the nesting into a loop (before, a Release build did, and passed even at 3 GB).
__attribute__((noinline)) unsigned long nest(unsigned long levels) {
    char frame[kFrameBytes];
    for (std::size_t i = 0; i < kFrameBytes; i += 4096) frame[i] = static_cast<char>(levels | 1u);
    g_frame = frame;
    const unsigned long below = levels ? nest(levels - 1) : 0;
    unsigned long sum = below;
    for (std::size_t i = 0; i < kFrameBytes; i += 4096) sum += static_cast<unsigned char>(frame[i]);
    return sum;
}

}  // namespace

int main(int argc, char **argv) {
    const std::size_t megabytes = argc > 1 ? std::strtoul(argv[1], nullptr, 10) : 32;
    const unsigned long levels = static_cast<unsigned long>(megabytes * 1024 * 1024 / kFrameBytes);
#ifdef PS2_SMALL_STACK
    std::printf("built with PS2_SMALL_STACK: the thread has the system's default stack, so this must end with a stack overflow\n");
#endif
    std::fflush(stdout);
    int failed = 0;
    unsigned long sum = 0;
    {
        ps2::host::BigStackThread thread([&] { sum = nest(levels); });
        thread.join();
    }
    std::printf("nested %lu levels of %zu KB (about %zu MB of stack): sum %lu\n", levels, kFrameBytes >> 10, megabytes, sum);
    if (sum == 0 && levels > 1) ++failed;  // the frames were touched, so the sum cannot be 0
#ifdef PS2_SMALL_STACK
    ++failed;  // it should not have got here
#endif
    std::printf("big stack: %d failed\n", failed);
    return failed ? 1 : 0;
}
