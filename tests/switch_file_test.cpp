// Offline test of the switch file reader (host/switch_file.cpp): no game, no runtime, no SDL.
//
//   clang++ -std=c++20 -O2 tests/switch_file_test.cpp host/switch_file.cpp -o /tmp/ps2-switch-file-test
//   /tmp/ps2-switch-file-test        (prints "switch file: N checked, 0 failed")
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "../host/switch_file.hpp"

namespace {

int g_failed = 0, g_checked = 0;

void check(bool ok, const char *what) {
    ++g_checked;
    if (!ok) {
        ++g_failed;
        std::printf("FAIL: %s\n", what);
    }
}

using Switches = std::vector<std::pair<std::string, std::string>>;

void expect(const std::string &text, const Switches &switches, std::size_t ignored, const char *what) {
    const switch_file::Parsed p = switch_file::parse(text);
    check(p.switches == switches, what);
    check(p.ignored.size() == ignored, what);
    if (p.switches != switches || p.ignored.size() != ignored) {
        for (const auto &[n, v] : p.switches) std::printf("    got %s=%s\n", n.c_str(), v.c_str());
        for (const auto &note : p.ignored) std::printf("    ignored %s\n", note.c_str());
    }
}

}  // namespace

int main() {
    expect("", {}, 0, "empty file");
    expect("PS2_WAIT_STATS=1", {{"PS2_WAIT_STATS", "1"}}, 0, "one line, no line end");
    expect("PS2_WAIT_STATS=1\r\nPS2_SCALE=2\r\n", {{"PS2_WAIT_STATS", "1"}, {"PS2_SCALE", "2"}}, 0, "Windows line ends");
    expect("\n# a comment\n   \n\t# indented comment\nPS2_SCALE=3\n\n", {{"PS2_SCALE", "3"}}, 0, "blank lines and comments");
    expect("  PS2_SCALE  =  2  \n", {{"PS2_SCALE", "2"}}, 0, "blanks around name and value");
    expect("set PS2_WAIT_STATS=1\nSET PS2_SCALE=2\n", {{"PS2_WAIT_STATS", "1"}, {"PS2_SCALE", "2"}}, 0, "a leading set");
    expect("set \"PS2_WAIT_STATS=1\"\nPS2_LOOK=\"crt\"\n", {{"PS2_WAIT_STATS", "1"}, {"PS2_LOOK", "crt"}}, 0, "quotes");
    expect("\xEF\xBB\xBFPS2_SCALE=2\n", {{"PS2_SCALE", "2"}}, 0, "UTF-8 byte order mark");
    expect(std::string("\xFF\xFEP\0S\0" "2\0_\0X\0=\0" "1\0\r\0\n\0", 20), {{"PS2_X", "1"}}, 0, "UTF-16 (Notepad's Unicode)");
    expect("PS2_SCALE=2\nPS2_SCALE=3\n", {{"PS2_SCALE", "3"}}, 0, "a name twice keeps the last value");
    expect("PS2_TRACE=a=b\n", {{"PS2_TRACE", "a=b"}}, 0, "an '=' inside the value stays");
    expect("PS2_FRAMES_DIR=C:\\Users\\x y\\shots\n", {{"PS2_FRAMES_DIR", "C:\\Users\\x y\\shots"}}, 0, "a path with a blank");
    // what is not taken
    expect("PATH=C:\\evil\n", {}, 1, "a name that is not PS2_");
    expect("ps2_scale=2\n", {}, 1, "lower case is not PS2_");
    expect("PS2_=1\n", {}, 1, "PS2_ alone");
    expect("PS2_SCALE\n", {}, 1, "no '='");
    expect("PS2_SCALE=\n", {}, 1, "no value");
    expect("PS2 SCALE=2\nPS2_SC-ALE=2\n", {}, 2, "a name with a blank or a dash");
    expect("PS2_WAIT_STATS=1\nhello\nPS2_SCALE=2\n", {{"PS2_WAIT_STATS", "1"}, {"PS2_SCALE", "2"}}, 1, "a bad line between good ones");
    {
        const auto p = switch_file::parse("PS2_SCALE=2\n\nFOO=1\n");
        check(p.ignored.size() == 1 && p.ignored[0] == "line 3: FOO (not a PS2_ name)", "the note names the line and the reason");
    }
    {
        const auto p = switch_file::parse("PS2_" + std::string(200, 'A') + "\n");
        check(p.ignored.size() == 1 && p.ignored[0].size() < 80, "a long line makes a short note");
    }
    std::printf("switch file: %d checked, %d failed\n", g_checked, g_failed);
    return g_failed ? 1 : 0;
}
