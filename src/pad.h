#pragma once

// A game pad for the cutscene skip, read through XInput: the game reads its pad
// through DirectInput (GOG's dinput.dll in front of it), which XInput can read
// alongside, and an XInput pad is what Steam Input and most controllers show
// a Windows game. The game's own button words cannot serve, because while the
// skip asks its question the game is held and polls nothing.
namespace re3cc::pad {

// XINPUT_GAMEPAD's button bits.
enum Button : unsigned {
  kNone = 0,
  kStart = 0x0010,
  kBack = 0x0020,
  kLeftThumb = 0x0040,
  kRightThumb = 0x0080,
  kLeftShoulder = 0x0100,
  kRightShoulder = 0x0200,
  kA = 0x1000,
  kB = 0x2000,
  kX = 0x4000,
  kY = 0x8000,
};

// Reads every connected pad and returns the buttons pressed since the last
// call (main thread). Pads that are not connected are looked for again every
// couple of seconds rather than on every call: XInput is slow to say no.
unsigned poll();
bool connected();  // a pad answered the last poll

unsigned parse(const char* name);  // "Start", "Back", "A", ... (0 for "None" or unknown)
const char* name(unsigned button);

}  // namespace re3cc::pad
