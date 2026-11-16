// Tests the game controller path without a controller: SDL virtual controllers go through host/window.cpp (the code a window
// run uses) into the console's controller ports (runtime/src/iop/sio2.cpp), and the test reads back the bytes a game's pad
// driver gets, the way the IOP asks for them. No game is needed.
//
//   ninja -C build/native-<game> ps2padtest && build/native-<game>/ps2padtest
//
// It prints every check and ends with "pad test: N checks, M failed"; the exit code is 1 when one failed.

#define SDL_MAIN_HANDLED
#include "iop/sio2.hpp"
#include "ps2/system.hpp"
#include "window.hpp"

#include <SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace {

int g_checks = 0, g_failed = 0;

// One pass of the window's input code, then the frame boundary the emulation would reach: the host's pad state only goes on to the
// console's ports when the emulation takes it over once per frame (latch_pads in runtime/src/system.cpp, since 2026-10-25).
void pump() {
    poll_input();
    ps2::latch_pads_now();
}

void check(bool ok, const char *format, ...) {
    ++g_checks;
    if (!ok) ++g_failed;
    std::printf("  [%s] ", ok ? "ok" : "FAIL");
    va_list args;
    va_start(args, format);
    std::vprintf(format, args);
    va_end(args);
    std::printf("\n");
}

// One exchange with the controller in `port` (0 or 1) as the IOP's pad driver makes it: the command bytes go into the SIO2
// FIFO and the same number of bytes comes back.
std::vector<std::uint8_t> exchange(int port, std::vector<std::uint8_t> command) {
    namespace sio2 = ps2::iop::sio2;
    sio2::write(0x1F808268u, 0xCu);
    const auto n = static_cast<std::uint32_t>(command.size());
    sio2::write(0x1F808200u, static_cast<std::uint32_t>(port) | (n << 8) | (n << 18));
    for (std::uint8_t b : command) sio2::write(0x1F808260u, b);
    std::vector<std::uint8_t> answer;
    for (std::uint32_t i = 0; i < n; ++i) answer.push_back(static_cast<std::uint8_t>(sio2::read(0x1F808264u)));
    return answer;
}

// the PS2 pad's state request (0x42) in pressure mode: 21 bytes back, buttons active low in 3 and 4, then the sticks (right
// x, right y, left x, left y) and twelve pressures (right left up down triangle circle cross square L1 R1 L2 R2)
std::vector<std::uint8_t> poll_state(int port, std::uint8_t small = 0, std::uint8_t large = 0) {
    std::vector<std::uint8_t> command(21, 0);
    command[0] = 0x01;
    command[1] = 0x42;
    command[3] = small;
    command[4] = large;
    return exchange(port, command);
}

std::uint16_t buttons(const std::vector<std::uint8_t> &a) { return static_cast<std::uint16_t>(~(a[3] | (a[4] << 8))); }

// what a game's pad library does at start: config mode, analog and locked, all pressures on, motors 0 and 1, config off
void set_up_pad(int port) {
    exchange(port, {0x01, 0x43, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00});
    exchange(port, {0x01, 0x44, 0x00, 0x01, 0x03, 0x00, 0x00, 0x00, 0x00});
    exchange(port, {0x01, 0x4F, 0x00, 0xFF, 0xFF, 0x03, 0x00, 0x00, 0x00});
    exchange(port, {0x01, 0x4D, 0x00, 0x00, 0x01, 0xFF, 0xFF, 0xFF, 0xFF});
    exchange(port, {0x01, 0x43, 0x00, 0x00, 0x5A, 0x5A, 0x5A, 0x5A, 0x5A});
}

struct Virtual {
    SDL_Joystick *joystick = nullptr;
    Uint16 low = 0, high = 0;
    int rumbles = 0;
};
Virtual g_virtual[2];

int SDLCALL on_rumble(void *user, Uint16 low, Uint16 high) {
    Virtual &v = *static_cast<Virtual *>(user);
    v.low = low;
    v.high = high;
    ++v.rumbles;
    return 0;
}

