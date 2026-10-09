#pragma once

#include "game.h"

// The inventory editor and the item box, on the status screen's panel. Both
// are the game's own: the ten slots of whoever is in play and the 64-slot item
// box in the same block, saved with the game. The panel edits an inventory
// slot, stores an item from the inventory in the box, takes one back, or throws
// a box item away.
//
// Everything is a write between frames while the status screen is up - never
// while the game itself could be using the inventory. The typewriter's save
// screen, which borrows the slots, is a different screen on the same task, so
// it can never be up at the same time. Moves follow the game's rules: the
// inventory is kept packed, as the game's own remove_item_slot keeps it - an
// emptied slot leaves it and the slots after it move up - and anything new goes
// into the first empty slot. Ammo, ink ribbons and the reloading tool - the
// items the box screen combines - top up a stack of the same item first, each
// to what one slot holds, and only the rest takes a free slot. What fits
// nowhere stays where it was.
//
// The equipped weapon is two bytes: the slot and the weapon's item id. Emptying
// or storing the equipped slot unequips it (as the box screen does), putting
// another weapon in it equips that one; the status screen hands the player the
// new weapon as it closes.
//
// The status screen draws its item icons from video memory it fills as it
// opens, so it shows a change the next time it is opened: every write tells it
// to rebuild them (game::mark_icons_stale), as the game's own item removal does.
namespace re3cc::inventory {

using Slot = game::ItemSlot;
constexpr int kBagMax = game::kMaxSlots;
constexpr int kBoxMax = game::kBoxSlots;

// A copy of everything the panel draws, taken under the lock.
struct View {
  bool known = false;      // the inventory was found in the game
  bool box_known = false;  // ...and the item box
  bool in_game = false;
  bool open = false;       // the status screen is up: the only time anything is changed
  bool put_away = false;   // the status screen holds the weapon back for an item (equip changes refused)
  bool redraw_due = false; // the inventory changed while this opening of the status screen was up
  int who = -1;            // 0 Jill's block, 1 Carlos's
  int size = 0;            // the slots this character may use (8, or 10 with the hip pouch)
  int held = 0;            // slots in use
  int equipped = -1;       // the equipped slot, 0-based (-1 = none)
  Slot bag[kBagMax];
  int box_size = 0;
  Slot box[kBoxMax];
  char note[200] = {};     // what the last change came to (logged; the panel shows a refusal only)
  bool note_error = false;
};
View view();  // any thread
const char* who_name(int who);

// What the editor lets a slot of an item hold. 0 = the game keeps no count for
// it; a gun holds 0 (empty) up to its magazine, anything else that counts 1 up
// to what one slot of it holds.
int count_limit(int id);
int count_floor(int id);
int default_count(int id);  // what an item starts with when the editor picks it

// Where an item goes when it is moved: onto the stacks of it already there (the
// items the game combines, each up to one slot's worth, in slot order), then
// into the first free slot. What fits nowhere stays where it was. The panel
// asks this for what a button offers, and the move does what it answered.
struct Fit {
  int onto[kBoxMax] = {};  // how many go onto the stack in each slot
  int slot = -1;           // the free slot the rest goes into (-1 = none, or none needed)
  int into = 0;            // how many go into it
  int left = 0;            // how many stay behind
  int topped() const;
  bool ok() const { return slot >= 0 || topped() > 0; }  // the move moves anything at all
};
Fit fit_into_bag(const View& v, int id, int count);
Fit fit_into_box(const View& v, int id, int count);

// Requests from the panel, carried out on the next main-thread tick while the
// status screen is up. `seen` is what the panel showed in that slot: a slot
// that holds something else by then is left alone - acting on the wrong item is
// worse than not acting.
void request_set(int slot, int id, int count, const Slot& seen);  // the editor
void request_store(int slot, const Slot& seen);                    // inventory slot -> the box
void request_take(int box_slot, const Slot& seen);                 // box slot -> the inventory
void request_drop(int box_slot, const Slot& seen);                 // throw a box item away

void init();                                 // from DllMain: the lock
void tick(bool in_game, bool status_screen);  // main thread, every tick

}  // namespace re3cc::inventory
