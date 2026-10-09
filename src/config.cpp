#include "config.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "log.h"
#include "pad.h"

namespace re3cc::config {
namespace {

Settings g_settings;
char g_path[MAX_PATH] = {};
char g_dir[MAX_PATH] = {};

bool truthy(const char* v) {
  return !_stricmp(v, "1") || !_stricmp(v, "on") || !_stricmp(v, "true") || !_stricmp(v, "yes");
}

void write_defaults() {
  FILE* f = std::fopen(g_path, "w");
  if (!f) return;
  std::fprintf(f,
               "# RE3-1999CabbyCodes\n"
               "#\n"
               "#   ToggleKey          = 0x76  ; virtual-key code that hides/shows the panel (0x76 = F7)\n"
               "#   SkipKey            = 0x77  ; virtual-key code that skips a cutscene, after asking (0x77 = F8)\n"
               "#   SkipPadButton      = Start ; the pad button that does the same: Start, Back, A, B, X, Y, LB, RB,\n"
               "#                               ; LS, RS, or None\n"
               "#   GodMode            = 0     ; the cheats; the panel switches them, and the mod writes\n"
               "#   OneHitKills        = 0     ; every change back here, so they stay as they were left\n"
               "#   InfiniteAmmo       = 0     ; from one launch to the next\n"
               "#   InfiniteInkRibbons = 0     ; save without a ribbon, and use none\n"
               "#   NoSaveCount        = 0     ; save without incrementing the save counter\n"
               "#   FreezePlaytime     = 0\n"
               "#   NoRecoveryCount    = 0     ; heal without adding to the recovery total the ending ranks you on\n"
               "#   FreezeMercsTimer   = 0     ; any countdown stands still, the Mercenaries' and the main game's\n"
               "#   SkipCutscenes      = 1     ; SkipKey offers to skip the cutscene or movie playing\n"
               "#   AlwaysShow         = 0     ; debug: draw the panel everywhere, not only on its screens\n"
               "#   Trace              = 0     ; verbose diagnostics in RE3-1999CabbyCodes.log\n"
               "#   DumpImage          = 0     ; write the unpacked game image beside the DLL once\n"
               "#   DumpDelay          = 0     ; seconds to wait after the unpack before doing it\n"
               "#   Watch              =       ; debug: 0xBE41C0:4,0xD22777:1 - log those values as they change\n"
               "#   Disable            =       ; comma list of subsystems to turn off: overlay,dispatch,game,gpa,input,cursor\n"
               "ToggleKey = 0x76\n"
               "SkipKey = 0x77\n"
               "SkipPadButton = Start\n"
               "GodMode = 0\n"
               "OneHitKills = 0\n"
               "InfiniteAmmo = 0\n"
               "InfiniteInkRibbons = 0\n"
               "NoSaveCount = 0\n"
               "FreezePlaytime = 0\n"
               "NoRecoveryCount = 0\n"
               "FreezeMercsTimer = 0\n"
               "SkipCutscenes = 1\n"
               "AlwaysShow = 0\n"
               "Trace = 0\n"
               "DumpImage = 0\n"
               "DumpDelay = 0\n"
               "Watch =\n"
               "Disable =\n");
  std::fclose(f);
}

// "0xBE41C0:4, 0xD22777:1" -> the watch list (sizes 1, 2 or 4; 4 when omitted).
void parse_watch(const char* val) {
  g_settings.nwatch = 0;
  const char* p = val;
  while (*p && g_settings.nwatch < kMaxWatch) {
    while (*p == ' ' || *p == '\t' || *p == ',') ++p;
    if (!*p) break;
    char* end = nullptr;
    const unsigned long addr = std::strtoul(p, &end, 0);
    if (end == p) break;
    p = end;
    unsigned size = 4;
    if (*p == ':') {
      size = static_cast<unsigned>(std::strtoul(p + 1, &end, 0));
      p = end;
    }
    if (addr && (size == 1 || size == 2 || size == 4)) {
      g_settings.watch[g_settings.nwatch].addr = static_cast<unsigned>(addr);
      g_settings.watch[g_settings.nwatch].size = size;
      ++g_settings.nwatch;
    }
    while (*p && *p != ',') ++p;
  }
}

// "  GodMode = 1 ; comment" -> whether the line sets `key` (case as the parser
// takes it: any, with spaces either side of the name).
bool line_sets(const char* line, const char* key) {
  while (*line == ' ' || *line == '\t') ++line;
  const size_t n = std::strlen(key);
  if (_strnicmp(line, key, n)) return false;
  line += n;
  while (*line == ' ' || *line == '\t') ++line;
  return *line == '=';
}

}  // namespace

bool save_cheats(const Cheats& c) {
  struct Item {
    const char* key;
    bool on;
    bool* setting;
    bool written;
  } items[] = {{"GodMode", c.god_mode, &g_settings.god_mode, false},
               {"OneHitKills", c.one_hit_kills, &g_settings.one_hit_kills, false},
               {"InfiniteAmmo", c.infinite_ammo, &g_settings.infinite_ammo, false},
               {"InfiniteInkRibbons", c.infinite_ink, &g_settings.infinite_ink, false},
               {"NoSaveCount", c.no_save_count, &g_settings.no_save_count, false},
               {"FreezePlaytime", c.freeze_playtime, &g_settings.freeze_playtime, false},
               {"NoRecoveryCount", c.no_recovery_count, &g_settings.no_recovery_count, false},
               {"FreezeMercsTimer", c.freeze_mercs_timer, &g_settings.freeze_mercs_timer, false},
               {"SkipCutscenes", c.skip_cutscenes, &g_settings.skip_cutscenes, false}};
  if (!g_path[0]) return false;
  std::string out;
  if (FILE* f = std::fopen(g_path, "r")) {
    char line[512];
    while (std::fgets(line, sizeof(line), f)) {
      const char* p = line;
      while (*p == ' ' || *p == '\t') ++p;
      bool replaced = false;
      if (*p != '#' && *p != ';')
        for (Item& it : items)
          if (line_sets(p, it.key)) {
            // A second line for the same key would win on the next load, so it
            // gets the value too.
            out += it.key;
            out += it.on ? " = 1\n" : " = 0\n";
            it.written = true;
            replaced = true;
            break;
          }
      if (!replaced) {
        out += line;
        if (!out.empty() && out.back() != '\n') out += '\n';
      }
    }
    std::fclose(f);
  }
  for (Item& it : items) {
    *it.setting = it.on;
    if (it.written) continue;
    out += it.key;
    out += it.on ? " = 1\n" : " = 0\n";
  }
  // Written beside the ini and moved over it, so a crash halfway leaves the old
  // file rather than half of a new one.
  char tmp[MAX_PATH + 8];
  std::snprintf(tmp, sizeof(tmp), "%s.tmp", g_path);
  FILE* f = std::fopen(tmp, "wb");
  if (!f) {
    logf("config: cannot write %s - the cheat switches are not saved", tmp);
    return false;
  }
  const bool ok = std::fwrite(out.data(), 1, out.size(), f) == out.size();
  if (std::fclose(f) != 0 || !ok || !MoveFileExA(tmp, g_path, MOVEFILE_REPLACE_EXISTING)) {
    DeleteFileA(tmp);
    logf("config: could not save the cheat switches to %s", g_path);
    return false;
  }
  return true;
}

const Settings& get() { return g_settings; }
const char* dir() { return g_dir; }

void load(const char* dir) {
  std::snprintf(g_dir, sizeof(g_dir), "%s", dir);
  std::snprintf(g_path, sizeof(g_path), "%sRE3-1999CabbyCodes.ini", dir);
  FILE* f = std::fopen(g_path, "r");
  if (!f) {
    write_defaults();
    logf("config: no %s - wrote one with the defaults", g_path);
    return;
  }
  char line[512];
  while (std::fgets(line, sizeof(line), f)) {
    char* p = line;
    while (*p == ' ' || *p == '\t') ++p;
    if (*p == '#' || *p == ';' || *p == '[' || *p == '\n' || *p == '\r' || !*p) continue;
    char* eq = std::strchr(p, '=');
    if (!eq) continue;
    *eq = '\0';
    char* key = p;
    char* val = eq + 1;
    for (char* e = key + std::strlen(key); e > key && (e[-1] == ' ' || e[-1] == '\t');) *--e = '\0';
    while (*val == ' ' || *val == '\t') ++val;
    // A trailing "; comment" is the file's own documentation style.
    if (char* c = std::strchr(val, ';')) *c = '\0';
    for (char* e = val + std::strlen(val);
         e > val && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t');)
      *--e = '\0';

    if (!_stricmp(key, "ToggleKey")) {
      int v = static_cast<int>(std::strtol(val, nullptr, 0));
      if (v > 0 && v < 256) g_settings.toggle_key = v;
    } else if (!_stricmp(key, "SkipKey")) {
      int v = static_cast<int>(std::strtol(val, nullptr, 0));
      if (v > 0 && v < 256) g_settings.skip_key = v;
    } else if (!_stricmp(key, "GodMode")) {
      g_settings.god_mode = truthy(val);
    } else if (!_stricmp(key, "OneHitKills")) {
      g_settings.one_hit_kills = truthy(val);
    } else if (!_stricmp(key, "InfiniteAmmo")) {
      g_settings.infinite_ammo = truthy(val);
    } else if (!_stricmp(key, "InfiniteInkRibbons")) {
      g_settings.infinite_ink = truthy(val);
    } else if (!_stricmp(key, "NoSaveCount")) {
      g_settings.no_save_count = truthy(val);
    } else if (!_stricmp(key, "FreezePlaytime")) {
      g_settings.freeze_playtime = truthy(val);
    } else if (!_stricmp(key, "NoRecoveryCount")) {
      g_settings.no_recovery_count = truthy(val);
    } else if (!_stricmp(key, "FreezeMercsTimer")) {
      g_settings.freeze_mercs_timer = truthy(val);
    } else if (!_stricmp(key, "SkipPadButton")) {
      g_settings.skip_pad = static_cast<int>(pad::parse(val));
      if (!g_settings.skip_pad && _stricmp(val, "None") && val[0])
        logf("config: SkipPadButton '%s' is not a button - no pad button skips", val);
    } else if (!_stricmp(key, "SkipCutscenes")) {
      g_settings.skip_cutscenes = truthy(val);
    } else if (!_stricmp(key, "AlwaysShow")) {
      g_settings.always_show = truthy(val);
    } else if (!_stricmp(key, "Trace")) {
      g_settings.trace = truthy(val);
    } else if (!_stricmp(key, "DumpImage")) {
      g_settings.dump_image = truthy(val);
    } else if (!_stricmp(key, "DumpDelay")) {
      const int v = static_cast<int>(std::strtol(val, nullptr, 0));
      if (v >= 0 && v <= 600) g_settings.dump_delay = v;
    } else if (!_stricmp(key, "Watch")) {
      parse_watch(val);
    } else if (!_stricmp(key, "Disable")) {
      g_settings.disable_overlay = std::strstr(val, "overlay") != nullptr;
      g_settings.disable_dispatch = std::strstr(val, "dispatch") != nullptr;
      g_settings.disable_game = std::strstr(val, "game") != nullptr;
      g_settings.disable_gpa = std::strstr(val, "gpa") != nullptr;
      g_settings.disable_input = std::strstr(val, "input") != nullptr;
      g_settings.disable_cursor = std::strstr(val, "cursor") != nullptr;
    } else {
      logf("config: unknown key '%s' ignored", key);
    }
  }
  std::fclose(f);
  logf("config: ToggleKey=0x%02X SkipKey=0x%02X SkipPadButton=%s GodMode=%d OneHitKills=%d InfiniteAmmo=%d InfiniteInkRibbons=%d "
       "NoSaveCount=%d FreezePlaytime=%d NoRecoveryCount=%d FreezeMercsTimer=%d SkipCutscenes=%d AlwaysShow=%d Trace=%d "
       "DumpImage=%d DumpDelay=%d Watch=%d value(s) Disable=%s%s%s%s%s%s",
       g_settings.toggle_key, g_settings.skip_key,
       g_settings.skip_pad ? pad::name(static_cast<unsigned>(g_settings.skip_pad)) : "None", g_settings.god_mode, g_settings.one_hit_kills,
       g_settings.infinite_ammo, g_settings.infinite_ink, g_settings.no_save_count, g_settings.freeze_playtime,
       g_settings.no_recovery_count, g_settings.freeze_mercs_timer, g_settings.skip_cutscenes, g_settings.always_show,
       g_settings.trace, g_settings.dump_image, g_settings.dump_delay, g_settings.nwatch,
       g_settings.disable_overlay ? "overlay " : "", g_settings.disable_dispatch ? "dispatch " : "",
       g_settings.disable_game ? "game " : "", g_settings.disable_gpa ? "gpa " : "",
       g_settings.disable_input ? "input " : "", g_settings.disable_cursor ? "cursor" : "");
}

}  // namespace re3cc::config
