#include "kernel.hpp"

#include "ps2/disc.hpp"

#include "ps2/state.hpp"
#include "ps2/system.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <map>
#include <memory>

namespace ps2::iop {

// libs.cpp: a guest size that reaches past the end of memory is said there (rate-limited).
void bad_span(const char *what, std::uint32_t address, std::uint64_t size, const char *outcome);

namespace kernel {

namespace {

// More sectors than fit in the IOP's 2 MiB are cut to 2 MiB: the memory is mirrored, so more would only write over the same bytes
// again, and a bad count must not ask the host for a buffer of up to 8 TB.
std::uint32_t iop_sectors(const char *what, std::uint32_t dst, std::uint32_t sectors) {
    constexpr std::uint32_t kMost = kRamSize / disc::kSector;
    if (sectors <= kMost) return sectors;
    bad_span(what, dst, std::uint64_t{sectors} * disc::kSector, "cut to 2 MiB");
    return kMost;
}

std::uint32_t arg(const Cpu &c, int i) { return i < 4 ? c.r[4 + i] : read32(c.r[29] + 16u + static_cast<std::uint32_t>(i - 4) * 4u); }

void def(const char *library, std::uint32_t index, const char *name, std::function<void(Cpu &)> body) {
    register_hle(library, index, [library, name, body = std::move(body)](Cpu &c) {
        trace_hle(library, name, c);
        body(c);
    });
}

struct CdState {
    bool busy = false;
    std::uint32_t callback = 0;
    std::uint32_t error = 0;
    std::vector<int> sync_waiters;
} g_cd;

// The disc stream of the IOP library (sceCdSt*). A game's own IOP module (Robotech: Battlecry's vcIOPFile) streams music and
// files with it: sceCdStInit, sceCdStStart at a sector, then sceCdStRead into its own buffer. Here the stream is virtual: a
// read takes the next sectors straight from the disc, and the "ring buffer" always counts as full.
struct StreamState {
    std::uint32_t buffer_sectors = 0;  // sceCdStInit: the ring buffer's size
    std::uint32_t bank_sectors = 0;
    std::uint32_t buffer_address = 0;
    std::uint32_t position = 0;        // the next sector sceCdStRead delivers
    bool active = false;
} g_st;

// PS2_TRACE_CD=1 prints every stream call (sceCdSt*) of the IOP.
bool trace_cd() {
    static const bool on = std::getenv("PS2_TRACE_CD") != nullptr;
    return on;
}

std::uint8_t bcd(int v) { return static_cast<std::uint8_t>(((v / 10) << 4) | (v % 10)); }

void finish_read() {
    g_cd.busy = false;
    for (int id : g_cd.sync_waiters)
        if (Thread *t = thread(id)) wake(*t, 0);
    if (!g_cd.sync_waiters.empty()) reschedule();
    g_cd.sync_waiters.clear();
    if (g_cd.callback) call_guest(g_cd.callback, 1);
}

std::uint64_t read_cycles(std::uint32_t sectors) {
    return usec_to_cycles(400) + static_cast<std::uint64_t>(sectors) * usec_to_cycles(250);
}

bool start_read(std::uint32_t lsn, std::uint32_t sectors, std::uint32_t buffer, std::uint32_t mode, bool ee_memory);

}

std::vector<std::uint8_t> read_disc_file(const std::string &path) {
    const auto entry = disc::find(path);
    if (!entry || entry->directory) return {};
    // The size comes from the image: a damaged one can say almost 4 GiB, which would wrap the sector count below and then grow the
    // vector to 4 GiB. What is read whole is a module or a small text file; the IOP has 2 MiB of memory.
    constexpr std::uint32_t kMaxFile = 64u << 20;
    if (entry->size > kMaxFile) {
        std::fprintf(stderr, "[disc] %s: %u bytes is too large to load whole (damaged image?)\n", path.c_str(), entry->size);
        return {};
    }
    std::vector<std::uint8_t> data((entry->size + disc::kSector - 1) / disc::kSector * disc::kSector);
    disc::read(entry->sector, static_cast<std::uint32_t>(data.size() / disc::kSector), data.data());
    data.resize(entry->size);
    return data;
}

namespace {

bool start_read(std::uint32_t lsn, std::uint32_t sectors, std::uint32_t buffer, std::uint32_t, bool) {
    if (g_cd.busy) return false;
    sectors = iop_sectors("sceCdRead", buffer, sectors);
    g_cd.busy = true;
    g_cd.error = 0;
    disc::ready(lsn, sectors);
    schedule(now() + read_cycles(sectors), Job{kJobCdRead, lsn, sectors, buffer});
    return true;
}

}

void install_cdvd() {
    g_cd = CdState{};
    g_st = StreamState{};
    register_job(kJobCdRead, [](const Job &j) {
        if (!disc::ready(j.a, j.b)) {
            schedule(now() + usec_to_cycles(1000), j);
            return;
        }
        std::vector<std::uint8_t> data(static_cast<std::size_t>(j.b) * disc::kSector);
        disc::read(j.a, j.b, data.data());
        for (std::size_t i = 0; i < data.size(); ++i) write8(j.c + static_cast<std::uint32_t>(i), data[i]);
        finish_read();
    });
    register_hle_library("cdvdman", 0x0101);
    def("cdvdman", 4, "sceCdInit", [](Cpu &c) { c.r[2] = 1; });
    def("cdvdman", 5, "sceCdStandby", [](Cpu &c) { c.r[2] = 1; });
    def("cdvdman", 6, "sceCdRead", [](Cpu &c) {
        if (g_trace_hle) std::fprintf(stderr, "[cdvd] read lsn %u count %u to 0x%08X\n", arg(c, 0), arg(c, 1), arg(c, 2));
        c.r[2] = start_read(arg(c, 0), arg(c, 1), arg(c, 2), arg(c, 3), false) ? 1u : 0u;
    });
    def("cdvdman", 7, "sceCdSeek", [](Cpu &c) { c.r[2] = 1; });
    def("cdvdman", 8, "sceCdGetError", [](Cpu &c) { c.r[2] = g_cd.error; });
    def("cdvdman", 10, "sceCdSearchFile", [](Cpu &c) {
        const std::string name = ram_string(arg(c, 1), 256);
        const auto entry = disc::find(name);
        if (!entry) { c.r[2] = 0; return; }
        const std::uint32_t p = arg(c, 0);
        write32(p, entry->sector);
        write32(p + 4, entry->size);
        const std::string base = name.substr(name.find_last_of("\\/") == std::string::npos ? 0 : name.find_last_of("\\/") + 1);
        for (std::uint32_t i = 0; i < 16; ++i) write8(p + 8 + i, i < base.size() ? static_cast<std::uint8_t>(base[i]) : 0);
        c.r[2] = 1;
    });
    def("cdvdman", 11, "sceCdSync", [](Cpu &c) {
        if (arg(c, 0) == 1 || arg(c, 0) == 17) { c.r[2] = g_cd.busy ? 1u : 0u; return; }
        c.r[2] = 0;
        if (!g_cd.busy || !current()) return;
        g_cd.sync_waiters.push_back(current()->id);
        block(c, kWaitHle, 0);
    });
    def("cdvdman", 12, "sceCdGetDiskType", [](Cpu &c) { c.r[2] = 0x14; });
    def("cdvdman", 13, "sceCdDiskReady", [](Cpu &c) { c.r[2] = 2; });
    def("cdvdman", 14, "sceCdTrayReq", [](Cpu &c) { if (arg(c, 1)) write32(arg(c, 1), 0); c.r[2] = 1; });
    def("cdvdman", 15, "sceCdStop", [](Cpu &c) { c.r[2] = 1; });
    def("cdvdman", 22, "sceCdRI", [](Cpu &c) {
        for (std::uint32_t i = 0; i < 8; ++i) write8(arg(c, 0) + i, 0);
        if (arg(c, 1)) write32(arg(c, 1), 0);
        c.r[2] = 1;
    });
    def("cdvdman", 24, "sceCdReadClock", [](Cpu &c) {
        const std::time_t t = static_cast<std::time_t>(rtc_seconds());
        const std::tm *tm = std::gmtime(&t);
        const std::uint32_t p = arg(c, 0);
        write8(p + 0, 0);
        write8(p + 1, bcd(tm->tm_sec));
        write8(p + 2, bcd(tm->tm_min));
        write8(p + 3, bcd((tm->tm_hour + 9) % 24));
        write8(p + 4, 0);
        write8(p + 5, bcd(tm->tm_mday));
        write8(p + 6, bcd(tm->tm_mon + 1));
        write8(p + 7, bcd(tm->tm_year % 100));
        c.r[2] = 1;
    });
    def("cdvdman", 28, "sceCdStatus", [](Cpu &c) { c.r[2] = g_cd.busy ? 0x06u : 0x0Au; });
    def("cdvdman", 37, "sceCdCallback", [](Cpu &c) { c.r[2] = g_cd.callback; g_cd.callback = arg(c, 0); });
    def("cdvdman", 38, "sceCdPause", [](Cpu &c) { c.r[2] = 1; });
    def("cdvdman", 39, "sceCdBreak", [](Cpu &c) { c.r[2] = 1; });
    // The stream functions, numbered in the alphabetical order of the library's export table (Init, Read, Seek, Start, Stat, Stop).
    def("cdvdman", 56, "sceCdStInit", [](Cpu &c) {
        g_st = StreamState{};
        if (trace_cd()) std::fprintf(stderr, "[cd] StInit buffer %u bank %u at 0x%08X\n", arg(c, 0), arg(c, 1), arg(c, 2));
        g_st.buffer_sectors = arg(c, 0);
        g_st.bank_sectors = arg(c, 1);
        g_st.buffer_address = arg(c, 2);
        c.r[2] = 1;
    });
    def("cdvdman", 57, "sceCdStRead", [](Cpu &c) {
        // (sectors, destination in IOP memory, mode 0 = wait / 1 = do not wait, error out): returns the sectors delivered
        const std::uint32_t dst = arg(c, 1), error = arg(c, 3);
        const std::uint32_t sectors = iop_sectors("sceCdStRead", dst, arg(c, 0));
        if (trace_cd()) std::fprintf(stderr, "[cd] StRead %u sectors from %u to 0x%08X mode %u\n", sectors, g_st.position, dst, arg(c, 2));
        if (error) write32(error, 0);
        if (!g_st.active || sectors == 0 || (dst & 0x1FFFFFFFu) >= kRamSize) { c.r[2] = 0; return; }
        std::vector<std::uint8_t> data(static_cast<std::size_t>(sectors) * disc::kSector);
        disc::read(g_st.position, sectors, data.data());
        for (std::size_t i = 0; i < data.size(); ++i) write8(dst + static_cast<std::uint32_t>(i), data[i]);
        g_st.position += sectors;
        c.r[2] = sectors;
    });
    def("cdvdman", 58, "sceCdStSeek", [](Cpu &c) {
        if (trace_cd()) std::fprintf(stderr, "[cd] StSeek %u (was at %u)\n", arg(c, 0), g_st.position);
        g_st.position = arg(c, 0);
        disc::ready(g_st.position, g_st.buffer_sectors ? g_st.buffer_sectors : 16u);
        c.r[2] = 1;
    });
    def("cdvdman", 59, "sceCdStStart", [](Cpu &c) {
        if (trace_cd()) std::fprintf(stderr, "[cd] StStart %u (was at %u)\n", arg(c, 0), g_st.position);
        g_st.position = arg(c, 0);
        g_st.active = true;
        disc::ready(g_st.position, g_st.buffer_sectors ? g_st.buffer_sectors : 16u);
        c.r[2] = 1;
    });
    def("cdvdman", 60, "sceCdStStat", [](Cpu &c) {
        c.r[2] = g_st.active ? g_st.buffer_sectors : 0u;
        if (trace_cd()) std::fprintf(stderr, "[cd] StStat -> %u (position %u)\n", c.r[2], g_st.position);
    });
    def("cdvdman", 61, "sceCdStStop", [](Cpu &c) {
        if (trace_cd()) std::fprintf(stderr, "[cd] StStop at %u\n", g_st.position);
        g_st.active = false;
        c.r[2] = 1;
    });
    def("cdvdman", 74, "sceCdPowerOff", [](Cpu &c) { c.r[2] = 1; });
    def("cdvdman", 75, "sceCdMmode", [](Cpu &c) { c.r[2] = 1; });
    def("cdvdman", 78, "sceCdPOffCallback", [](Cpu &c) { c.r[2] = 0; });
    def("cdvdman", 83, "sceCdReadDvdDualInfo", [](Cpu &c) {
        write32(arg(c, 0), disc::layer1_start() ? 1u : 0u);
        write32(arg(c, 1), disc::layer1_start());
        c.r[2] = 1;
    });
    def("cdvdman", 84, "sceCdLayerSearchFile", [](Cpu &c) {
        const std::string name = ram_string(arg(c, 1), 256);
        const auto entry = disc::find(name);
        if (!entry) { c.r[2] = 0; return; }
        write32(arg(c, 0), entry->sector);
        write32(arg(c, 0) + 4, entry->size);
        c.r[2] = 1;
    });
}

bool cd_busy() { return g_cd.busy; }

}

std::int32_t load_module(const std::string &path, const std::string &args, std::function<void(int, int)> done) {
    std::vector<std::uint8_t> data;
    std::string lower = path;
    for (char &ch : lower) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (lower.rfind("cdrom", 0) == 0) data = kernel::read_disc_file(path);
    if (data.empty() && lower.rfind("rom0:", 0) != 0) {
        std::fprintf(stderr, "[iop] cannot read module %s\n", path.c_str());
    }
    return load_module_buffer(data, path, args, std::move(done));
}

}

namespace ps2::iop::kernel {

void cdvd_state(ps2::state::Stream &s) {
    s.tag("cdvd");
    s.pod(g_cd.busy);
    s.pod(g_cd.callback);
    s.pod(g_cd.error);
    s.vec(g_cd.sync_waiters);
    if (s.version() >= 6) s.pod(g_st);
}

}
