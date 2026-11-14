// No game data: the sound lines of the F9 problem report (spu2::summary()). Each case drives the sound chip the way
// a game's driver would and checks that the one-line reading says the right thing:
//   1. nothing touched the chip yet,
//   2. sound data loaded but no voice started,
//   3. a voice started with data and volumes: sound comes out,
//   4. the same with the master volume at zero,
//   5. a voice started at a start address that holds only zeros (the sound bank did not land),
//   6. a stream (ADMA) that was on and moving, then stopped,
//   7. nothing plays any more some seconds after a voice ended.
// Build: ninja ps2spu2summarytest, run build/.../ps2spu2summarytest (prints "spu2 summary: 0 failed").
#include "iop/iop.hpp"
#include "iop/kernel.hpp"
#include "iop/spu2.hpp"

#include <cstdio>
#include <cstring>
#include <string>

namespace {
namespace iop = ps2::iop;
namespace spu2 = ps2::iop::spu2;

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

constexpr std::uint32_t kSpu2 = 0x1F900000u;
constexpr std::uint64_t kSecondEeCycles = 294'912'000;
constexpr std::uint32_t kBank = 0x10000;  // where the made-up sound bank goes in sound memory

void reg(std::uint32_t offset, std::uint32_t value) { spu2::write(kSpu2 + offset, value, 2); }
void run_seconds(double seconds) { iop::run(static_cast<std::uint64_t>(seconds * kSecondEeCycles)); }

// one looping block (flags: loop start, repeat, end) of a loud square-ish wave, written through the data port like a driver's PIO upload
void upload_block(std::uint32_t at, bool silent) {
    reg(0x1A8, at >> 16);
    reg(0x1AA, at & 0xFFFF);
    reg(0x1AC, silent ? 0x0700 : 0x0702);  // header: flags 7, shift 2, filter 0
    for (int i = 0; i < 7; ++i) reg(0x1AC, silent ? 0 : 0x7777);
    for (int i = 0; i < 8; ++i) reg(0x1AC, 0);  // a second, empty block behind it
}

void setup_core(int core, std::uint32_t master) {
    const std::uint32_t master_regs = 0x760u + core * 0x28u;
    reg(core * 0x400u + 0x19A, 0x8000);
    reg(master_regs + 0x0, master);
    reg(master_regs + 0x2, master);
    reg(master_regs + 0xC, 0x7FFF);
    reg(master_regs + 0xE, 0x7FFF);
}

// voice 0 of core 0 at `at`, loud, sent to the left and right mixer, started now
void start_voice(std::uint32_t at) {
    reg(0x0, 0x3FFF);
    reg(0x2, 0x3FFF);
    reg(0x4, 0x1000);
    reg(0x6, 0x00FF);  // fastest attack, sustain at the top
    reg(0x8, 0x0000);
    reg(0x1C0, at >> 16);
    reg(0x1C2, at & 0xFFFF);
    reg(0x188, 1);  // dry left: voice 0
    reg(0x190, 1);  // dry right: voice 0
    reg(0x1A0, 1);  // key on
}

std::string fresh() {
    iop::reset();
    spu2::set_sink({});
    return spu2::summary();
}

void show(const char *title, const std::string &text) {
    if (g_verbose) std::printf("--- %s\n%s", title, text.c_str());
}

}  // namespace

int main(int argc, char **argv) {
    g_verbose = argc > 1;

    std::string s = fresh();
    show("1. nothing yet", s);
    check(has(s, "has not used the sound chip"), "a fresh machine says the chip was not used");

    fresh();
    setup_core(0, 0x3FFF);
    upload_block(kBank, false);
    run_seconds(1.0);
    s = spu2::summary();
    show("2. data loaded, no voice", s);
    check(has(s, "sound data was loaded, but no voice was ever started"), "loaded data without a started voice is said");
    check(has(s, "since start 32 bytes"), "the bytes loaded by the data port are counted");

    fresh();
    setup_core(0, 0x3FFF);
    upload_block(kBank, false);
    start_voice(kBank);
    run_seconds(1.0);
    s = spu2::summary();
    show("3. a voice with data", s);
    check(has(s, "core 0: last 5 s 1 on / 0 off"), "one voice start is counted");
    check(has(s, "core 0: 1 voices"), "the voice is playing");
    check(has(s, "data at start"), "the voice's data at its start address is seen");
    check(has(s, "sound is coming out of the chip"), "a voice with data and volume is reported as audible");

    fresh();
    setup_core(0, 0);
    upload_block(kBank, false);
    start_voice(kBank);
    run_seconds(1.0);
    s = spu2::summary();
    show("4. master volume zero", s);
    check(has(s, "core 0 makes sound but its master volume is zero"), "a core whose master volume is zero is said");

    fresh();
    setup_core(0, 0x3FFF);
    start_voice(kBank + 0x4000);  // memory that holds nothing
    run_seconds(1.0);
    s = spu2::summary();
    show("5. zeros at the start address", s);
    check(has(s, "1 of the 1 voices started since start had only zeros"), "a start address with zeros is counted");
    check(has(s, "voices are started but silent"), "voices started on zeros are reported as silent with the likely cause");

    fresh();
    setup_core(0, 0x3FFF);
    reg(0x1B0, 1);  // ADMA on for core 0
    std::memset(iop::g_ram + 0x8E2D0, 0x40, 2048);
    spu2::dma(0, 0x8E2D0, 512, true, -1);
    run_seconds(0.5);
    s = spu2::summary();
    show("6a. a stream that moves", s);
    check(has(s, "core 0: ON, last 5 s 2048 bytes moved"), "a stream that took a block in is counted");
    run_seconds(8.0);
    s = spu2::summary();
    show("6b. a stream that stopped", s);
    check(has(s, "the streaming transfer has stopped"), "a stream that is on but moves no data for 5 s is said to have stopped");

    fresh();
    setup_core(0, 0x3FFF);
    upload_block(kBank, false);
    start_voice(kBank);
    run_seconds(0.5);
    reg(0x1A4, 1);  // key off: the voice releases
    run_seconds(9.0);
    s = spu2::summary();
    show("7. a voice that ended", s);
    check(has(s, "last 5 s 0 on / 0 off") && has(s, "since start 1 on / 1 off"), "events older than 5 s leave the window but stay in the total");
    check(has(s, "nothing plays right now"), "silence after the voice ended is reported as nothing playing");

    std::printf("spu2 summary: %d of %d checks failed\n", failed, checks);
    std::printf("spu2 summary: %d failed\n", failed);
    return failed ? 1 : 0;
}
