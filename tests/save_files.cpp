// No game data: checks that the memory card file and save states are never lost. A card file that is there but cannot be used
// (too short, too long, a folder, held open by another program) must stay byte for byte as it was, and the run uses a fresh card
// beside it; a write that is cut off right before its final rename (as a crash there would) must leave the old file whole; after a
// crash the newest card goes beside the card file, never over it. Each card case runs this program again as a child process, since
// the card is loaded once per run. Build: ninja ps2savetest, run build/.../ps2savetest [work folder] (prints "save files: 0 failed").
#include "iop/sio2.hpp"
#include "ps2/os.hpp"
#include "ps2/system.hpp"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <process.h>
#include <share.h>
#include <sys/stat.h>
#else
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {
namespace fs = std::filesystem;
using Bytes = std::vector<std::uint8_t>;

constexpr std::size_t kCardBytes = 16384 * 528, kMarkerAt = 4000 * 528;
const char kMarker[] = "ps2-native-recomp card flush test, frame ";

int failed = 0, checks = 0;
void check(bool ok, const std::string &message) {
    ++checks;
    if (!ok) {
        ++failed;
        std::fprintf(stderr, "FAIL: %s\n", message.c_str());
    }
}

Bytes read_file(const fs::path &p) {
    std::ifstream in(p, std::ios::binary);
    return Bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
void write_file(const fs::path &p, const Bytes &data) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char *>(data.data()), static_cast<std::streamsize>(data.size()));
}
Bytes pattern(std::size_t size, unsigned seed) {
    Bytes b(size);
    for (std::size_t i = 0; i < size; ++i) b[i] = static_cast<std::uint8_t>((i * 31 + seed * 7 + (i >> 9)) & 0xFF);
    return b;
}
bool has_marker(const Bytes &card) {
    return card.size() == kCardBytes && std::memcmp(card.data() + kMarkerAt, kMarker, sizeof kMarker - 1) == 0;
}
// the card with the marker page put back to what it was: equal to `before` when only the marker changed
bool same_but_marker(Bytes card, const Bytes &before) {
    if (card.size() != before.size() || card.size() < kMarkerAt + 64) return false;
    std::memcpy(card.data() + kMarkerAt, before.data() + kMarkerAt, 64);
    return card == before;
}

std::string g_self;

