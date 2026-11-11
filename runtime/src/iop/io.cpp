#include "io.hpp"
#include "kernel.hpp"

#include "ps2/disc.hpp"

#include "ps2/state.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <map>

namespace ps2::iop::io {

namespace {

constexpr int kNoEntry = -2;
constexpr int kBadFd = -9;
constexpr int kTooMany = -24;

struct File {
    std::uint32_t sector = 0;
    std::uint32_t size = 0;
    std::uint64_t position = 0;
    std::vector<std::uint8_t> memory;
    bool in_memory = false;
};

std::map<int, File> g_files;

// The drivers that games' modules add with AddDrv (ioman 1.4, the iomanX layout: an iop_device_t with the name at +0 and the ops table
// at +16, whose first two entries are init and deinit; 16 places, ps2sdk iomanX.c, AFL-2.0). Only AddDrv and DelDrv use it so far:
// file calls on such a device are not passed to the driver yet (open says so). Not in save states, whose layout stays: after a state is
// loaded, a DelDrv of a driver added before it fails, as it did before this table.
struct Device {
    std::string name;
    std::uint32_t address;
};
constexpr std::size_t kMaxDevices = 16;
std::vector<Device> g_devices;

std::string lower_device(std::string d) {
    for (char &ch : d) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    while (!d.empty() && std::isdigit(static_cast<unsigned char>(d.back()))) d.pop_back();
    return d;
}

std::string device_of(const std::string &path) {
    const auto colon = path.find(':');
    return lower_device(colon == std::string::npos ? "" : path.substr(0, colon));
}

// The region letter of rom0:ROMVER ("0220EC20060210": version 2.20, region E, retail, built 2006-02-10) follows the disc: the game
// ID in front of the boot file in SYSTEM.CNF's BOOT2 line says where a disc was sold (SCUS, SLUS, PBPX: America; SLES, SCES, PCPX:
// Europe; SLPS, SCPS, SLPM, PAPX: Japan; SLKA, SCKA: Korea (K); SLAJ, SCAJ: Asia (H)). A game reads it to pick its language or video mode.
// Without a disc or a name this tells nothing and the European one stays, as before.
char disc_region() {
    const auto cnf = disc::find("SYSTEM.CNF");
    if (!cnf || cnf->directory || cnf->size == 0 || cnf->size > 65536) return 'E';
    std::vector<std::uint8_t> bytes((cnf->size + disc::kSector - 1) / disc::kSector * disc::kSector);
    if (!disc::read(cnf->sector, static_cast<std::uint32_t>(bytes.size() / disc::kSector), bytes.data())) return 'E';
    std::string text(bytes.begin(), bytes.begin() + cnf->size);
    for (char &ch : text) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    const auto boot = text.find("BOOT2");
    const auto colon = boot == std::string::npos ? boot : text.find(':', boot);
    if (colon == std::string::npos) return 'E';
    auto at = colon + 1;
    while (at < text.size() && (text[at] == '\\' || text[at] == '/' || text[at] == ' ')) ++at;
    const std::string id = text.substr(at, 4);
    static const struct { const char *prefix; char region; } kRegions[] = {
        {"SCUS", 'A'}, {"SLUS", 'A'}, {"PBPX", 'A'}, {"SLES", 'E'}, {"SCES", 'E'}, {"SLED", 'E'}, {"SCED", 'E'}, {"PCPX", 'E'},
        {"SLPS", 'J'}, {"SCPS", 'J'}, {"SLPM", 'J'}, {"SCPM", 'J'}, {"PAPX", 'J'}, {"SLKA", 'K'}, {"SCKA", 'K'}, {"SLAJ", 'H'}, {"SCAJ", 'H'},
    };
    for (const auto &r : kRegions)
        if (id == r.prefix) return r.region;
    return 'E';
}

int allocate() {
    for (int fd = 3; fd < 64; ++fd)
        if (!g_files.count(fd)) return fd;
    return kTooMany;
}

}

int open(const std::string &path, std::uint32_t flags) {
    const std::string device = device_of(path);
    File f;
    if (device == "cdrom") {
        const auto entry = disc::find(path);
        if (!entry || entry->directory) return kNoEntry;
        f.sector = entry->sector;
        f.size = entry->size;
    } else if (device == "rom") {
        std::string name = path.substr(path.find(':') + 1);
        if (name == "ROMVER") {
            std::string text = "0220EC20060210\n";
            text[4] = disc_region();
            f.memory.assign(text.begin(), text.end());
        } else if (name == "OSDSYS" || name.rfind("rom0:", 0) == 0) {
            return kNoEntry;
        } else {
            return kNoEntry;
        }
        f.in_memory = true;
        f.size = static_cast<std::uint32_t>(f.memory.size());
    } else {
        const bool driver = std::any_of(g_devices.begin(), g_devices.end(), [&](const Device &d) { return lower_device(d.name) == device; });
        std::fprintf(stderr, "[io] open %s (flags 0x%X) on unsupported device%s\n", path.c_str(), flags,
                     driver ? " (a game driver added it with AddDrv; its file calls are not passed to it yet)" : "");
        return kNoEntry;
    }
    const int fd = allocate();
    if (fd < 0) return fd;
    g_files[fd] = std::move(f);
    if (kernel::g_trace_hle) std::fprintf(stderr, "[io] open %s -> %d\n", path.c_str(), fd);
    return fd;
}

int close(int fd) { return g_files.erase(fd) ? 0 : kBadFd; }

int read(int fd, std::uint8_t *out, std::uint32_t size) {
    auto it = g_files.find(fd);
    if (it == g_files.end()) return kBadFd;
    File &f = it->second;
    if (f.position >= f.size) return 0;
    const std::uint32_t n = static_cast<std::uint32_t>(std::min<std::uint64_t>(size, f.size - f.position));
    if (f.in_memory) {
        std::memcpy(out, f.memory.data() + f.position, n);
    } else {
        const std::uint64_t start = f.position, end = f.position + n;
        const std::uint32_t first = static_cast<std::uint32_t>(start / disc::kSector);
        const std::uint32_t last = static_cast<std::uint32_t>((end + disc::kSector - 1) / disc::kSector);
        std::vector<std::uint8_t> buffer(static_cast<std::size_t>(last - first) * disc::kSector);
        disc::read(f.sector + first, last - first, buffer.data());
        std::memcpy(out, buffer.data() + (start - static_cast<std::uint64_t>(first) * disc::kSector), n);
    }
    f.position += n;
    return static_cast<int>(n);
}

int write(int fd, const std::uint8_t *, std::uint32_t size) {
    if (fd == 1 || fd == 2) return static_cast<int>(size);
    return g_files.count(fd) ? static_cast<int>(size) : kBadFd;
}

std::int64_t seek(int fd, std::int64_t offset, int whence) {
    auto it = g_files.find(fd);
    if (it == g_files.end()) return kBadFd;
    File &f = it->second;
    std::int64_t base = whence == 1 ? static_cast<std::int64_t>(f.position) : whence == 2 ? f.size : 0;
    const std::int64_t target = std::clamp<std::int64_t>(base + offset, 0, f.size);
    f.position = static_cast<std::uint64_t>(target);
    return target;
}

bool stat(const std::string &path, std::uint32_t &mode, std::uint32_t &size) {
    if (device_of(path) != "cdrom") return false;
    const auto entry = disc::find(path);
    if (!entry) return false;
    mode = entry->directory ? 0x0027u : 0x0017u;
    size = entry->size;
    return true;
}

void reset() {
    g_files.clear();
    g_devices.clear();
}

}

