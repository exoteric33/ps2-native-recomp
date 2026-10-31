// No game data: exercise the IOP thread manager (thbase) through a synthetic IOP module whose imports are linked to the HLE
// functions, the way a game's own modules call them. Covers the exports added for issue #14 (Valkyrie Profile 2 and Star Ocean 3
// stop at "missing thbase #42"): 30 iSuspendThread, 32 iResumeThread, 42 GetThreadCurrentPriority, 44 ReferSystemStatus,
// 45 ReferThreadRunStatus, 46 GetThreadStackFreeSize, 47 GetThreadmanIdList, and SuspendThread/ResumeThread (29, 31) with their
// real behaviour. Build: ninja ps2thbasetest, run build/.../ps2thbasetest (prints "thbase: 0 failed").
#include "iop/kernel.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

namespace {
namespace iop = ps2::iop;
namespace kernel = ps2::iop::kernel;

int failed = 0, checks = 0;
void check(bool ok, const char *message) {
    ++checks;
    if (!ok) { ++failed; std::fprintf(stderr, "FAIL: %s\n", message); }
}
void check_eq(std::int64_t got, std::int64_t want, const char *message) {
    ++checks;
    if (got != want) { ++failed; std::fprintf(stderr, "FAIL: %s (got %lld, want %lld)\n", message, static_cast<long long>(got), static_cast<long long>(want)); }
}

// Import tables inside the loaded image: thbase at 0x000, thsemap at 0x100. Code: 0x1C0 spins (b .), 0x1E0 is the module start (jr ra).
const std::vector<unsigned> kThbase{22, 24, 25, 29, 30, 31, 32, 42, 44, 45, 46, 47};
const std::vector<unsigned> kThsemap{4, 8};

std::vector<std::uint8_t> module() {
    std::vector<std::uint8_t> elf(0x300);
    const auto put = [&](std::size_t at, auto value) { std::memcpy(elf.data() + at, &value, sizeof value); };
    std::memcpy(elf.data(), "\177ELF\1\1\1", 7);
    put(16, std::uint16_t{0xFF80}); put(18, std::uint16_t{8}); put(20, 1u);
    put(28, 52u); put(40, std::uint16_t{52}); put(42, std::uint16_t{32}); put(44, std::uint16_t{2});
    put(52, 0x70000080u); put(56, 0x80u);          // the iopmod record at file offset 0x80
    put(84, 1u); put(88, 0x100u); put(100, 0x200u); put(104, 0x200u);  // the loaded image: file 0x100, 0x200 bytes
    put(0x84, 0x1E0u);                               // entry of the module start
    const auto table = [&](std::size_t at, const char *name, const std::vector<unsigned> &functions) {
        put(0x100 + at, 0x41E00000u); put(0x108 + at, 0x101u);
        std::memcpy(elf.data() + 0x10C + at, name, std::strlen(name));
        for (std::size_t i = 0; i < functions.size(); ++i) {
            put(0x114 + at + i * 8, 0x03E00008u);
            put(0x118 + at + i * 8, 0x24000000u | functions[i]);
        }
    };
    table(0x000, "thbase", kThbase);
    table(0x100, "thsemap", kThsemap);
    put(0x2C0, 0x1000FFFFu);  // 0x1C0 in the image: beq zero, zero, .   (the delay slot is a nop)
    put(0x2E0, 0x03E00008u);  // 0x1E0: jr ra; nop
    return elf;
}

std::map<unsigned, std::uint32_t> g_thbase, g_thsemap;
std::uint32_t g_base = 0, g_scratch = 0;

std::uint32_t call(std::map<unsigned, std::uint32_t> &library, unsigned index, std::uint32_t a0 = 0, std::uint32_t a1 = 0, std::uint32_t a2 = 0, std::uint32_t a3 = 0) {
    iop::Cpu c{};
    c.r[4] = a0; c.r[5] = a1; c.r[6] = a2; c.r[7] = a3;
    iop::call_hle(c, library.at(index));
    return c.r[2];
}
std::int32_t th(unsigned index, std::uint32_t a0 = 0, std::uint32_t a1 = 0, std::uint32_t a2 = 0, std::uint32_t a3 = 0) {
    return static_cast<std::int32_t>(call(g_thbase, index, a0, a1, a2, a3));
}
std::uint32_t stub_address(const std::vector<unsigned> &list, unsigned index, std::uint32_t table_at) {
    const auto it = std::find(list.begin(), list.end(), index);
    return g_base + table_at + 0x14 + static_cast<std::uint32_t>(it - list.begin()) * 8;
}
int status(int id) { return kernel::thread(id)->status; }
void run() { iop::run(65536); }

// ids written by GetThreadmanIdList
std::vector<int> id_list(int type, std::uint32_t *count = nullptr) {
    const std::uint32_t buf = g_scratch + 0x100, count_at = g_scratch + 0x1F0;
    iop::set_ram32(count_at, 0xDEADBEEFu);
    const int n = th(47, static_cast<std::uint32_t>(type), buf, 16, count_at);
    std::vector<int> ids;
    for (int i = 0; i < n; ++i) ids.push_back(static_cast<int>(iop::ram32(buf + 4u * i)));
    if (count) *count = iop::ram32(count_at);
    return ids;
}
bool has(const std::vector<int> &v, int id) { return std::find(v.begin(), v.end(), id) != v.end(); }
}  // namespace

