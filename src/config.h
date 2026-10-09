#pragma once

namespace re3cc::config {

// One value the Watch diagnostic logs whenever it changes (reverse engineering
// aid only: the addresses are the user's, typed into the ini; nothing the mod
// does depends on them).
struct Watch {
  unsigned addr = 0;
  unsigned size = 0;  // 1, 2 or 4 bytes
};
constexpr int kMaxWatch = 24;

struct Settings {
  int toggle_key = 0x76;      // VK_F7: hide/show the panel while it is up
  int skip_key = 0x77;        // VK_F8: skip the cutscene playing (it asks first)
  int skip_pad = 0x10;        // the pad button that does the same (XInput's Start; 0 = none)
  // Each cheat's switch. The panel changes them, and every change is written
  // back to the ini (save_cheats), so they come back on the next launch.
  bool god_mode = false;
  bool one_hit_kills = false;
  bool infinite_ammo = false;
  bool infinite_ink = false;
  bool no_save_count = false;
  bool freeze_playtime = false;
  bool no_recovery_count = false;   // healing adds nothing to the recovery total
  bool freeze_mercs_timer = false;  // "Freeze countdown timer": any countdown, the main game's too
  bool skip_cutscenes = true;
  bool trace = false;         // verbose diagnostics
  bool always_show = false;   // debug: draw the panel everywhere, not only on its screens
  bool dump_image = false;    // write the unpacked exe image beside the DLL once
  int dump_delay = 0;         // seconds to wait after the unpack first, so a dump has a running game in it
  int nwatch = 0;             // "Watch = 0xBE41C0:4,0xD22777:1" - log those values as they change
  Watch watch[kMaxWatch];
  // "Disable = overlay,dispatch,game,gpa,input,cursor" turns whole subsystems
  // off so a fault can be bisected to one of them.
  bool disable_overlay = false;
  bool disable_dispatch = false;
  bool disable_game = false;
  bool disable_gpa = false;     // the exe's GetProcAddress import hook (Enigma resolves through it)
  bool disable_input = false;   // the GetAsyncKeyState guard while the panel captures input
  bool disable_cursor = false;  // letting the pointer go while the panel is up (GOG's wrapper keeps it in the picture)
};

const Settings& get();

// RE3-1999CabbyCodes.ini next to the DLL. Written with documented defaults the
// first time so the options are discoverable.
void load(const char* dir);
const char* dir();  // the DLL's directory, with a trailing separator

// The cheat switches as they are now, written into the ini: each one's line
// rewritten in place (or added, for an ini written before it existed), every
// other line and comment kept as it was.
struct Cheats {
  bool god_mode, one_hit_kills, infinite_ammo, infinite_ink, no_save_count, freeze_playtime, no_recovery_count,
      freeze_mercs_timer, skip_cutscenes;
};
bool save_cheats(const Cheats& c);

}  // namespace re3cc::config
