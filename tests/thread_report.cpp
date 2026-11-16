// No game data: the thread lists of the F9 problem report (kernel::thread_summary / thread_report and the IOP's).
// The EE side is driven with system calls on the main thread (CreateSema, WaitSema, SignalSema, SleepThread); the IOP side with a
// synthetic module that imports thsemap, thevent and thbase, and four threads that call WaitSema, WaitEventFlag, DelayThread and
// SleepThread the way a game's module does. Each state's text is checked for the plain words a tester's report must show:
//   EE:  one thread running; it waits on a semaphore (id and count); it sleeps; it is woken again.
//   IOP: threads waiting on a semaphore, an event flag, a delay and a sleep, and one that spins (so not every thread waits).
// Build: ninja ps2threadreporttest, run build/.../ps2threadreporttest (prints "thread report: 0 failed"; with the argument v it also
// prints every text).
#include "hw.hpp"
#include "iop/iop.hpp"
#include "iop/kernel.hpp"
#include "kernel.hpp"
#include "ps2/cpu.hpp"
#include "ps2/generated.hpp"
#include "ps2/memory.hpp"
#include "ps2/system.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {
namespace iop = ps2::iop;
namespace ikernel = ps2::iop::kernel;
namespace kernel = ps2::kernel;

int failed = 0, checks = 0;
bool g_verbose = false;
void check(bool ok, const char *message) {
    ++checks;
    if (!ok) {
        ++failed;
        std::fprintf(stderr, "FAIL: %s\n", message);
    }
}
bool has(const std::string &text, const char *part) { return text.find(part) != std::string::npos; }
void show(const char *title, const std::string &text) {
    if (g_verbose) std::printf("--- %s\n%s%s", title, text.c_str(), text.empty() || text.back() == '\n' ? "" : "\n");
}
void check_has(const std::string &text, const char *part, const char *message) {
    ++checks;
    if (!has(text, part)) {
        ++failed;
        std::fprintf(stderr, "FAIL: %s (wanted \"%s\" in:\n%s)\n", message, part, text.c_str());
    }
}

// ---- EE ------------------------------------------------------------------------------------------------------------------

constexpr std::uint32_t kCaller = 0x00100000u;
std::int32_t syscall_with(std::uint32_t n, std::uint32_t a0 = 0, std::uint32_t a1 = 0) {
    ps2::Cpu &c = ps2::g_cpu;
    c.r[3].sd[0] = n;
    c.r[4].sd[0] = a0;
    c.r[5].sd[0] = a1;
    c.pc = kCaller + 4;
    ps2::syscall(c, kCaller);
    return static_cast<std::int32_t>(c.r[2].sd[0]);
}

void ee_part() {
    ps2::hw::reset();
    kernel::reset();
    kernel::start_main_thread(kCaller, 0, 0, 0);
    ps2::Cpu &c = ps2::g_cpu;
    c.cop0[12] = 0x70010C01u;

    show("EE, the main thread runs", kernel::thread_summary() + "\n" + kernel::thread_report());
    check_has(kernel::thread_summary(), "1 of 1 EE threads run or are ready", "EE: a running main thread is not 'waiting'");
    check_has(kernel::thread_report(), "EE thread 1 (main), priority", "EE: the main thread is listed");
    check_has(kernel::thread_report(), "running", "EE: and says it runs");

    // CreateSema: the parameter block {count, max, init, wait, attr, option}
    const std::uint32_t block = 0x1000;
    ps2::store(ps2::g_ram + block + 0, 0u);
    ps2::store(ps2::g_ram + block + 4, 1u);
    ps2::store(ps2::g_ram + block + 8, 0u);
    const std::int32_t sema = syscall_with(0x40, block);
    check(sema >= 0, "EE: CreateSema gives an id");
    syscall_with(0x44, static_cast<std::uint32_t>(sema));  // WaitSema with count 0: the main thread waits
    const std::string waits = "main thread on semaphore " + std::to_string(sema) + " (count 0, 1 waiting)";
    show("EE, main waits on a semaphore", kernel::thread_summary() + "\n" + kernel::thread_report());
    check_has(kernel::thread_summary(), ("every EE thread waits: " + waits).c_str(), "EE: the plain line names the semaphore and its count");
    check_has(kernel::thread_report(), ("waits on semaphore " + std::to_string(sema) + " (count 0, 1 waiting)").c_str(), "EE: the thread line too");

    syscall_with(0x42, static_cast<std::uint32_t>(sema));  // SignalSema: the main thread is released
    check(!has(kernel::thread_summary(), "every EE thread waits"), "EE: after SignalSema not every thread waits");

    syscall_with(0x32);  // SleepThread
    show("EE, main sleeps", kernel::thread_summary() + "\n" + kernel::thread_report());
    check_has(kernel::thread_summary(), "every EE thread waits: main thread asleep", "EE: a sleeping thread is said to sleep");
    check_has(kernel::thread_report(), "asleep (SleepThread", "EE: the thread line says it sleeps");
}

// ---- IOP -----------------------------------------------------------------------------------------------------------------

// Import tables inside the loaded image: thbase at 0x000, thsemap at 0x100, thevent at 0x200. Code: 0x300 (+16 each) are thread entries
// made after the load (j <import stub>), 0x3C0 spins, 0x3E0 is the module start (jr ra).
const std::vector<unsigned> kThbase{24, 33};
const std::vector<unsigned> kThsemap{4, 8};
const std::vector<unsigned> kThevent{4, 10};

