#pragma once

// The keyboard and mouse layout of a window run (docs/keys.md): which key or mouse button works which button of the console's
// controller, the key file the player can edit (keys.ini), and the key card (the quick menu's Keys entry) made from what is in force.

#include <string>
#include <vector>

namespace keymap {

// What a key can do. The first 16 are the pad's buttons in the order of the controller layout in window.cpp.
enum Action {
    DpadUp, DpadDown, DpadLeft, DpadRight,
    LeftUp, LeftDown, LeftLeft, LeftRight,      // left stick, pushed all the way
    RightUp, RightDown, RightLeft, RightRight,  // right stick, pushed all the way
    Cross, Circle, Square, Triangle,
    L1, R1, L2, R2, L3, R3,
    Start, Select,
    Roll,  // while held, the direction keys of the left stick push the right stick as well (God of War's evade roll)
    Rage,  // L3 and R3 pressed together, which one key can do and two keys can hardly
    Help,  // a key for the key card (none by default; the quick menu's Keys entry shows it)
    Menu,  // another key for the quick menu (none by default; F1 always opens it), host/quickmenu.cpp
    ActionCount
};

// An input is a number: a keyboard scancode (SDL's, below 512: the key by its place on the keyboard), or one of these.
constexpr int kMouseLeft = 1000, kMouseMiddle = 1001, kMouseRight = 1002, kMouseX1 = 1003, kMouseX2 = 1004;
constexpr int kWheelUp = 1010, kWheelDown = 1011;

struct Config {
    std::vector<int> inputs[ActionCount];
    int mouse_stick = 0;       // 0 the mouse moves no stick, 1 it pushes the left stick, 2 the right stick
    double mouse_speed = 1.0;  // 0.1 to 10
    int mouse_invert = 0;      // 0 none, 1 left-right, 2 up-down, 3 both
    double mouse_deadzone = 0; // 0 to 0.9: how far the stick jumps out as soon as the mouse moves
    int look = -1;             // the picture look (F2) written in keys.ini, -1 when the file has no 'look' line
    int scale = 0;             // the inner resolution (1 to 3 times the console's) used at the next start, 0 when the file has no 'scale' line
};

// The picture looks the F2 key goes through. The number is also the video option bits (ps2/video.hpp): bit 0 smooth edges, bit 1 soft, bit 2 sharp (look 4: cubic enlargement + light sharpening; look 5 = sharp + smooth edges).
constexpr int kLookCount = 6;
constexpr int kDefaultLook = 5;
const char *look_name(int look);           // "Soft + smooth edges", for the title and the key card
const char *look_word(int look);           // the word written in keys.ini
// keys.ini text with its 'look' line set to `look` (other lines untouched; the line is added at the end when missing)
std::string with_look(const std::string &file_text, int look);
// the same for the 'scale' line (the quick menu's resolution, used when the game starts the next time)
std::string with_scale(const std::string &file_text, int scale);

const char *action_name(int action);  // the word used in keys.ini
Config defaults();

// Reads the text of a key file on top of `config` (which starts as defaults()). Every problem becomes a line in `problems`; the
// line it is about is ignored, the rest still counts.
void parse(const std::string &text, Config &config, std::vector<std::string> &problems);

std::string input_name(int input);              // "Left Shift", "Left Mouse"
std::string default_file_text();                // keys.ini as written on the first start
std::string card_text(const Config &config);    // the lines of the key card
// the program's own keys cannot be given to the game: F1, F2, F5, F8, F9, F11
bool is_reserved(int input);

}  // namespace keymap
