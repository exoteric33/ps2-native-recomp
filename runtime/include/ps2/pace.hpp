#pragma once
// The picture pace of a game (issue #44): some games step their world once per picture and, on a real PS2,
// need more than one TV refresh for a picture. Our console counts only the main processor's instructions, so such a game is always
// done in time, shows a picture every refresh and runs too fast. For those games a setting holds the game one refresh whenever it
// shows a new picture sooner than every second (or third) refresh. Off for every other game.
//
// The setting: 'pace = 30' in the game's keys.ini (or PS2_PACE=30), which wins over the small built-in list below in both
// directions ('pace = 60' or 'pace = off' turns a listed game's pace off). 30 means one picture every 2 refreshes, 20 every 3,
// 15 every 4 (at 50 Hz the same number of refreshes). An optional word after the number names how a new picture is seen; only
// 'flip' (the game changes DISPFB1 or DISPFB2, the buffer the TV shows) is built.
//
// Header only, no runtime state: the hold's rule (Hold) is tested without a game (tests/pace.cpp).
#include <cstdint>
#include <string>

namespace ps2::pace {

// Games that get the pace without a keys.ini line, by the serial of their disc (the boot program's name, SLUS_205.92 ->
// SLUS-20592). Each one comes from a player's report that compares with a real PS2.
struct Listed {
    const char *serial;
    int every;          // refreshes per picture
    const char *why;
};
inline constexpr Listed kListed[] = {
    {"SLUS-20592", 2, "HSX Hypersonic Xtreme (USA): about 30 pictures a second on a PS2, its speed tied to them (issue #44)"},
};

// The serial from a boot program's path or name (".../SLUS_205.92;1" -> "SLUS-20592"), or "" when the name is not one.
inline std::string serial_of(const std::string &path) {
    std::string name = path.substr(path.find_last_of("/\\:") + 1);
    name = name.substr(0, name.find(';'));
    if (name.size() != 11 || name[4] != '_' || name[8] != '.') return {};
    for (int i : {0, 1, 2, 3}) {
        char &c = name[static_cast<std::size_t>(i)];
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
        if (c < 'A' || c > 'Z') return {};
    }
    for (int i : {5, 6, 7, 9, 10})
        if (name[static_cast<std::size_t>(i)] < '0' || name[static_cast<std::size_t>(i)] > '9') return {};
    return name.substr(0, 4) + "-" + name.substr(5, 3) + name.substr(9, 2);
}

inline const Listed *listed(const std::string &serial) {
    for (const Listed &l : kListed)
        if (serial == l.serial) return &l;
    return nullptr;
}

// Reads a setting's value: "30", "30 flip", "20", "15", "60", "off". Returns the refreshes per picture (1 = off), or 0 when
// the text is not a setting.
inline int parse(std::string text) {
    std::string word;
    for (char c : text) word += c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    while (!word.empty() && (word.back() == ' ' || word.back() == '\t' || word.back() == '\r')) word.pop_back();
    while (!word.empty() && (word.front() == ' ' || word.front() == '\t')) word.erase(word.begin());
    if (const std::size_t space = word.find_first_of(" \t"); space != std::string::npos) {
        std::string method = word.substr(space);
        while (!method.empty() && (method.front() == ' ' || method.front() == '\t')) method.erase(method.begin());
        if (method != "flip") return 0;
        word.resize(space);
    }
    if (word == "off" || word == "no" || word == "none" || word == "0" || word == "60" || word == "50") return 1;
    if (word == "30" || word == "25") return 2;
    if (word == "20") return 3;
    if (word == "15") return 4;
    return 0;
}

// The rule of the hold, fed with the refresh count (vertical blanks since power-on) at every new picture.
struct Hold {
    int every = 1;              // refreshes per picture the game is held to; 1 = off
    bool have_last = false;
    std::uint64_t last = 0;     // the refresh the last picture counts for: when it was shown, or when a hold ended

    // A new picture at refresh `frame`. Returns the refresh until which the game is held (it runs again at that vertical
    // blank), or 0 when it is not held. A second change in the same refresh (DISPFB1 and DISPFB2, or a write while a hold still
    // runs) belongs to the same picture.
    std::uint64_t picture(std::uint64_t frame) {
        if (have_last && frame <= last) return 0;
        if (every > 1 && have_last && frame < last + static_cast<std::uint64_t>(every)) {
            last += static_cast<std::uint64_t>(every);
            return last;
        }
        have_last = true;
        last = frame;
        return 0;
    }
};

// Counters for the end-of-run line of every game (cheap: touched only when the shown buffer changes and once a refresh).
struct Count {
    std::uint64_t refreshes = 0;   // vertical blanks of this run
    std::uint64_t pictures = 0;    // refreshes in which the shown buffer changed
    std::uint64_t holds = 0;
    std::uint64_t gaps[4] = {};    // refreshes between two pictures: 1, 2, 3, 4 or more
    bool have_seen = false;
    std::uint64_t seen = 0;        // the refresh of the last picture counted

    void picture(std::uint64_t frame) {
        if (have_seen && frame == seen) return;
        if (have_seen && frame > seen) ++gaps[frame - seen >= 4 ? 3 : frame - seen - 1];
        have_seen = true;
        seen = frame;
        ++pictures;
    }
};

}  // namespace ps2::pace
