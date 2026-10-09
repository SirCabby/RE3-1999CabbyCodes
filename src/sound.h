#pragma once

#include <windows.h>

// The game's sound, for the cutscene skip. Everything it plays - voices,
// effects, music, a movie's soundtrack - is a DirectSound secondary buffer,
// and every secondary buffer of one DirectSound implementation shares one
// vtable. A buffer of our own, made and thrown away once, shows where that
// vtable is; hooking Play/Stop/GetStatus/Lock/SetVolume/Release in it lets the
// mod see every buffer the game uses (whenever it was made) and step in front
// of a Play. No game code is touched.
namespace re3cc::sound {

bool install(HWND hwnd);  // main thread, once the game's window exists
void uninstall();
bool ready();

// The pause: every buffer that is playing stops where it is...
void pause();
// ...and starts again from there - all of them, or (a skip) only the ones that
// loop: music and ambient sound carry on, while what was playing once (a
// voice, an effect) stays stopped and is wound back to its start. The game's
// voice player sees its line has stopped and ends it the way it ends any line
// that has played out.
void resume(bool one_shots);
// While quiet, a buffer told to play once is not started: the effects and
// voices a fast-forwarded scene fires off are never heard. Looping sound plays.
void set_quiet(bool on);

}  // namespace re3cc::sound
