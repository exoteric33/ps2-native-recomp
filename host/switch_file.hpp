#pragma once
// The switch file: PS2_* switches a player writes into switches.txt in the game's folder, next to keys.ini, instead
// of setting them in a command window (a switch typed with `set` in a Command Prompt never reaches a game started from its shortcut).
// This part only reads the text; host/main.cpp finds the file, applies what it holds and logs it. No SDL, no runtime: a test links it alone
// (tests/switch_file_test.cpp).
#include <string>
#include <utility>
#include <vector>

namespace switch_file {

constexpr const char *kName = "switches.txt";

struct Parsed {
    std::vector<std::pair<std::string, std::string>> switches;  // name, value; in file order, a name written twice keeps the last value
    std::vector<std::string> ignored;                           // one short note per line that was not taken ("line 3: FOO (not a PS2_ name)")
};

// One switch per line, `PS2_NAME=value`. Blank lines and lines starting with # are skipped. Also taken: blanks around the name, the '='
// and the value, a leading `set ` (a line copied from a command window), a pair of quotes around the whole line or the value, Windows line
// ends, the byte order mark Notepad may write, and a file saved as UTF-16 (Notepad's "Unicode", letters outside ASCII lost).
// Not taken (each gets a note): a name that does not start with PS2_ or holds characters other than letters, digits and _, a line with
// no '=', an empty value.
Parsed parse(const std::string &text);

}  // namespace switch_file