void plug(int n) {
    g_virtual[n] = Virtual{};
    SDL_VirtualJoystickDesc desc;
    SDL_zero(desc);
    desc.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
    desc.type = SDL_JOYSTICK_TYPE_GAMECONTROLLER;
    desc.naxes = SDL_CONTROLLER_AXIS_MAX;
    desc.nbuttons = SDL_CONTROLLER_BUTTON_DPAD_RIGHT + 1;
    desc.name = n ? "pad test controller B" : "pad test controller A";
    desc.userdata = &g_virtual[n];
    desc.Rumble = on_rumble;
    const int device = SDL_JoystickAttachVirtualEx(&desc);
    g_virtual[n].joystick = device >= 0 ? SDL_JoystickOpen(device) : nullptr;
    if (!g_virtual[n].joystick) {
        std::printf("cannot attach a virtual controller: %s\n", SDL_GetError());
        return;
    }
    SDL_JoystickSetVirtualAxis(g_virtual[n].joystick, SDL_CONTROLLER_AXIS_TRIGGERLEFT, SDL_JOYSTICK_AXIS_MIN);
    SDL_JoystickSetVirtualAxis(g_virtual[n].joystick, SDL_CONTROLLER_AXIS_TRIGGERRIGHT, SDL_JOYSTICK_AXIS_MIN);
}

void unplug(int n) {
    const SDL_JoystickID id = SDL_JoystickInstanceID(g_virtual[n].joystick);
    SDL_JoystickClose(g_virtual[n].joystick);
    g_virtual[n].joystick = nullptr;
    for (int i = 0; i < SDL_NumJoysticks(); ++i)
        if (SDL_JoystickGetDeviceInstanceID(i) == id) {
            SDL_JoystickDetachVirtual(i);
            break;
        }
}

void button(int n, SDL_GameControllerButton b, bool down) {
    SDL_JoystickSetVirtualButton(g_virtual[n].joystick, b, down ? SDL_PRESSED : SDL_RELEASED);
    pump();
}

void axis(int n, SDL_GameControllerAxis a, int value) {
    SDL_JoystickSetVirtualAxis(g_virtual[n].joystick, a, static_cast<Sint16>(value));
    pump();
}

// a trigger value as the game controller layer gives it (0..32767) set on the virtual joystick's whole range
void trigger(int n, SDL_GameControllerAxis a, int value) { axis(n, a, std::min(value * 2 - 32768, 32767)); }

struct Expect {
    SDL_GameControllerButton sdl;
    const char *name;
    int bit;
    int pressure;  // index in the pressure bytes, -1 for buttons without one
};

