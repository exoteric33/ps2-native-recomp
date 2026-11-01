#include "io.hpp"
#include "kernel.hpp"

#include "ps2/disc.hpp"

#include "ps2/state.hpp"

#include <algorithm>
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

std::string device_of(const std::string &path) {
    const auto colon = path.find(':');
    std::string d = colon == std::string::npos ? "" : path.substr(0, colon);
    for (char &ch : d) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    while (!d.empty() && std::isdigit(static_cast<unsigned char>(d.back()))) d.pop_back();
    return d;
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
            const std::string text = "0220EC20060210\n";
            f.memory.assign(text.begin(), text.end());
        } else if (name == "OSDSYS" || name.rfind("rom0:", 0) == 0) {
            return kNoEntry;
        } else {
            return kNoEntry;
        }
        f.in_memory = true;
        f.size = static_cast<std::uint32_t>(f.memory.size());
    } else {
        std::fprintf(stderr, "[io] open %s (flags 0x%X) on unsupported device\n", path.c_str(), flags);
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

void reset() { g_files.clear(); }

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
    for (std::uint32_t i : {9u, 10u, 11u, 12u, 13u, 14u, 15u, 17u, 20u, 21u, 25u, 27u, 31u})
        def("ioman", i, "ioman", [](Cpu &c) { c.r[2] = static_cast<std::uint32_t>(-1); });
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
