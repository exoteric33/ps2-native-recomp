#pragma once

// The quick menu (F1, or Select then Start / Guide on a controller): a panel drawn over the game picture in the window, while the game
// waits. This file only knows the entries, the highlight and how the panel looks; host/window.cpp opens it, holds the game, feeds it the
// keys and does what an entry asks for. The panel is a picture made here on the CPU (a built-in 5 x 8 pixel font, our own), which the
// window lays over the game's picture: the WebGPU present pass (runtime/src/gs/gpu/overlay.hpp) or the SDL renderer. The game itself,
// its saved pictures and its save states never see it.

#include <cstdint>
#include <string>
#include <vector>

namespace quickmenu {

enum class Command { none, resume, save, load, look, scale, fullscreen, keys, quit };

// What the entries show; the window fills it in before each drawing.
struct Values {
    std::string look;       // "Sharp + smooth edges"
    int scale_now = 1;      // the inner resolution the game runs at
    int scale_next = 1;     // the one written in keys.ini for the next start
    bool scale_locked = false;  // PS2_SCALE is set: keys.ini does not count
    bool fullscreen = false;
};

// The panel: straight (not premultiplied) RGBA, one std::uint32_t per pixel with red in the low byte, placed at x, y in window pixels.
struct Picture {
    std::vector<std::uint32_t> rgba;
    int width = 0, height = 0, x = 0, y = 0;
};

class Menu {
public:
    bool is_open() const { return open_; }
    void open();
    void close() { open_ = false; }
    void move(int delta);                      // up (-1) and down (+1), round at the ends
    Command choose();                          // Enter, Cross, a click: what the highlighted entry does (Quit asks once: the first time it only arms itself)
    Command side() const;                      // left and right: the entries that go through values (look, resolution), else none
    bool quit_armed() const { return quit_armed_; }
    bool select_at(int x, int y);              // the mouse: highlight the entry under the pointer (window pixels); false when none is
    void draw(int window_width, int window_height, const Values &values, Picture &out);
    // A small panel with one line of text at the top of the window (for a few seconds after the menu is closed: "Saved", "Loaded")
    static void draw_note(int window_width, int window_height, const std::string &text, Picture &out);

private:
    bool open_ = false;
    int selected_ = 0;
    bool quit_armed_ = false;  // Quit was chosen once; the next Enter ends the game, anything else takes it back
    // where the last drawing put the entries, for the mouse
    int entry_x_ = 0, entry_y_ = 0, entry_w_ = 0, entry_h_ = 0;
};

}  // namespace quickmenu
