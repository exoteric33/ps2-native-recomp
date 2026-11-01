#pragma once

// A picture laid over the game's picture in the window: the quick menu of the native runner (host/quickmenu.cpp). It is drawn in the
// present pass only, after the game's picture, so the game, the pictures it saves (--frames-dir, F9) and its save states never contain it.
// Called on the thread that presents (the window thread in a run with threads, the emulation thread in a run with one).

#include <cstdint>

namespace ps2::gs::gpu {

// rgba: straight alpha, red in the low byte, `width` x `height`, its top left corner at x, y in window pixels. nullptr takes it away.
void set_overlay(const std::uint32_t *rgba, std::uint32_t width, std::uint32_t height, std::uint32_t x, std::uint32_t y);
// The last picture of the game once more, with the picture look of now and the overlay: for a game that waits (the menu is open) and
// sends no new pictures. False when there is no window surface or no picture yet.
bool present_again();

}  // namespace ps2::gs::gpu
