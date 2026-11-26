#include "switch_file.hpp"

#include <algorithm>
#include <cctype>

namespace switch_file {
namespace {

bool blank(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f'; }

std::string trim(const std::string &text) {
    std::size_t begin = 0, end = text.size();
    while (begin < end && blank(text[begin])) ++begin;
    while (end > begin && blank(text[end - 1])) --end;
    return text.substr(begin, end - begin);
}

std::string unquote(std::string text) {
    if (text.size() >= 2 && text.front() == '"' && text.back() == '"') text = trim(text.substr(1, text.size() - 2));
    return text;
}

// The name as it goes into a note: short, so a note never carries a long line (a log is posted in public)
std::string for_note(const std::string &text) { return text.size() > 40 ? text.substr(0, 40) + "..." : text; }

}  // namespace

Parsed parse(const std::string &input) {
    std::string text = input;
    // UTF-16 little endian (Notepad's "Unicode"): the switches are ASCII, so every second byte is the letter
    if (text.size() >= 2 && static_cast<unsigned char>(text[0]) == 0xFF && static_cast<unsigned char>(text[1]) == 0xFE) {
        std::string narrow;
        for (std::size_t i = 2; i + 1 < text.size(); i += 2) narrow += text[i + 1] == 0 ? text[i] : '?';
        text = narrow;
    }
    if (text.rfind("\xEF\xBB\xBF", 0) == 0) text.erase(0, 3);  // UTF-8 byte order mark

    Parsed out;
    std::size_t at = 0;
    int number = 0;
    while (at <= text.size()) {
        const std::size_t end = std::min(text.find('\n', at), text.size());
        std::string line = trim(text.substr(at, end - at));
        at = end + 1;
        ++number;
        if (line.empty() || line[0] == '#') continue;
        if (line.size() > 4 && (line[0] == 's' || line[0] == 'S') && (line[1] == 'e' || line[1] == 'E') && (line[2] == 't' || line[2] == 'T') &&
            blank(line[3]))
            line = trim(line.substr(4));
        line = unquote(line);
        const std::string where = "line " + std::to_string(number) + ": ";
        const std::size_t equals = line.find('=');
        if (equals == std::string::npos) {
            out.ignored.push_back(where + for_note(line) + " (no '=')");
            continue;
        }
        const std::string name = trim(line.substr(0, equals));
        const std::string value = unquote(trim(line.substr(equals + 1)));
        const bool ps2 = name.rfind("PS2_", 0) == 0 && name.size() > 4;
        const bool word = std::all_of(name.begin(), name.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; });
        if (!ps2 || !word) {
            out.ignored.push_back(where + for_note(name) + " (not a PS2_ name)");
            continue;
        }
        if (value.empty()) {
            out.ignored.push_back(where + for_note(name) + " (no value)");
            continue;
        }
        const auto same = std::find_if(out.switches.begin(), out.switches.end(), [&](const auto &s) { return s.first == name; });
        if (same != out.switches.end()) same->second = value;
        else out.switches.emplace_back(name, value);
    }
    return out;
}

}  // namespace switch_file