std::vector<std::uint8_t> module() {
    std::vector<std::uint8_t> elf(0x100 + 0x400);
    const auto put = [&](std::size_t at, auto value) { std::memcpy(elf.data() + at, &value, sizeof value); };
    std::memcpy(elf.data(), "\177ELF\1\1\1", 7);
    put(16, std::uint16_t{0xFF80});
    put(18, std::uint16_t{8});
    put(20, 1u);
    put(28, 52u);
    put(40, std::uint16_t{52});
    put(42, std::uint16_t{32});
    put(44, std::uint16_t{2});
    put(52, 0x70000080u);
    put(56, 0x80u);  // the iopmod record at file offset 0x80
    put(84, 1u);
    put(88, 0x100u);
    put(100, 0x400u);
    put(104, 0x400u);  // the loaded image: file 0x100, 0x400 bytes
    put(0x84, 0x3E0u);  // entry of the module start
    const auto table = [&](std::size_t at, const char *name, const std::vector<unsigned> &functions) {
        put(0x100 + at, 0x41E00000u);
        put(0x108 + at, 0x101u);
        std::memcpy(elf.data() + 0x10C + at, name, std::strlen(name));
        for (std::size_t i = 0; i < functions.size(); ++i) {
            put(0x114 + at + i * 8, 0x03E00008u);
            put(0x118 + at + i * 8, 0x24000000u | functions[i]);
        }
    };
    table(0x000, "thbase", kThbase);
    table(0x100, "thsemap", kThsemap);
    table(0x200, "thevent", kThevent);
    put(0x4C0, 0x1000FFFFu);  // image 0x3C0: beq zero, zero, .
    put(0x4E0, 0x03E00008u);  // image 0x3E0: jr ra; nop
    return elf;
}

std::uint32_t g_base = 0;
std::uint32_t stub(const std::vector<unsigned> &list, unsigned index, std::uint32_t table_at) {
    const auto it = std::find(list.begin(), list.end(), index);
    return g_base + table_at + 0x14 + static_cast<std::uint32_t>(it - list.begin()) * 8;
}
std::uint32_t hle_id(std::uint32_t at) { return iop::ram32(at) & 0x03FFFFFFu; }
std::uint32_t call(std::uint32_t id, std::uint32_t a0) {
    iop::Cpu c{};
    c.r[4] = a0;
    iop::call_hle(c, id);
    return c.r[2];
}
// a thread entry at image offset `slot`: j <stub>; nop (the thread's a0 and a1 are the call's arguments)
std::uint32_t entry(unsigned slot, std::uint32_t target) {
    const std::uint32_t at = g_base + 0x300 + slot * 16;
    iop::set_ram32(at, 0x08000000u | ((target >> 2) & 0x03FFFFFFu));
    iop::set_ram32(at + 4, 0);
    return at;
}

void iop_part() {
    iop::reset();
    const int module_id = iop::load_module_buffer(module(), "thread-report-test.irx", "");
    check(module_id > 0, "IOP: the synthetic module loads");
    if (module_id <= 0) return;
    g_base = ikernel::modules().at(module_id).base;
    iop::run(65536);  // the module's start thread ends

    const std::uint32_t scratch = ikernel::alloc(0x100);
    iop::set_ram32(scratch + 0, 0);   // attr
    iop::set_ram32(scratch + 4, 0);   // option
    iop::set_ram32(scratch + 8, 0);   // initial count / bits
    iop::set_ram32(scratch + 12, 1);  // max
    const std::uint32_t sema = call(hle_id(stub(kThsemap, 4, 0x100)), scratch);
    const std::uint32_t flag = call(hle_id(stub(kThevent, 4, 0x200)), scratch);
    check(sema > 0 && flag > 0, "IOP: a semaphore and an event flag are made");

    show("IOP, no thread waits yet", iop::thread_summary() + "\n" + iop::thread_report());

    const int a = ikernel::create_thread(entry(0, stub(kThsemap, 8, 0x100)), 0x1000, 20, 0, 0, 0);
    const int b = ikernel::create_thread(entry(1, stub(kThevent, 10, 0x200)), 0x1000, 21, 0, 0, 0);
    const int d = ikernel::create_thread(entry(2, stub(kThbase, 33, 0)), 0x1000, 22, 0, 0, 0);
    const int s = ikernel::create_thread(entry(3, stub(kThbase, 24, 0)), 0x1000, 23, 0, 0, 0);
    const int spin = ikernel::create_thread(g_base + 0x3C0, 0x1000, 30, 0, 0, 0);
    ikernel::start_thread(a, sema);
    ikernel::start_thread(b, flag, 0x5);  // wants bits 0x5 (mode 0: all of them)
    ikernel::start_thread(d, 5'000'000);  // five seconds
    ikernel::start_thread(s, 0);
    ikernel::start_thread(spin, 0);
    iop::run(65536);

    const std::string report = iop::thread_report();
    show("IOP, four threads wait and one spins", iop::thread_summary() + "\n" + report);
    check_has(report, ("waits on semaphore " + std::to_string(sema) + " (count 0, 1 waiting)").c_str(), "IOP: semaphore id and count");
    check_has(report, ("waits on event flag " + std::to_string(flag) + " (bits now 0x0, it wants 0x5, all of them)").c_str(), "IOP: event flag, its bits and what the thread wants");
    check_has(report, "waits out a delay (DelayThread)", "IOP: a delay");
    check_has(report, "asleep (SleepThread: waits for a wake-up)", "IOP: a sleep");
    check_has(report, "THREAD-REPORT-TEST", "IOP: the module a thread belongs to is named");
    check_has(iop::thread_summary(), "IOP threads run or are ready, 4 wait", "IOP: with one spinning thread not every thread waits");
}

}  // namespace

int main(int argc, char **argv) {
    g_verbose = argc > 1 && std::strcmp(argv[1], "v") == 0;
    ee_part();
    iop_part();
    std::printf("thread report: %d failed\n", failed);
    return failed ? 1 : 0;
}
