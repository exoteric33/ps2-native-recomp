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
    int scale = 0;             // the inner resolution (1 to 4 times the console's) used at the next start, 0 when the file has no 'scale' line
    int sharp_2d = -1;         // how upscaled 2D art is filtered (0 soft, 1 sharp, 2 sharpest) at the next start, -1 when the file has no line
    std::string pace;          // the picture pace ('30', '60', 'off', ...; ps2/pace.hpp) used at the next start, "" when the file has no 'pace' line
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
// the same for the 'sharp_2d' line (the quick menu's 2D art filter, used when the game starts the next time)
std::string with_sharp_2d(const std::string &file_text, int sharp);
const char *sharp_2d_word(int sharp);  // "soft", "sharp", "sharpest"

const char *action_name(int action);  // the word used in keys.ini

// The quick menu's "Change keys" page (docs/keys.md): one line for every button of the controller and for roll and rage, in the order of
// keys.ini (the actions 0 to kChangeableCount - 1; help and menu are left to the file).
constexpr int kChangeableCount = Rage + 1;
const char *action_label(int action);                      // "Square", "Left stick Up", "Rage (L3 + R3)"
std::string keys_text(const Config &config, int action);   // "Left Mouse", "Space, Enter, Keypad Enter", "none"
// keys.ini text with the line of `action` set to the keys in `config` (other lines, comments included, untouched; the line is added at the end when missing)
std::string with_inputs(const std::string &file_text, int action, const std::vector<int> &inputs);
// Gives `input` (a key or mouse button) to `action`, which loses the keys it had. A button that already had `input` swaps: it gets the keys
// `action` had. Returns false and a reason in `message` when the input belongs to the program. Otherwise `message` says what happened and
// `changed` holds every action whose keys are different now (the one asked for first).
bool assign_input(Config &config, int action, int input, std::vector<int> &changed, std::string &message);
// The keys of the controller's buttons, roll and rage back to the defaults (the mouse and picture lines are not touched). `changed` as above.
void restore_default_inputs(Config &config, std::vector<int> &changed);
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
