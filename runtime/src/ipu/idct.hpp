#pragma once

#include <cstdint>

namespace ps2::ipu {

// The 8 x 8 inverse transform of BDEC: 64 dequantised coefficients in (row v, column u), 64 pixels out (row y, column x), each the
// double-precision sum of cos(y, v) * cos(x, u) * coefficient rounded half up and clamped to -256..255.
// `idct` is the fast version (AVX2 when the build has it), `idct_plain` the plain C++ one; both give the same pixels bit for bit for
// every input (tests/idct_exact.cpp checks that against the old code).
void idct(const int *in, std::int16_t *out);
void idct_plain(const int *in, std::int16_t *out);

}  // namespace ps2::ipu