constexpr Expect kButtons[] = {
    {SDL_CONTROLLER_BUTTON_A, "A -> cross", 14, 6},
    {SDL_CONTROLLER_BUTTON_B, "B -> circle", 13, 5},
    {SDL_CONTROLLER_BUTTON_X, "X -> square", 15, 7},
    {SDL_CONTROLLER_BUTTON_Y, "Y -> triangle", 12, 4},
    {SDL_CONTROLLER_BUTTON_LEFTSHOULDER, "LB -> L1", 10, 8},
    {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, "RB -> R1", 11, 9},
    {SDL_CONTROLLER_BUTTON_LEFTSTICK, "left stick press -> L3", 1, -1},
    {SDL_CONTROLLER_BUTTON_RIGHTSTICK, "right stick press -> R3", 2, -1},
    {SDL_CONTROLLER_BUTTON_BACK, "Back/View -> select", 0, -1},
    {SDL_CONTROLLER_BUTTON_START, "Start/Menu -> start", 3, -1},
    {SDL_CONTROLLER_BUTTON_DPAD_UP, "d-pad up -> up", 4, 2},
    {SDL_CONTROLLER_BUTTON_DPAD_DOWN, "d-pad down -> down", 6, 3},
    {SDL_CONTROLLER_BUTTON_DPAD_LEFT, "d-pad left -> left", 7, 1},
    {SDL_CONTROLLER_BUTTON_DPAD_RIGHT, "d-pad right -> right", 5, 0},
};

}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_GAMECONTROLLER) != 0) {
        std::printf("SDL: %s\n", SDL_GetError());
        return 1;
    }
    SDL_version v;
    SDL_GetVersion(&v);
    std::printf("SDL %d.%d.%d\n", v.major, v.minor, v.patch);
    for (int i = 0; i < SDL_NumJoysticks(); ++i)
        std::printf("note: a real controller is connected (%s); the checks below assume it is idle\n", SDL_JoystickNameForIndex(i));
    ps2::iop::sio2::reset();
    pump();

    std::printf("port 1 with no controller (the keyboard drives it)\n");
    {
        const auto a = exchange(0, {0x01, 0x42, 0x00, 0x00, 0x00});
        check(a[1] == 0x41 && a[2] == 0x5A, "port 1 answers as a digital pad: %02X %02X", a[1], a[2]);
        check(buttons(a) == 0, "no button pressed: %04X", buttons(a));
    }

    std::printf("controller A plugged in\n");
    plug(0);
    pump();
    check(SDL_NumJoysticks() >= 1 && SDL_IsGameController(SDL_NumJoysticks() - 1), "SDL sees it as a game controller");
    set_up_pad(0);
    {
        const auto a = poll_state(0);
        check(a[1] == 0x79, "the pad driver switched port 1 to analog with pressure: mode %02X", a[1]);
        check(buttons(a) == 0 && a[5] == 0x80 && a[6] == 0x80 && a[7] == 0x80 && a[8] == 0x80, "at rest: buttons %04X, sticks %02X %02X %02X %02X",
              buttons(a), a[5], a[6], a[7], a[8]);
    }

    std::printf("each button\n");
    for (const Expect &e : kButtons) {
        button(0, e.sdl, true);
        // the window holds Select back for 300 ms (the quick menu is Select, then Start), then gives it to the game
        if (e.sdl == SDL_CONTROLLER_BUTTON_BACK) {
            SDL_Delay(320);
            pump();
        }
        const auto a = poll_state(0);
        const bool pressure_ok = e.pressure < 0 || a[9 + e.pressure] == 0xFF;
        int others = 0;
        for (int i = 0; i < 12; ++i)
            if (i != e.pressure && a[9 + i]) ++others;
        check(buttons(a) == (1u << e.bit) && pressure_ok && !others, "%-24s buttons %04X%s", e.name, buttons(a),
              e.pressure < 0 ? "" : (std::string(", pressure ") + std::to_string(a[9 + e.pressure])).c_str());
        button(0, e.sdl, false);
        // a Select that was let go is still given to the game for 80 ms
        if (e.sdl == SDL_CONTROLLER_BUTTON_BACK) {
            SDL_Delay(120);
            pump();
        }
        check(buttons(poll_state(0)) == 0, "%-24s released", e.name);
    }
    button(0, SDL_CONTROLLER_BUTTON_GUIDE, true);
    // the Guide / Xbox / PS button has no PS2 button; it opens the quick menu (since 2026-10-26), which keeps every button from the game
    // until it is closed, and pressing Guide again closes it
    check(buttons(poll_state(0)) == 0, "Guide/Xbox/PS button: no PS2 button (it has none; it opens the quick menu)");
    button(0, SDL_CONTROLLER_BUTTON_GUIDE, false);
    button(0, SDL_CONTROLLER_BUTTON_A, true);
    check(buttons(poll_state(0)) == 0, "quick menu open: the game gets no button");
    button(0, SDL_CONTROLLER_BUTTON_A, false);
    button(0, SDL_CONTROLLER_BUTTON_GUIDE, true);
    button(0, SDL_CONTROLLER_BUTTON_GUIDE, false);

    std::printf("triggers (L2, R2) with pressure\n");
    for (const auto &[sdl, name, bit, index] : {std::tuple{SDL_CONTROLLER_AXIS_TRIGGERLEFT, "LT -> L2", 8, 19}, std::tuple{SDL_CONTROLLER_AXIS_TRIGGERRIGHT, "RT -> R2", 9, 20}}) {
        for (const int value : {1000, 8192, 16384, 32767}) {
            trigger(0, sdl, value);
            const auto a = poll_state(0);
            const bool down = value > 2000;
            const int want = down ? std::max(1, value * 255 / 32767) : 0;
            const int got = a[index];
            check(((buttons(a) >> bit) & 1u) == (down ? 1u : 0u) && std::abs(got - want) <= 2, "%s at %5d: %s, pressure %3d (expected about %d)", name, value,
                  (buttons(a) >> bit) & 1u ? "pressed" : "not pressed", got, want);
        }
        trigger(0, sdl, 0);
        check(buttons(poll_state(0)) == 0, "%s released", name);
    }

    std::printf("sticks\n");
    struct StickCase {
        SDL_GameControllerAxis sdl;
        const char *name;
        int byte;
    };
    for (const StickCase &s : {StickCase{SDL_CONTROLLER_AXIS_LEFTX, "left stick x", 7}, StickCase{SDL_CONTROLLER_AXIS_LEFTY, "left stick y", 8},
                               StickCase{SDL_CONTROLLER_AXIS_RIGHTX, "right stick x", 5}, StickCase{SDL_CONTROLLER_AXIS_RIGHTY, "right stick y", 6}}) {
        std::string seen;
        bool ok = true;
        for (const auto &[value, want] : {std::pair{-32768, 0x00}, std::pair{-16384, 0x40}, std::pair{0, 0x80}, std::pair{16384, 0xC0}, std::pair{32767, 0xFF}}) {
            axis(0, s.sdl, value);
            const auto a = poll_state(0);
            ok = ok && a[s.byte] == want && buttons(a) == 0;
            for (int b = 5; b <= 8; ++b)
                if (b != s.byte && a[b] != 0x80) ok = false;
            char text[16];
            std::snprintf(text, sizeof text, " %02X", a[s.byte]);
            seen += text;
        }
        axis(0, s.sdl, 0);
        check(ok, "%-14s from full left/up to full right/down:%s (expected 00 40 80 C0 FF, the others stay 80)", s.name, seen.c_str());
    }

    std::printf("rumble\n");
    {
        poll_state(0, 1, 0xC0);
        pump();
        check(g_virtual[0].rumbles > 0 && g_virtual[0].high == 0xFFFF && std::abs(g_virtual[0].low - static_cast<int>(std::sqrt(0xC0 / 255.0) * 65535.0)) < 64,
              "game asks for small motor on, large motor 0xC0: the controller gets low %u high %u", g_virtual[0].low, g_virtual[0].high);
        poll_state(0, 0, 0);
        pump();
        check(g_virtual[0].low == 0 && g_virtual[0].high == 0, "game stops the motors: low %u high %u", g_virtual[0].low, g_virtual[0].high);
        poll_state(0, 1, 0xFF);
        pump();
        unplug(0);
        pump();
        const int before = g_virtual[0].rumbles;
        poll_state(0, 1, 0xFF);
        pump();
        check(g_virtual[0].rumbles == before, "rumble request after the controller was pulled out: nothing sent, no crash");
        poll_state(0, 0, 0);
    }

    std::printf("hot-plug\n");
    {
        plug(0);
        pump();
        button(0, SDL_CONTROLLER_BUTTON_A, true);
        check(buttons(poll_state(0)) == (1u << 14), "plugged in again while running: A -> cross works");
        unplug(0);
        pump();
        const auto a = poll_state(0);
        check(buttons(a) == 0 && a[7] == 0x80, "pulled out while A was held: no button stays stuck (%04X)", buttons(a));
        plug(0);
        pump();
        axis(0, SDL_CONTROLLER_AXIS_LEFTX, -32768);
        check(poll_state(0)[7] == 0x00, "plugged in a third time: the left stick works");
        axis(0, SDL_CONTROLLER_AXIS_LEFTX, 0);
    }

    std::printf("second controller\n");
    {
        const auto before = exchange(1, {0x01, 0x42, 0x00, 0x00, 0x00});
        check(before[1] == 0xFF, "port 2 is empty before: %02X", before[1]);
        plug(1);
        pump();
        const auto digital = exchange(1, {0x01, 0x42, 0x00, 0x00, 0x00});
        check(digital[1] == 0x41, "controller B plugged in: port 2 answers (%02X)", digital[1]);
        set_up_pad(1);
        button(1, SDL_CONTROLLER_BUTTON_B, true);
        const auto a = poll_state(1), b = poll_state(0);
        check(buttons(a) == (1u << 13) && buttons(b) == 0, "B on controller B: port 2 circle (%04X), port 1 nothing (%04X)", buttons(a), buttons(b));
        button(1, SDL_CONTROLLER_BUTTON_B, false);
        button(0, SDL_CONTROLLER_BUTTON_Y, true);
        check(buttons(poll_state(0)) == (1u << 12) && buttons(poll_state(1)) == 0, "Y on controller A: port 1 triangle, port 2 nothing");
        button(0, SDL_CONTROLLER_BUTTON_Y, false);
        poll_state(1, 1, 0x80);
        pump();
        check(g_virtual[1].rumbles > 0 && g_virtual[0].low == 0, "rumble for port 2 goes to controller B only: B low %u high %u, A low %u", g_virtual[1].low,
              g_virtual[1].high, g_virtual[0].low);
        poll_state(1, 0, 0);
        unplug(1);
        pump();
        check(exchange(1, {0x01, 0x42, 0x00, 0x00, 0x00})[1] == 0xFF, "controller B pulled out: port 2 is empty again");
        check(buttons(poll_state(0)) == 0 && poll_state(0)[1] == 0x79, "port 1 is not disturbed");
    }

    unplug(0);
    pump();
    std::printf("pad test: %d checks, %d failed\n", g_checks, g_failed);
    SDL_Quit();
    return g_failed ? 1 : 0;
}
