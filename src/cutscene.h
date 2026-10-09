#pragma once

#include <windows.h>

// Skip cutscenes. While a scripted scene (the letterbox) or a movie is playing,
// the skip key or pad button holds the game where it is - picture and sound -
// and asks. A "yes" ends a movie the way the movie player ends one that has run
// out, and runs a scripted scene through to its end as fast as the game can
// go: every step of its script still runs, so every flag, item and room change
// it makes is made, but what it plays once (speech, effects) is never heard,
// and a line of speech is cut short the way the game cuts one short itself, so
// the scene does not wait for it. A page to turn or a choice to make stops the
// run until the player has dealt with it; a status screen the scene opens ends
// it there. "No" carries on where it paused.
//
// Everything here runs on the game's main thread: the window procedure, the
// present hook and the PeekMessageA hook all do.
namespace re3cc::cutscene {

enum class Phase { kIdle, kAsking, kSkipping };

using PeekFn = BOOL(WINAPI*)(LPMSG, HWND, UINT, UINT, UINT);

// The window procedure's key-downs (no auto-repeat). True when the key was
// this module's: the skip key during a cutscene, and Enter/Space/Escape while
// asking.
bool on_key(int vk);
// Every call of the game's PeekMessageA: a question is asked here (the game
// waits inside this call until it is answered), and a skip is driven.
void pump(PeekFn real_peek);
void tick(bool game_ready);  // the main-thread tick

Phase phase();
bool skippable();           // a cutscene or a movie is playing and the skip is on
bool waiting_for_player();  // a skip held at a page or a choice
float hint_seconds();       // how long the current one has been on screen (-1 none)
const char* key_name();     // the skip key, as the prompt names it
const char* pad_name();     // the skip pad button, or "" with no pad
void decide(bool skip);     // the prompt's buttons
bool hold_input();          // the game's buttons are held up (asking, skipping, just after)
bool want_capture();        // the present hook keeps a copy of each frame to show while the game is held
bool drop_present();        // the present hook: this frame of the game's is not shown

// The game paces itself by timeGetTime alone: its logic task spins on it until
// the next emulated vertical blank is due. While a skip runs, that clock moves
// on by a millisecond every time it is read, so the spin ends at once - which
// is what lets a scene run as fast as the machine allows. The import slot is
// found by value, as the message pump's is.
void install_time_hooks();
void remove_time_hooks();

}  // namespace re3cc::cutscene
