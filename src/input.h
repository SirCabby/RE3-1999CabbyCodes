#pragma once

// The input guard. RE3 reads the keyboard and the pad through DirectInput (the
// exe imports DINPUT!DirectInputCreateA and the GOG wrapper beside it answers),
// and publishes one frame's buttons as three words its whole UI reads: held,
// newly pressed, and auto-repeating. Rather than stand between the game and
// DirectInput, the guard stops those three words being written at all - four
// verified stores at the end of the input poll turn into NOPs while a panel
// field has keyboard focus, and the words are zeroed once as that happens, so
// the game sees every button up until the patches come out again.
//
// That takes the whole keyboard and pad away from the game for as long as a
// panel field has focus, Escape included; the panel's own key (F7) is read
// through the ImGui backend, so it always works.
namespace re3cc::input {

void tick(bool capture);  // main thread: apply or revert the guard for this frame
bool guarding();          // whether the game's buttons are being held down
void uninstall();         // teardown: the stores go back

}  // namespace re3cc::input