// runs this program as a child with the mode and the path; returns its exit code
int child(const char *mode, const fs::path &p) {
    const std::string where = p.string();
#ifdef _WIN32
    const std::string self = "\"" + g_self + "\"", quoted = "\"" + where + "\"";  // _spawnv joins the arguments with spaces
    const char *args[] = {self.c_str(), mode, quoted.c_str(), nullptr};
    std::fflush(nullptr);
    return static_cast<int>(_spawnv(_P_WAIT, g_self.c_str(), args));
#else
    std::fflush(nullptr);
    const pid_t pid = fork();
    if (pid == 0) {
        execl(g_self.c_str(), g_self.c_str(), mode, where.c_str(), static_cast<char *>(nullptr));
        _exit(127);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128;
#endif
}

// The child: put the card file in slot 1 (as the runner does at boot), let the test switch of sio2.cpp change page 4000 at frame 0
// as if the game had saved, then end the run the usual way (the card file is written) or as after a crash.
int run_child(const std::string &mode, const std::string &card) {
    ps2::g_options.memcard_path = card;
    ps2::os::set_env("PS2_TEST_CARD_DIRTY", "0");
    if (mode == "cut-card" || mode == "cut-file") ps2::os::set_env("PS2_TEST_CUT_BEFORE_RENAME", "1");
    if (mode == "cut-file") {
        const char text[] = "the new file";
        return ps2::os::replace_file(card, text, sizeof text - 1) ? 0 : 1;  // never returns: cut before the rename
    }
    ps2::iop::sio2::reset();
    ps2::iop::sio2::flush_cards();  // frame 0: the test switch marks the card as changed
    if (mode == "crash-card") return ps2::save_memory_card_after_crash(nullptr) == 1 ? 0 : 1;
    ps2::flush_memory_card(false);
    return 0;
}

fs::path g_work;
int g_case = 0;
fs::path fresh_folder() {
    const fs::path dir = g_work / ("case" + std::to_string(++g_case));
    std::error_code error;
    fs::remove_all(dir, error);
    fs::create_directories(dir);
    return dir;
}

// A card file that cannot be used stays untouched; the run uses (and writes) card.fresh.ps2 beside it.
void bad_card(const char *what, const Bytes &content) {
    const fs::path dir = fresh_folder(), card = dir / "card.ps2", fresh = dir / "card.fresh.ps2";
    write_file(card, content);
    check(child("card", card) == 0, std::string(what) + ": the run ended normally");
    check(read_file(card) == content, std::string(what) + ": the card file is byte for byte as it was");
    check(has_marker(read_file(fresh)), std::string(what) + ": the game's save went to card.fresh.ps2");
}

void card_cases() {
    {
        const fs::path dir = fresh_folder(), card = dir / "card.ps2";
        check(child("card", card) == 0 && has_marker(read_file(card)), "no card file: a new card is written");
        check(!fs::exists(dir / "card.fresh.ps2") && !fs::exists(dir / "card.ps2.tmp"), "no card file: no other file is left");
    }
    {
        const fs::path dir = fresh_folder(), card = dir / "card.ps2";
        const Bytes before = pattern(kCardBytes, 1);
        write_file(card, before);
        check(child("card", card) == 0, "valid card: the run ended normally");
        const Bytes after = read_file(card);
        check(has_marker(after) && same_but_marker(after, before), "valid card: only the changed page is different");
        check(!fs::exists(dir / "card.fresh.ps2"), "valid card: no fresh card is made");
    }
    bad_card("short file", pattern(1000, 2));
    bad_card("raw image without ECC", pattern(16384 * 512, 3));
    bad_card("too long", pattern(kCardBytes + 528, 4));
    {
        const fs::path dir = fresh_folder(), card = dir / "card.ps2";
        write_file(card, Bytes());  // an empty file holds no saves: it becomes a new card
        check(child("card", card) == 0 && has_marker(read_file(card)), "empty file: a new card is written into it");
    }
    {
        const fs::path dir = fresh_folder(), card = dir / "card.ps2";
        fs::create_directories(card / "inside");
        check(child("card", card) == 0, "a folder: the run ended normally");
        check(fs::is_directory(card / "inside"), "a folder: it is still there");
        check(has_marker(read_file(dir / "card.fresh.ps2")), "a folder: the save went to card.fresh.ps2");
    }
    {
        // a valid card that another program holds open so that nobody else can read it (Windows), or that may not be read (others)
        const fs::path dir = fresh_folder(), card = dir / "card.ps2";
        const Bytes before = pattern(kCardBytes, 5);
        write_file(card, before);
#ifdef _WIN32
        int fd = -1;
        _wsopen_s(&fd, card.c_str(), _O_RDONLY | _O_BINARY, _SH_DENYRW, _S_IREAD);
        const bool locked = fd >= 0;
#else
        chmod(card.c_str(), 0);
        const bool locked = access(card.c_str(), R_OK) != 0;  // root may read it anyway: then the case says nothing
#endif
        if (locked) {
            check(child("card", card) == 0, "unreadable card: the run ended normally");
            check(has_marker(read_file(dir / "card.fresh.ps2")), "unreadable card: the save went to card.fresh.ps2");
        }
#ifdef _WIN32
        if (fd >= 0) _close(fd);
#else
        chmod(card.c_str(), 0644);
#endif
        check(!locked || read_file(card) == before, "unreadable card: the card file is byte for byte as it was");
        if (!locked) std::fprintf(stderr, "note: the unreadable-card case was left out (the file could still be read)\n");
    }
    {
        // a fresh card made by an earlier run is loaded and kept, while the bad card file stays as it is
        const fs::path dir = fresh_folder(), card = dir / "card.ps2", fresh = dir / "card.fresh.ps2";
        const Bytes bad = pattern(1000, 6), earlier = pattern(kCardBytes, 7);
        write_file(card, bad);
        write_file(fresh, earlier);
        check(child("card", card) == 0, "earlier fresh card: the run ended normally");
        check(read_file(card) == bad, "earlier fresh card: the bad card file is as it was");
        const Bytes after = read_file(fresh);
        check(has_marker(after) && same_but_marker(after, earlier), "earlier fresh card: it was loaded and only the changed page is different");
    }
    {
        // both unusable: no card in the slot, nothing is written
        const fs::path dir = fresh_folder(), card = dir / "card.ps2", fresh = dir / "card.fresh.ps2";
        const Bytes bad = pattern(1000, 8), bad_fresh = pattern(2000, 9);
        write_file(card, bad);
        write_file(fresh, bad_fresh);
        check(child("card", card) == 0, "both unusable: the run ended normally");
        check(read_file(card) == bad && read_file(fresh) == bad_fresh, "both unusable: neither file was changed");
    }
    {
        // the card write is cut off right before the rename: the old card is whole
        const fs::path dir = fresh_folder(), card = dir / "card.ps2";
        const Bytes before = pattern(kCardBytes, 10);
        write_file(card, before);
        check(child("cut-card", card) == 3, "card write cut before the rename: the run was cut off there");
        check(read_file(card) == before, "card write cut before the rename: the old card file is whole");
    }
    {
        // after a crash the newest card goes to card.crash.ps2, the card file is not touched
        const fs::path dir = fresh_folder(), card = dir / "card.ps2";
        const Bytes before = pattern(kCardBytes, 11);
        write_file(card, before);
        check(child("crash-card", card) == 0, "crash: the copy was written");
        check(read_file(card) == before, "crash: the card file is byte for byte as it was");
        const Bytes copy = read_file(dir / "card.crash.ps2");
        check(has_marker(copy) && same_but_marker(copy, before), "crash: card.crash.ps2 holds the newest card");
    }
}

// os::replace_file, which writes save states (system.cpp save_state) and the card
void file_cases() {
    const fs::path dir = fresh_folder(), target = dir / "state.p2s";
    const Bytes before = pattern(300000, 12);
    write_file(target, before);
    check(child("cut-file", target) == 3, "state write cut before the rename: the run was cut off there");
    check(read_file(target) == before, "state write cut before the rename: the old file is whole");

    check(fs::exists(dir / "state.p2s.tmp"), "state write cut before the rename: the new bytes were in the temporary file");
    fs::remove(dir / "state.p2s.tmp");  // what the cut left; the next write would overwrite it
    fs::create_directories(dir / "state.p2s.tmp");  // the temporary file cannot be made (as on a full or read-only disk)
    const Bytes next = pattern(5000, 13);
    std::string why;
    check(!ps2::os::replace_file(target.string(), next.data(), next.size(), &why) && !why.empty(), "write that cannot start: it fails and says why");
    check(read_file(target) == before, "write that cannot start: the old file is whole");
    fs::remove_all(dir / "state.p2s.tmp");

    check(ps2::os::replace_file(target.string(), next.data(), next.size(), &why), "normal write: it succeeds");
    check(read_file(target) == next, "normal write: the new content is there");
    check(!fs::exists(dir / "state.p2s.tmp"), "normal write: no temporary file is left");
}

}

int main(int argc, char **argv) {
    if (argc == 3 && std::string(argv[1]).find("card") != std::string::npos) return run_child(argv[1], argv[2]);
    if (argc == 3 && std::string(argv[1]) == "cut-file") return run_child(argv[1], argv[2]);
    g_self = fs::absolute(argv[0]).string();
#ifdef _WIN32
    if (g_self.size() < 4 || g_self.substr(g_self.size() - 4) != ".exe") g_self += ".exe";
#endif
    g_work = argc > 1 ? fs::path(argv[1]) : fs::temp_directory_path() / "ps2savetest";
    std::error_code error;
    fs::remove_all(g_work, error);
    fs::create_directories(g_work);
    card_cases();
    file_cases();
    std::printf("save files: %d of %d checks passed, %d failed\n", checks - failed, checks, failed);
    if (!failed) fs::remove_all(g_work, error);
    return failed ? 1 : 0;
}
