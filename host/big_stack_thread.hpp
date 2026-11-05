#pragma once
// The emulation thread with a stack of its own size. The translated game calls one C++ function per call of the game, and a game
// that nests calls deeply (Tekken 5 at start-up: a call chain of about 200 levels, 4-5 KB of stack each) needs about 1 MB. Linux and
// macOS give a new thread 8 MB, but Windows gives it 1 MB, and Tekken 5 ended there with a stack overflow (exception C00000FD, public
// issue 42). So the thread is made here with a size that does not depend on the system's default.
// pthread_create is used on every system: Linux, macOS, the browser build (host/web.cpp does the same) and Windows, where
// llvm-mingw's std::thread sits on the same winpthreads library (the Windows part is untested).
#include <pthread.h>
#ifdef _WIN32
#include <windows.h>
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <utility>

namespace ps2::host {

constexpr std::size_t kEmulationStackBytes = std::size_t{64} << 20;  // address space, not memory: pages are used when touched

class BigStackThread {
public:
    explicit BigStackThread(std::function<void()> body) : body_(std::move(body)) {
        pthread_attr_t attr;
        const bool have_attr = pthread_attr_init(&attr) == 0;
#ifndef PS2_SMALL_STACK  // only the Windows test bench defines it (CMake option PS2_WIN_SMALL_STACK): the old default-size thread
        if (have_attr) pthread_attr_setstacksize(&attr, kEmulationStackBytes);  // if refused, the system's default size stays
#endif
        started_ = pthread_create(&thread_, have_attr ? &attr : nullptr, &BigStackThread::enter, this) == 0;
        if (have_attr) pthread_attr_destroy(&attr);
        if (!started_) {
            std::fprintf(stderr, "[thread] cannot start the emulation thread\n");
            std::abort();
        }
    }
    BigStackThread(const BigStackThread &) = delete;
    BigStackThread &operator=(const BigStackThread &) = delete;
    void join() {
        if (started_) pthread_join(thread_, nullptr);
        started_ = false;
    }
    ~BigStackThread() { join(); }

private:
    static void *enter(void *self) {
        auto *me = static_cast<BigStackThread *>(self);
        // PS2_STACK_EAT_KB=<n> (a test aid of scripts/wine-bench.sh): the thread starts with n KB of its stack already used, to show
        // what a game that nests a little deeper than Tekken 5 does on a system with 1 MB stacks (and that the big stack holds).
        if (const char *eat = std::getenv("PS2_STACK_EAT_KB")) me->run_with_used_stack(static_cast<std::size_t>(std::strtoul(eat, nullptr, 10)) << 10);
        else me->body_();
#if defined(__linux__) || defined(_WIN32)
        if (std::getenv("PS2_STACK_REPORT")) report();
#endif
        return nullptr;
    }
#ifdef _WIN32
    // PS2_STACK_REPORT=1 (Windows, also under Wine): Windows commits a thread's stack as it grows and never takes pages back, so the
    // distance from the top of the stack to the lowest committed page (the thread's StackLimit) is the deepest point it reached.
    static void report() {
        const NT_TIB *tib = reinterpret_cast<const NT_TIB *>(NtCurrentTeb());
        const auto used = static_cast<std::size_t>(static_cast<const char *>(tib->StackBase) - static_cast<const char *>(tib->StackLimit));
        std::fprintf(stderr, "[stack] the emulation thread used at most %zu KB of its stack (committed)\n", used >> 10);
    }
#endif
#ifdef __linux__
    // PS2_STACK_REPORT=1 (Linux): when the thread ends, say how much of its stack it used at most. A new stack is all zero bytes, so
    // the lowest word that is not zero is the deepest point the thread reached.
    static void report() {
        pthread_attr_t attr;
        void *low = nullptr;
        std::size_t size = 0;
        if (pthread_getattr_np(pthread_self(), &attr) != 0) return;
        pthread_attr_getstack(&attr, &low, &size);
        pthread_attr_destroy(&attr);
        const auto *words = static_cast<const unsigned long *>(low);
        std::size_t zero = 0;
        while (zero < size / sizeof(unsigned long) && words[zero] == 0) ++zero;
        std::fprintf(stderr, "[stack] the emulation thread used at most %zu KB of its %zu KB\n", (size - zero * sizeof(unsigned long)) >> 10, size >> 10);
    }
#endif
    __attribute__((noinline)) void run_with_used_stack(std::size_t bytes) {
        std::fprintf(stderr, "[thread] PS2_STACK_EAT_KB: %zu KB of the emulation thread's stack are used up front (a test aid)\n", bytes >> 10);
        volatile char *used = static_cast<volatile char *>(__builtin_alloca(bytes + 1));
        for (std::size_t i = bytes; i >= 4096; i -= 4096) used[i] = 1;  // from the top down: Windows grows a stack one guard page at a time
        used[0] = 1;
        body_();
    }
    std::function<void()> body_;
    pthread_t thread_{};
    bool started_ = false;
};

}  // namespace ps2::host
