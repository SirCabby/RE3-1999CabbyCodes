#pragma once

#include <windows.h>

#include <cstdint>

#include "game.h"

// The save files, managed beside the game's memory card screen - the title's
// Load the game and a typewriter's Save the game: while the screen is up the
// panel lists the fifteen slots of the game's memory card, and can delete a
// save or copy one into another slot, empty or not.
//
// RE3 keeps every save on an emulated PlayStation memory card, a 128 KB image
// (saves\bu00.sav) the game reads once as it starts, keeps in memory and writes
// back whole after each save and as it closes (game.h). A change is therefore
// made to both copies: the file first - through a temp file and one rename,
// read back to check - and then the game's own image, which would otherwise
// put the file back the way it was at the next save or when the game closes.
// A save is a one-block file named after its slot. Delete clears its directory
// entry and its block, which is what a slot that was never used looks like on
// the game's cards. Copy writes the source's block into the target slot's file
// - a new one in the first free directory entry when the slot is empty - with
// the slot number in the save's title made the new slot's, as a save made
// there would have it.
//
// The screen lists the card into a table of its own and draws its list and
// loads from that table, so after a change the mod sends it back through its
// own "read the card" state (game::rescan_card_screen): its list shows the
// change at once and never loads a save from a file that is gone. Changes are
// made only while that list is up and waiting for the player.
namespace re3cc::savefiles {

constexpr int kSlots = game::kCardMaxSlots;

// One slot of the card, as the game's list shows it.
struct Save {
  bool known = false;   // the card could be read
  bool used = false;    // a save of this game is in the slot
  bool other = false;   // the game shows "Other data" here: another game's files take the room
  bool odd = false;     // a file the game would not write (not one block, or two files claim the slot): left alone
  int entry = -1;       // the save's directory entry, and so its block (1..15)
  uint32_t frames = 0;  // its play time, sixtieths of a second
  int saves = -1;       // its save count (0xFF on a "Next Game" save: none made yet)
  int location = -1;    // where it was made (game::location_name)
  int clears = 0;       // how many times the game had been cleared
  bool easy = false;
  uint32_t hash = 0;    // of its directory entry and its block
};
// Two readings of a slot are the same save: what a request checks before it acts.
bool same_save(const Save& a, const Save& b);
// "Monitor Room, 1:02:03, 7 saves, Hard" - or "no data" / "other data".
const char* describe(const Save& s, char* out, size_t n);

// A copy of everything the panel draws, taken under the lock.
struct View {
  bool ready = false;      // the cards and the screen were found
  bool up = false;         // the memory card screen is up
  bool card_ok = false;    // the card could be read, and is laid out the way the game writes it...
  char why[96] = {};       // ...or why not
  bool waiting = false;    // the screen's list of slots is up, waiting for the player: changes can be made
  int cursor = -1;         // the slot the game's cursor is on, while its list waits
  int slots = 0;
  Save save[kSlots];
  int free_entries = 0;    // directory entries free for a new file
  char file[MAX_PATH] = {};  // the card's file, as the game names it
  char note[240] = {};     // what the last change came to
  bool note_error = false;
};

void init();     // from DllMain: the lock
View view();     // any thread
bool showing();  // main thread: the memory card screen is up, so the panel belongs on screen

// Requests from the panel, carried out on the next main-thread tick while the
// game's list waits. What the panel saw of the slots goes with them: the card
// can change between the click and the tick, and changing a save that is not
// the one clicked is worse than not changing anything.
void request_delete(int slot, const Save& seen);
void request_copy(int from, int to, const Save& from_seen, const Save& to_seen);

void tick(bool screen_up);  // main thread, every tick

}  // namespace re3cc::savefiles
