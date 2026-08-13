// XInput -> original Xbox controller state.
//
// The game reads controller state through the XAPI function XInputGetState, which
// fills an Xbox XINPUT_STATE whose gamepad layout differs from the PC XInput one:
// the original Duke/S pad exposes the two triggers and the black/white buttons as
// *analog* bytes in bAnalogButtons[8], while A/B/X/Y are also pressure-sensitive.
// This module reads a modern PC XInput pad and produces that Xbox-shaped state so
// the decompiled game code can consume it unchanged.
#pragma once
#include <cstdint>

namespace tj::input {

// Digital button bitmask (matches the Xbox XINPUT_GAMEPAD wButtons bits).
enum XboxButton : uint16_t {
    XPAD_DPAD_UP        = 0x0001,
    XPAD_DPAD_DOWN      = 0x0002,
    XPAD_DPAD_LEFT      = 0x0004,
    XPAD_DPAD_RIGHT     = 0x0008,
    XPAD_START          = 0x0010,
    XPAD_BACK           = 0x0020,
    XPAD_LEFT_THUMB     = 0x0040,
    XPAD_RIGHT_THUMB    = 0x0080,
};

// Indices into bAnalogButtons[] on the original Xbox pad.
enum XboxAnalog : int {
    XANA_A = 0, XANA_B = 1, XANA_X = 2, XANA_Y = 3,
    XANA_BLACK = 4, XANA_WHITE = 5,
    XANA_LEFT_TRIGGER = 6, XANA_RIGHT_TRIGGER = 7,
    XANA_COUNT = 8,
};

// Byte-compatible with the Xbox XINPUT_GAMEPAD struct the game expects.
#pragma pack(push, 1)
struct XboxGamepad {
    uint16_t wButtons;
    uint8_t  bAnalogButtons[XANA_COUNT];
    int16_t  sThumbLX, sThumbLY;
    int16_t  sThumbRX, sThumbRY;
};
#pragma pack(pop)

// Poll PC XInput pad `port` (0..3). Returns false if no controller is connected.
// On success `out` holds the translated Xbox-shaped state.
bool Poll(int port, XboxGamepad& out);

// True the frame `button` transitioned from up to down on `port` (menu navigation).
bool Pressed(int port, XboxButton button);

// Advance edge-detection state; call once per frame after all Poll/Pressed queries.
void EndFrame();

} // namespace tj::input
