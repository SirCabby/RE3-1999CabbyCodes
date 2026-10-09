#pragma once

#include <cstdint>

#include "game.h"

// The cheats. Their switches are shared between the panel (it runs in the
// present hook) and the tick that applies them (the game's PeekMessageA), so
// the switches are plain atomics, edits travel through a small request queue,
// and everything that touches the game runs from tick() on the main thread.
namespace re3cc::cheats {

enum Kind : int {
  kGodMode = 0,
  kOneHitKills,
  kInfiniteAmmo,
  kInfiniteInk,
  kNoSaveCount,
  kFreezePlaytime,
  kNoRecoveryCount,   // healing adds nothing to the recovery total the ending ranks on
  kFreezeCountdown,   // the countdown: the Mercenaries' clock, and the main game's timed scenes
  kSkipCutscenes,     // cutscene.cpp: the skip key offers to skip the scene
  kCount
};

// Every switch starts as the ini left it, and every change is written back to
// it, so the cheats stay as they were left from one launch to the next.
void init();  // from DllMain: the locks
const char* name(Kind k);
bool enabled(Kind k);
void set_enabled(Kind k, bool on);  // any thread; takes effect on the next tick

// What the panel shows (built by tick, handed over under a lock).
struct Status {
  bool in_game = false;
  bool player_ok = false;
  game::Player player;
  bool site_damage = false;     // the player's damage routine, whose hits God mode stops
  bool damage_patched = false;
  bool entities_ok = false;  // the entity list is known
  int live_entities = 0;     // room entities with HP left, this tick
  bool ammo_ok = false;      // an equipped weapon whose count can be held
  int ammo_item = 0;         // ...which one
  bool site_ink = false;        // the save's take of a ribbon
  bool ink_patched = false;
  bool site_ink_check = false;  // the typewriter's "do you carry one" test
  bool ink_check_patched = false;
  bool site_savecount = false;
  bool savecount_patched = false;
  bool counters_ok = false;
  bool site_timer = false;     // the play time's vblank count and the game's two adds to it
  bool playtime_hold = false;  // ...taken out: the clock is frozen
  bool site_recovery = false;   // the healing's add to the recovery total
  bool recovery_patched = false;
  bool recovery_ok = false;     // the recovery total was found
  int recovery = -1;            // ...and what it holds
  bool clears_ok = false;       // the clear count was found
  int clears = -1;              // ...and what it holds: the times the game has been finished
  int epilogues = 0;            // the epilogues the clears unlock, one each
  bool outfit_ok = false;       // Jill's outfit was found
  int outfit = -1;              // the outfit she wears from the next model load
  bool outfit_boutique = false; // ...one the PlayStation boutique put on her, as a character type of its own
  bool jill = false;            // Jill is in play (not Carlos)
  bool outfit_pending = false;  // an outfit set here, waiting for the next room load to load her model
  bool site_countdown = false;  // the countdown's two sites
  bool countdown_patched = false;
  bool countdown_running = false;
  int countdown = 0;            // its seconds left, while it runs
  bool site_missile = false;    // the radar's step off the missile's distance, at the very end
  bool missile_patched = false;
  bool missile_running = false;
  int missile = 0;              // the seconds the missile has left at the radar's pace, while it counts
  bool money_ok = false;        // the Mercenaries money was found
  bool mercenaries = false;     // this is the Mercenaries (its section and money are shown only then)
  int takings = -1;             // this run's takings, in dollars
  int bank = -1;                // the bank the results pay them into
  int bank_max = 0;             // ...and the most it holds
  bool mix_ok = false;          // the reloading tool's batch counts were found
  int mixes[game::kMixKinds] = {};  // batches of each kind made so far
  bool easy = false;            // Easy, which doubles every batch
  int saves = -1;        // saves made
  uint32_t timer = 0;    // play time as the game will count it, 60 per second
  int stage = -1, room = -1;
  char last_action[120] = {};
};
Status status();

// Requests from the panel (applied on the next tick, on the main thread).
void request_saves(int saves);
void request_playtime_seconds(int seconds);
void request_recovery(int total);  // held to 0..game::kRecoveryMax
// The clear count, held to 0..game::kClearsMax: the main game's own (the
// Mercenaries has no ending to count it).
void request_clears(int n);
// The Mercenaries money, each held to the range the game handles (game.h).
void request_takings(int dollars);
void request_bank(int dollars);
// The reloading tool's proficiency in one kind of ammo, as the batches of it
// made so far (game.h): the level the next batch is made at follows from it.
void request_mixes(int kind, int made);
// Jill's outfit (game.h), the main game's own: she changes into it at the next
// room load, which loads her model again for it.
void request_outfit(int outfit);

// Main thread.
void tick(bool in_game, bool status_screen);
void remove_hooks();  // teardown: revert every patch

}  // namespace re3cc::cheats
