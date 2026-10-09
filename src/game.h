#pragma once

#include <cstdint>

#include "mem.h"

// The game side: what the mod knows about ResidentEvil3.exe at run time. The
// exe is Enigma Protector-wrapped, so nothing is read until the wrapper has
// unpacked it; after that everything is found by signature in the 1999 code,
// with each global read out of the matched instruction's operands and
// cross-checked between sites. Nothing here is an absolute address - the VAs in
// CLAUDE.md document build 21774525 (English) only.
namespace re3cc::game {

// --- the unpack gate, recon and discovery (mod thread) -----------------------------
bool unpacked(mem::TextStats* out);        // Enigma has decompressed the game's code
void recon(const char* dir);               // log the layout, the rebuilt import table and the save folder
bool dump_image(const char* dir);          // the unpacked 1999 image beside the DLL, plus an import map
void note_iat_slot(uintptr_t slot, uintptr_t original);  // so a dump carries the game's values, not our hooks
bool discover();                           // every signature; ready() once the screens are known
bool ready();
const char* status_text();

// --- the game's import table ------------------------------------------------------
// Enigma rebuilds it at run time with the real API addresses and its names are
// gone, so a slot is found by the address it holds: every slot in the table
// image::find_iat() located holding `real` is re-pointed to `hook`. Returns how
// many changed.
int hook_import(const char* what, void* real, void* hook);

// --- screens (main thread) ------------------------------------------------------------
// The game runs its screens as cooperative tasks: Task_chain(id, entry) puts an
// entry in a 0x80-byte task block and marks it runnable. The status screen -
// the inventory, file and map screen the player opens mid-game, and the mod's
// equivalent of RE1's option screen - runs as task 1.
bool in_game();          // a game is being played (the inventory pointer names an inventory)
bool status_screen();    // the status screen is up: the panel's screen
bool player_dead();      // the state flags' death bit
int task_entry(int id);  // a task's entry point, 0 when unknown
int task_state(int id);
void trace_tick();       // Watch values (main thread only)

// --- the player -------------------------------------------------------------------
// The player entity the health display reads. Unlike RE1's option screen, RE3's
// status screen does not copy the entity away, so it can be written at any time.
struct Player {
  int hp = 0;        // s16
  int max_hp = 0;    // 200 for Jill
  int condition = 0; // 0x0200 poison, 0x0800 dying
};
constexpr int kPoison = 0x0200, kDying = 0x0800;
bool player(Player* out);
bool set_player_hp(int hp);
bool set_player_condition(int condition);

// --- Jill's outfit ---------------------------------------------------------------
// The PC version has Jill pick an outfit at the title, after the difficulty,
// from a grid of eight: a value in the save block that the player-model loader
// reads for Jill's own two types - 0, and 1 once she has the backpack - and
// turns into another of her models, or her regular or biker one with a texture
// of its own loaded over it. The model is loaded only as a session starts and
// when a room load finds the model's type is not the character it is to put in
// play, so a new outfit shows from the next room load, which the mod has load
// the model by taking that test out until the loader has run
// (kSiteModelReload). The PlayStation boutique's outfits are character types of
// their own (2..7), which the loader takes before the value. Main thread only,
// but for outfit_name().
constexpr int kOutfits = 8;
bool outfit_known();
const char* outfit_name(int outfit);  // the title's names: "Normal", "Special 1 (biker)", ...
int outfit();                         // the value, -1 unknown
// The outfit Jill wears from the next model load: the value, or for a boutique
// type the outfit with the same model (*boutique set); -1 unknown.
int outfit_next(bool* boutique);
int character();      // the character the room loads put in play: Jill below 8, Carlos 8; -1 unknown
bool jill_in_play();  // Jill's model is loaded and Jill is the character to be in play
bool set_outfit(int outfit);  // a boutique type goes back to Jill's own regular type
// The model loader writes its file name afresh each time it runs: the mod marks
// the name, and finds the mark gone once the loader has loaded a model.
bool mark_model_load();
bool model_loaded();

// --- other entities ---------------------------------------------------------------
// A null-terminated array of entity pointers; index 0 is the player, 1 its
// partner, and everything from index 2 is the room's own - the enemies among
// them. Each entity keeps its HP as an s16 at the same offset as the player's.
struct Entity {
  int index = -1;
  uintptr_t at = 0;
  int type = -1;  // the entity's model/type byte
  int hp = 0;
};
constexpr int kFirstRoomEntity = 2;
bool entities_known();
int entities(Entity* out, int max);  // the live room entities (the player and its partner left out)
bool set_entity_hp(uintptr_t at, int hp);

// --- counters -------------------------------------------------------------------------
// The play time is kept the way the PlayStation game kept it: a total in the save
// block, in sixtieths of a second, and beside it the vblank count it was last
// brought up to date at - its base. The game brings it up to date (the vblanks
// since the base added on, the base moved to now) only as it saves, just before
// the block goes to the card, and - adding without moving the base - as the
// ending starts. In between the total stands still, so the play time is the
// total plus the vblanks since the base; a load and a new game set the base to
// the moment the session starts. Main thread only.
bool counters_ready();
uint32_t game_timer();  // the play time as the game will count it, 60 per second
bool set_game_timer(uint32_t v);  // ...set to v, counting on from now
// Freeze play time: the vblank count is known and the game's two adds to the
// total (kSiteTimerSave, kSiteTimerEnd) can be taken out.
bool timer_freeze_known();
bool settle_game_timer();   // what a save does first: the vblanks since the base onto the total
bool restart_game_timer();  // the base moved to now: the vblanks since it are never counted
int save_count();  // saves made, -1 unknown
bool set_save_count(int n);
int stage();
int room();

// --- the recovery total ------------------------------------------------------------------
// The ending ranks the player on three things: the saves, the play time and
// this - a total in the save block that every healing item used adds its whole
// amount to (a first aid spray the player's maximum HP, a green herb a quarter
// of it), however much of that the player actually needed. The rank scores it
// in steps, the lower the better: each step a highest total and the score it
// earns. The total is read and written on the main thread; the steps are
// copied out at discovery and may be read from any thread.
constexpr int kRankSteps = 10;
struct RankStep {
  int most = 0;   // a total at or under this...
  int score = 0;  // ...scores this (the steps go down in total and up in score)
};
constexpr int kRecoveryMax = 99999;  // the editor's ceiling: some 500 first aid sprays, far past the last step
bool recovery_known();
int recovery();                   // the total, -1 unknown
bool set_recovery(int total);     // clamped to 0..kRecoveryMax
bool recovery_scale(RankStep* out);  // kRankSteps of them, the lowest score first
int recovery_score(int total);    // what the ending's rank makes of a total, -1 unknown

// --- the clear count and the epilogues ----------------------------------------------------
// How many times the game has been finished: a u16 in the save block, so a save
// carries it. The ending shows the epilogue that count names while it is under
// the number of epilogues (not on Easy), then adds one to it and records it in
// saves\data.sav; the title menu's EPILOGUE screen reads it back from there and
// unlocks the epilogues up to it. Main thread only, but for epilogues().
constexpr int kClearsMax = 99;  // the editor's ceiling: the game's save titles show up to two digits
bool clears_known();
int clears();             // the count, -1 unknown
bool set_clears(int n);   // clamped to 0..kClearsMax
int epilogues();          // how many epilogues there are (8), 0 unknown

// --- the inventory and the item box --------------------------------------------------
// Ten four-byte slots {item id, count, flags, spare} at the front of a
// 0x140-byte block; the game's slot pointer names the block of whoever is in
// play (Jill's, or Carlos's own). The item box follows in the same block: 64
// slots of the same shape, which the box screen scrolls through as a ring. Then
// the equipped slot, the equipped weapon's item id and the count of usable slots.
struct ItemSlot {
  int id = 0;
  int count = 0;
  int flags = 0;
};
constexpr int kMaxSlots = 10;
constexpr int kBoxSlots = 64;
struct Bag {
  int slots = 0;        // how many of the ten the character may use
  int equipped = -1;    // the equipped slot, 0-based (-1 = none, 0xFF in the game)
  int weapon = 0;       // the equipped weapon's item id (0 = none)
  ItemSlot slot[kMaxSlots];
};
bool items_known();
bool box_known();
bool read_bag(Bag* out);
bool read_box(ItemSlot* out);  // kBoxSlots of them
// One slot of the inventory or the box. `flags` < 0 keeps the slot's flags
// byte (a count change); an empty slot is written as all zeroes.
bool write_slot(int index, int id, int count, int flags = -1);
bool write_box_slot(int index, int id, int count, int flags);
// The game keeps the inventory packed: taking a slot out moves every slot after
// it up one and empties the last, and the equipped slot moves with its weapon -
// what the game's own remove_item_slot (0x4DEA60) does when an item is used up.
bool remove_slot(int index);
// The status screen keeps its item icons and the equipped marker in video
// memory and rebuilds them only as it opens, when its state says the inventory
// changed. Every inventory write sets that, as the game's own item removal does,
// so the next opening shows the inventory as it is.
bool mark_icons_stale();
// The equipped slot and the weapon id beside it, as the status screen's own
// equip writes them (-1 for none: 0xFF and 0). The screen hands the player the
// weapon that id names when it closes.
bool set_equipped(int index);
int inventory_index();  // 0 the shared block (Jill), 1 Carlos's own
// The status screen puts the weapon away for an item that needs both hands and
// keeps the equipped slot to give back the next time it opens; an equip change
// made meanwhile would be undone by that, so the editor leaves it alone.
bool weapon_put_away();

// The game's own item rules, read out of its item table (items.h stands in for
// a build where the table is not found).
bool item_rules_known();
int item_max(int id);   // how many of this item one slot holds
int item_kind(int id);  // the table's kind column: 1 weapon, 2 ammo, 3 heal, 4 other, 5 key, 6 ribbon/tool, 7 file, 8 map
int item_table_last();  // the last id the table has a record for
// Whether two slots of this item combine, as the box screen combines them:
// ammo, ink ribbons and the reloading tool - anything of kind 2, 5 or 6 that a
// slot holds more than one of.
bool item_stacks(int id);

// --- cutscenes -----------------------------------------------------------------------
// A scripted scene runs with bit 0x8 of the save block's flags set - the bit the
// game draws its letterbox for and refuses the status screen under - and a movie
// with bit 0x10000 of the state flags. Main thread only.
bool cutscenes_known();
bool cinematic();      // a scripted scene is playing
bool movie_playing();  // a movie (FMV) is playing
bool movie_end_known();
// Ends the movie playing as if it had run out: the next movie frame closes it.
bool end_movie();
// A line of speech: the voice player keeps it going (and bit 0x20 of the save
// flags set) until the sound has played out, and the scenes wait for that.
bool voice_known();
bool voice_playing();
bool end_voice();  // the game's own way of cutting a line short
bool message_up();       // the message system has text on screen (narration included)
bool message_waiting();  // ...and it is waiting for the player: a page to turn, or a choice

// --- the countdown -------------------------------------------------------------------
// The Mercenaries runs against a clock, and the main game's room scripts start
// the same one for its timed scenes (the hospital's time bomb, four minutes late
// in the Dead Factory): the seconds left, in the save block, taken down one at a
// time by a frame count the gameplay loop keeps beside it (30 frames to the
// second) and, in the Mercenaries, topped up by the bonuses kills earn. It runs
// while bit 0x4000000 of the save block's flags is set. Main thread only.
bool countdown_known();
bool countdown_running();
int countdown_seconds();  // the seconds left, -1 unknown
// The missile at the very end has a count of its own. Once the radar receiver
// is taken (bit 0x2000000 of the save flags) the gameplay loop runs the radar
// in the clock's place, and while the countdown's bit is set too the radar
// takes the missile's distance down by a fixed step a frame; when it is out the
// game ends as it does when the clock runs out. Main thread only.
bool missile_known();     // the distance, and the store that takes the step off
bool missile_running();   // the radar is counting the missile in
int missile_seconds();    // how long it has left at that pace, -1 unknown

// --- the Mercenaries money ------------------------------------------------------------
// Two amounts: the takings of the run under way (a u16 the kills pay into, which
// each new run clears) and the bank (a u32 the results pay a run into, the shop
// after them spends, and saves\data.sav keeps). The results add half the seconds
// left and a $100 bonus to the takings in a 16-bit count that the game reads as
// signed when the player skips the count-up, and stop the bank at a maximum of
// their own ($999,999). Main thread only.
bool money_known();
bool mercenaries();  // the state flags' Mercenaries bit: RE3_MERCE.exe, from its title on
int takings();       // this run's takings in dollars, -1 unknown
int bank();          // the bank in dollars, -1 unknown
int bank_max();      // the results' cap on the bank, 0 unknown
// The editor's ceiling for the takings: far above an A rank ($1,500), and low
// enough that the results' sum stays under 32,768 with whatever time bonus and
// takings the rest of the run adds.
constexpr int kTakingsMax = 9999;
bool set_takings(int dollars);  // clamped to 0..kTakingsMax
bool set_bank(int dollars);     // clamped to 0..bank_max()

// --- the reloading tool's proficiency ---------------------------------------------------
// A batch of ammo made with the reloading tool comes out bigger the more batches
// of that kind of ammo have been made. The game counts them per kind - four
// u16s in the inventory block in play, so they are saved with the game and
// Carlos keeps his own - and looks the count up in a table of five levels, each
// a bonus in tenths of the batch. Handgun bullets and shotgun shells also offer
// enhanced rounds, at the bonus of a lower level, once more than a few batches
// of them have been made. Powder mixed straight into grenade rounds counts as a
// grenade batch. The counts are read and written on the main thread; the
// tables are copied out at discovery and may be read from any thread.
enum MixKind { kMixHandgun = 0, kMixShotgun, kMixMagnum, kMixGrenade, kMixKinds };
constexpr int kMixLevels = 5;
constexpr int kMixMax = 999;  // the editor's ceiling for a count: far past the last level
bool mixing_known();
const char* mix_kind_name(int kind);  // "Handgun bullets", ...
int mixes_made(int kind);             // batches of this kind made so far, -1 unknown
bool set_mixes_made(int kind, int n); // clamped to 0..kMixMax
// The level (0-based) the next batch of this kind is made at, after `made`
// batches; the fewest batches made that give a level; and its bonus in tenths.
int mix_level(int kind, int made);
int mix_level_from(int kind, int level);
int mix_bonus(int kind, int level);
// The bonus enhanced rounds are made at instead of `bonus`, -1 unknown.
int mix_enhanced_bonus(int bonus);
// Whether this kind offers enhanced rounds, and how many batches have to have
// been made before it does (more than this many).
bool mix_offers_enhanced(int kind);
int mix_enhanced_after();
// What one batch of `base` rounds comes to at `bonus` tenths, as the game
// works it out: the bonus rounded to the nearest round, the whole doubled on Easy.
int mix_batch(int base, int bonus, bool easy);
// The reloading tool's recipes for one kind, from the game's combination table:
// the powder, what it makes and how many with no bonus, and the enhanced rounds
// the same powder makes instead when the player takes them (0 = none).
struct MixRecipe {
  int powder = 0;
  int result = 0, base = 0;
  int enhanced = 0, enhanced_base = 0;
};
int mix_recipes(int kind, MixRecipe* out, int max);
bool easy_mode();  // the state flags' Easy bit, which doubles every batch (main thread)

// --- the memory cards and the save screen -------------------------------------------------
// The game keeps its saves on emulated PlayStation memory cards: each one a
// 128 KB image that a card object holds in memory, read from the card's file
// (saves\bu00.sav, saves\bu10.sav) once as the game starts and written back
// whole after every save and as the game closes. The image is a card's sixteen
// 8 KB blocks; block 0 holds the directory, whose entries 1..15 each describe
// the file whose first block has the same number. A save of this game is a
// one-block file named after its slot: the prefix the game's code gives, then
// the slot in two digits. The save and load screen - the title's Load the game,
// a typewriter's Save the game - is the memory card screen: as it opens, and
// whenever its state machine goes back through its "read the card" state, it
// lists the card into a table of its own, from which it draws its list of
// slots and loads. Main thread only, but for card_layout and location_name.
constexpr int kCardBytes = 0x20000;  // one card's image
constexpr int kCardBlockBytes = 0x2000;
constexpr int kCardBlocks = kCardBytes / kCardBlockBytes;
constexpr int kCardEntryBytes = 0x80;  // a directory entry, in block 0
constexpr int kCardMaxSlots = 15;      // save slots on a card (one block each, blocks 1..15)
struct CardLayout {
  int slots = 0;          // save slots per card
  int data_off = 0;       // where a save's data - the save block, play time first - starts in its block
  int saves_off = 0;      // ...and in the data: the save count (u8)
  int location_off = 0;   // the location it was saved in (u8)
  int clears_off = 0;     // the times the game has been cleared (u16)
  int flags_off = 0;      // a dword of flags...
  uint32_t easy_mask = 0; // ...one of them Easy
  char prefix[16] = {};   // a save's file name less its two digits ("BASLUS-00923")
};
bool cards_known();
bool card_layout(CardLayout* out);      // any thread, once discovery has run
const char* location_name(int id);      // the game's own name for a save's location; null when it has none
bool card_screen();                     // the memory card screen is up
int card_screen_state();                // its state machine's state, -1 unknown
bool card_list_waiting();               // ...its list of slots is up, waiting for the player
int card_screen_card();                 // the card it is showing, -1 unknown
int card_screen_cursor();               // the slot its cursor is on, -1 unknown
// Sends the screen back through its own "read the card" state, from its list:
// in the next frame it lists the card again and returns to the list, the cursor
// where it was. False when the list is not up.
bool rescan_card_screen();
// A card's in-memory image (kCardBytes), or null when the card object is not
// where discovery left it.
uint8_t* card_image(int card);
// The file the game writes that image to, as it builds the path: the card's
// folder, its name with ".sav" in place of the colon. Relative, like the game's.
bool card_file(int card, char* out, size_t n);

// --- code sites --------------------------------------------------------------------------
enum Site {
  kSiteSaveCount = 0,    // the save count's +1 at the end of a save
  kSiteInkRibbon,        // the save's take of one ink ribbon, jumped over
  kSiteInkCheck,         // the typewriter's "do you carry a ribbon" test, always passed
  kSiteInputMain,        // the three stores that publish a frame's button state
  kSiteInputRepeat,      // the fourth, for the auto-repeat bits
  kSiteCountdownTick,    // the countdown taking a second off
  kSiteCountdownFrames,  // ...and the frame count that says when, held still
  kSiteMissileStep,      // the radar taking a frame's step off the missile's distance
  kSitePlayerDamage,     // the player's HP going down by a hit, skipped
  kSiteRecovery,         // the healing's add to the recovery total, not stored
  kSiteTimerSave,        // a save's add to the play time, not stored
  kSiteTimerEnd,         // the ending's add to the play time, not stored
  kSiteModelReload,      // the room load's "same model" test, so the next one loads the player's model again
  kSiteCount
};
mem::Patch* site(Site s);  // a prepared patch, or null when the site was not found
const char* site_name(Site s);
// The input guard needs the button words zeroed once, the frame the patches go
// in: after that the game writes nothing there until they come back out.
bool clear_input_state();

}  // namespace re3cc::game
