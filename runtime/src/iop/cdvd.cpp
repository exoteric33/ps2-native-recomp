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
    // The running read, for sceCdGetReadPos only (not in save states: after loading one, the position of a read in flight reads as 0).
    std::uint64_t read_start = 0;
    std::uint32_t read_sectors = 0;
    // What sceCdSC (#50) keeps for its callers, with the values cdvdman starts with (ps2sdk cdvdman.c, AFL-2.0).
    std::uint32_t stream_flag = 0;
    std::int32_t sync_timeout = 15000;    // milliseconds
    std::int32_t stream_timeout = 5000;   // milliseconds
    std::int32_t read_timeout = 0;
    std::int32_t spin_control = 0;
    std::int32_t ee_call = 0;             // the EE's N command that runs (cdvdfsv tells it with code 0xFFFFFFF6)
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
    g_cd.read_sectors = 0;
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

// The drive's clock as the S command 0x08 answers it (ps2sdk sceCdCLOCK): status 0 (read fine), second, minute, hour, 0, day, month,
// year, each a BCD byte, in Japan time (UTC+9) as the console's clock chip keeps it. Month is written without its top bit. The whole date
// is shifted to Japan time, not only the hour: from 15:00 to 24:00 UTC the day there is already the next one (GTA III's save of
// 2026-10-29 19:01 UTC was stamped 10-29 04:01 instead of 10-30 04:01).
void write_clock(std::uint32_t p) {
    const std::time_t t = static_cast<std::time_t>(rtc_seconds() + 9 * 3600);
    const std::tm *tm = std::gmtime(&t);
    write8(p + 0, 0);
    write8(p + 1, bcd(tm->tm_sec));
    write8(p + 2, bcd(tm->tm_min));
    write8(p + 3, bcd(tm->tm_hour));
    write8(p + 4, 0);
    write8(p + 5, bcd(tm->tm_mday));
    write8(p + 6, bcd(tm->tm_mon + 1));
    write8(p + 7, bcd(tm->tm_year % 100));
}

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
    g_cd.read_start = now();
    g_cd.read_sectors = sectors;
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
        write_clock(arg(c, 0));
        c.r[2] = 1;
    });
    // #44 sceCdGetReadPos (ps2sdk iop/cdvd/cdvdman/src/exports.tab and cdvdman.c, AFL-2.0): how many bytes of the running sceCdRead have
    // reached memory; 0 when no read runs. The disc here is read in one piece when the read's time is up, so the answer grows evenly with
    // the time the read takes (whole sectors of 0x800 bytes, always below the total while it runs).
    def("cdvdman", 44, "sceCdGetReadPos", [](Cpu &c) {
        c.r[2] = 0;
        if (!g_cd.busy || !g_cd.read_sectors) return;
        const std::uint64_t whole = read_cycles(g_cd.read_sectors), spent = now() > g_cd.read_start ? now() - g_cd.read_start : 0;
        const std::uint64_t done = std::min<std::uint64_t>(g_cd.read_sectors - 1, spent * g_cd.read_sectors / std::max<std::uint64_t>(whole, 1));
        c.r[2] = static_cast<std::uint32_t>(done * disc::kSector);
    });
    // #50 sceCdSC and #51 sceCdRC (ps2sdk iop/cdvd/cdvdman/src/exports.tab and cdvdman.c, AFL-2.0; the retail modules of the games keep the
    // same two entries).
    // sceCdSC(code, param) is cdvdman's back door for the other IOP modules of the disc system (cdvdstm, cdvdfsv, mcman, a game's own):
    // `code` is a negative number that picks a value to read or set, `param` is a pointer to an int for some of them. The answers of the
    // codes a disc in a working drive gives are here; a code that cdvdman does not know answers 0, as it does. Not answered (they hand out
    // pointers into cdvdman's own variables, or event flags this driver does not create): 0xFFFFFFF0, 0xFFFFFFF2, 0xFFFFFFF5 (the
    // interrupt event flag's id, what the stream module cdvdstm asks for when it starts, public issue 57), 0xFFFFFFE7 (the S command's flag)
    // and 0xFFFFFFE9 (the second layer's sector change). They read 0, and a WaitEventFlag on 0 fails at once instead of waiting, which
    // suits a drive that is always ready.
    def("cdvdman", 50, "sceCdSC", [](Cpu &c) {
        const std::uint32_t code = arg(c, 0), param = arg(c, 1);
        c.r[2] = 0;
        switch (code) {
        case 0xFFFFFFFFu: case 0: case 1: case 2:  // the last error goes to *param; codes 0 to 2 also set the stream flag, -1 reads it
            if (param) write32(param, g_cd.error & 0xFF);
            if (code != 0xFFFFFFFFu) g_cd.stream_flag = code;
            c.r[2] = g_cd.stream_flag;
            break;
        case 0xFFFFFFFEu:  // set the last error from the byte at *param
            if (param) { g_cd.error = read8(param); c.r[2] = g_cd.error; }
            break;
        case 0xFFFFFFFDu: c.r[2] = g_cd.busy ? 1u : 0u; break;  // a read is running
        case 0xFFFFFFFCu: c.r[2] = 0; break;                    // the disc's key is not set (no copy-protected disc here)
        case 0xFFFFFFF8u: if (param) g_cd.spin_control = static_cast<std::int32_t>(read32(param)); c.r[2] = 1; break;
        case 0xFFFFFFF7u: c.r[2] = 0x0226; break;               // cdvdman's version, 2.38 (IRX_ID("cdvd_driver", 2, 38))
        case 0xFFFFFFF6u:                                       // cdvdfsv: the EE's N command that runs now, 0 when none
            if (param) { g_cd.ee_call = static_cast<std::int32_t>(read32(param)); c.r[2] = static_cast<std::uint32_t>(g_cd.ee_call); }
            break;
        case 0xFFFFFFF4u: c.r[2] = 1; break;                    // the disc's kind fits the drive's mode: always
        case 0xFFFFFFF3u: c.r[2] = 1; break;                    // set the drive's mode from the boot mode (nothing to set)
        case 0xFFFFFFF1u: c.r[2] = static_cast<std::uint32_t>(g_cd.sync_timeout); break;
        case 0xFFFFFFEFu: c.r[2] = static_cast<std::uint32_t>(g_cd.stream_timeout); break;
        case 0xFFFFFFEEu: if (param) g_cd.read_timeout = static_cast<std::int32_t>(read32(param)); break;
        case 0xFFFFFFEAu: c.r[2] = 1; break;                    // the disc's layer information was read
        case 0xFFFFFFE6u: if (param) write32(param, 0); break;  // no power-off callback (sceCdPOffCallback); its user data is 0
        default: break;
        }
    });
    // sceCdRC(clock) is sceCdReadClock without the top bit of the month cleared (ps2sdk cdvdman.h: both send S command 0x08; in the newer
    // modules sceCdReadClock cleans that bit and sceCdRC gives the drive's bytes as they are). The clock chip's month byte has it clear, so
    // both answer alike. Returns 1 when the clock was read. mcman calls it (mcman_getmcrtime, ps2sdk mcman main.c) for the time it stamps
    // on every directory entry it writes: up to 64 times until the answer is not 0, then it reads the bytes. With the call missing (answer
    // 0) the card got a time from whatever the stack held (GTA III's save and Robotech's card format).
    def("cdvdman", 51, "sceCdRC", [](Cpu &c) {
        if (!arg(c, 0)) { c.r[2] = 0; return; }
        write_clock(arg(c, 0));
        c.r[2] = 1;
    });
    // #64 sceCdRM(name, status) reads the console's model name, e.g. "SCPH-39001", into 16 bytes (two S commands 0x17 of 8 bytes each),
    // sets *status to 0 and returns 1 (ps2sdk iop/cdvd/cdvdman exports.tab, cdvdman.c, libcdvd-common.h; AFL-2.0). Unreal Tournament's
    // i.LINK module PSX2ILI calls it when it starts (public issue 91). The answer is one fixed retail model.
    def("cdvdman", 64, "sceCdRM", [](Cpu &c) {
        static const char kModel[16] = "SCPH-39001";
        for (std::uint32_t i = 0; i < 16; ++i) write8(arg(c, 0) + i, static_cast<std::uint8_t>(kModel[i]));
        if (arg(c, 1)) write32(arg(c, 1), 0);
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
    def("cdvdman", 84, "sceCdLayerSearchFile", [](Cpu &c) {  // (file, path, layer)
        const std::string name = ram_string(arg(c, 1), 256);
        const auto entry = disc::find(name, static_cast<int>(arg(c, 2)));
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