int main() {
    iop::reset();
    const int module_id = iop::load_module_buffer(module(), "thbase-test.irx", "");
    check(module_id > 0, "synthetic IOP module loads");
    if (module_id <= 0) return 1;
    g_base = kernel::modules().at(module_id).base;
    for (unsigned index : kThbase) {
        const std::uint32_t op = iop::ram32(stub_address(kThbase, index, 0));
        check(op >> 26 == iop::kHleOpcode, "thbase import links to an HLE function");
        g_thbase[index] = op & 0x03FFFFFFu;
    }
    for (unsigned index : kThsemap) g_thsemap[index] = iop::ram32(stub_address(kThsemap, index, 0x100)) & 0x03FFFFFFu;
    run();  // finish the module's start thread
    g_scratch = kernel::alloc(0x1000);

    // 42 GetThreadCurrentPriority: no thread running means the idle thread (127)
    check_eq(th(42), 127, "GetThreadCurrentPriority with no thread running");

    // 44 ReferSystemStatus
    for (unsigned i = 0; i < 0x50; i += 4) iop::set_ram32(g_scratch + i, 0xEEEEEEEEu);
    check_eq(th(44, g_scratch, 0x3C), -1, "ReferSystemStatus refuses a size below 0x40");
    check_eq(th(44, g_scratch, 0x40), 0, "ReferSystemStatus succeeds with size 0x40");
    check_eq(iop::ram32(g_scratch + 0), 0, "ReferSystemStatus: status is 0 in a thread");
    check_eq(iop::ram32(g_scratch + 4), 32, "ReferSystemStatus: the low timer is 32 bits wide");
    check_eq(iop::ram32(g_scratch + 0x3C), 0, "ReferSystemStatus clears the reserved words");
    check_eq(iop::ram32(g_scratch + 0x40), 0xEEEEEEEEu, "ReferSystemStatus writes no more than size bytes");

    // threads: A spins, B stays dormant
    const std::uint32_t spin = g_base + 0x1C0;
    const int A = kernel::create_thread(spin, 0x1000, 20, 0, 0, 0);
    const int B = kernel::create_thread(spin, 0x1000, 30, 0, 0, 0);
    kernel::start_thread(A, 0);
    run();
    check_eq(status(A), kernel::kRun, "thread A runs");
    check_eq(th(42), 20, "GetThreadCurrentPriority inside thread A");

    // 45 ReferThreadRunStatus
    check_eq(th(45, A, g_scratch, 0x2C), 0, "ReferThreadRunStatus on a running thread");
    check_eq(iop::ram32(g_scratch + 0), 1, "ReferThreadRunStatus: status RUN");
    check_eq(iop::ram32(g_scratch + 4), 20, "ReferThreadRunStatus: current priority");
    check_eq(th(45, A, g_scratch, 0x2B), -432, "ReferThreadRunStatus: size too small");
    check_eq(th(45, 9999, g_scratch, 0x2C), -407, "ReferThreadRunStatus: unknown thread");
    check_eq(th(45, static_cast<std::uint32_t>(-5), g_scratch, 0x2C), -407, "ReferThreadRunStatus: negative id");
    check_eq(th(45, B, g_scratch, 0x2C), 0, "ReferThreadRunStatus on a dormant thread");
    check_eq(iop::ram32(g_scratch + 0), 0x10, "ReferThreadRunStatus: status DORMANT");

    // 46 GetThreadStackFreeSize
    check_eq(th(46, B), 0x1000, "GetThreadStackFreeSize of a dormant thread is the whole stack");
    check_eq(th(46, 9999), -407, "GetThreadStackFreeSize: unknown thread");

    // 29 / 30 / 31 / 32 SuspendThread, ResumeThread
    check_eq(th(29, 0), -406, "SuspendThread(TH_SELF) is refused");
    check_eq(th(29, 9999), -407, "SuspendThread: unknown thread");
    check_eq(th(29, B), -413, "SuspendThread of a dormant thread");
    check_eq(th(31, B), -415, "ResumeThread of a thread that is not suspended");
    check_eq(th(31, 0), -406, "ResumeThread(TH_SELF) is refused");
    check_eq(th(29, A), 0, "SuspendThread of the running thread");
    check_eq(status(A), kernel::kSuspend, "thread A is suspended");
    run();
    check_eq(status(A), kernel::kSuspend, "a suspended thread does not run");
    check_eq(th(42), 127, "nothing runs while the only thread is suspended");
    check_eq(th(29, A), -1, "SuspendThread of a suspended thread");
    check(has(id_list(1), A) && has(id_list(1), B), "GetThreadmanIdList(thread) lists suspended and dormant threads");
    check_eq(th(31, A), 0, "ResumeThread");
    run();
    check_eq(status(A), kernel::kRun, "a resumed thread runs again");
    check_eq(th(30, A), 0, "iSuspendThread");
    check_eq(status(A), kernel::kSuspend, "iSuspendThread suspends");
    check_eq(th(32, A), 0, "iResumeThread");
    run();
    check_eq(status(A), kernel::kRun, "iResumeThread resumes");

    // a sleeping thread S (priority 10, above A)
    const int S = kernel::create_thread(stub_address(kThbase, 24, 0), 0x1000, 10, 0, 0, 0);
    kernel::start_thread(S, 0);
    run();
    check_eq(status(S), kernel::kWait, "thread S sleeps");
    check(has(id_list(7), S), "GetThreadmanIdList(sleeping threads) lists S");
    check(!has(id_list(7), A), "GetThreadmanIdList(sleeping threads) leaves out A");
    check(has(id_list(9), B) && !has(id_list(9), S), "GetThreadmanIdList(dormant threads)");
    check_eq(th(22, S, g_scratch), 0, "ReferThreadStatus");
    check_eq(iop::ram32(g_scratch + 36), 1, "ReferThreadStatus: wait type SLEEP");
    check_eq(th(46, S), 0x1000 - 0x10, "GetThreadStackFreeSize of a sleeping thread");
    check_eq(th(29, S), 0, "SuspendThread of a sleeping thread");
    check_eq(status(S), kernel::kWaitSuspend, "S is WAIT-SUSPEND");
    check_eq(th(45, S, g_scratch, 0x2C), 0, "ReferThreadRunStatus on a WAIT-SUSPEND thread");
    check_eq(iop::ram32(g_scratch + 0), 0x0C, "ReferThreadRunStatus: status WAIT-SUSPEND");
    check_eq(iop::ram32(g_scratch + 8), 1, "ReferThreadRunStatus: wait type SLEEP");
    check(has(id_list(7), S), "a WAIT-SUSPEND sleeper is still in the sleeping list");
    check_eq(th(31, S), 0, "ResumeThread of a WAIT-SUSPEND thread");
    check_eq(status(S), kernel::kWait, "S waits again");
    check_eq(th(29, S), 0, "SuspendThread again");
    check_eq(th(25, S), 0, "WakeupThread of a WAIT-SUSPEND sleeper");
    check_eq(status(S), kernel::kSuspend, "the wake-up ends the wait but S stays suspended");
    run();
    check_eq(status(S), kernel::kSuspend, "S does not run while suspended");
    check_eq(th(31, S), 0, "ResumeThread after the wake-up");
    run();
    check_eq(status(S), kernel::kDormant, "S runs on and exits");

    // a thread that suspends itself (the SuspendThread import is its entry; the thread's own id is the argument)
    const int T = kernel::create_thread(stub_address(kThbase, 29, 0), 0x1000, 10, 0, 0, 0);
    kernel::start_thread(T, static_cast<std::uint32_t>(T));
    run();
    check_eq(status(T), kernel::kSuspend, "SuspendThread on itself from inside the thread");
    check_eq(th(31, T), 0, "ResumeThread of the self-suspended thread");
    run();
    check_eq(status(T), kernel::kDormant, "the self-suspended thread goes on and exits");

    // a thread waiting for a semaphore; object ids above 9 so they cannot be taken for a TMID_ type
    int sema = 0;
    for (int i = 0; i < 10; ++i) {
        iop::set_ram32(g_scratch + 0, 0); iop::set_ram32(g_scratch + 4, 0);
        iop::set_ram32(g_scratch + 8, 0); iop::set_ram32(g_scratch + 12, 1);
        sema = static_cast<int>(call(g_thsemap, 4, g_scratch));
    }
    check(sema > 9, "tenth semaphore has an id above 9");
    const int W = kernel::create_thread(stub_address(kThsemap, 8, 0x100), 0x1000, 10, 0, 0, 0);
    kernel::start_thread(W, static_cast<std::uint32_t>(sema));
    run();
    check_eq(status(W), kernel::kWait, "thread W waits for the semaphore");
    check_eq(th(45, W, g_scratch, 0x2C), 0, "ReferThreadRunStatus on a semaphore waiter");
    check_eq(iop::ram32(g_scratch + 8), 3, "wait type SEMA");
    check_eq(iop::ram32(g_scratch + 12), sema, "wait id is the semaphore");
    std::uint32_t count = 0;
    auto waiters = id_list(sema, &count);
    check(waiters.size() == 1 && waiters[0] == W && count == 1, "GetThreadmanIdList(semaphore id) lists its waiters");
    auto semas = id_list(2, &count);
    check(semas.size() == 10 && count == 10 && has(semas, sema), "GetThreadmanIdList(semaphores)");
    check_eq(th(47, 2, g_scratch + 0x100, 3, g_scratch + 0x1F0), 3, "GetThreadmanIdList writes at most bufsize ids");
    check_eq(iop::ram32(g_scratch + 0x1F0), 10, "GetThreadmanIdList still counts all objects");
    check_eq(th(47, 2, g_scratch + 0x100, 3, 0), 3, "GetThreadmanIdList accepts a NULL count");
    check_eq(th(47, 12345, g_scratch + 0x100, 4, 0), -431, "GetThreadmanIdList: an id that is no object");

    std::printf("thbase: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
