#pragma once

// A game controller's analog stick as the console's pad bytes. No SDL here, so a test can run it without a window.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>

namespace stick_map {

// The stick's two axes as SDL reports them (-32768..32767, 0 in the middle) become the two pad bytes (0..255, 0x80 in the middle).
//
// A modern pad's stick moves in a round gate: pushed all the way on a diagonal it reads about 0.71 on each axis. A console pad
// reads close to the corner of the square there. A game that tests each axis on its own (Harvest Moon: A Wonderful Life runs only
// on a full push) then never sees a full push on a diagonal. So the push is stretched from the circle to the square: the same
// direction, and the longer of the two axes takes the length of the whole push. A push along one axis is not changed, and a push
// that is already in a corner stays there.
inline void to_bytes(int vx, int vy, std::uint8_t &x, std::uint8_t &y) {
    const int m = std::max(std::abs(vx), std::abs(vy));
    const double scale = m ? std::min(std::hypot(static_cast<double>(vx), static_cast<double>(vy)), 32768.0) / m : 1.0;
    const auto byte = [scale](int v) {
        const long s = std::clamp(std::lround(v * scale), -32768L, 32767L);
        return static_cast<std::uint8_t>((s + 32768) >> 8);
    };
    x = byte(vx);
    y = byte(vy);
}

}  // namespace stick_map
