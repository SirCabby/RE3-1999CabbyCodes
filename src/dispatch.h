#pragma once

#include <windows.h>

// The main-thread tick. ResidentEvil3.exe runs everything on its primary
// thread - a cooperative task scheduler switches stacks there - and its message
// pump calls PeekMessageA every time round, screens and all. Enigma rebuilds
// the game's import table with the real API addresses, so once the game is
// unpacked its PeekMessageA slot is re-pointed here, and everything that
// touches game memory runs from that callback.
namespace re3cc::dispatch {

void init();                // from DllMain: records the primary thread
bool hook_message_pump();   // mod thread, once the game is unpacked
void unhook_message_pump();
void tick();                // from the PeekMessageA hook; ignored on any other thread

struct Snapshot {
  bool unpacked = false;      // the mod thread confirmed the game's code is unpacked
  bool game_ready = false;    // discovery finished
  bool in_game = false;        // a game is being played
  bool status_screen = false;  // the game's status screen is up: the panel's screen
  bool card_screen = false;    // the memory card screen (load or save) is up: the panel is the save file manager
  bool show_panel = false;     // the visibility rule, evaluated on the main thread
  unsigned long main_thread = 0;
  unsigned long long ticks = 0;
};
Snapshot snapshot();

bool on_main_thread();
HANDLE first_tick_event();    // signalled once, from the first tick
void set_unpacked(bool on);   // the mod thread's gate result

}  // namespace re3cc::dispatch
