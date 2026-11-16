#pragma once

#include "ps2/cpu.hpp"
#include "ps2/system.hpp"

#include <cstdint>
#include <functional>
#include <vector>

namespace ps2::state {
class Stream;
}

namespace ps2::gs {

inline constexpr std::uint32_t kMemorySize = 4u << 20;

enum Psm : std::uint32_t {
    PSMCT32 = 0x00, PSMCT24 = 0x01, PSMCT16 = 0x02, PSMCT16S = 0x0A,
    PSMT8 = 0x13, PSMT4 = 0x14, PSMT8H = 0x1B, PSMT4HL = 0x24, PSMT4HH = 0x2C,
    PSMZ32 = 0x30, PSMZ24 = 0x31, PSMZ16 = 0x32, PSMZ16S = 0x3A,
};

struct Vertex {
    std::uint32_t x, y, z;
    std::uint8_t r, g, b, a;
    float q, s, t;
    std::uint32_t u, v;
    std::uint8_t f;
};

struct Context {
    std::uint64_t tex0, clamp, tex1, tex2, xyoffset, miptbp1, miptbp2, scissor, alpha, test, fba, frame, zbuf;
};

struct State {
    std::uint64_t prim, prmode, prmodecont, rgbaq, st, uv, fog, texclut, scanmsk, texa, fogcol, dimx, dthe, colclamp, pabe;
    std::uint64_t bitbltbuf, trxpos, trxreg, trxdir;
    Context ctx[2];
    float q;
    Vertex queue[3];
    int count;
    std::uint32_t clut[512];
};

extern State g_state;
extern std::uint32_t g_setup_generation;
std::uint8_t *memory();

void init();
void state(ps2::state::Stream &s);
void reset();
void write(std::uint32_t reg, std::uint64_t value);
void signal(std::uint32_t reg, std::uint64_t value);
void raise_finish();
void note_transfer(std::uint32_t reg, std::uint64_t value);
void write_packed(std::uint32_t reg, const Reg128 &q);
void write_packed_run(const std::uint8_t *data, std::uint32_t count, std::uint64_t regs, std::uint32_t nreg, std::uint32_t reg);
void write_image(const std::uint8_t *data, std::uint32_t qwords);
void read_image(std::uint8_t *data, std::uint32_t qwords);
void render_read_image(std::uint8_t *data, std::uint32_t qwords);
void render_reset();

std::uint64_t read_privileged(std::uint32_t address);
void write_privileged(std::uint32_t address, std::uint64_t value);
void vblank(bool start);
bool interlaced_odd_field();

// Base pointer and buffer width of the texture's mip levels 0 to 6 (TEX0, TEX1 MTBA, MIPTBP1/2), the same for both renderers.
void mip_levels(const Context &c, std::uint32_t bp[7], std::uint32_t bw[7]);
std::uint32_t address32(std::uint32_t bp, std::uint32_t bw, std::uint32_t x, std::uint32_t y);
std::uint32_t read_pixel(std::uint32_t psm, std::uint32_t bp, std::uint32_t bw, std::uint32_t x, std::uint32_t y);
void write_pixel(std::uint32_t psm, std::uint32_t bp, std::uint32_t bw, std::uint32_t x, std::uint32_t y, std::uint32_t value);

// The second read circuit of the picture: when a game turns on both circuits of the display (PMODE EN1 and EN2), the picture of
// circuit 2 is the one in `DisplaySource`, and circuit 1 shows over a rectangle of it (a strip, a window) instead of it. Positions are
// in pixels of the picture, measured from its top left corner; the rectangle may stick out of the picture, which clips it.
struct DisplayOverlay {
    bool active = false;
    std::uint32_t fbp = 0, fbw = 0, psm = 0, dbx = 0, dby = 0;  // where circuit 1 reads, like the source's own
    std::int32_t x = 0, y = 0;                                  // top left corner of its rectangle in the picture
    std::uint32_t width = 0, height = 0;                        // size of its rectangle, in picture pixels
};

struct DisplayRegs {
    std::uint64_t pmode, smode2, dispfb[2], display[2];
};
DisplayRegs display_regs();

bool display(std::vector<std::uint32_t> &rgba, std::uint32_t &width, std::uint32_t &height);
bool display(const DisplayRegs &regs, std::vector<std::uint32_t> &rgba, std::uint32_t &width, std::uint32_t &height);
// Frame for hosts that draw it themselves, with interlaced fields blended like present(); may lag a frame.
bool copy_frame(std::vector<std::uint32_t> &rgba, std::uint32_t &width, std::uint32_t &height);
bool copy_frame(const DisplayRegs &regs, std::vector<std::uint32_t> &rgba, std::uint32_t &width, std::uint32_t &height);
bool attach_window(int kind, void *display, std::uint64_t window, void *surface, const char *selector, std::uint32_t width, std::uint32_t height);
void resize_window(std::uint32_t width, std::uint32_t height);
bool present();
bool present(const DisplayRegs &regs);
void start_thread();
void flush_and_wait();
void post_present();
std::uint32_t present_frame();  // the emulation frame the picture that is being presented belongs to (drawing thread)
Rendered render_pending(const std::function<void(const DisplayRegs &)> &on_present, double budget_ms);
double gpu_busy_ms();
std::uint64_t primitives_drawn();
void stats();

}
