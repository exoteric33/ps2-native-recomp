#include "keymap.hpp"

#include <SDL.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>

namespace keymap {
namespace {

struct Info {
    const char *name;     // the word in keys.ini
    const char *comment;  // the line in keys.ini
};

// One row for every Action, in the order of the enum.
constexpr Info kInfo[ActionCount] = {
    {"dpad_up", "D-pad: the menus, and whatever the game does with it"},
    {"dpad_down", ""},
    {"dpad_left", ""},
    {"dpad_right", ""},
    {"left_up", "left stick (walking): all the way in one direction; two keys make a diagonal"},
    {"left_down", ""},
    {"left_left", ""},
    {"left_right", ""},
    {"right_up", "right stick (God of War II: the evade roll; most games: the camera)"},
    {"right_down", ""},
    {"right_left", ""},
    {"right_right", ""},
    {"cross", "the four buttons on the right"},
    {"circle", ""},
    {"square", ""},
    {"triangle", ""},
    {"l1", "the four shoulder buttons"},
    {"r1", ""},
    {"l2", ""},
    {"r2", ""},
    {"l3", "the sticks pressed in"},
    {"r3", ""},
    {"start", "Start and Select"},
    {"select", ""},
    {"roll", "roll: while it is held, the walk keys push the right stick as well, which is God of War's evade roll"},
    {"rage", "one key that presses L3 and R3 together (God of War's rage)"},
    {"help", "a key that shows the key card at once (none by default: F1 opens the quick menu, whose Keys entry shows the card)"},
    {"menu", "another key for the quick menu (none by default: F1 opens it; on a controller: hold Select, then press Start, or press the Guide button)"},
};

constexpr int kScancodes = SDL_NUM_SCANCODES;

std::string squash(const std::string &text) {
    std::string out;
    for (unsigned char c : text)
        if (std::isalnum(c)) out += static_cast<char>(std::tolower(c));
    return out;
}

std::string trim(const std::string &s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

// every name a key or mouse button may be written with, squashed (lower case, letters and digits only) -> input
const std::map<std::string, int> &names() {
    static const std::map<std::string, int> table = [] {
        std::map<std::string, int> t;
        for (int sc = 1; sc < kScancodes; ++sc) {
            const char *name = SDL_GetScancodeName(static_cast<SDL_Scancode>(sc));
            if (name && *name && !squash(name).empty()) t.emplace(squash(name), sc);
        }
        const auto add = [&](const char *name, int input) { t[squash(name)] = input; };
        add("Ctrl", SDL_SCANCODE_LCTRL), add("Control", SDL_SCANCODE_LCTRL), add("Strg", SDL_SCANCODE_LCTRL);
        add("LCtrl", SDL_SCANCODE_LCTRL), add("RCtrl", SDL_SCANCODE_RCTRL);
        add("Shift", SDL_SCANCODE_LSHIFT), add("LShift", SDL_SCANCODE_LSHIFT), add("RShift", SDL_SCANCODE_RSHIFT);
        add("Alt", SDL_SCANCODE_LALT), add("LAlt", SDL_SCANCODE_LALT), add("RAlt", SDL_SCANCODE_RALT);
        add("Enter", SDL_SCANCODE_RETURN), add("Esc", SDL_SCANCODE_ESCAPE), add("Leertaste", SDL_SCANCODE_SPACE);
        add("KPEnter", SDL_SCANCODE_KP_ENTER), add("NumEnter", SDL_SCANCODE_KP_ENTER), add("NumpadEnter", SDL_SCANCODE_KP_ENTER);
        add("Del", SDL_SCANCODE_DELETE), add("Ins", SDL_SCANCODE_INSERT), add("PgUp", SDL_SCANCODE_PAGEUP), add("PgDn", SDL_SCANCODE_PAGEDOWN);
        add("Caps", SDL_SCANCODE_CAPSLOCK), add("Comma", SDL_SCANCODE_COMMA), add("Period", SDL_SCANCODE_PERIOD);
        add("Semicolon", SDL_SCANCODE_SEMICOLON), add("Minus", SDL_SCANCODE_MINUS), add("Plus", SDL_SCANCODE_EQUALS);
        add("Left Mouse", kMouseLeft), add("LMB", kMouseLeft), add("Mouse1", kMouseLeft), add("MouseLeft", kMouseLeft);
        add("Right Mouse", kMouseRight), add("RMB", kMouseRight), add("Mouse2", kMouseRight), add("MouseRight", kMouseRight);
        add("Middle Mouse", kMouseMiddle), add("MMB", kMouseMiddle), add("Mouse3", kMouseMiddle), add("MouseMiddle", kMouseMiddle);
        add("Wheel Click", kMouseMiddle);
        add("Mouse X1", kMouseX1), add("Mouse4", kMouseX1), add("MouseBack", kMouseX1), add("Mouse Side 1", kMouseX1);
        add("Mouse X2", kMouseX2), add("Mouse5", kMouseX2), add("MouseForward", kMouseX2), add("Mouse Side 2", kMouseX2);
        add("Wheel Up", kWheelUp), add("Mouse Wheel Up", kWheelUp), add("Wheel Down", kWheelDown), add("Mouse Wheel Down", kWheelDown);
        return t;
    }();
    return table;
}

bool lookup(const std::string &text, int &input) {
    const std::string key = squash(text);
    if (key.empty()) return false;
    const auto it = names().find(key);
    if (it == names().end()) return false;
    input = it->second;
    return true;
}

}  // namespace

const char *action_name(int action) { return kInfo[action].name; }

const char *look_name(int look) {
    static const char *const names[kLookCount] = {"Crisp", "Crisp + smooth edges", "Soft", "Soft + smooth edges", "Sharp", "Sharp + smooth edges"};
    return names[look < 0 || look >= kLookCount ? 0 : look];
}

const char *look_word(int look) {
    static const char *const words[kLookCount] = {"crisp", "crisp_edges", "soft", "soft_edges", "sharp", "sharp_edges"};
    return words[look < 0 || look >= kLookCount ? 0 : look];
}

namespace {
// keys.ini text with its `name` line set to `line` (other lines untouched; the line is added at the end when missing)
std::string with_line(const std::string &file_text, const std::string &name, const std::string &line) {
    std::string out;
    bool done = false;
    std::size_t pos = 0;
    while (pos < file_text.size()) {
        std::size_t end = file_text.find('\n', pos);
        const bool last = end == std::string::npos;
        std::string row = file_text.substr(pos, last ? std::string::npos : end - pos);
        const std::size_t eq = row.find('=');
        if (!done && eq != std::string::npos && row[0] != '#' && row[0] != ';' && squash(row.substr(0, eq)) == name) {
            row = line + (!row.empty() && row.back() == '\r' ? "\r" : "");
            done = true;
        }
        out += row;
        if (!last) out += '\n';
        pos = last ? file_text.size() : end + 1;
    }
    if (!done) out += std::string(out.empty() || out.back() == '\n' ? "" : "\n") + line + "\n";
    return out;
}

}  // namespace

std::string with_look(const std::string &file_text, int look) { return with_line(file_text, "look", std::string("look = ") + look_word(look)); }

std::string with_scale(const std::string &file_text, int scale) { return with_line(file_text, "scale", "scale = " + std::to_string(scale)); }

bool is_reserved(int input) {
    return input == SDL_SCANCODE_F1 || input == SDL_SCANCODE_F2 || input == SDL_SCANCODE_F5 || input == SDL_SCANCODE_F8 || input == SDL_SCANCODE_F9 || input == SDL_SCANCODE_F11;
}

Config defaults() {
    Config c;
    const auto set = [&](Action a, std::vector<int> inputs) { c.inputs[a] = std::move(inputs); };
    set(DpadUp, {SDL_SCANCODE_UP}), set(DpadDown, {SDL_SCANCODE_DOWN}), set(DpadLeft, {SDL_SCANCODE_LEFT}), set(DpadRight, {SDL_SCANCODE_RIGHT});
    set(LeftUp, {SDL_SCANCODE_W}), set(LeftDown, {SDL_SCANCODE_S}), set(LeftLeft, {SDL_SCANCODE_A}), set(LeftRight, {SDL_SCANCODE_D});
    set(RightUp, {SDL_SCANCODE_I}), set(RightDown, {SDL_SCANCODE_K}), set(RightLeft, {SDL_SCANCODE_J}), set(RightRight, {SDL_SCANCODE_L});
    set(Cross, {SDL_SCANCODE_SPACE, SDL_SCANCODE_RETURN, SDL_SCANCODE_KP_ENTER});
    set(Circle, {SDL_SCANCODE_E, kMouseMiddle});
    set(Square, {kMouseLeft});
    set(Triangle, {kMouseRight});
    set(L1, {SDL_SCANCODE_LSHIFT, kMouseX2});
    set(R1, {SDL_SCANCODE_F});
    set(L2, {SDL_SCANCODE_Q});
    set(R2, {SDL_SCANCODE_R});
    set(L3, {SDL_SCANCODE_X});
    set(R3, {SDL_SCANCODE_C});
    set(Start, {SDL_SCANCODE_ESCAPE});
    set(Select, {SDL_SCANCODE_TAB, SDL_SCANCODE_BACKSPACE});
    set(Roll, {SDL_SCANCODE_LCTRL, kMouseX1});
    set(Rage, {SDL_SCANCODE_V});
    set(Help, {});  // H used to be the key card; it was easy to hit by mistake, F1 opens the quick menu now
    set(Menu, {});
    return c;
}

std::string input_name(int input) {
    switch (input) {
    case kMouseLeft: return "Left Mouse";
    case kMouseRight: return "Right Mouse";
    case kMouseMiddle: return "Middle Mouse";
    case kMouseX1: return "Mouse X1";
    case kMouseX2: return "Mouse X2";
    case kWheelUp: return "Wheel Up";
    case kWheelDown: return "Wheel Down";
    case SDL_SCANCODE_RETURN: return "Enter";
    case SDL_SCANCODE_ESCAPE: return "Esc";
    case SDL_SCANCODE_KP_ENTER: return "Keypad Enter";
    default: break;
    }
    if (input > 0 && input < kScancodes) {
        const char *name = SDL_GetScancodeName(static_cast<SDL_Scancode>(input));
        if (name && *name) return name;
    }
    return "key " + std::to_string(input);
}

void parse(const std::string &text, Config &config, std::vector<std::string> &problems) {
    std::map<std::string, int> actions;
    for (int a = 0; a < ActionCount; ++a) actions[kInfo[a].name] = a;
    // a few spellings people will try
    actions["jump"] = Cross, actions["x"] = Cross, actions["pause"] = Start, actions["keycard"] = Help;
    int number = 0;
    std::size_t pos = 0;
    while (pos <= text.size()) {
        std::size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        std::string line = trim(text.substr(pos, end - pos));
        pos = end + 1;
        ++number;
        const auto bad = [&](const std::string &why) { problems.push_back("line " + std::to_string(number) + " (" + line + "): " + why); };
        if (line.empty() || line[0] == '#' || line[0] == ';' || line[0] == '[') continue;
        // a comment after the value: " # ..."
        if (const std::size_t hash = line.find(" #"); hash != std::string::npos) line = trim(line.substr(0, hash));
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos) {
            bad("not 'name = keys', ignored");
            continue;
        }
        const std::string name = squash(line.substr(0, eq));
        std::string name_with_underscore;
        for (char c : trim(line.substr(0, eq))) name_with_underscore += c == ' ' || c == '-' ? '_' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        const std::string value = trim(line.substr(eq + 1));
        if (name == "mousestick") {
            const std::string v = squash(value);
            if (v == "off" || v == "no" || v == "none" || v == "0") config.mouse_stick = 0;
            else if (v == "left") config.mouse_stick = 1;
            else if (v == "right") config.mouse_stick = 2;
            else bad("mouse_stick is off, left or right, ignored");
            continue;
        }
        if (name == "mouseinvert") {
            const std::string v = squash(value);
            if (v == "off" || v == "no" || v == "none" || v == "0") config.mouse_invert = 0;
            else if (v == "x") config.mouse_invert = 1;
            else if (v == "y") config.mouse_invert = 2;
            else if (v == "both" || v == "xy") config.mouse_invert = 3;
            else bad("mouse_invert is off, x, y or both, ignored");
            continue;
        }
        if (name == "mousedeadzone") {
            char *tail = nullptr;
            const double v = std::strtod(value.c_str(), &tail);
            if (value.empty() || *tail || !(v >= 0 && v <= 90)) bad("mouse_deadzone is a number from 0 to 90 (percent), ignored");
            else config.mouse_deadzone = v / 100.0;
            continue;
        }
        if (name == "look") {
            const std::string v = squash(value);
            int found = -1;
            for (int i = 0; i < kLookCount; ++i)
                if (v == squash(look_word(i)) || v == std::to_string(i)) found = i;
            if (found < 0) bad("look is sharp edges, sharp, crisp, crisp edges, soft or soft edges, ignored");
            else config.look = found;
            continue;
        }
        if (name == "scale") {
            const std::string v = squash(value);
            if (v == "1" || v == "2" || v == "3") config.scale = v[0] - '0';
            else bad("scale is 1, 2 or 3, ignored");
            continue;
        }
        if (name == "mousespeed") {
            char *tail = nullptr;
            const double v = std::strtod(value.c_str(), &tail);
            if (value.empty() || *tail || !(v >= 0.1 && v <= 10.0)) bad("mouse_speed is a number from 0.1 to 10, ignored");
            else config.mouse_speed = v;
            continue;
        }
        auto it = actions.find(name_with_underscore);
        if (it == actions.end()) it = actions.find(name);
        if (it == actions.end()) {
            bad("unknown name '" + trim(line.substr(0, eq)) + "', ignored");
            continue;
        }
        std::vector<int> inputs;
        // 'help = H' and 'menu = M' were the keys of earlier versions (H showed the key card and was easy to hit by mistake): files
        // that still have them from the old default are not honoured; any other key written there is
        if (squash(value) == "h" && it->second == Help) continue;
        if (squash(value) == "m" && it->second == Menu) continue;
        const bool none = squash(value) == "none" || value.empty();
        std::size_t from = 0;
        while (!none && from <= value.size()) {
            std::size_t comma = value.find(',', from);
            if (comma == std::string::npos) comma = value.size();
            std::string token = trim(value.substr(from, comma - from));
            from = comma + 1;
            if (token.empty()) continue;  // the comma key itself is written "Comma"
            int input = 0;
            if (!lookup(token, input)) {
                bad("'" + token + "' is not a key or mouse button, that one is ignored");
            } else if (is_reserved(input)) {
                bad("'" + token + "' is one of the program's own keys (F1, F2, F5, F8, F9, F11), that one is ignored");
            } else if (std::find(inputs.begin(), inputs.end(), input) == inputs.end()) {
                inputs.push_back(input);
            }
        }
        if (inputs.empty() && !none) {
            problems.push_back("line " + std::to_string(number) + ": nothing usable for " + kInfo[it->second].name + ", the default stays");
            continue;
        }
        config.inputs[it->second] = inputs;
    }
}

namespace {

std::string list(const Config &c, int action) {
    std::string out;
    for (int input : c.inputs[action]) out += (out.empty() ? "" : ", ") + input_name(input);
    return out.empty() ? "none" : out;
}

}  // namespace

std::string default_file_text() {
    const Config c = defaults();
    std::string t =
        "# keys.ini: which key or mouse button works which button of the game's controller.\n"
        "#\n"
        "# Change a line, save the file, click back into the game window: the new keys are used at once.\n"
        "# Delete the file to get the original keys back (it is written again the next time the game starts).\n"
        "#\n"
        "# A line is   name = key, key, key   (several keys for one button are fine; 'none' means no key).\n"
        "# Keys are written the English way: Space, Enter, Esc, Tab, Left Shift, Left Ctrl, Backspace, Up, Down, A, B, 1, 2, ...\n"
        "# and mean the place on the keyboard, whatever is printed there: on a German keyboard 'Z' is the key labelled Y.\n"
        "# Mouse: Left Mouse, Right Mouse, Middle Mouse, Mouse X1 and Mouse X2 (the two side buttons), Wheel Up, Wheel Down.\n"
        "# F1, F2, F5, F8, F9 and F11 belong to the program (quick menu, picture look, quick save, quick load, problem report,\n"
        "# full screen). On some laptops they need the Fn key: Fn + F1 opens the quick menu.\n"
        "# A line that cannot be read is ignored and written to the log (logs folder); the rest still works.\n"
        "\n";
    for (int a = 0; a < ActionCount; ++a) {
        if (kInfo[a].comment[0]) t += std::string(a ? "\n" : "") + "# " + kInfo[a].comment + "\n";
        t += std::string(kInfo[a].name) + std::string(12 - std::min<std::size_t>(11, std::strlen(kInfo[a].name)), ' ') + "= " + list(c, a) + "\n";
    }
    t +=
        "\n# The mouse can also push a stick (off, left or right). Off: the mouse only clicks. On: the pointer is locked in the window\n"
        "# and moving the mouse pushes that stick; mouse_speed is how far (1 is a good start). mouse_invert turns a direction\n"
        "# round: off, x (left and right), y (up and down) or both. mouse_deadzone (0 to 90 percent) pushes the stick that far\n"
        "# as soon as the mouse moves, for games that ignore a stick pushed only a little.\n"
        "mouse_stick = off\n"
        "mouse_speed = 1\n"
        "mouse_invert = off\n"
        "mouse_deadzone = 0\n"
        "\n# How the picture is enlarged to the window (F2 in the game goes through these and writes the choice here):\n"
        "#   sharp_edges   sharp, and the jagged stairs along edges are smoothed (the default)\n"
        "#   sharp         a cubic enlargement plus light sharpening: text and the health bar keep their edges\n"
        "#   crisp         hard pixels, only their borders blend\n"
        "#   crisp_edges   crisp, and the jagged stairs along edges are smoothed\n"
        "#   soft          a smooth blend, like an old TV\n"
        "#   soft_edges    soft, and the jagged stairs along edges are smoothed\n"
        "look = " + std::string(look_word(kDefaultLook)) + "\n"
        "\n# The inner resolution: 1, 2 or 3 times the console's (sharper, and more work for the graphics chip). Used when the game\n"
        "# starts; the quick menu (F1) writes it here. The environment variable PS2_SCALE, when it is set, wins over this line.\n"
        "# scale = 1\n";
    return t;
}

std::string card_text(const Config &c) {
    std::string t = "The game window must be in front.\n\n";
    const auto line = [&](const std::string &label, const std::string &keys) { t += label + ":  " + keys + "\n"; };
    line("Walk (up, left, down, right)", list(c, LeftUp) + ", " + list(c, LeftLeft) + ", " + list(c, LeftDown) + ", " + list(c, LeftRight));
    std::string roll;
    for (int input : c.inputs[Roll]) roll += (roll.empty() ? "" : " or ") + input_name(input);
    line("Roll (evade)", roll.empty() ? "none" : "hold " + roll + " and press a walk key");
    line("Cross (jump, confirm)", list(c, Cross));
    line("Square (light attack)", list(c, Square));
    line("Triangle (heavy attack, back)", list(c, Triangle));
    line("Circle (grab)", list(c, Circle));
    line("L1 (block)", list(c, L1));
    line("R1 (use, open, climb)", list(c, R1));
    line("L2 (magic)", list(c, L2));
    line("R2 (sub-weapon)", list(c, R2));
    line("Rage (L3 and R3 together)", list(c, Rage));
    line("Start (pause, press start)", list(c, Start));
    line("Select", list(c, Select));
    line("D-pad (menus)", list(c, DpadUp) + ", " + list(c, DpadDown) + ", " + list(c, DpadLeft) + ", " + list(c, DpadRight));
    line("L3, R3 (sticks pressed in)", list(c, L3) + ", " + list(c, R3));
    t += "\nF1: the quick menu (save, load, picture look, resolution, full screen, this card, quit)";
    if (!c.inputs[Menu].empty()) t += "; also " + list(c, Menu);
    t += "\nOn a controller: hold Select, then press Start (or press the Guide button)\n";
    t += "F5 quick save   F8 quick load   F9 something is wrong   F11 full screen\n";
    t += "F2 picture look, now: " + std::string(look_name(c.look < 0 ? kDefaultLook : c.look)) + " (sharp + smooth edges, sharp, crisp, crisp + smooth edges, soft, soft + smooth edges)\n";
    t += "On some laptops the F keys need the Fn key held down: Fn + F1 opens the quick menu. The buttons below work without Fn.\n";
    if (!c.inputs[Help].empty()) t += "This card at once: " + list(c, Help) + "\n";
    return t;
}

}  // namespace keymap