namespace ps2::iop {
// libs.cpp: a guest size that reaches past the end of memory is said (rate-limited), and a fill stays inside the IOP's memory.
void bad_span(const char *what, std::uint32_t address, std::uint64_t size, const char *outcome);
void fill_ram(const char *what, std::uint32_t dst, std::uint8_t value, std::uint64_t n);
}

namespace ps2::iop::kernel {

namespace {

std::uint32_t arg(const Cpu &c, int i) { return i < 4 ? c.r[4 + i] : read32(c.r[29] + 16u + static_cast<std::uint32_t>(i - 4) * 4u); }

void def(const char *library, std::uint32_t index, const char *name, std::function<void(Cpu &)> body) {
    register_hle(library, index, [library, name, body = std::move(body)](Cpu &c) {
        trace_hle(library, name, c);
        body(c);
    });
}

// A driver's init or deinit runs inside the ioman call, as on the console: the HLE call goes on into the driver's function, with the
// device as its argument, and that returns to `back`, which finishes the ioman call. The caller's return address and the device wait on
// the guest stack (above the 16 bytes the callee may use for its arguments), so a state saved while the driver runs keeps them.
void call_driver(Cpu &c, std::uint32_t function, std::uint32_t device, std::uint32_t back) {
    c.r[29] -= 32;
    write32(c.r[29] + 16, c.r[31]);
    write32(c.r[29] + 20, device);
    c.r[4] = device;
    c.r[31] = back;
    c.pc = function;
    c.npc = function + 4;
}

void back_from_driver(Cpu &c, std::uint32_t &device) {
    const std::uint32_t ra = read32(c.r[29] + 16);
    device = read32(c.r[29] + 20);
    c.r[29] += 32;
    c.r[31] = ra;
    c.pc = ra;
    c.npc = ra + 4;
}

std::uint32_t g_added_back = 0, g_deleted_back = 0;    // where the two return stubs are now
std::uint32_t g_added_boot = 0, g_deleted_boot = 0;    // where this boot made them (set by install_io_stubs)

}

void install_io() {
    io::reset();
    register_hle_library("ioman", 0x0104);
    def("ioman", 4, "open", [](Cpu &c) { c.r[2] = static_cast<std::uint32_t>(io::open(ram_string(arg(c, 0)), arg(c, 1))); });
    def("ioman", 5, "close", [](Cpu &c) { c.r[2] = static_cast<std::uint32_t>(io::close(static_cast<int>(arg(c, 0)))); });
    def("ioman", 6, "read", [](Cpu &c) {
        // More than the IOP's whole memory cannot land anywhere: cut, so a bad size does not ask the host for up to 4 GiB.
        std::uint32_t size = arg(c, 2);
        if (size > kRamSize) { bad_span("ioman read", arg(c, 1), size, "cut to 2 MiB"); size = kRamSize; }
        std::vector<std::uint8_t> buffer(size);
        const int n = io::read(static_cast<int>(arg(c, 0)), buffer.data(), size);
        for (int i = 0; i < n; ++i) write8(arg(c, 1) + static_cast<std::uint32_t>(i), buffer[static_cast<std::size_t>(i)]);
        c.r[2] = static_cast<std::uint32_t>(n);
    });
    def("ioman", 7, "write", [](Cpu &c) {
        if (arg(c, 0) == 1 || arg(c, 0) == 2) std::fprintf(stderr, "[iop-tty] %s", ram_string(arg(c, 1), arg(c, 2)).c_str());
        c.r[2] = static_cast<std::uint32_t>(io::write(static_cast<int>(arg(c, 0)), nullptr, arg(c, 2)));
    });
    def("ioman", 8, "lseek", [](Cpu &c) {
        c.r[2] = static_cast<std::uint32_t>(io::seek(static_cast<int>(arg(c, 0)), static_cast<std::int32_t>(arg(c, 1)), static_cast<int>(arg(c, 2))));
    });
    def("ioman", 16, "getstat", [](Cpu &c) {
        std::uint32_t mode = 0, size = 0;
        if (!io::stat(ram_string(arg(c, 0)), mode, size)) { c.r[2] = static_cast<std::uint32_t>(-2); return; }
        const std::uint32_t p = arg(c, 1);
        fill_ram("ioman getstat", p, 0, 40);
        write32(p, mode);
        write32(p + 8, size);
        c.r[2] = 0;
    });
    // AddDrv(device) and DelDrv(name) as ps2sdk's iomanX does them: a free place (16) or the name, then the driver's init (deinit), whose
    // result below 0 makes the call fail and keeps the table as it was. Every call is shown: a game adds few drivers, and a driver that is
    // missing or fails is worth seeing in a tester's log. Their two return stubs come from install_io_stubs, after the rest of the boot.
    def("ioman", 20, "AddDrv", [](Cpu &c) {
        const std::uint32_t device = arg(c, 0), ops = read32(device + 16), init = ops ? read32(ops) : 0;
        const std::string name = ram_string(read32(device));
        std::fprintf(stderr, "[io] AddDrv %s (device 0x%08X, init 0x%08X, ra 0x%08X)\n", name.c_str(), device, init, c.r[31]);
        if (io::g_devices.size() >= io::kMaxDevices) { c.r[2] = static_cast<std::uint32_t>(-1); return; }
        io::g_devices.push_back(io::Device{name, device});
        if (init) call_driver(c, init, device, g_added_back);
        else c.r[2] = 0;
    });
    def("ioman", 21, "DelDrv", [](Cpu &c) {
        const std::string name = ram_string(arg(c, 0));
        const auto it = std::find_if(io::g_devices.begin(), io::g_devices.end(), [&](const io::Device &d) { return d.name == name; });
        std::fprintf(stderr, "[io] DelDrv %s%s\n", name.c_str(), it == io::g_devices.end() ? ": no such driver" : "");
        if (it == io::g_devices.end()) { c.r[2] = static_cast<std::uint32_t>(-1); return; }
        const std::uint32_t device = it->address, ops = read32(device + 16), deinit = ops ? read32(ops + 4) : 0;
        if (deinit) { call_driver(c, deinit, device, g_deleted_back); return; }
        io::g_devices.erase(it);
        c.r[2] = 0;
    });
    // The rest is not done: each answers -1 and says so the first time a game calls it, by default, so a tester's normal log shows a
    // driver or a game that needs it. Names from ps2sdk's ioman 1.4 export table (iomanX exports.tab).
    struct Missing {
        std::uint32_t index;
        const char *name;
        bool path; // the first argument is a path or a name (else a file or directory descriptor)
    };
    static const Missing kMissing[] = {{9, "ioctl", false},  {10, "remove", true}, {11, "mkdir", true}, {12, "rmdir", true},
                                       {13, "dopen", true},  {14, "dclose", false}, {15, "dread", false}, {17, "chstat", true},
                                       {25, "rename", true}, {27, "sync", true},   {31, "devctl", true}};
    for (const Missing &m : kMissing)
        def("ioman", m.index, m.name, [&m](Cpu &c) {
            static bool said[64] = {};
            if (!said[m.index]) {
                said[m.index] = true;
                if (m.path) std::fprintf(stderr, "[io] ioman %s(%s) is not supported, answered -1 (shown once, ra 0x%08X)\n", m.name, ram_string(arg(c, 0)).c_str(), c.r[31]);
                else std::fprintf(stderr, "[io] ioman %s(%d) is not supported, answered -1 (shown once, ra 0x%08X)\n", m.name, static_cast<int>(arg(c, 0)), c.r[31]);
            }
            c.r[2] = static_cast<std::uint32_t>(-1);
        });
}

// The return stubs of AddDrv and DelDrv. The boot hands out the kernel area's stubs one after the other, and a saved state keeps
// their addresses and the words in them in its memory image, so a stub that comes before the others moves all of them: a state
// saved before these two existed then loaded with the RPC loop's stubs at other addresses and the IOP's sound and disc calls
// never finished. These come last (after install_sif), so every older address stays where it was.
void install_io_stubs() {
    g_added_back = hle_stub([](Cpu &c) {
        std::uint32_t device = 0;
        back_from_driver(c, device);
        const auto result = static_cast<std::int32_t>(c.r[2]);
        if (result < 0) {
            std::erase_if(io::g_devices, [&](const io::Device &d) { return d.address == device; });
            std::fprintf(stderr, "[io] AddDrv: the init of driver 0x%08X failed (%d)\n", device, result);
        }
        c.r[2] = result < 0 ? static_cast<std::uint32_t>(-1) : 0;
    });
    g_deleted_back = hle_stub([](Cpu &c) {
        std::uint32_t device = 0;
        back_from_driver(c, device);
        const auto result = static_cast<std::int32_t>(c.r[2]);
        if (result < 0) std::fprintf(stderr, "[io] DelDrv: the deinit of driver 0x%08X failed (%d)\n", device, result);
        else std::erase_if(io::g_devices, [&](const io::Device &d) { return d.address == device; });
        c.r[2] = result < 0 ? static_cast<std::uint32_t>(-1) : 0;
    });
    g_added_boot = g_added_back;
    g_deleted_boot = g_deleted_back;
}

// A state saved when these two stubs came first in the kernel area (before the RPC loop's) keeps them at `first` and `first + 16`; any
// other state has them where this boot made them (`first` is 0). Adds the moves from where they are now, and notes the new places.
void plan_io_stubs(std::vector<std::pair<std::uint32_t, std::uint32_t>> &moves, std::uint32_t first) {
    const std::uint32_t added = first ? first : g_added_boot, deleted = first ? first + 16 : g_deleted_boot;
    moves.emplace_back(g_added_back, added);
    moves.emplace_back(g_deleted_back, deleted);
    g_added_back = added;
    g_deleted_back = deleted;
}

}

namespace ps2::iop::io {

void state(ps2::state::Stream &s) {
    s.tag("iopio");
    s.map(g_files, [&s](File &f) {
        s.pod(f.sector);
        s.pod(f.size);
        s.pod(f.position);
        s.vec(f.memory);
        s.pod(f.in_memory);
    });
}

}
