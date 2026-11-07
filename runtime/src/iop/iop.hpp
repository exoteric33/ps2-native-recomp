#pragma once

#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace ps2::state {
class Stream;
}

namespace ps2::iop {

void state(ps2::state::Stream &s);

inline constexpr std::uint32_t kRamSize = 2u << 20;
inline constexpr std::uint32_t kClockDivider = 8;
inline constexpr std::uint32_t kHleOpcode = 0x3Fu;

extern std::uint8_t g_ram[];

struct Cpu {
    std::uint32_t r[32];
    std::uint32_t hi, lo;
    std::uint32_t pc, npc;
    std::uint32_t status, cause, epc;
};

extern Cpu g_cpu;

inline std::uint32_t phys(std::uint32_t a) { return a & 0x1FFFFFFFu; }

inline std::uint8_t *ram_ptr(std::uint32_t a) { return g_ram + (phys(a) & (kRamSize - 1)); }

inline std::uint32_t ram32(std::uint32_t a) {
    std::uint32_t v;
    std::memcpy(&v, ram_ptr(a), 4);
    return v;
}

inline void set_ram32(std::uint32_t a, std::uint32_t v) { std::memcpy(ram_ptr(a), &v, 4); }

inline std::string ram_string(std::uint32_t a, std::size_t limit = 256) {
    std::string s;
    for (std::size_t i = 0; i < limit; ++i) {
        const char ch = static_cast<char>(*ram_ptr(a + static_cast<std::uint32_t>(i)));
        if (!ch) break;
        s += ch;
    }
    return s;
}

std::uint8_t read8(std::uint32_t a);
std::uint16_t read16(std::uint32_t a);
std::uint32_t read32(std::uint32_t a);
void write8(std::uint32_t a, std::uint8_t v);
void write16(std::uint32_t a, std::uint16_t v);
void write32(std::uint32_t a, std::uint32_t v);
// PS2_WATCH_IOP (hw.cpp): the library functions that write memory on the IOP's behalf say so here
void watch_copy(const char *what, std::uint32_t dst, std::uint32_t src, std::uint32_t bytes, std::uint32_t caller);

using HleFunction = std::function<void(Cpu &)>;
std::uint32_t register_hle(const std::string &library, std::uint32_t index, HleFunction function);
void call_hle(Cpu &c, std::uint32_t id);

void reset();
// The IOP starts again with only its own (native) modules, as after SIF command 0x80000003 with no image: every module, thread,
// semaphore, timer, interrupt handler, file and RPC server of the game is gone and the SIF state is new. The clock goes on.
void soft_reset();
void run(std::uint64_t ee_cycles);
std::uint64_t cycles();
void raise_interrupt(unsigned line);
void vblank(bool start);
bool idle();
void dump();

std::int32_t load_module(const std::string &path, const std::string &args, std::function<void(int, int)> done = {});
std::int32_t load_module_buffer(const std::vector<std::uint8_t> &data, const std::string &path, const std::string &args,
                                std::function<void(int, int)> done = {});
// modload's LoadModuleBuffer and StartModule: link a module without running it, then run its start routine later
std::int32_t link_module(const std::vector<std::uint8_t> &data, const std::string &path);
bool start_module(int id, const std::string &args, std::function<void(int, int)> done);
void set_trace(bool modules, bool hle);

}
