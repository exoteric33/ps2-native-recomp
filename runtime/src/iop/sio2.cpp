#include "sio2.hpp"

#include "hw.hpp"
#include "iop.hpp"
#include "ps2/os.hpp"
#include "ps2/system.hpp"

#include "ps2/state.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <io.h>
#elif !defined(__EMSCRIPTEN__)
#include <unistd.h>
#endif

namespace ps2::iop::sio2 {

namespace {

constexpr std::uint8_t kDigital = 0x41, kAnalog = 0x73, kPressure = 0x79, kConfig = 0xF3;

struct Pad {
    bool connected = false;
    std::uint16_t buttons = 0;
    std::uint8_t axes[4] = {0x80, 0x80, 0x80, 0x80};
    std::uint8_t mode = kDigital;
    bool config = false;
    bool locked = false;
    std::uint8_t poll_mask[3] = {0xFF, 0xFF, 0x03};
};

constexpr std::uint8_t kLibpadMotors[6] = {0x00, 0x01, 0xFF, 0xFF, 0xFF, 0xFF};

struct Feel {
    std::uint8_t motors[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    std::uint8_t pressure[16] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    std::atomic<std::uint16_t> rumble{0};
};

struct Dma {
    bool armed = false;
    std::uint32_t madr = 0, block_words = 0, blocks = 0;
};

constexpr std::uint32_t kPageBytes = 528, kPages = 0x4000, kBlockPages = 16;
constexpr std::size_t kCardBytes = static_cast<std::size_t>(kPageBytes) * kPages;

struct Card {
    bool present = false;
    bool looked = false;  // the card file was looked at (once per run: a game resets the IOP several times while it boots)
    std::string file;     // the file this run reads and writes: the one given with --memcard, or the fresh card beside it
    std::string crash_file;  // where the card goes after a crash (never over `file`)
    std::vector<std::uint8_t> data;
    std::size_t address = 0;
    std::uint8_t term = 0x55;
    // The emulation thread changes the card; the window thread (window closed) or a console handler (Ctrl+C) may write the file
    // while that goes on, so a card command and a file write exclude each other.
    std::mutex lock;
    std::atomic<bool> dirty{false};
    std::atomic<std::uint32_t> quiet{0};  // frames since the last change; the file is written after 50
    std::uint64_t reads = 0, writes = 0, erases = 0, reported = 0;  // pages read, pages written, blocks erased (one line at the end of the run)
};

std::uint32_t g_send3[16];
std::uint32_t g_ctrl1[4], g_ctrl2[4];
std::uint32_t g_index = 0;
std::uint32_t g_stat = 0;
std::deque<std::uint8_t> g_in, g_out;
Pad g_pads[2];
// which ports the host has a controller in; kept out of save states so loading one does not unplug a pad
bool g_plugged[2] = {true, false};
Feel g_feel[2];
Card g_card;
Dma g_dma[2];

void no_device() { g_stat |= 0xC000u | (0x10000u << (g_index & 15u)); }

// PS2_PAD_LOG=1: what the game's pad driver asks of a controller port. Every command except the poll (0x42) is printed once
// per use with its argument bytes and the mode the pad is in afterwards, and so is the first poll after the mode changed, with
// the number of polls before it.
bool pad_log() {
    static const bool on = [] {
        const std::string value = os::env("PS2_PAD_LOG");
        return !value.empty() && value != "0";
    }();
    return on;
}

const char *pad_mode_name(std::uint8_t mode, bool config) {
    if (config) return "config mode";
    return mode == kPressure ? "analog with pressure" : mode == kAnalog ? "analog" : "digital";
}

void log_pad_command(int port, std::uint8_t cmd, const std::vector<std::uint8_t> &in, const Pad &p) {
    static std::uint64_t polls[2] = {};
    static int last_poll_mode[2] = {-1, -1};
    const int mode = p.config ? kConfig : p.mode;
    if (cmd == 0x42) {
        ++polls[port];
        if (last_poll_mode[port] == mode) return;
        last_poll_mode[port] = mode;
        std::fprintf(stderr, "[pad] frame %llu port %d poll: pad is in %s (id %02X), %llu polls so far\n", static_cast<unsigned long long>(ps2::frames_run()), port + 1,
                     pad_mode_name(p.mode, p.config), mode, static_cast<unsigned long long>(polls[port]));
        return;
    }
    static const char *const kNames[] = {"0x40 ?", "0x41 query poll mask", "0x42 poll", "0x43 config mode", "0x44 set mode", "0x45 query model", "0x46 query act",
                                         "0x47 query comb", "0x48 ?", "0x49 ?", "0x4A ?", "0x4B ?", "0x4C query mode", "0x4D motor mapping", "0x4E ?", "0x4F pressure mode"};
    char args[32] = {};
    for (std::size_t i = 3; i < in.size() && i < 9; ++i) std::snprintf(args + (i - 3) * 3, 4, "%02X ", in[i]);
    std::fprintf(stderr, "[pad] frame %llu port %d command %s %s-> %s%s\n", static_cast<unsigned long long>(ps2::frames_run()), port + 1,
                 cmd >= 0x40 && cmd <= 0x4F ? kNames[cmd - 0x40] : "unknown", args, pad_mode_name(p.mode, p.config), p.locked ? ", locked" : "");
}

void controller(int port, std::size_t at, std::uint32_t out_size) {
    const std::vector<std::uint8_t> in(g_in.begin(), g_in.end());
    if (port > 1 || !g_pads[port].connected || in.empty() || in[0] != 0x01) {
        no_device();
        return;
    }
    Pad &p = g_pads[port];
    Feel &f = g_feel[port];
    const std::uint8_t cmd = in.size() > 1 ? in[1] : 0;
    auto put = [&](std::size_t i, std::uint8_t v) {
        if (i < out_size) g_out[at + i] = v;
    };
    put(0, 0xFF);
    put(1, p.config ? kConfig : p.mode);
    put(2, 0x5A);
    const std::uint16_t raw = static_cast<std::uint16_t>(~p.buttons);
    auto data = [&]() {
        put(3, static_cast<std::uint8_t>(raw));
        put(4, static_cast<std::uint8_t>(raw >> 8));
        if (p.mode != kDigital) {
            put(5, p.axes[0]);
            put(6, p.axes[1]);
            put(7, p.axes[2]);
            put(8, p.axes[3]);
        }
        if (p.mode == kPressure) {
            static const int kBits[12] = {5, 7, 4, 6, 12, 13, 14, 15, 10, 11, 8, 9};
            for (int i = 0; i < 12; ++i) put(9 + static_cast<std::size_t>(i), (p.buttons >> kBits[i]) & 1u ? f.pressure[kBits[i]] : 0x00);
        }
    };
    switch (cmd) {
    case 0x40:
        put(3, 0); put(4, 0); put(5, 2); put(6, 0); put(7, 0); put(8, 0x5A);
        break;
    case 0x41:
        if (p.mode == kDigital) for (int i = 3; i < 9; ++i) put(static_cast<std::size_t>(i), 0);
        else { put(3, p.poll_mask[0]); put(4, p.poll_mask[1]); put(5, p.poll_mask[2]); put(6, 0); put(7, 0); put(8, 0x5A); }
        break;
    case 0x42: {
        data();
        std::uint8_t small = 0, large = 0;
        for (std::size_t i = 0; i < 6 && 3 + i < in.size(); ++i) {
            if (f.motors[i] == 0x00) small = in[3 + i] & 1u;
            if (f.motors[i] == 0x01) large = in[3 + i];
        }
        f.rumble.store(static_cast<std::uint16_t>(small | (large << 8)), std::memory_order_relaxed);
        break;
    }
    case 0x43:
        if (!p.config) data();
        else for (int i = 3; i < 9; ++i) put(static_cast<std::size_t>(i), 0);
        if (in.size() > 3) p.config = in[3] == 1;
        break;
    case 0x44:
        if (in.size() > 4) {
            p.mode = in[3] ? kAnalog : kDigital;
            p.locked = in[4] == 3;
        }
        for (int i = 3; i < 9; ++i) put(static_cast<std::size_t>(i), 0);
        break;
    case 0x45:
        put(3, 0x03); put(4, 0x02); put(5, p.mode == kDigital ? 0 : 1); put(6, 0x02); put(7, 0x01); put(8, 0x00);
        break;
    case 0x46:
        put(3, 0);
        if (in.size() > 3 && in[3] == 0) { put(4, 0x00); put(5, 0x01); put(6, 0x02); put(7, 0x00); put(8, 0x0A); }
        else { put(4, 0x00); put(5, 0x01); put(6, 0x01); put(7, 0x01); put(8, 0x14); }
        break;
    case 0x47:
        put(3, 0); put(4, 0x00); put(5, 0x02); put(6, 0x00); put(7, 0x01); put(8, 0x00);
        break;
    case 0x4C:
        put(3, 0);
        if (in.size() > 3 && in[3] == 0) { put(4, 0); put(5, 0); put(6, 0x04); put(7, 0); put(8, 0); }
        else { put(4, 0); put(5, 0); put(6, 0x07); put(7, 0); put(8, 0); }
        break;
    case 0x4D:
        for (int i = 3; i < 9; ++i) put(static_cast<std::size_t>(i), 0xFF);
        for (std::size_t i = 0; i < 6 && 3 + i < in.size(); ++i) f.motors[i] = in[3 + i];
        f.rumble.store(0, std::memory_order_relaxed);
        break;
    case 0x4F:
        if (in.size() > 5) {
            p.poll_mask[0] = in[3];
            p.poll_mask[1] = in[4];
            p.poll_mask[2] = in[5];
        }
        p.mode = kPressure;
        put(3, 0); put(4, 0); put(5, 0); put(6, 0); put(7, 0); put(8, 0x5A);
        break;
    default:
        data();
        break;
    }
    if (pad_log()) log_pad_command(port, cmd, in, p);
}

std::uint8_t xor_of(const std::uint8_t *p, std::size_t n) {
    std::uint8_t v = 0;
    for (std::size_t i = 0; i < n; ++i) v ^= p[i];
    return v;
}

// "<folder>/<name>.<tag><extension>" beside a card file: gow2.ps2 -> gow2.fresh.ps2
std::string beside(const std::string &card, const char *tag) {
    const std::filesystem::path p = os::path(card);
    std::filesystem::path name = p.stem();
    name += std::string(".") + tag;
    name += p.extension();
    return os::utf8(p.parent_path() / name);
}

bool exists(const std::string &file) {
    std::error_code error;
    return std::filesystem::exists(os::path(file), error);
}

enum class CardFile { missing, good, bad };

// Reads a card file into `data` only when it is a whole card: 16384 pages of 512 bytes data + 16 bytes spare (ECC), 8650752 bytes.
// `why` says what is wrong with a file that is there but cannot be used (a raw image without the spare bytes, a bigger card, a
// folder, a file another program holds open).
CardFile read_card_file(const std::string &file, std::vector<std::uint8_t> &data, std::string &why) {
    namespace fs = std::filesystem;
    const fs::path p = os::path(file);
    std::error_code error;
    const fs::file_status status = fs::status(p, error);
    if (status.type() == fs::file_type::not_found) return CardFile::missing;
    if (error) {
        why = "it cannot be looked at (" + error.message() + ")";
        return CardFile::bad;
    }
    if (!fs::is_regular_file(status)) {
        why = "it is not a file (a folder card of another emulator?)";
        return CardFile::bad;
    }
    const std::uintmax_t size = fs::file_size(p, error);
    if (!error && size == 0) return CardFile::missing;  // an empty file holds no saves: it becomes a new card, as a missing one
    if (error || size != kCardBytes) {
        why = error ? "its size cannot be read (" + error.message() + ")"
                    : "it has " + std::to_string(size) + " bytes, a card file has " + std::to_string(kCardBytes) +
                          (size == static_cast<std::uintmax_t>(512) * kPages ? " (a raw image without the spare ECC bytes?)" : "");
        return CardFile::bad;
    }
    std::vector<std::uint8_t> got(kCardBytes);
    std::ifstream in(p, std::ios::binary);
    if (in) in.read(reinterpret_cast<char *>(got.data()), static_cast<std::streamsize>(kCardBytes));
    if (!in || in.gcount() != static_cast<std::streamsize>(kCardBytes)) {
        why = "it cannot be read (does another program have it open, or is it not allowed?)";
        return CardFile::bad;
    }
    data.swap(got);
    return CardFile::good;
}

// A card file that is there but cannot be used is never taken as a blank card: the game would offer to format it, and the
// formatted card would then be written over the file and the saves in it would be gone. Such a file is left exactly as it is,
// and the run uses a fresh card beside it ("<name>.fresh.ps2", loaded if an earlier run made it), so the game can still save.
void load_card() {
    Card &c = g_card;
    if (c.present || c.looked || g_options.memcard_path.empty()) return;
    std::lock_guard<std::mutex> hold(c.lock);
    c.looked = true;
    const std::string &named = g_options.memcard_path;
    const std::string fresh = beside(named, "fresh");
    std::vector<std::uint8_t> data;
    std::string why;
    c.file = named;
    CardFile got = read_card_file(named, data, why);
    if (got == CardFile::bad) {
        std::fprintf(stderr, "[mc] warning: %s cannot be used as a memory card: %s.\n", named.c_str(), why.c_str());
        std::fprintf(stderr, "[mc]          It is left exactly as it is. This run uses the card %s instead, so saves go there.\n", fresh.c_str());
        c.file = fresh;
        got = read_card_file(fresh, data, why);
        if (got == CardFile::bad) {
            std::fprintf(stderr, "[mc] warning: %s cannot be used either: %s. No card is in slot 1; nothing is written.\n", fresh.c_str(), why.c_str());
            return;
        }
    } else if (exists(fresh)) {
        std::fprintf(stderr, "[mc] note: there is also %s, a card made on a run when %s could not be used; its saves are not on this card.\n", fresh.c_str(),
                     named.c_str());
    }
    c.crash_file = beside(c.file, "crash");
    if (exists(c.crash_file))
        std::fprintf(stderr, "[mc] note: %s holds the card as it was when the program crashed on an earlier run; %s is the last card written normally.\n",
                     c.crash_file.c_str(), c.file.c_str());
    if (got == CardFile::good) c.data.swap(data);
    else c.data.assign(kCardBytes, 0xFF);
    c.present = true;
    std::fprintf(stderr, "[mc] card in slot 1: %s%s\n", c.file.c_str(), got == CardFile::missing ? " (new, unformatted)" : "");
}

void memory_card(std::size_t at, std::uint32_t out_size) {
    Card &c = g_card;
    std::lock_guard<std::mutex> hold(c.lock);
    const std::vector<std::uint8_t> in(g_in.begin(), g_in.end());
    if (!c.present || in.size() < 2 || in[0] != 0x81) {
        no_device();
        return;
    }
    const std::size_t n = out_size;
    auto put = [&](std::size_t i, std::uint8_t v) {
        if (i < n) g_out[at + i] = v;
    };
    auto ack = [&](std::size_t from) {
        for (std::size_t i = from; i + 2 < n; ++i) put(i, 0x00);
        if (n >= 2) put(n - 2, 0x2B);
        if (n >= 1) put(n - 1, c.term);
    };
    const std::uint8_t cmd = in[1];
    static const bool trace = std::getenv("PS2_TRACE_MC") != nullptr;
    if (trace) std::fprintf(stderr, "[mc] cmd %02X in %zu out %zu page %zu packet %u\n", cmd, in.size(), n, c.address / kPageBytes, g_index);
    switch (cmd) {
    case 0x21: case 0x22: case 0x23: {
        const std::uint32_t page = in.size() > 5 ? static_cast<std::uint32_t>(in[2] | (in[3] << 8) | (in[4] << 16) | (in[5] << 24)) : 0u;
        c.address = static_cast<std::size_t>(std::min(page, kPages - 1)) * kPageBytes;
        if (trace) std::fprintf(stderr, "[mc] %s page %u (frame %llu)\n", cmd == 0x21 ? "erase" : cmd == 0x22 ? "write" : "read", page, static_cast<unsigned long long>(ps2::frames_run()));
        if (cmd == 0x22) ++c.writes;
        if (cmd == 0x23) ++c.reads;
        ack(2);
        break;
    }
    case 0x26: {
        const std::uint8_t spec[8] = {0x00, 0x02, kBlockPages, 0x00, 0x00, 0x40, 0x00, 0x00};
        put(2, 0x2B);
        for (std::size_t i = 0; i < 8; ++i) put(3 + i, spec[i]);
        put(11, xor_of(spec, 8));
        put(12, c.term);
        break;
    }
    case 0x27: {
        // 81 27 <new terminator> 00 00 -> the answer ends with the OLD terminator (public documentation: ps2tek, "SIO2 PS2 Memcards";
        // mcman asks again until it sees the new one, which is why either way works for it)
        const std::uint8_t old = c.term;
        if (in.size() > 2) c.term = in[2];
        ack(2);
        put(n - 1, old);
        break;
    }
    case 0x28:
        put(2, 0x2B);
        put(3, c.term);
        put(4, 0x55);
        break;
    case 0x42: {
        const std::size_t len = in.size() > 2 ? in[2] : 0;
        for (std::size_t i = 0; i < len && 3 + i < in.size() && c.address < kCardBytes; ++i) c.data[c.address++] = in[3 + i];
        c.dirty = true;
        c.quiet = 0;
        // the answer is 2B, the terminator, the filler, and the terminator again as the last byte (ps2tek; mcman only looks at the last)
        ack(2);
        put(n - 2, 0x00);
        put(2, 0x2B);
        put(3, c.term);
        break;
    }
    case 0x43: {
        const std::size_t len = in.size() > 2 ? in[2] : 0;
        std::uint8_t sum = 0;
        put(2, 0x2B);
        put(3, c.term);
        for (std::size_t i = 0; i < len; ++i) {
            const std::uint8_t v = c.address < kCardBytes ? c.data[c.address++] : 0xFF;
            sum ^= v;
            put(4 + i, v);
        }
        put(4 + len, sum);
        put(5 + len, c.term);
        break;
    }
    case 0x82: {
        ++c.erases;
        const std::size_t block = c.address / (static_cast<std::size_t>(kPageBytes) * kBlockPages) * kPageBytes * kBlockPages;
        std::fill_n(c.data.begin() + static_cast<std::ptrdiff_t>(block), std::min<std::size_t>(kPageBytes * kBlockPages, kCardBytes - block), 0xFF);
        c.dirty = true;
        c.quiet = 0;
        ack(2);
        break;
    }
    default:
        ack(2);
        break;
    }
}

void process() {
    const std::uint32_t reg = g_send3[g_index & 15u];
    const std::uint32_t in_size = (reg >> 8) & 0x1FFu, out_size = (reg >> 18) & 0x1FFu;
    if (g_in.size() < in_size) return;
    const int port = static_cast<int>(reg & 3u);
    const std::size_t at = g_out.size();
    for (std::uint32_t i = 0; i < out_size; ++i) g_out.push_back(0xFF);
    if (g_index == 0) g_stat = 0x1100u;
    const std::uint32_t device = g_ctrl2[port];
    if (port == 2) memory_card(at, out_size);
    else if (device == 0x5FFFFu || device == 0x00030064u || port >= 2) no_device();
    else controller(port, at, out_size);
    g_in.clear();
    ++g_index;
}

void run_transfer() {
    Dma &in = g_dma[0], &out = g_dma[1];
    if (in.armed) {
        // A transfer by DMA starts at the first send3 entry, whatever the one before did (mcman sends a read of several packets
        // and then the next transfer without writing the control register's reset bits in between; Resident Evil 4's save failed
        // because the second transfer was read with the entries 16 to 26, so its commands and sizes did not match).
        g_index = 0;
        g_in.clear();
        for (std::uint32_t b = 0; b < in.blocks; ++b) {
            for (std::uint32_t i = 0; i < in.block_words * 4; ++i) g_in.push_back(read8(in.madr + b * in.block_words * 4 + i));
            process();
        }
        in.armed = false;
        hw::dma_done(11);
    }
    if (out.armed) {
        for (std::uint32_t b = 0; b < out.blocks && b < 16; ++b) {
            const std::uint32_t size = (g_send3[b] >> 18) & 0x1FFu;
            for (std::uint32_t i = 0; i < size && !g_out.empty(); ++i) {
                write8(out.madr + b * out.block_words * 4 + i, g_out.front());
                g_out.pop_front();
            }
        }
        out.armed = false;
        hw::dma_done(12);
    }
    hw::raise_interrupt(17);
}

}

void reset() {
    std::memset(g_send3, 0, sizeof g_send3);
    std::memset(g_ctrl1, 0, sizeof g_ctrl1);
    std::memset(g_ctrl2, 0, sizeof g_ctrl2);
    g_index = 0;
    g_stat = 0;
    g_in.clear();
    g_out.clear();
    g_pads[0] = Pad{};
    g_pads[1] = Pad{};
    g_pads[0].connected = g_plugged[0];
    g_pads[1].connected = g_plugged[1];
    for (Feel &f : g_feel) {
        std::fill(std::begin(f.motors), std::end(f.motors), 0xFF);
        f.rumble.store(0);
    }
    g_dma[0] = g_dma[1] = Dma{};
    load_card();
}

namespace {

// Writes the card to its file so that the file is always a whole card, the old one or the new one: the bytes go to
// "<file>.tmp", are forced to the disk, and the finished file then replaces the old one in one step (os::replace_file). A crash,
// a kill or a full disk in the middle leaves the old card where it was (and a stray .tmp file that the next write overwrites).
bool write_card_file(const Card &c) { return os::replace_file(c.file, c.data.data(), c.data.size()); }

// PS2_TEST_CARD_DIRTY=<frame>[,stop][,hold][,crash] is a switch for scripts/test_memcard.py, not for players. At that frame a marker text is
// put into page 4000 of the card, which counts as changed as if the game had just written it. With ",stop" the run ends in the same
// frame, as if the game had quit, so only a write at the end of the run can save the card: the one 50 frames after the last change
// has not had its time yet. With ",hold" the 50-frame write never comes, so a test can take its time to end the run from outside
// (close the window, press Ctrl+Break) and the file can only be there if that way of ending wrote it. With ",crash" the program
// writes to address 0 right after the change (a crash; on Windows the handler in host/main.cpp then writes the card).
void test_hook(Card &c) {
    static const std::string spec = os::env("PS2_TEST_CARD_DIRTY");
    if (spec.empty() || !c.present) return;
    const std::uint64_t at = std::strtoull(spec.c_str(), nullptr, 10);
    if (ps2::frames_run() > at && spec.find(",hold") != std::string::npos) c.quiet = 0;
    if (ps2::frames_run() != at) return;
    {
        std::lock_guard<std::mutex> hold(c.lock);
        char marker[64];
        std::snprintf(marker, sizeof marker, "ps2-native-recomp card flush test, frame %llu", static_cast<unsigned long long>(ps2::frames_run()));
        std::memcpy(c.data.data() + static_cast<std::size_t>(4000) * kPageBytes, marker, std::strlen(marker) + 1);
        c.dirty = true;
        c.quiet = 0;
    }
    std::fprintf(stderr, "[mc] test: card marked as changed at frame %llu\n", static_cast<unsigned long long>(ps2::frames_run()));
    if (spec.find(",stop") != std::string::npos) ps2::halt("memory card test");
    if (spec.find(",crash") != std::string::npos) {
        volatile int *nowhere = nullptr;
        *nowhere = 1;
    }
}

// Writes the card file if the game changed the card since the last write. In a hurry (Ctrl+C, the console or terminal closing)
// the card lock is waited for only a moment; a card command that still holds it is in the middle of changing a page, so then
// the file is not written at all and keeps the last whole card.
void write_card(bool hurry) {
    Card &c = g_card;
    if (!c.present || !c.dirty || c.file.empty()) return;
    std::unique_lock<std::mutex> hold(c.lock, std::defer_lock);
    if (!hurry) hold.lock();
    else
        for (int i = 0; i < 100 && !hold.try_lock(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    if (!hold.owns_lock()) {
        std::fprintf(stderr, "[mc] the card is busy, so %s is not written now; it keeps the card written last\n", c.file.c_str());
        return;
    }
    if (!c.dirty.exchange(false)) return;  // another thread wrote it in the meantime
    c.quiet = 0;
    if (!write_card_file(c)) {
        static bool complained = false;
        if (!complained) std::fprintf(stderr, "[mc] cannot write %s (the card stays in memory and is tried again)\n", c.file.c_str());
        complained = true;
        c.dirty = true;
        return;
    }
    std::fprintf(stderr, "[mc] card file written: %s\n", c.file.c_str());
    if (g_options.on_memcard_saved) g_options.on_memcard_saved();
}

}

// every frame: the file is written 50 frames after the card was last changed (a game writes it in many small steps)
void flush_cards() {
    Card &c = g_card;
    test_hook(c);
    if (!c.dirty || c.quiet.fetch_add(1) + 1 < 50) return;
    write_card(false);
}

// the end of the run (the window was closed, the frame limit was reached, the game stopped, Ctrl+C): the card file is written now
// if the card changed after the last write
void flush_cards_now(bool hurry) {
    write_card(hurry);
    Card &c = g_card;
    const std::uint64_t used = c.reads + c.writes + c.erases;
    if (!hurry && used != c.reported) {  // what the game did with the card, so a log tells at a glance whether it ever saved
        c.reported = used;
        std::fprintf(stderr, "[mc] card use so far: %llu page reads, %llu page writes, %llu block erases\n", static_cast<unsigned long long>(c.reads),
                     static_cast<unsigned long long>(c.writes), static_cast<unsigned long long>(c.erases));
    }
}

// After a crash. The card file is written 50 frames (under a second) after the game's last card write, so a card that changed
// since then may be half of a save, or damaged along with the rest of the program's memory: it never goes over the card file,
// which keeps the last whole card. It goes to "<name>.crash.ps2" beside it instead, which a player can try (copy it over the
// card file) if the newest save is missing. The card lock is waited for only a moment (the crashed thread may hold it, in the
// middle of a page), and nothing is written without it. Nothing is printed here (the crashed thread may hold the lock of stderr):
// returns 0 when the card had not changed, 1 when the copy was written (`where` is its name), 2 when the card was busy, 3 when
// the copy could not be written.
int save_card_after_crash(const char **where) {
    Card &c = g_card;
    if (!c.present || !c.dirty || c.crash_file.empty()) return 0;
    std::unique_lock<std::mutex> hold(c.lock, std::defer_lock);
    for (int i = 0; i < 100 && !hold.try_lock(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    if (!hold.owns_lock()) return 2;
    if (!os::replace_file(c.crash_file, c.data.data(), c.data.size())) return 3;
    if (where) *where = c.crash_file.c_str();
    return 1;
}

// A copy of the card as the game sees it now: the file is written 50 frames after a change, so it can be behind. With wait false the
// card is given up after a moment when a card command holds it (the report of a hanging game must not hang too).
bool card_image(std::vector<std::uint8_t> &out, bool wait) {
    Card &c = g_card;
    if (!c.present) return false;
    std::unique_lock<std::mutex> hold(c.lock, std::defer_lock);
    if (wait) hold.lock();
    else
        for (int i = 0; i < 40 && !hold.try_lock(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    if (!hold.owns_lock()) return false;
    out = c.data;
    return true;
}

std::uint32_t read(std::uint32_t a) {
    if (a >= 0x1F808200u && a < 0x1F808240u) return g_send3[(a - 0x1F808200u) / 4];
    if (a >= 0x1F808240u && a < 0x1F808260u) {
        const std::uint32_t port = (a - 0x1F808240u) / 8;
        return (a & 4u) ? g_ctrl2[port] : g_ctrl1[port];
    }
    switch (a) {
    case 0x1F808264u: {
        if (g_out.empty()) return 0xFF;
        const std::uint8_t v = g_out.front();
        g_out.pop_front();
        return v;
    }
    case 0x1F808268u: return 0;
    case 0x1F80826Cu: return g_stat;
    case 0x1F808270u: return 0xF;
    case 0x1F808274u: return 0;
    default: return 0;
    }
}

void write(std::uint32_t a, std::uint32_t v) {
    if (a >= 0x1F808200u && a < 0x1F808240u) { g_send3[(a - 0x1F808200u) / 4] = v; return; }
    if (a >= 0x1F808240u && a < 0x1F808260u) {
        const std::uint32_t port = (a - 0x1F808240u) / 8;
        if (a & 4u) g_ctrl2[port] = v;
        else g_ctrl1[port] = v;
        return;
    }
    switch (a) {
    case 0x1F808260u:
        g_in.push_back(static_cast<std::uint8_t>(v));
        process();
        break;
    case 0x1F808268u:
        if ((v & 0xCu) == 0xCu) {
            g_index = 0;
            g_in.clear();
            g_out.clear();
        }
        if (v & 1u) run_transfer();
        break;
    default:
        break;
    }
}

void arm_dma(int channel, std::uint32_t madr, std::uint32_t block_words, std::uint32_t blocks) {
    Dma &d = g_dma[channel == 11 ? 0 : 1];
    d.armed = true;
    d.madr = madr;
    d.block_words = block_words;
    d.blocks = blocks;
}

void set_buttons(int pad, std::uint16_t pressed_mask) {
    if (pad >= 0 && pad < 2) g_pads[pad].buttons = pressed_mask;
}

void set_axis(int pad, int axis, std::uint8_t value) {
    if (pad >= 0 && pad < 2 && axis >= 0 && axis < 4) g_pads[pad].axes[axis] = value;
}

void set_pressure(int pad, int button, std::uint8_t value) {
    if (pad >= 0 && pad < 2 && button >= 0 && button < 16) g_feel[pad].pressure[button] = value;
}

void set_connected(int pad, bool connected) {
    if (pad >= 0 && pad < 2) g_pads[pad].connected = g_plugged[pad] = connected;
}

std::uint16_t rumble(int pad) { return pad >= 0 && pad < 2 ? g_feel[pad].rumble.load(std::memory_order_relaxed) : 0; }

}

namespace ps2::iop::sio2 {

void state(ps2::state::Stream &s) {
    s.tag("sio2");
    s.pod(g_send3);
    s.pod(g_ctrl1);
    s.pod(g_ctrl2);
    s.pod(g_index);
    s.pod(g_stat);
    s.deque(g_in);
    s.deque(g_out);
    s.pod(g_pads);
    if (s.loading()) {
        g_pads[0].connected = g_plugged[0];
        g_pads[1].connected = g_plugged[1];
        for (Feel &f : g_feel) {
            std::copy(std::begin(kLibpadMotors), std::end(kLibpadMotors), f.motors);
            f.rumble.store(0);
        }
    }
    s.pod(g_dma);
    s.size(g_card.address);
    s.pod(g_card.term);
}

}

namespace ps2 {

// Writes the memory card file if the game changed the card since it was last written. The host calls this wherever the game can end
// (after the emulation returns, when the window is closed, on Ctrl+C or a crash with hurry set) so that a save is never lost.
void flush_memory_card(bool hurry) { iop::sio2::flush_cards_now(hurry); }

bool memory_card_image(std::vector<std::uint8_t> &out, bool wait) { return iop::sio2::card_image(out, wait); }

// The crash path of the host (host/main.cpp declares it; see save_card_after_crash above for what it does and returns).
int save_memory_card_after_crash(const char **where) { return iop::sio2::save_card_after_crash(where); }

}
