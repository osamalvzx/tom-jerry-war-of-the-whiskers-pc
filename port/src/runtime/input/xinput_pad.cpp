#include "runtime/input/xinput_pad.h"
#include <windows.h>
#include <xinput.h>

namespace tj::input {
namespace {
constexpr int kMaxPorts = 4;
uint16_t g_prevButtons[kMaxPorts] = {};
uint16_t g_currButtons[kMaxPorts] = {};

// Map a PC XInput button mask to the Xbox digital layout (shared bits + thumbs/start/back).
uint16_t TranslateDigital(WORD pc) {
    uint16_t x = 0;
    if (pc & XINPUT_GAMEPAD_DPAD_UP)        x |= XPAD_DPAD_UP;
    if (pc & XINPUT_GAMEPAD_DPAD_DOWN)      x |= XPAD_DPAD_DOWN;
    if (pc & XINPUT_GAMEPAD_DPAD_LEFT)      x |= XPAD_DPAD_LEFT;
    if (pc & XINPUT_GAMEPAD_DPAD_RIGHT)     x |= XPAD_DPAD_RIGHT;
    if (pc & XINPUT_GAMEPAD_START)          x |= XPAD_START;
    if (pc & XINPUT_GAMEPAD_BACK)           x |= XPAD_BACK;
    if (pc & XINPUT_GAMEPAD_LEFT_THUMB)     x |= XPAD_LEFT_THUMB;
    if (pc & XINPUT_GAMEPAD_RIGHT_THUMB)    x |= XPAD_RIGHT_THUMB;
    return x;
}
} // namespace

bool Poll(int port, XboxGamepad& out) {
    if (port < 0 || port >= kMaxPorts) return false;

    XINPUT_STATE st{};
    if (XInputGetState(port, &st) != ERROR_SUCCESS) {
        g_currButtons[port] = 0;
        return false;
    }
    const XINPUT_GAMEPAD& gp = st.Gamepad;

    out = XboxGamepad{};
    out.wButtons = TranslateDigital(gp.wButtons);

    // Face buttons: PC XInput exposes them digitally; the Xbox pad wants a 0/255
    // pressure byte, which is what the game's threshold checks compare against.
    out.bAnalogButtons[XANA_A]     = (gp.wButtons & XINPUT_GAMEPAD_A) ? 255 : 0;
    out.bAnalogButtons[XANA_B]     = (gp.wButtons & XINPUT_GAMEPAD_B) ? 255 : 0;
    out.bAnalogButtons[XANA_X]     = (gp.wButtons & XINPUT_GAMEPAD_X) ? 255 : 0;
    out.bAnalogButtons[XANA_Y]     = (gp.wButtons & XINPUT_GAMEPAD_Y) ? 255 : 0;
    // Black/White map to the shoulder bumpers (no dedicated buttons on modern pads).
    out.bAnalogButtons[XANA_BLACK] = (gp.wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) ? 255 : 0;
    out.bAnalogButtons[XANA_WHITE] = (gp.wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER) ? 255 : 0;
    // Triggers are already analog (0..255) on both.
    out.bAnalogButtons[XANA_LEFT_TRIGGER]  = gp.bLeftTrigger;
    out.bAnalogButtons[XANA_RIGHT_TRIGGER] = gp.bRightTrigger;

    out.sThumbLX = gp.sThumbLX; out.sThumbLY = gp.sThumbLY;
    out.sThumbRX = gp.sThumbRX; out.sThumbRY = gp.sThumbRY;

    g_currButtons[port] = out.wButtons;
    return true;
}

bool Pressed(int port, XboxButton button) {
    if (port < 0 || port >= kMaxPorts) return false;
    return (g_currButtons[port] & button) && !(g_prevButtons[port] & button);
}

void EndFrame() {
    for (int i = 0; i < kMaxPorts; ++i) g_prevButtons[i] = g_currButtons[i];
}

} // namespace tj::input
