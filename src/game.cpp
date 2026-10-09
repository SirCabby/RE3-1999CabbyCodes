#include "game.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "config.h"
#include "image.h"
#include "items.h"
#include "log.h"

namespace re3cc::game {
namespace {

// Our own patches of the game's import slots, so a dump shows what the game had.
struct IatNote {
  uintptr_t slot;
  uintptr_t original;
};
IatNote g_iat[16];
int g_iat_n = 0;

// --- export names, for the rebuilt import table --------------------------------
// The game's import slots hold bare addresses once Enigma has rebuilt them; this
// says which export of which loaded module one of them is.
struct ModuleExports {
  HMODULE mod = nullptr;
  char name[64] = {};
  std::vector<std::pair<uint32_t, const char*>> by_rva;
};
ModuleExports g_exports[64];
int g_nexports = 0;

const ModuleExports* exports_of(HMODULE m) {
  for (int i = 0; i < g_nexports; ++i)
    if (g_exports[i].mod == m) return &g_exports[i];
  if (g_nexports >= 64) return nullptr;
  ModuleExports& e = g_exports[g_nexports++];
  e.mod = m;
  char path[MAX_PATH] = "?";
  GetModuleFileNameA(m, path, MAX_PATH);
  const char* leaf = std::strrchr(path, '\\');
  std::snprintf(e.name, sizeof(e.name), "%s", leaf ? leaf + 1 : path);
  const auto base = reinterpret_cast<uintptr_t>(m);
  if (IMAGE_NT_HEADERS* nt = mem::nt_headers(m)) {
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (dir.VirtualAddress && dir.Size) {
      const auto* ed = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + dir.VirtualAddress);
      const auto* funcs = reinterpret_cast<const DWORD*>(base + ed->AddressOfFunctions);
      const auto* names = reinterpret_cast<const DWORD*>(base + ed->AddressOfNames);
      const auto* ords = reinterpret_cast<const WORD*>(base + ed->AddressOfNameOrdinals);
      for (DWORD i = 0; i < ed->NumberOfNames; ++i)
        if (ords[i] < ed->NumberOfFunctions)
          e.by_rva.emplace_back(funcs[ords[i]], reinterpret_cast<const char*>(base + names[i]));
    }
  }
  return &e;
}

// "USER32.dll!GetAsyncKeyState" (true), "module+0xRVA" or "?" (false).
bool export_name(uintptr_t v, char* out, size_t n) {
  std::snprintf(out, n, "?");
  HMODULE m = nullptr;
  if (!v || !GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                reinterpret_cast<LPCSTR>(v), &m) ||
      !m)
    return false;
  const ModuleExports* e = exports_of(m);
  if (!e) return false;
  const auto rva = static_cast<uint32_t>(v - reinterpret_cast<uintptr_t>(m));
  for (const auto& [r, name] : e->by_rva)
    if (r == rva) {
      std::snprintf(out, n, "%s!%s", e->name, name);
      return true;
    }
  std::snprintf(out, n, "%s+0x%X", e->name, rva);
  return false;
}

// Every pointer in the rebuilt import table, resolved to a name where it can be:
// counted in the log, listed one per line in `map` when one is open.
void survey_imports(FILE* map) {
  const mem::Range& r = image::iat();
  int total = 0, named = 0;
  for (uintptr_t a = r.begin; a + 4 <= r.end; a += 4) {
    uint32_t v = 0;
    if (!mem::read_safe(a, &v) || !v || image::game_image().contains(v)) continue;  // RVAs of names, not pointers
    ++total;
    char name[160];
    if (export_name(v, name, sizeof(name))) ++named;
    if (map) std::fprintf(map, "%08X %08X %s\n", static_cast<unsigned>(a - image::base()), v, name);
  }
  logf("imports: the table (0x%X bytes) holds %d pointer(s) outside the image: %d resolve to DLL exports, %d do not",
       static_cast<unsigned>(r.size()), total, named, total - named);
  // The three this mod cares about, by the address a direct resolution leaves.
  struct Want {
    const char* dll;
    const char* fn;
  } const wants[] = {{"user32.dll", "PeekMessageA"},
                     {"user32.dll", "GetAsyncKeyState"},
                     {"kernel32.dll", "SetUnhandledExceptionFilter"}};
  for (const Want& w : wants) {
    HMODULE m = GetModuleHandleA(w.dll);
    const auto real = m ? reinterpret_cast<uint32_t>(GetProcAddress(m, w.fn)) : 0u;
    const auto hits = real ? mem::find_dwords(r, real, 4) : std::vector<uintptr_t>{};
    if (hits.empty())
      logf("imports:   %s!%s (%p) is not in the table", w.dll, w.fn, reinterpret_cast<void*>(real));
    else
      logf("imports:   %s!%s (%p) is in the table at exe+0x%X%s", w.dll, w.fn, reinterpret_cast<void*>(real),
           static_cast<unsigned>(hits[0] - image::base()), hits.size() > 1 ? " (and more)" : "");
  }
}

// The game's save files, in the folder bio3.ini's Save= names (".\saves"): the
// two emulated PlayStation memory cards, 128 KB images the game reads once as
// it starts and writes back whole after every save and as it closes (0x4142D0
// reads one, 0x4143D0 writes it; savefiles.cpp manages the saves on the first),
// and the Mercenaries' own profile. Only their sizes and dates are logged here.
void survey_saves() {
  char cwd[MAX_PATH] = "?";
  GetCurrentDirectoryA(MAX_PATH, cwd);
  char exe[MAX_PATH] = "?";
  GetModuleFileNameA(nullptr, exe, MAX_PATH);
  logf("recon: exe %s, working directory %s", exe, cwd);
  const struct {
    const char* file;
    const char* what;
  } saves[] = {{"bu00.sav", "memory card 1"}, {"bu10.sav", "memory card 2"}, {"data.sav", "the Mercenaries' profile"}};
  for (const auto& s : saves) {
    char path[MAX_PATH];
    std::snprintf(path, sizeof(path), "%s\\saves\\%s", cwd, s.file);
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &fa)) {
      logf("recon: no %s at %s yet", s.what, path);
      continue;
    }
    SYSTEMTIME st{};
    FileTimeToSystemTime(&fa.ftLastWriteTime, &st);
    logf("recon: %s %s: %lu bytes, written %04u-%02u-%02u %02u:%02u:%02u UTC", s.what, path, fa.nFileSizeLow,
         st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
  }
}

// Watch: log the configured values whenever they change, at most ~60 lines a
// second so a counter left in the list cannot flood the log.
uint32_t g_watch_last[config::kMaxWatch];
bool g_watch_seen[config::kMaxWatch];
DWORD g_watch_second = 0;
int g_watch_lines = 0;

}  // namespace

bool unpacked(mem::TextStats* out) {
  image::init();
  const mem::TextStats st = mem::text_stats(image::text());
  if (out) *out = st;
  return mem::looks_like_code(st);
}

void note_iat_slot(uintptr_t slot, uintptr_t original) {
  if (!slot || g_iat_n >= 16) return;
  g_iat[g_iat_n++] = IatNote{slot, original};
}

int hook_import(const char* what, void* real, void* hook) {
  image::init();
  if (!real || !hook || image::iat().empty()) return 0;
  const auto from = reinterpret_cast<uint32_t>(real), to = reinterpret_cast<uint32_t>(hook);
  uintptr_t first = 0;
  const int n = mem::replace_dwords(image::iat(), from, to, &first);
  if (n) {
    note_iat_slot(first, from);
    logf("imports: the game's %s slot (exe+0x%X) now points at our hook%s", what,
         static_cast<unsigned>(first - image::base()), n > 1 ? " (more than one slot)" : "");
  } else if (!mem::find_dwords(image::iat(), to, 1).empty()) {
    logf("imports: the game's %s slot already holds our hook (handed over through GetProcAddress)", what);
  } else {
    logf("imports: %s is not in the game's import table as %p - Enigma resolved it some other way", what, real);
  }
  return n;
}

void recon(const char* dir) {
  image::init();
  image::log_sections();
  image::find_iat();
  survey_saves();
  FILE* map = nullptr;
  char path[MAX_PATH];
  if (config::get().dump_image) {
    std::snprintf(path, sizeof(path), "%sResidentEvil3.imports.txt", dir);
    map = std::fopen(path, "w");
    if (map) std::fprintf(map, "# RVA of the slot, its value, the export it resolves to\n");
  }
  survey_imports(map);
  if (map) {
    std::fclose(map);
    logf("imports: map written to %s", path);
  }
  if (config::get().dump_image) dump_image(dir);
}

// The unpacked 1997 image - its six sections only, not Enigma's - written with
// raw offsets == RVAs, so the file maps 1:1 to the loaded image. The entry point
// and the data directories still describe Enigma's wrapper; tools/fix_dump.py
// points the entry at the game's own and clears the rest.
bool dump_image(const char* dir) {
  image::init();
  IMAGE_NT_HEADERS* nt = mem::nt_headers(image::module());
  if (!nt || !image::layout_ok()) {
    logf("ERROR: dump: the image layout is not the expected one - not dumping");
    return false;
  }
  const size_t size = image::game_image().size();
  std::vector<uint8_t> buf(size, 0);
  SIZE_T got = 0;
  ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(image::base()), buf.data(),
                    nt->OptionalHeader.SizeOfHeaders, &got);
  const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
  for (unsigned i = 0; i < 6; ++i) {
    const uintptr_t va = image::base() + sec[i].VirtualAddress;
    const size_t n = sec[i].Misc.VirtualSize;
    for (size_t off = 0; off < n && sec[i].VirtualAddress + off < size; off += 0x1000) {
      size_t chunk = (n - off) < 0x1000 ? (n - off) : 0x1000;
      if (sec[i].VirtualAddress + off + chunk > size) chunk = size - (sec[i].VirtualAddress + off);
      ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(va + off),
                        buf.data() + sec[i].VirtualAddress + off, chunk, &got);
    }
  }
  // Put our own import-slot patches back so the dump is the game's image, not ours.
  for (int i = 0; i < g_iat_n; ++i) {
    const uintptr_t off = g_iat[i].slot - image::base();
    if (off + 4 <= size) std::memcpy(buf.data() + off, &g_iat[i].original, 4);
  }
  auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(buf.data());
  auto* nt2 = reinterpret_cast<IMAGE_NT_HEADERS*>(buf.data() + dos->e_lfanew);
  nt2->FileHeader.NumberOfSections = 6;
  nt2->OptionalHeader.SizeOfImage = static_cast<DWORD>(size);
  nt2->OptionalHeader.FileAlignment = 0x1000;
  auto* s2 = IMAGE_FIRST_SECTION(nt2);
  for (unsigned i = 0; i < 6; ++i) {
    s2[i].PointerToRawData = s2[i].VirtualAddress;
    s2[i].SizeOfRawData = (s2[i].Misc.VirtualSize + 0xFFF) & ~0xFFFu;
  }
  char path[MAX_PATH];
  std::snprintf(path, sizeof(path), "%sResidentEvil3.dumped.exe", dir);
  FILE* f = std::fopen(path, "wb");
  if (!f) {
    logf("ERROR: dump: cannot write %s", path);
    return false;
  }
  std::fwrite(buf.data(), 1, size, f);
  std::fclose(f);
  logf("dump: wrote %s (%u bytes; entry point and data directories are still Enigma's - run tools/fix_dump.py)", path,
       static_cast<unsigned>(size));
  return true;
}


namespace {

// --- what discovery found ------------------------------------------------------------
// Every one of these is read out of an instruction operand and cross-checked;
// CLAUDE.md lists the values build 21774525 gives.
struct Anchors {
  // The cooperative task scheduler.
  uintptr_t task_state_tab = 0;  // a task's state word (0 = not running)
  uintptr_t task_entry_tab = 0;  // its entry point, four bytes on
  uint32_t task_stride = 0;      // 0x80 per task
  uintptr_t task_current = 0;    // the running task's block
  uintptr_t status_entry = 0;    // the status screen's task entry (task 1)
  uintptr_t status_state = 0;    // the screen's own state struct
  // State words.
  uintptr_t flags = 0;       // the game state flags (0x20000 = the player died)
  uintptr_t save_flags = 0;  // the save block's flags dword
  // The player.
  uintptr_t hp = 0, max_hp = 0, condition = 0;
  // The entity pointer array: [0] the player, [1] its partner, [2..] the room's.
  uintptr_t entities = 0, entities_end = 0;
  // Counters, all inside the save block but the play time's base.
  uintptr_t timer = 0, tick_base = 0, save_count = 0, stage = 0, room = 0;
  // The vblank count the play time is measured in: a field of the system object
  // this pointer names, read only once the object's vtable is seen to send
  // VSync to the method discovery found answering it.
  uintptr_t vsync_obj = 0, vsync_method = 0;
  uint32_t vsync_slot = 0, vblank_off = 0;
  // The inventory.
  uintptr_t inv_ptr = 0, inv_base = 0, inv_index = 0;
  uintptr_t inv_select = 0;  // the function that picks whose block is in play
  uint32_t inv_stride = 0;
  int inv_slots_off = 0;  // the usable-slot count, inside the block
  uintptr_t find_slot = 0;  // find_item_slot(id, from)
  bool icons_known = false;  // the status screen's "rebuild the icons" bit, cross-checked
  int box_off = 0;          // the item box, inside the block
  uintptr_t item_table = 0;
  int item_table_last = 0;
  // Input.
  uintptr_t in_held = 0, in_pressed = 0, in_repeat = 0;
  // Cutscenes.
  bool cinema_known = false;   // the save flags' 0x8 is the one the letterbox reads
  bool movie_known = false;    // the state flags' 0x10000 is the one each frame plays a movie on
  uintptr_t movie_end = 0;     // the movie player's "it has run out" byte...
  uintptr_t movie_chain = 0;   // ...and the one that carries a skip on to the next part
  uintptr_t msg_flags = 0;     // the message box's flags (0x80 while one is up)
  uintptr_t msg_state = 0;     // ...and its state machine's state
  uintptr_t voice_state = 0;   // the voice player's state (0 idle, 1 starting, 2 playing, 3 stopping)
  // The countdown (the Mercenaries' and the main game's timed scenes): the seconds left.
  uintptr_t countdown = 0;
  // The missile at the end: the radar's struct (its state byte first, its own
  // state `radar_sub` bytes in), the distance inside it and the step a frame
  // takes off, and the states that count: the struct's for running, the
  // radar's own for far out and close in.
  uintptr_t radar = 0, missile = 0;
  int radar_sub = 0, radar_run = -1, radar_far = -1, radar_near = -1, missile_step = 0;
  // The Mercenaries money: the run's takings, the bank and the bank's cap.
  uintptr_t takings = 0, bank = 0;
  uint32_t bank_max = 0;
  // The recovery total the healing adds to, and the steps the ending's rank
  // scores it in.
  uintptr_t recovery = 0;
  RankStep rank_steps[kRankSteps];
  // The clear count the ending adds to and the EPILOGUE screen unlocks up to,
  // and how many epilogues there are.
  uintptr_t clears = 0;
  int epilogues = 0;
  uint32_t ending_easy = 0;  // the state flag the ending shows no epilogue under (Easy)
  // Jill's outfit: the value the model loader reads; the character the room
  // loads put in play, and the one they keep for Jill while another is played;
  // the loaded model's type byte; the first digit of the loader's file name,
  // which it writes afresh every time it runs; the first character not Jill.
  uintptr_t outfit = 0, character = 0, kept_character = 0, model_type = 0, model_mark = 0;
  int first_other = 0;
};
Anchors g;

// The reloading tool's proficiency: the offset of the first kind's count in the
// inventory block, and the game's own tables, copied out at discovery so the
// panel can read them from its own thread.
constexpr int kMaxMixRecipes = 8;
struct Mixing {
  bool known = false;
  int count_off = 0;                        // u16 per kind from here, inside the inventory block
  uint8_t threshold[kMixKinds][kMixLevels]; // level i holds while the count is at most threshold[i]
  uint8_t bonus[kMixKinds][kMixLevels];     // ...and adds this many tenths to a batch
  int enhanced_bonus[8];                    // the bonus enhanced rounds take instead (-1 unknown)
  int enhanced_kinds = 0;                   // kinds 0..n-1 offer enhanced rounds...
  int enhanced_after = 0;                   // ...once more than this many batches were made
  uint32_t easy_mask = 0;                   // the state flag that doubles every batch
  MixRecipe recipe[kMixKinds][kMaxMixRecipes];
  int recipes[kMixKinds];
};
Mixing g_mix;
const char* const kMixNames[kMixKinds] = {"Handgun bullets", "Shotgun shells", "Magnum bullets", "Grenade rounds"};

// The memory cards and the memory card screen (discover_cards). The card
// objects are read through the container's global each time, and checked
// against the vtables discovery found before anything is touched; the layout
// and the location names are copied out, so any thread can read them.
constexpr int kMaxLocations = 32;  // the game's table has 30 (14 rooms, blanks, "Next Game", "???")
struct Cards {
  bool known = false;
  uintptr_t screen_entry = 0;   // the screen's task entry
  uintptr_t screen_ptr = 0;     // the pointer its loop reads its state struct through...
  uintptr_t screen = 0;         // ...and the struct its init points that at
  int state_rescan = -1;        // the state that reads the card...
  int state_list = -1;          // ...and the one that waits on the list
  uintptr_t container_ptr = 0;  // the global the card container is reached through
  uint32_t container_vt = 0, card_vt = 0;
  int cards = 0, first_card = 0, card_stride = 0;
  int image_off = 0, name_off = 0, path_off = 0;
  char ext[8] = {};             // ".sav", over the name's last character
  CardLayout layout;
  char locations[kMaxLocations][24] = {};
};
Cards g_cards;

volatile LONG g_ready = 0;
char g_status[160] = "waiting for the game to unpack";
mem::Patch g_sites[kSiteCount];
bool g_site_ok[kSiteCount] = {};
const char* const kSiteNames[kSiteCount] = {"save count +1",         "the save's ink ribbon",
                                            "the typewriter's ribbon check", "button state stores",
                                            "button repeat store",   "the countdown's second",
                                            "the countdown's frame count", "the missile's step",
                                            "the player's damage",   "the recovery total's store",
                                            "the save's play-time add", "the ending's play-time add",
                                            "the room load's model test"};

constexpr int kStatusTask = 1;
constexpr uint32_t kDeadBit = 0x20000;
// Inside the 0x140-byte inventory block.
constexpr int kSlotStride = 4, kSlotId = 0, kSlotCount = 1, kSlotFlags = 2;
constexpr int kEquippedOff = 0x128, kWeaponOff = 0x129, kSlotsOff = 0x12A, kBoxOff = 0x28;
// Inside the status screen's state struct: set while it has put the weapon away
// for an item, with the equipped slot and weapon it will give back after it.
constexpr int kPutAwayOff = 0x2E, kSavedSlotOff = 0x2F;
// ...and its flags word, whose 0x40000000 asks the next opening to rebuild the
// item icons and the equipped marker.
constexpr int kScreenFlagsOff = 0x14;
constexpr uint32_t kIconsStale = 0x40000000;
constexpr int kEntityHp = 0xCC, kEntityType = 0x4A;
constexpr int kStatusPage = 4;  // the status screen's own state: which page it is on

// The signatures. Each is unique in the code section; every global they yield is
// checked against the other sites that name it.
//
// Task_chain(id, entry): the entry goes into the task block, whose state word
// (four bytes in front) becomes 2. The shift gives the 0x80 stride.
const char* const kSigTaskChain =
    "8B 44 24 04 8B 4C 24 08 C1 E0 07 89 88 ?? ?? ?? ?? 66 C7 80 ?? ?? ?? ?? 02 00 C3";
// Task_state(id): the same two tables, read as a word.
const char* const kSigTaskState = "8B 44 24 04 33 C9 C1 E0 07 66 8B 88 ?? ?? ?? ?? 8B C1 C3";
// Task_sleep(frames): it writes through the running task's block.
const char* const kSigTaskSleep =
    "A1 ?? ?? ?? ?? 66 8B 4C 24 04 68 00 00 00 FF 66 89 48 02 66 C7 00 01 00 E8";
// The gameplay task launching the status screen on task 1 when bit 0x200 of the
// save block's flags is set.
const char* const kSigStatusChain =
    "8B 0D ?? ?? ?? ?? A1 ?? ?? ?? ?? BB 00 00 00 01 83 C4 04 0B CB F6 C4 02 C6 05 ?? ?? ?? ?? 00 "
    "89 3D ?? ?? ?? ?? 89 0D ?? ?? ?? ?? 74 33 68 ?? ?? ?? ?? 6A 01 E8";
// The status screen's task entry: it calls the screen with its own state struct.
const char* const kSigStatusEntry = "56 68 ?? ?? ?? ?? 33 F6 E8";
// The player's damage: HP down by the hit, held at 0 with the dying bit when it
// would go under. The dying bit is set on the condition word's high byte. Below
// that (not in the signature) a hit that takes HP more than 10 under 0 sets the
// death flag there and then, in the frame it lands - so the `sub` is what God
// mode takes out: a hold that tops HP back up between frames comes too late for
// a hit bigger than the player's whole health, such as a barrel going off at
// point blank. Every hit on the player comes through here (the blast walk at
// 0x454EF0 included), and the player's death state is only ever set on its
// "dead" answer or on HP below 0.
const char* const kSigDamage =
    "66 8B 0D ?? ?? ?? ?? 66 81 25 ?? ?? ?? ?? FF E0 66 2B C8 8B 44 24 08 83 F8 01 66 89 0D ?? ?? ?? ?? "
    "75 16 66 85 C9 7D 69 66 C7 05 ?? ?? ?? ?? 00 00 80 0D ?? ?? ?? ?? 08 C3";
// A new game: HP and its maximum both set to 200.
const char* const kSigNewGameHp = "B8 C8 00 00 00 66 A3 ?? ?? ?? ?? 66 A3 ?? ?? ?? ?? 74 63";
// The play time, brought up to date as a save starts (0x44F0F0, before the
// block is filled and written to the card): unless the state flags have the
// Mercenaries' 0x80, the vblanks since its base are added on and the base moved
// to now - or, under 0x4000, the total is cleared. The `mov` that stores the
// total is what Freeze play time takes out.
const char* const kSigTimer =
    "F6 05 ?? ?? ?? ?? 80 75 3D A1 ?? ?? ?? ?? F6 C4 40 74 0C C7 05 ?? ?? ?? ?? 00 00 00 00 EB 27 6A FF "
    "E8 ?? ?? ?? ?? 8B 15 ?? ?? ?? ?? 8B C8 2B CA 8B 15 ?? ?? ?? ?? 83 C4 04 03 D1 89 15 ?? ?? ?? ?? A3 ?? ?? ?? ??";
// ...the same as the ending starts (0x442CD0), though the base is left: the
// ending's results and rank read the total next.
const char* const kSigTimerEnd =
    "6A FF E8 ?? ?? ?? ?? 8B 0D ?? ?? ?? ?? 8B 15 ?? ?? ?? ?? 2B C1 6A 05 03 D0 89 15 ?? ?? ?? ??";
// VSync(-1), which both call for the vblank count: a call through the system
// object's vtable...
const char* const kSigVSync = "8B 0D ?? ?? ?? ?? 8B 54 24 04 52 8B 01 FF 50 78 C3";
// ...to a method that answers a negative argument with a field of the object...
const char* const kSigVSyncCount =
    "8B 44 24 04 83 EC 08 85 C0 56 8B F1 7D 15 8B 4E 04 8B 01 FF 50 10 8B 86 ?? ?? ?? ?? 5E 83 C4 08 C2 04 00";
// ...which the logic thread's pacing loop raises by one each time the clock
// passes the next vblank due.
const char* const kSigVBlank = "8B 86 ?? ?? ?? ?? C7 44 24 0C 00 00 00 00 40 89 86 ?? ?? ?? ?? 40";
// The end of a save: one ink ribbon taken, then the save count raised (capped at 99).
const char* const kSigSave =
    "A1 ?? ?? ?? ?? F6 C4 40 75 20 A8 80 75 1C 6A 00 6A 01 6A 00 68 ?? 00 00 00 E8 ?? ?? ?? ?? 83 C4 08 50 "
    "E8 ?? ?? ?? ?? 83 C4 0C 8B 1D ?? ?? ?? ?? A0 ?? ?? ?? ?? 81 E3 FF 00 00 00 3C 63 72 0A A1 ?? ?? ?? ?? "
    "F6 C4 40 74 0F F6 05 ?? ?? ?? ?? 80 75 06 FE 05 ?? ?? ?? ??";
// Choosing whose inventory is in play: character 8 has one of his own, everyone
// else shares the first. The block is 5 * 0x40 bytes.
const char* const kSigInvSelect =
    "8B 44 24 04 83 F8 07 76 11 83 F8 08 75 15 B8 01 00 00 00 A3 ?? ?? ?? ?? EB 0E 33 C0 A3 ?? ?? ?? ?? "
    "EB 05 A1 ?? ?? ?? ?? 8D 04 80 C1 E0 06 05 ?? ?? ?? ?? A3 ?? ?? ?? ??";
// find_item_slot(id, from): the slots are four bytes apart and the usable count
// sits at +0x12A of the block.
const char* const kSigFindSlot =
    "8A 44 24 08 56 8B 35 ?? ?? ?? ?? 57 88 44 24 10 8A 8E ?? ?? 00 00 3A C1 73 1D 8A 54 24 0C 8B 7C 24 10 "
    "81 E7 FF 00 00 00 38 14 BE 74 0C FE C0 3A C1";
// The typewriter's script command: find_item_slot(ink ribbon), and without one
// the message that offers no save (the jl).
const char* const kSigTypewriter =
    "53 68 ?? 00 00 00 E8 ?? ?? ?? ?? 83 C4 08 84 C0 7C 1A 68 00 00 00 FF 6A 01 53 53 E8";
// The end of remove_item_slot: the equipped slot (or the status screen's saved
// copy of it, while it holds the weapon back) moves up with the slots, and the
// status screen's state is told to rebuild its icons.
const char* const kSigRemoveSlot =
    "8A 0D ?? ?? ?? ?? 5E 3A C8 5B 74 24 A0 ?? ?? ?? ?? 3A C2 72 37 3C FF 74 33 FE C8 A2 ?? ?? ?? ?? "
    "A1 ?? ?? ?? ?? 0D 00 00 00 40 A3 ?? ?? ?? ??";
// The item box screen drawing its list: the cursor less three, wrapped into the
// box's 64 slots, read at +0x28 of the inventory block.
const char* const kSigItemBox =
    "2C 03 88 44 24 24 79 06 04 40 88 44 24 24 33 C9 BB ?? ?? ?? ?? 89 4C 24 18 89 5C 24 1C EB 08 8A 44 24 24 "
    "8B 4C 24 18 0F BE D0 A1 ?? ?? ?? ?? 8D 7C 90 28 8A 44 90 28 84 C0";
// A pickup topping up a slot: the item table's second column is how much of the
// item one slot holds.
const char* const kSigItemTable =
    "8B 35 ?? ?? ?? ?? 33 C9 33 D2 8A 0C 86 8A 14 8D ?? ?? ?? ?? 33 C9 8A 4C 86 01 03 CD 3B D1 73 03 40";
// The walk over the room's entities: from index 2 up to the list's live end.
const char* const kSigEntityWalk =
    "8B 35 ?? ?? ?? ?? 81 FE ?? ?? ?? ?? C6 44 24 14 02 74 50 B8 ?? ?? ?? ?? 8B 00 8A 88 2F 01 00 00 3A CB "
    "74 20 83 38 00 74 1B";
const char* const kSigEntityBase = "8D 04 8D ?? ?? ?? ?? 3B C6 75 BB";
// The input poll publishing the frame's buttons: held, pressed and repeat.
const char* const kSigInputMain = "89 2D ?? ?? ?? ?? 89 15 ?? ?? ?? ?? 89 3D ?? ?? ?? ?? 74 47";
const char* const kSigInputRepeat = "89 3D ?? ?? ?? ?? 5F 5E 5D 5B 59 C3";
// The letterbox the game draws over its scripted scenes: a ramp up to full
// while bit 0x8 of the save block's flags is set, and back down after.
const char* const kSigCinema = "A0 ?? ?? ?? ?? 56 8B 35 ?? ?? ?? ?? A8 08 A0 ?? ?? ?? ?? 74 0C 3C EF 77 04 04 10";
// Each frame runs the movie player's own frame while bit 0x10000 of the state
// flags is set.
const char* const kSigMovieFrame = "F7 05 ?? ?? ?? ?? 00 00 01 00 74 05 E8";
// The movie player's frame deciding the movie is over: it ran out (the end
// byte 1, nothing carried on), or the skip button was pressed (2 and 2).
const char* const kSigMovieEnd =
    "66 3D 01 00 75 0E A2 ?? ?? ?? ?? C6 05 ?? ?? ?? ?? 00 EB 59 B3 02 EB 37 A0 ?? ?? ?? ?? B3 02 84 C3 75 0A "
    "66 83 3D ?? ?? ?? ?? 3C 76 40";
// The message box's frame: run while its flags have 0x80 (the box object is
// passed in, and the flags are its +0x75C8)...
const char* const kSigMessageCall =
    "A0 ?? ?? ?? ?? A8 40 74 10 F6 05 ?? ?? ?? ?? 80 75 07 24 BF A2 ?? ?? ?? ?? A8 80 74 17 68 ?? ?? ?? ?? E8 "
    "?? ?? ?? ?? 68 ?? ?? ?? ??";
// ...whose state machine switches on its state at +0x75CA: 2 and 5 wait for a
// press, 4 for a choice; the rest print the text or count down on their own.
const char* const kSigMessage =
    "8A 15 ?? ?? ?? ?? 33 C0 80 FA 01 55 56 57 8B 7C 24 10 0F 94 C0 8B B7 D4 75 00 00 8B E8 33 C0 8A 87 CA 75 00 00";
// The voice player starting a line: the file under "/data_a/voice/", the
// volume, and its state set to 1 - after which it runs as 2 until the line has
// played, and 3 stops it (and clears bit 0x20 of the save flags, which it set
// for as long as the line plays).
const char* const kSigVoicePlay = "68 ?? ?? ?? ?? C7 05 ?? ?? ?? ?? 78 00 00 00 C6 05 ?? ?? ?? ?? 01";
// The game cutting a line short: whatever the state, 3.
const char* const kSigVoiceStop = "A0 ?? ?? ?? ?? 84 C0 74 07 C6 05 ?? ?? ?? ?? 03 8B 0D";
// The gameplay loop's countdown, while bit 0x4000000 of the save flags is set
// (bit 0x2000000 runs the radar receiver's display in its place): the
// Mercenaries' bonus read-out (state flags 0x80), then a second off the clock -
// reached through the [esi+] block the loop keeps its state in - whenever the
// frame count has gone past 29 or the clock was not running last frame, which
// also starts the frame count again.
const char* const kSigCountdownTick =
    "A1 ?? ?? ?? ?? A9 00 00 00 02 74 1D F7 05 ?? ?? ?? ?? 00 00 00 02 75 07 C6 05 ?? ?? ?? ?? 00 E8 ?? ?? ?? ?? "
    "E9 ?? ?? ?? ?? A9 00 00 00 04 0F 84 ?? ?? ?? ?? F6 05 ?? ?? ?? ?? 80 74 05 E8 ?? ?? ?? ?? 66 83 3D ?? ?? ?? ?? 1D "
    "7F 0C F7 05 ?? ?? ?? ?? 00 00 00 04 75 27 66 FF 8E ?? ?? ?? ?? A0 ?? ?? ?? ?? A8 80 66 89 3D ?? ?? ?? ?? "
    "66 C7 05 ?? ?? ?? ?? 08 00 74 07 66 FF 05 ?? ?? ?? ??";
// ...and, once the clock has been drawn, the frame count moving on unless a room
// change or the status screen is under way - with the hundredths the clock
// shows going round beside it - and this frame's save flags kept for the next
// one's test.
const char* const kSigCountdownFrames =
    "84 C0 75 2F A0 ?? ?? ?? ?? 84 C0 75 26 66 A1 ?? ?? ?? ?? 66 FF 05 ?? ?? ?? ?? 66 05 FD FF 66 3D 01 00 "
    "66 A3 ?? ?? ?? ?? 7D 09 66 C7 05 ?? ?? ?? ?? 08 00 8B 86 ?? ?? ?? ?? A3 ?? ?? ?? ??";
// The Mercenaries' bonus for a kill: seconds onto the clock (and onto the
// bonus it shows), money onto the takings.
const char* const kSigCountdownBonus =
    "8B 44 24 08 66 01 05 ?? ?? ?? ?? 66 01 05 ?? ?? ?? ?? 66 8B 44 24 04 66 01 05 ?? ?? ?? ?? A0 ?? ?? ?? ?? "
    "84 C0 75 07 C6 05 ?? ?? ?? ?? 5A C3";
// The radar the gameplay loop calls in the countdown's place (the countdown
// tick's `call`, after it has zeroed the radar's state byte the frame its bit
// comes on): the handler that state byte picks, handed the radar's struct...
const char* const kSigRadar = "A1 ?? ?? ?? ?? 68 ?? ?? ?? ?? 25 FF 00 00 00 FF 14 85 ?? ?? ?? ?? 59 C3";
// ...and the handler that runs it once it is set up, through states of its own.
const char* const kSigRadarRun =
    "53 56 8B 74 24 0C 57 8A 5E ?? 8B C3 25 FF 00 00 00 83 F8 ?? 0F 87 ?? ?? ?? ?? FF 24 85 ?? ?? ?? ??";
// Two of those states count the missile in, while the countdown's bit is set:
// the one while it is far out, which runs the step and moves on to the close
// view once the distance is under a tenth of its start...
const char* const kSigMissileFar =
    "F7 05 ?? ?? ?? ?? 00 00 00 04 75 09 C6 46 04 00 E9 ?? ?? ?? ?? 56 C6 46 04 03 E8 ?? ?? ?? ?? 8B 7E ?? 83 C4 04 "
    "81 FF ?? ?? ?? ?? 0F 8D";
// ...and the close one, which runs the step and, once the distance is out and
// no room change or other screen is under way, cuts the voice, clears the
// radar's bit and the countdown's and sets the state flags' 0x200: the end the
// countdown running out has too.
const char* const kSigMissileNear =
    "F7 05 ?? ?? ?? ?? 00 00 00 04 75 09 C6 46 04 00 E9 ?? ?? ?? ?? B3 03 56 88 5E 04 E8 ?? ?? ?? ?? 8B 46 ?? 33 FF "
    "83 C4 04 3B C7 7F 7F 89 7E ?? A0 ?? ?? ?? ?? 84 C0 75 73 A0 ?? ?? ?? ?? 84 C0 75 6A 57 E8 ?? ?? ?? ?? 83 C4 04 "
    "85 C0 75 5D 6A 01 E8 ?? ?? ?? ?? 83 C4 04 85 C0 75 4F A0 ?? ?? ?? ?? 84 C0 74 06 88 1D ?? ?? ?? ?? "
    "8B 0D ?? ?? ?? ?? A1 ?? ?? ?? ?? 81 E1 FF FF FF F9 80 CC 02 89 0D ?? ?? ?? ?? A3 ?? ?? ?? ??";
// The step both run: unless the state flags hold the game still, the radar's
// sweep turns and the distance comes down by a fixed amount, every frame.
const char* const kSigMissileStep =
    "A1 ?? ?? ?? ?? F6 C4 04 75 4C F7 05 ?? ?? ?? ?? 00 00 00 10 74 19 A0 ?? ?? ?? ?? C6 05 ?? ?? ?? ?? 02 84 C0 74 09 "
    "A0 ?? ?? ?? ?? 84 C0 75 27 8B 44 24 04 8B 48 ?? 66 83 40 ?? C0 81 C1 ?? ?? ?? ?? 89 48 ??";
// The Mercenaries results paying a run into the bank: half the seconds left, a
// bonus and the takings summed into a 16-bit count (which a skip of the count-up
// reads signed), then counted into the bank three a frame, the bank stopping at
// its maximum.
const char* const kSigMoneyTally =
    "0F BF 05 ?? ?? ?? ?? 99 2B C2 D1 F8 66 03 46 18 66 03 05 ?? ?? ?? ?? 66 89 46 12 75 04 C6 46 01 07 "
    "66 C7 46 06 00 00 5F 5E C3 A1 ?? ?? ?? ?? BF ?? ?? ?? ?? 40 3B C7 A3 ?? ?? ?? ?? 76 11 B9 01 00 00 00 "
    "89 3D ?? ?? ?? ??";
// The other results screen: half the takings, into the same bank, with the same cap.
const char* const kSigMoneyHalf =
    "66 A1 ?? ?? ?? ?? 66 D1 E8 66 89 46 12 75 04 C6 46 01 07 66 C7 46 06 00 00 5F 5E C3 A1 ?? ?? ?? ?? "
    "BF ?? ?? ?? ?? 40 BA 01 00 00 00 3B C7 A3 ?? ?? ?? ?? 76 0A 89 3D ?? ?? ?? ??";
// The status screen's combine: an entry of the combination table found for the
// two items, then sent by its type - one type to the reloading tool's batch,
// another to powder mixed straight into grenade rounds.
const char* const kSigMixDispatch = "8A 01 89 4C 24 24 83 E8 ?? 0F 84 ?? ?? ?? ?? 83 E8 ?? 0F 84 ?? ?? ?? ??";
// The reloading tool's batch: the kind of ammo it makes picks a count of batches
// made in the inventory block and a row of the level table, the first level
// whose threshold the count does not pass gives the bonus, and enhanced rounds
// (the two ids compared) take a lower bonus out of a switch on it.
const char* const kSigMixYield =
    "8A 56 6A 8A 41 04 88 54 24 18 88 46 65 8B 6C 24 18 A1 ?? ?? ?? ?? 81 E5 FF 00 00 00 C6 44 24 10 00 "
    "66 8B BC 68 ?? ?? 00 00 8D 54 AD 00 8B 44 24 10 8B D8 81 E3 FF 00 00 00 03 DA 66 0F B6 1C 9D ?? ?? ?? ?? "
    "66 3B DF 73 0E FE C0 3C ?? 88 44 24 10 72 DA 8B 44 24 10 25 FF 00 00 00 03 C2 8A 14 85 ?? ?? ?? ?? "
    "8A 41 03 3C ?? 88 54 24 10 72 2C 3C ?? 77 28 8B 44 24 10 25 FF 00 00 00 83 F8 07 77 1A FF 24 85 ?? ?? ?? ?? "
    "C6 44 24 10 ?? EB 0C C6 44 24 10 ?? EB 05 C6 44 24 10 ??";
// ...the batch doubled when a state flag (Easy) is set, and the count up one.
const char* const kSigMixCount =
    "6A ?? 68 ?? ?? ?? ?? E8 ?? ?? ?? ?? 83 C4 08 85 C0 74 08 8A 56 65 D0 E2 88 56 65 A1 ?? ?? ?? ?? 8B 54 24 24 "
    "66 FF 84 68 ?? ?? 00 00";
// Before the batch: the kind of the ammo it makes, out of a table of {item id,
// kind} pairs, and for the first kinds, once the count has passed a threshold,
// the question whether to make the enhanced rounds instead.
const char* const kSigMixOffer =
    "8A 0D ?? ?? ?? ?? 33 FF 80 F9 FF 74 1D 33 D2 8A 50 03 B8 ?? ?? ?? ?? 0F BE C9 3B CA 74 0C 8A 48 02 83 C0 02 47 "
    "80 F9 FF 75 ED 8A 04 7D ?? ?? ?? ?? 3C ?? 88 46 6A 88 44 24 10 0F 87 ?? ?? ?? ?? 8B 54 24 10 A1 ?? ?? ?? ?? "
    "81 E2 FF 00 00 00 66 83 BC 50 ?? ?? 00 00 ?? 0F 86";
// Powder mixed into grenade rounds: the grenade kind's count and level row named
// directly, the bonus, the doubling on Easy, and the count up one.
const char* const kSigGrenadeMix =
    "8A 51 04 88 56 65 A1 ?? ?? ?? ?? 32 D2 66 8B B8 ?? ?? 00 00 88 54 24 10 8B 44 24 10 8B D8 81 E3 FF 00 00 00 "
    "66 0F B6 1C 9D ?? ?? ?? ?? 66 3B DF 73 0F FE C2 80 FA ?? 88 54 24 10 72 DB 8B 44 24 10 8A 49 04 25 FF 00 00 00 "
    "33 D2 6A ?? 8A 14 85 ?? ?? ?? ?? 8B C1 25 FF 00 00 00 68 ?? ?? ?? ?? 0F AF D0 B8 67 66 66 66 F7 EA C1 FA 02 "
    "8B C2 C1 E8 1F 03 D0 02 D1 88 56 65 E8 ?? ?? ?? ?? 83 C4 08 85 C0 74 08 8A 4E 65 D0 E1 88 4E 65 A1 ?? ?? ?? ?? "
    "66 FF 80 ?? ?? 00 00";
// The game's bit test: bit n of a dword array, counted from the top bit.
const char* const kSigBitTest = "8B 54 24 08 B8 00 00 00 80 8B CA 56 83 E1 1F D3 E8 8B 4C 24 08 C1 EA 05 8B 34 91 23 C6 5E C3";
// The combination table's lookup: eight-byte entries {type, item, item, result,
// count}, either item first, to the one whose first item is 0xFF.
const char* const kSigCombine =
    "8A 0D ?? ?? ?? ?? 53 80 F9 FF B8 ?? ?? ?? ?? 74 25 8A 54 24 10 8A 5C 24 0C 3A CB 75 05 38 50 02 74 17 3A CA "
    "75 05 38 58 02 74 0E 8A 48 09 83 C0 08 80 F9 FF 75 E3 83 C8 FF 5B C3";
// Where the yield's switch on the bonus lands: three cases that set a lower one,
// and its end, which keeps it.
constexpr int kMixCase[3] = {137, 144, 151};
constexpr int kMixCaseKeep = 156;
// The status screen's healing, once the item has been used: the poison cured
// if the item cures it, HP up by the item's amount (held to the maximum after),
// and the same amount onto the recovery total - all of it, however little of it
// the player needed.
const char* const kSigHeal =
    "8A 46 36 84 C0 74 09 66 81 25 ?? ?? ?? ?? FF FD 8B 6E 18 66 C7 46 32 E0 FF 66 0F B6 4E 35 "
    "81 E5 FF FF 7F FF 89 6E 18 66 A1 ?? ?? ?? ?? 8B 2D ?? ?? ?? ?? 66 03 C1 33 C9 66 A3 ?? ?? ?? ?? "
    "8A 4E 35 03 E9 66 8B 0D ?? ?? ?? ?? 66 3B C1 89 2D ?? ?? ?? ?? 7E 07 66 89 0D ?? ?? ?? ??";
// The ending's rank scoring the recovery total: ten steps, each a highest total
// and the score it earns (`cmp edx,total / ja / mov eax,score`, so the last that
// holds wins), then that score added to the play time's and the saves' for the
// sum the rank is set from.
const char* const kSigRankRecovery =
    "8B 15 ?? ?? ?? ?? 33 C0 "
    "81 FA ?? ?? ?? ?? 77 05 B8 ?? 00 00 00 81 FA ?? ?? ?? ?? 77 05 B8 ?? 00 00 00 "
    "81 FA ?? ?? ?? ?? 77 05 B8 ?? 00 00 00 81 FA ?? ?? ?? ?? 77 05 B8 ?? 00 00 00 "
    "81 FA ?? ?? ?? ?? 77 05 B8 ?? 00 00 00 81 FA ?? ?? ?? ?? 77 05 B8 ?? 00 00 00 "
    "81 FA ?? ?? ?? ?? 77 05 B8 ?? 00 00 00 81 FA ?? ?? ?? ?? 77 05 B8 ?? 00 00 00 "
    "81 FA ?? ?? ?? ?? 77 05 B8 ?? 00 00 00 81 FA ?? ?? ?? ?? 77 05 B8 ?? 00 00 00 "
    "03 C1 03 F0 8B 44 24 10 83 FE ?? C6 40 04 00";
constexpr int kRankStepBytes = 13;
// The ending (ending.bin, after the results): unless the state flags have
// Easy, and while the clear count is under the number of epilogues, the
// results' epilogue screen (result.bin's screen 5) shows the epilogue the count
// names; then, after a fade, the count goes up one - the call the signature
// ends on. Past it (not in the signature) the profile is written, which records
// the count in saves\data.sav (0x4262A0, key 10, whenever it is not 0).
const char* const kSigEndingEpilogue =
    "A1 ?? ?? ?? ?? F6 C4 ?? 75 24 66 83 3D ?? ?? ?? ?? ?? 73 1A 6A 08 6A 01 C6 05 ?? ?? ?? ?? 05 E8 ?? ?? ?? ?? "
    "6A 01 E8 ?? ?? ?? ?? 83 C4 0C 6A 01 68 FF FF FF 00 68 FF FF FF 00 6A 02 6A 02 6A 00 6A 00 E8 ?? ?? ?? ?? "
    "E8 ?? ?? ?? ??";
// ...which also makes the save the game offers next a "Next Game" one: the save
// count 0xFF, the location 0x1C, and the clear count up one.
const char* const kSigClearsUp =
    "A0 ?? ?? ?? ?? 68 00 00 10 80 C6 05 ?? ?? ?? ?? FF C6 05 ?? ?? ?? ?? 1C A2 ?? ?? ?? ?? E8 ?? ?? ?? ?? 50 E8 "
    "?? ?? ?? ?? 83 C4 08 66 FF 05 ?? ?? ?? ?? C3";
// The title menu's EPILOGUE screen (epilog.bin), once it has read the count back
// from saves\data.sav: an epilogue opens when the cursor is under the count,
// held to the number of epilogues; the rest are drawn locked.
const char* const kSigEpilogueMenu =
    "51 66 A1 ?? ?? ?? ?? 66 89 44 24 00 66 3D ?? 00 B8 ?? 00 00 00 73 04 8B 44 24 00 8B 0D ?? ?? ?? ??";
// The room load testing the player's model: the type byte of the model loaded
// against the character it is to put in play. The same, it jumps past the load
// (the `je` kSiteModelReload takes out); otherwise, going from one of Jill's
// types to the first character that is not hers or further, her health is
// filled and her type kept for when she comes back.
const char* const kSigModelCheck =
    "A0 ?? ?? ?? ?? 66 8B 0D ?? ?? ?? ?? 66 33 D2 8A D0 66 3B D1 0F 84 ?? ?? ?? ?? 66 83 F9 ?? BF C8 00 00 00 7C ?? "
    "3C ?? 73 ?? 66 89 3D ?? ?? ?? ?? 66 89 1D ?? ?? ?? ?? 66 89 15 ?? ?? ?? ??";
// ...and the end of that load, where the test's jump lands: the character into
// the model's type byte, the maximum health, the character's inventory picked
// (the inventory select), its model loaded and the player's state started
// afresh.
const char* const kSigModelReload =
    "0F BF 05 ?? ?? ?? ?? 8A 15 ?? ?? ?? ?? 83 C4 10 88 15 ?? ?? ?? ?? 66 89 3D ?? ?? ?? ?? 50 E8 ?? ?? ?? ?? 68 ?? ?? "
    "?? ?? E8 ?? ?? ?? ?? 83 C4 08 89 1D ?? ?? ?? ?? E8";
// The player-model loader: its file name's two digits are the model's type,
// except that for Jill's own two types the outfit value (through a getter)
// picks another of her models, or for the last two her regular or biker one
// with a texture of its own loaded over it.
const char* const kSigModelLoad =
    "B8 ?? ?? ?? ?? E8 ?? ?? ?? ?? 53 55 56 57 68 ?? ?? ?? ?? E8 ?? ?? ?? ?? 8B B4 24 ?? ?? ?? ?? B3 30 8B E8 88 1D ?? ?? "
    "?? ?? 88 1D ?? ?? ?? ?? 8A 46 4A 02 C3 83 C4 04 A2 ?? ?? ?? ?? 8A 46 4A 81 ED ?? ?? ?? ?? 3C 09 76 07 04 57 A2 ?? ?? "
    "?? ?? E8 ?? ?? ?? ?? 8A 4E 4A 89 44 24 ?? 84 C9 74 05 80 F9 01 75 ?? 8D 48 FF 83 F9 ?? 77 ?? FF 24 8D ?? ?? ?? ?? "
    "04 31 A2 ?? ?? ?? ?? EB ?? 8D 4C 24 ?? 51 68 ?? ?? ?? ?? E8 ?? ?? ?? ?? 83 C4 08 88 1D ?? ?? ?? ?? EB ?? 8D 54 24 "
    "?? 52 68 ?? ?? ?? ?? E8 ?? ?? ?? ?? 83 C4 08 C6 05 ?? ?? ?? ?? 32 55 68 ?? ?? ?? ?? E8";
// Where the loader's switch on the outfit (from 1) can land: the case that
// loads model value + 1, and the two that load the regular and the biker model
// with a texture of their own.
constexpr int kOutfitCaseModel = 116, kOutfitCaseTex1 = 125, kOutfitCaseTex2 = 151;
// The title's outfit grid, once a cell is picked: the outfit that cell is,
// out of a table, handed to the outfit's setter.
const char* const kSigOutfitGrid = "0F BE C8 8B 14 8D ?? ?? ?? ?? 52 E8 ?? ?? ?? ?? 53 56 88 5E 0F E8";
// The memory card screen's task entry (task 1, from the title's Load the game
// and a typewriter's Save the game alike): it runs the screen only while task 0
// is waiting for it (Task_state), and calls the screen's loop.
const char* const kSigCardEntry =
    "6A 00 E8 ?? ?? ?? ?? 83 C4 04 83 F8 01 75 1B 6A 00 E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? 6A 00 E8 ?? ?? ?? ?? 83 C4 08 E9 "
    "?? ?? ?? ??";
// The screen's loop: its state struct through a pointer, the frame's buttons
// kept for it, and a call through a table of state handlers on the struct's
// first byte.
const char* const kSigCardLoop =
    "53 56 8B 35 ?? ?? ?? ?? 56 E8 ?? ?? ?? ?? 83 C4 04 33 DB 88 5E 1A 53 53 68 FF 00 00 00 E8 ?? ?? ?? ?? 88 5E 11 "
    "66 A1 ?? ?? ?? ?? 66 89 46 1C 66 8B 0D ?? ?? ?? ?? 56 66 89 4E 1E E8 ?? ?? ?? ?? 83 C4 10 33 D2 56 8A 16 88 5E 16 "
    "FF 14 95 ?? ?? ?? ??";
// The screen's init: it clears its buffers and its 0x12C0-byte state struct,
// and points the loop's pointer at the struct.
const char* const kSigCardInit =
    "6A 41 68 ?? ?? ?? ?? E8 ?? ?? ?? ?? 68 ?? ?? ?? ?? 68 ?? ?? ?? ?? E8 ?? ?? ?? ?? 68 8C 01 00 00 68 ?? ?? ?? ?? E8 "
    "?? ?? ?? ?? 6A 16 68 ?? ?? ?? ?? E8 ?? ?? ?? ?? 6A 28 68 ?? ?? ?? ?? E8 ?? ?? ?? ?? 68 C0 12 00 00 68 ?? ?? ?? ?? "
    "E8 ?? ?? ?? ?? 6A 50 68 ?? ?? ?? ?? C7 05 ?? ?? ?? ?? 00 00 00 00 E8 ?? ?? ?? ?? 6A 50 68 ?? ?? ?? ?? E8 ?? ?? ?? ?? "
    "83 C4 40 C7 05 ?? ?? ?? ?? ?? ?? ?? ?? C3";
// The state that reads the card: it clears the struct's list of the card's
// files and has the card read into it (the card it shows at +0x0E, the slot
// map at +0x20), then goes on to the list.
const char* const kSigCardRescan =
    "53 56 8B 74 24 0C 33 C0 32 DB 8A 46 0E 88 5E 07 50 88 5E 13 E8 ?? ?? ?? ?? 83 C4 04 3A C3 88 46 0F 74 1E 76 0F 3C "
    "02 76 0E 3C 03 75 07 C6 06 03 C6 46 0F 01 5E 5B C3 C6 06 03 C6 46 0F 04 5E 5B C3 33 C9 8A 4E 0E 51 E8 ?? ?? ?? ?? "
    "8D 56 34 68 8C 01 00 00 52 88 46 0F E8 ?? ?? ?? ?? 8A 46 0F 83 C4 0C 3A C3 75 18 8D 46 20 50 E8 ?? ?? ?? ??";
// The state that waits on the list of slots: the cursor (+4) kept for the next
// opening, then the frame's buttons.
const char* const kSigCardList =
    "56 8B 74 24 08 8A 46 04 C6 46 07 0E A2 ?? ?? ?? ?? A1 ?? ?? ?? ?? F6 C4 10 75 49 8B 0D ?? ?? ?? ?? F6 C5 08 75 3E "
    "F6 C4 20 74 46";
// Reading the card: every file listed, the slots mapped from their names, and
// for each slot holding a save a few bytes of the save itself - read from where
// its data starts in the file - kept for the list to draw.
const char* const kSigCardRead =
    "53 55 56 57 8B 3D ?? ?? ?? ?? 33 C9 68 ?? ?? ?? ?? 8A 4F 0E 8D 47 34 50 51 E8 ?? ?? ?? ?? 8B 54 24 20 52 E8 ?? ?? ?? "
    "?? 8A 87 7F 01 00 00 83 C4 10 84 C0 76 62 33 DB 8D AF ?? ?? 00 00 8B 44 24 14 8A 04 03 A8 C0 75 3C 25 FF 00 00 00 "
    "68 ?? ?? 00 00 8D B7 C0 03 00 00 68 ?? 00 00 00 8D 0C 80 8D 14 48 8D 44 57 34 50 56 E8 ?? ?? ?? ?? 83 C4 10 85 C0 "
    "75 22 68 ?? 00 00 00 56 55 E8 ?? ?? ?? ?? 83 C4 0C 43 81 C5 ?? 00 00 00 83 FB ?? 72 AD";
// The slot map: a file whose name starts with the game's twelve-character
// prefix is the save in the slot its next two characters give, as two digits.
const char* const kSigCardSlots =
    "53 55 8B 6C 24 0C 56 8B 35 ?? ?? ?? ?? 8B CD B8 80 80 80 80 33 DB 89 01 89 41 04 89 41 08 66 89 41 0C 88 41 0E "
    "8A 86 7E 01 00 00 33 C9 C6 86 7F 01 00 00 00 84 C0 89 4C 24 10 76 59 57 8D 7E 40 6A 0C 8D 57 F4 68 ?? ?? ?? ?? 52 "
    "E8 ?? ?? ?? ?? 83 C4 0C 85 C0 75 25 8A 07 33 C9 8A 4F 01 8B D5 8D 04 80 03 D1 88 9C 42 F0 FD FF FF";
// The list drawing a slot that holds a save: from the bytes read for it, the
// times the game was cleared, the save count, the location's name out of the
// game's text, and the Easy flag through the bit test.
const char* const kSigCardRow =
    "8A 44 2E ?? 83 C4 20 3C ?? 0F 83 ?? ?? ?? ?? 8B CD 6A 0A C1 E1 07 66 83 BC 31 ?? ?? 00 00 64 8D BC 31 ?? ?? 00 00 "
    "73 26 33 D2 66 8B 15 ?? ?? ?? ?? 81 C2 ?? ?? ?? ?? 52 68 ?? ?? ?? ?? E8 ?? ?? ?? ?? 83 C4 0C C6 05 ?? ?? ?? ?? FE "
    "EB 1D A1 ?? ?? ?? ?? 25 FF FF 00 00 05 ?? ?? ?? ?? 50 68 ?? ?? ?? ?? E8 ?? ?? ?? ?? 83 C4 0C 33 C9 B8 67 66 66 66 "
    "8A 4F ?? 6A 18 F7 E9 A0 ?? ?? ?? ?? C1 FA 02 8B CA C1 E9 1F 03 D1 B9 0A 00 00 00 02 C2 A2 ?? ?? ?? ?? 33 C0 8A 47 "
    "?? 99 F7 F9 8A 0D ?? ?? ?? ?? 33 C0 02 CA 33 D2 88 0D ?? ?? ?? ?? 8A 57 ?? 66 8B 04 55 ?? ?? ?? ?? 05 ?? ?? ?? ?? "
    "50 68 ?? ?? ?? ?? E8 ?? ?? ?? ?? 83 C7 ?? 6A ?? 57 E8 ?? ?? ?? ??";
// The game building its memory card container: so many card objects of so
// many bytes from an offset, constructed and destroyed by the card class, the
// container's own vtable, and the container kept in a field of the game's app
// object...
const char* const kSigCardMaker =
    "68 ?? ?? ?? ?? E8 ?? ?? ?? ?? 8B F8 83 C4 04 89 7C 24 10 3B FB C7 84 24 ?? ?? ?? ?? 06 00 00 00 74 4A 89 6F 04 89 "
    "5F 08 C7 07 ?? ?? ?? ?? 68 ?? ?? ?? ?? 68 ?? ?? ?? ?? 6A ?? 8D 57 ?? 68 ?? ?? ?? ?? 52 C6 84 24 ?? ?? ?? ?? 07 E8 "
    "?? ?? ?? ?? C7 07 ?? ?? ?? ?? C7 84 24 ?? ?? ?? ?? FF FF FF FF 89 BE ?? ?? 00 00";
// ...which the app copies into the global the game reaches it through.
const char* const kSigCardGlobal =
    "8B 8E E4 00 00 00 89 0D ?? ?? ?? ?? 8B 96 FC 00 00 00 89 15 ?? ?? ?? ?? 8B 86 E8 00 00 00 A3 ?? ?? ?? ??";
// The card class: its constructor (the vtable, the folder emptied)...
const char* const kSigCardCtor = "56 8B F1 E8 ?? ?? ?? ?? C7 06 ?? ?? ?? ?? C6 86 ?? ?? ?? ?? 00 8B C6 5E C3";
// ...its destructor, which writes the image out...
const char* const kSigCardDtor =
    "6A FF 68 ?? ?? ?? ?? 64 A1 00 00 00 00 50 64 89 25 00 00 00 00 51 56 8B F1 89 74 24 04 C7 06 ?? ?? ?? ?? C7 44 24 "
    "10 00 00 00 00 E8 ?? ?? ?? ??";
// ...the write itself: the folder, the card's name (a virtual) with the file
// extension over its last character, fopen "wb", and the whole image in one
// fwrite...
const char* const kSigCardFlush =
    "81 EC 80 04 00 00 33 C0 8D 54 24 00 53 55 8B D9 56 57 83 C9 FF 8D BB ?? ?? ?? ?? F2 AE F7 D1 2B F9 8B C1 8B F7 8B "
    "FA 8B 13 C1 E9 02 F3 A5 8B C8 83 E1 03 F3 A4 8B CB FF 52 ?? 8B F8 83 C9 FF 33 C0 8D 54 24 10 F2 AE F7 D1 2B F9 8B "
    "F7 8B E9 8B FA 83 C9 FF F2 AE 8B CD 4F C1 E9 02 F3 A5 8B CD 83 E1 03 F3 A4 8D 7C 24 10 83 C9 FF F2 AE F7 D1 49 83 "
    "F9 02 72 62 8D 7C 24 10 83 C9 FF F2 AE F7 D1 49 BF ?? ?? ?? ?? 68 ?? ?? ?? ?? 8D 54 0C 13 83 C9 FF F2 AE F7 D1 2B "
    "F9 8B C1 8B F7 8B FA C1 E9 02 F3 A5 8B C8 83 E1 03 F3 A4 8D 4C 24 14 51 E8 ?? ?? ?? ?? 8B F0 83 C4 08 85 F6 74 1A "
    "56 68 ?? ?? ?? ?? 83 C3 ?? 6A 01 53 E8 ?? ?? ?? ?? 56 E8 ?? ?? ?? ?? 83 C4 14";
// ...and creating a file: the first free directory entry from entry 1 (state
// 0), which becomes a one-block file - state 0x51, 0x2000 bytes, no next block,
// the name at +0x0A, up to 0x15 characters.
const char* const kSigCardCreate =
    "B8 01 00 00 00 81 C1 ?? ?? ?? ?? 83 39 00 74 0F 40 81 C1 80 00 00 00 83 F8 10 7C EF 83 C8 FF C3 56 57 8B F1 E8 D7 "
    "FF FF FF 8B F8 85 FF 7D 08 5F 83 C8 FF 5E C2 04 00 8B 4C 24 0C 8B C7 C1 E0 07 6A 15 51 8D 74 30 ?? 8D 56 0A 52 E8 "
    "?? ?? ?? ?? 83 C4 0C 8B C7 C7 06 51 00 00 00 C7 46 04 00 20 00 00 66 C7 46 08 FF FF 5F 5E C2 04 00";
// Inside the memory card screen's state struct: the state the loop dispatches
// on (its first byte), the cursor and the card it shows - the offsets the list's
// and the read's handlers use (`8A 46 04`, `8A 46 0E`).
constexpr int kCardStateOff = 0, kCardCursorOff = 4, kCardCardOff = 0x0E;
constexpr int kCardScreenTask = 1;
// The save flags' countdown bit, and the radar receiver's, which runs the radar
// in the clock's place.
constexpr uint32_t kCountdownBit = 0x4000000, kRadarBit = 0x2000000;
// The gameplay loop's frames to the second: the countdown takes a second off
// once its frame count is past 29 (a constant of kSigCountdownTick).
constexpr int kLoopFps = 30;
constexpr uint32_t kMercenariesBit = 0x80;  // in the state flags
constexpr uint8_t kVoiceStop = 3;  // the voice player's states: 0 idle, 1 starting, 2 playing, 3 stopping
constexpr int kMessageFlagsOff = 0x75C8;
constexpr uint32_t kCinemaBit = 0x8, kMovieBit = 0x10000;
constexpr uint8_t kMessageUp = 0x80;
constexpr uint8_t kMessagePage = 2, kMessageChoice = 4, kMessageEnd = 5;

uint32_t u32(uintptr_t a) { return mem::read<uint32_t>(a); }
template <typename T>
T peek(uintptr_t a) {
  return *reinterpret_cast<volatile T*>(a);
}
// Game data is plain writable memory: no VirtualProtect for it (that is for code).
template <typename T>
void poke(uintptr_t a, T v) {
  *reinterpret_cast<volatile T*>(a) = v;
}

// The one match of `sig` in [lo, hi) (default: the code), or 0.
uintptr_t find_one(const char* name, const char* sig, uintptr_t lo = 0, uintptr_t hi = 0) {
  const mem::Range r = lo ? mem::Range{lo, hi} : image::text();
  const auto hits = mem::find_all(r, sig, 4);
  if (hits.size() == 1) return hits[0];
  logf("game: %s: %s", name, hits.empty() ? "not found" : "more than one match - not used");
  return 0;
}

void prepare(Site s, uintptr_t at, const char* expect, const std::vector<uint8_t>& bytes) {
  if (!at) return;
  if (!g_sites[s].prepare(at, expect, bytes, kSiteNames[s])) {
    logf("game: site %s at exe+0x%X does not hold the expected bytes - not used", kSiteNames[s], image::rva(at));
    return;
  }
  g_site_ok[s] = true;
  logf("game: site %s at exe+0x%X", kSiteNames[s], image::rva(at));
}

// The inventory block in play, or 0 when the pointer names neither.
uintptr_t bag_block() {
  if (!g.inv_ptr || !g.inv_base) return 0;
  const uint32_t p = u32(g.inv_ptr);
  if (p == g.inv_base) return p;
  if (p == g.inv_base + g.inv_stride) return p;
  return 0;
}

// Discovery step 12, the reloading tool's proficiency. Every site has to name
// the same count offset through the inventory pointer, and the grenade mix the
// grenade kind's count and level row by address, at that kind's place in both;
// the kind table has to file the ammo the way the panel names the kinds. Only
// then is g_mix filled in.
bool discover_mixing() {
  if (!g.inv_ptr || !g.inv_stride || !g.flags) return false;
  const uintptr_t d = find_one("mix dispatch", kSigMixDispatch);
  const uintptr_t y = find_one("mix yield", kSigMixYield);
  const uintptr_t c = find_one("mix count", kSigMixCount);
  const uintptr_t o = find_one("enhanced ammo offer", kSigMixOffer);
  const uintptr_t gm = find_one("grenade mix", kSigGrenadeMix);
  const uintptr_t bt = find_one("bit test", kSigBitTest);
  const uintptr_t cb = find_one("combine lookup", kSigCombine);
  if (!d || !y || !c || !o || !gm || !bt || !cb) return false;

  Mixing m{};
  // One type of combination goes to the batch, another to the grenade mix.
  const int tool_type = mem::read<uint8_t>(d + 8);
  const uintptr_t to_yield = d + 15 + static_cast<int32_t>(u32(d + 11));
  const uintptr_t to_grenade = d + 24 + static_cast<int32_t>(u32(d + 20));
  const uint32_t off = u32(y + 37), table = u32(y + 64), kinds = u32(o + 2), grenade_row = u32(gm + 41);
  const int levels = mem::read<uint8_t>(y + 76) + 1;
  const int easy_bit = mem::read<uint8_t>(c + 1);
  const uintptr_t test_c = c + 12 + static_cast<int32_t>(u32(c + 8));
  const uintptr_t test_g = gm + 126 + static_cast<int32_t>(u32(gm + 122));
  if (to_yield != y || to_grenade != gm || u32(y + 18) != g.inv_ptr || u32(c + 28) != g.inv_ptr ||
      u32(o + 69) != g.inv_ptr || u32(gm + 7) != g.inv_ptr || u32(gm + 142) != g.inv_ptr || u32(c + 40) != off ||
      u32(o + 83) != off || u32(gm + 16) != off + 2u * kMixGrenade || u32(gm + 149) != off + 2u * kMixGrenade ||
      off <= static_cast<uint32_t>(kSlotsOff) || off + 2u * kMixKinds > g.inv_stride || levels != kMixLevels ||
      mem::read<uint8_t>(gm + 54) + 1 != levels || u32(y + 97) != table + 1 ||
      grenade_row != table + 4u * kMixLevels * kMixGrenade || u32(gm + 80) != grenade_row + 1 ||
      u32(c + 3) != g.flags || u32(gm + 92) != g.flags || mem::read<uint8_t>(gm + 76) != easy_bit || easy_bit > 31 ||
      test_c != bt || test_g != bt || u32(o + 19) != kinds || u32(o + 45) != kinds + 1) {
    logf("game: the reloading tool's sites do not agree (counts at +%#x, levels %#x, kinds %#x) - its proficiency "
         "is not used",
         off, table, kinds);
    return false;
  }
  m.count_off = static_cast<int>(off);
  m.easy_mask = 0x80000000u >> easy_bit;

  // The kind table: {item id, kind} pairs to an id of 0xFF.
  uint8_t kind_of[256];
  std::memset(kind_of, 0xFF, sizeof(kind_of));
  int top = -1;
  for (int i = 0;; ++i) {
    uint8_t id = 0, k = 0;
    if (i >= 64 || !mem::read_safe(kinds + 2u * i, &id) || (id != 0xFF && !mem::read_safe(kinds + 2u * i + 1, &k))) {
      logf("game: the reloading tool's kind table at %#x does not end - its proficiency is not used", kinds);
      return false;
    }
    if (id == 0xFF) break;
    kind_of[id] = k;
    if (k > top) top = k;
  }
  if (top + 1 != kMixKinds || kind_of[items::kHandgunBulletsId] != kMixHandgun ||
      kind_of[items::kHandgunBulletsEId] != kMixHandgun || kind_of[items::kShotgunShellsId] != kMixShotgun ||
      kind_of[items::kShotgunShellsEId] != kMixShotgun || kind_of[items::kMagnumBulletsId] != kMixMagnum ||
      kind_of[items::kGrenadeRoundsId] != kMixGrenade) {
    logf("game: the reloading tool's kind table at %#x files the ammo differently - its proficiency is not used",
         kinds);
    return false;
  }
  m.enhanced_kinds = mem::read<uint8_t>(o + 50) + 1;
  m.enhanced_after = mem::read<uint8_t>(o + 87);
  if (m.enhanced_kinds > kMixKinds || m.enhanced_after > 0x7F) {
    logf("game: the reloading tool offers enhanced rounds for %d kinds after %d batches - its proficiency is not used",
         m.enhanced_kinds, m.enhanced_after);
    return false;
  }

  // The level table: a row of {threshold, bonus, 0, 0} per kind, the thresholds
  // going up (the last is never compared: the walk stops there).
  for (int k = 0; k < kMixKinds; ++k)
    for (int i = 0; i < kMixLevels; ++i) {
      const uintptr_t e = table + 4u * (k * kMixLevels + i);
      if (!mem::read_safe(e, &m.threshold[k][i]) || !mem::read_safe(e + 1, &m.bonus[k][i]) ||
          (i > 0 && i < kMixLevels - 1 && m.threshold[k][i] <= m.threshold[k][i - 1])) {
        logf("game: the reloading tool's level table at %#x does not go up - its proficiency is not used", table);
        return false;
      }
    }

  // Enhanced rounds (the two ids the yield compares, which must be the enhanced
  // handgun bullets and shotgun shells) take their bonus from a switch on the
  // normal one: a case that sets a lower bonus, or the end, which keeps it.
  for (int& b : m.enhanced_bonus) b = -1;
  const uint32_t jt = u32(y + 133);
  if (mem::read<uint8_t>(y + 105) == items::kHandgunBulletsEId && mem::read<uint8_t>(y + 113) == items::kShotgunShellsEId &&
      image::text().contains(jt) && image::text().contains(jt + 31))
    for (int b = 0; b < 8; ++b) {
      const uint32_t to = u32(jt + 4u * b) - static_cast<uint32_t>(y);
      if (to == kMixCaseKeep) m.enhanced_bonus[b] = b;
      for (int j = 0; j < 3; ++j)
        if (to == static_cast<uint32_t>(kMixCase[j])) m.enhanced_bonus[b] = mem::read<uint8_t>(y + kMixCase[j] + 4);
    }

  // The reloading tool's recipes, out of the combination table: an entry right
  // after one for the same two items is the enhanced rounds the game makes
  // instead when the player says yes (it moves on one entry, 0x4DB74B).
  const uint32_t combos = u32(cb + 11);
  if (u32(cb + 2) == combos + 1) {
    uint8_t prev[3] = {0xFF, 0xFF, 0xFF};
    MixRecipe* last = nullptr;
    for (int i = 0; i < 256; ++i) {
      uint8_t e[5];
      if (!mem::read_safe(combos + 8u * i, &e) || e[1] == 0xFF) break;
      const bool again = e[0] == prev[0] && e[1] == prev[1] && e[2] == prev[2];
      std::memcpy(prev, e, 3);
      if (!again) last = nullptr;
      if (e[0] != tool_type || (e[1] != items::kReloadingToolId && e[2] != items::kReloadingToolId)) continue;
      const int k = kind_of[e[3]];
      if (k >= kMixKinds) continue;
      if (again && last) {
        last->enhanced = e[3];
        last->enhanced_base = e[4];
        last = nullptr;
      } else if (m.recipes[k] < kMaxMixRecipes) {
        last = &m.recipe[k][m.recipes[k]++];
        last->powder = e[1] == items::kReloadingToolId ? e[2] : e[1];
        last->result = e[3];
        last->base = e[4];
      }
    }
  }
  m.known = true;
  g_mix = m;
  logf("game: reloading tool: batch counts at +%#x of the inventory block, levels from %#x (handgun: up to %d, %d, "
       "%d, %d batches, then the last; +%d/%d/%d/%d/%d tenths), enhanced rounds for %d kinds after %d batches, "
       "doubled under state flag %#x; %d/%d/%d/%d recipes",
       m.count_off, table, m.threshold[0][0], m.threshold[0][1], m.threshold[0][2], m.threshold[0][3], m.bonus[0][0],
       m.bonus[0][1], m.bonus[0][2], m.bonus[0][3], m.bonus[0][4], m.enhanced_kinds, m.enhanced_after, m.easy_mask,
       m.recipes[0], m.recipes[1], m.recipes[2], m.recipes[3]);
  return true;
}

// Where a rel32 call or jump operand at `at` lands.
uintptr_t rel_target(uintptr_t at) { return at + 4 + static_cast<int32_t>(u32(at)); }

// A NUL-terminated string at `at` that fits `n` with no control characters in
// it (a path may hold the code page's own letters, so bytes over 0x7F pass).
bool read_cstr(uintptr_t at, char* out, size_t n, bool allow_empty = false) {
  for (size_t i = 0; i < n; ++i) {
    uint8_t c = 0;
    if (!mem::read_safe(at + i, &c)) return false;
    out[i] = static_cast<char>(c);
    if (!c) return i > 0 || allow_empty;
    if (c < 0x20 || c == 0x7F) return false;
  }
  out[n - 1] = '\0';
  return false;
}

// The game's text as its messages keep it: each character its ASCII code less
// 0x24, a space for 0, a few punctuation marks of its own. Anything from 0xE0
// up - its glyphs, separators and line ends - ends what is read here.
void decode_text(uintptr_t at, char* out, size_t n) {
  size_t k = 0;
  for (int i = 0; i < 40 && k + 1 < n; ++i) {
    uint8_t c = 0;
    if (!mem::read_safe(at + i, &c) || c >= 0xE0) break;
    char ch = '?';
    switch (c) {
      case 0x00: ch = ' '; break;
      case 0x01: ch = '.'; break;
      case 0x09:
      case 0x0A: ch = '"'; break;
      case 0x18: ch = ','; break;
      case 0x1A: ch = '!'; break;
      case 0x3A: ch = '\''; break;
      case 0x3B: ch = '-'; break;
      default:
        if (c + 0x24 < 0x7F) ch = static_cast<char>(c + 0x24);
        break;
    }
    out[k++] = ch;
  }
  while (k && out[k - 1] == ' ') --k;
  out[k] = '\0';
}

// Discovery step 10, second half: the missile at the end, which the radar
// counts in once the radar receiver is taken. The gameplay loop (`loop`, the
// countdown tick's match) zeroes the radar's state byte and calls the radar,
// which has to dispatch on that byte and hand the handler the struct; one of
// its handlers runs the radar, and its own table of states has to hold the far
// and the close state once each. Both have to run the step and read the
// distance where the step takes it down, and the close one has to end the game
// on the save flags and the state flags discovery found, cutting the voice the
// voice player has.
bool discover_missile(uintptr_t loop) {
  const uintptr_t run = find_one("radar run", kSigRadarRun);
  const uintptr_t far_h = find_one("missile far", kSigMissileFar);
  const uintptr_t near_h = find_one("missile near", kSigMissileNear);
  const uintptr_t step = find_one("missile step", kSigMissileStep);
  if (!run || !far_h || !near_h || !step) return false;
  const uintptr_t radar = u32(loop + 26), show = rel_target(loop + 32);
  const bool show_ok = image::text().contains(show) && mem::matches(show, mem::parse_pattern(kSigRadar));
  const uint32_t handlers = show_ok ? u32(show + 18) : 0, states = u32(run + 29);
  const int off = mem::read<uint8_t>(step + 53), last = mem::read<uint8_t>(run + 19);
  const int32_t per_frame = -static_cast<int32_t>(u32(step + 61));
  if (!show_ok || u32(show + 1) != radar || u32(show + 6) != radar || !image::data().contains(radar) ||
      u32(step + 1) != g.flags || u32(step + 12) != g.save_flags || mem::read<uint8_t>(step + 67) != off ||
      rel_target(far_h + 27) != step || rel_target(near_h + 28) != step || mem::read<uint8_t>(far_h + 33) != off ||
      mem::read<uint8_t>(near_h + 34) != off || mem::read<uint8_t>(near_h + 46) != off ||
      u32(far_h + 2) != g.save_flags || u32(near_h + 2) != g.save_flags || u32(near_h + 109) != g.save_flags ||
      u32(near_h + 129) != g.save_flags || u32(near_h + 114) != g.flags || u32(near_h + 134) != g.flags ||
      (g.voice_state && (u32(near_h + 93) != g.voice_state || u32(near_h + 103) != g.voice_state)) || off < 4 ||
      off > 0x100 || per_frame <= 0 || per_frame > 0x100000 || last < 3 || last > 31) {
    logf("game: the radar's sites do not agree (struct %#x, distance +%#x) - the missile cannot be frozen", radar,
         off);
    return false;
  }
  int run_state = -1, runs = 0;
  for (int i = 0; i < 8; ++i) {
    uint32_t h = 0;
    if (!mem::read_safe(handlers + 4u * i, &h) || !image::text().contains(h)) break;
    if (h == run) {
      run_state = i;
      ++runs;
    }
  }
  int far_state = -1, near_state = -1, fars = 0, nears = 0;
  for (int i = 0; i <= last; ++i) {
    uint32_t h = 0;
    if (!mem::read_safe(states + 4u * i, &h)) break;
    if (h == far_h) {
      far_state = i;
      ++fars;
    }
    if (h == near_h) {
      near_state = i;
      ++nears;
    }
  }
  if (runs != 1 || fars != 1 || nears != 1) {
    logf("game: the radar's handlers at %#x hold its run %d time(s), and its states at %#x the far one %d and the "
         "close one %d - the missile cannot be frozen",
         handlers, runs, states, fars, nears);
    return false;
  }
  g.radar = radar;
  g.missile = radar + off;
  g.radar_sub = mem::read<uint8_t>(run + 9);
  g.radar_run = run_state;
  g.radar_far = far_state;
  g.radar_near = near_state;
  g.missile_step = per_frame;
  prepare(kSiteMissileStep, step + 65, "89 48 ??", mem::nops(3));
  logf("game: the missile: the radar's struct %#x runs in state %d, its own state at +%d counting in states %d and "
       "%d; the distance at +%#x, %d off a frame",
       radar, run_state, g.radar_sub, far_state, near_state, off, per_frame);
  return true;
}

// Discovery step 14, the memory cards and the memory card screen, for the save
// file manager. The screen's entry, loop, init, read and slot map have to name
// one state pointer and reach each other; the state table has to hold the
// read's handler and the list's once each; the list's row has to draw what the
// read copied, with the save count where the save block keeps it. The card
// container's maker and the global, the card class's constructor, destructor
// and write, and its file creation have to agree on the card objects, the image
// inside each, its folder and its name.
bool discover_cards() {
  if (!g.task_state_tab) return false;
  const uintptr_t e = find_one("card screen entry", kSigCardEntry);
  const uintptr_t l = find_one("card screen loop", kSigCardLoop);
  const uintptr_t in = find_one("card screen init", kSigCardInit);
  const uintptr_t rs = find_one("card list rescan", kSigCardRescan);
  const uintptr_t li = find_one("card list", kSigCardList);
  const uintptr_t rd = find_one("card read", kSigCardRead);
  const uintptr_t sl = find_one("card slots", kSigCardSlots);
  const uintptr_t row = find_one("card row", kSigCardRow);
  const uintptr_t mk = find_one("card maker", kSigCardMaker);
  const uintptr_t gl = find_one("card global", kSigCardGlobal);
  const uintptr_t cr = find_one("card create", kSigCardCreate);
  const uintptr_t fl = find_one("card flush", kSigCardFlush);
  const uintptr_t ts = find_one("Task_state", kSigTaskState);
  const uintptr_t bt = find_one("bit test", kSigBitTest);
  if (!e || !l || !in || !rs || !li || !rd || !sl || !row || !mk || !gl || !cr || !fl || !ts || !bt) return false;

  Cards c{};
  // The screen: its entry asks Task_state and runs the loop; the loop, the
  // init, the read and the slot map all name the one pointer, which the init
  // points at the struct it clears; the read runs the slot map and the state
  // that reads the card runs the read. Both the loop and the list read the
  // frame's buttons the input poll publishes.
  const uint32_t ptr = u32(l + 4), states = u32(l + 78), screen = u32(in + 72);
  if (rel_target(e + 3) != ts || rel_target(e + 23) != l || u32(in + 120) != ptr || u32(in + 124) != screen ||
      u32(rd + 6) != ptr || u32(sl + 9) != ptr || rel_target(rs + 108) != rd || rel_target(rd + 36) != sl ||
      (g.in_pressed && (u32(l + 39) != g.in_pressed || u32(li + 18) != g.in_pressed)) ||
      (g.in_held && (u32(l + 50) != g.in_held || u32(li + 29) != g.in_held)) || !image::data().contains(ptr) ||
      !image::data().contains(screen)) {
    logf("game: the memory card screen's sites do not agree (state %#x through %#x) - the save files are not managed",
         screen, ptr);
    return false;
  }
  int rescans = 0, lists = 0;
  for (int i = 0; i < 32; ++i) {
    uint32_t h = 0;
    if (!mem::read_safe(states + 4u * i, &h)) break;
    if (h == rs) {
      c.state_rescan = i;
      ++rescans;
    }
    if (h == li) {
      c.state_list = i;
      ++lists;
    }
  }
  if (rescans != 1 || lists != 1) {
    logf("game: the memory card screen's state table at %#x holds its read %d time(s) and its list %d - the save "
         "files are not managed",
         states, rescans, lists);
    return false;
  }

  // The saves: the read copies `n` bytes of each slot's save, from where its
  // data starts in the file, into the struct; the row draws from that copy; the
  // save count it shows is the save block's (the data is the save block, play
  // time first); the Easy flag is a bit of a dword, through the bit test.
  CardLayout& lay = c.layout;
  const int cache = mem::read<uint16_t>(rd + 57), n = mem::read<uint8_t>(rd + 89);
  const int row_cache = mem::read<uint16_t>(row + 34), bit = mem::read<uint8_t>(row + 205);
  lay.slots = mem::read<uint8_t>(rd + 141);
  lay.data_off = mem::read<uint16_t>(rd + 78);
  lay.saves_off = mem::read<uint8_t>(row + 116);
  lay.location_off = mem::read<uint8_t>(row + 176);
  lay.clears_off = mem::read<uint16_t>(row + 26) - row_cache;
  lay.flags_off = mem::read<uint8_t>(row + 203);
  if (row_cache != cache || mem::read<uint8_t>(row + 152) != lay.saves_off || mem::read<uint8_t>(row + 8) != lay.slots ||
      mem::read<uint8_t>(rd + 118) != n || mem::read<uint8_t>(rd + 135) != n || lay.slots < 1 ||
      lay.slots > kCardMaxSlots || rel_target(row + 208) != bt || bit > 31 || lay.saves_off >= n ||
      lay.location_off >= n || lay.clears_off < 0 || lay.clears_off + 2 > n || lay.flags_off + 4 > n ||
      lay.data_off < 4 || lay.data_off + n > kCardBlockBytes ||
      (g.timer && g.save_count && g.save_count - g.timer != static_cast<uint32_t>(lay.saves_off))) {
    logf("game: the memory card screen's list does not read its saves the way its read copies them (%d slots, data "
         "at +%#x) - the save files are not managed",
         lay.slots, lay.data_off);
    return false;
  }
  lay.easy_mask = 0x80000000u >> bit;  // bit n of the array counted from the top: n < 32 is in the first dword
  // A save's file name: the prefix the slot map compares (twelve characters,
  // the length its strncmp is given), then the slot's two digits.
  if (!read_cstr(u32(sl + 70), lay.prefix, sizeof(lay.prefix)) || std::strlen(lay.prefix) != 12) {
    logf("game: the memory card screen's save name prefix is not twelve characters - the save files are not managed");
    return false;
  }

  // The card objects: `cards` of `stride` bytes from `first` in a container of
  // `size`, built by the card class's constructor, destroyed by its destructor
  // (which writes the image out through the write found on its own); the image
  // where the write and the file creation both have it, the folder where the
  // constructor and the write have it, the name through the virtual the write
  // asks (`lea eax,[ecx+n]`).
  const uint32_t size = u32(mk + 1), dtor = u32(mk + 47), ctor = u32(mk + 52), stride = u32(mk + 62);
  const int cards = mem::read<uint8_t>(mk + 57), first = mem::read<uint8_t>(mk + 60);
  const uint32_t field = mem::read<uint16_t>(mk + 99), image_off = mem::read<uint8_t>(fl + 198);
  const bool ctor_ok = image::text().contains(ctor) && mem::matches(ctor, mem::parse_pattern(kSigCardCtor));
  const bool dtor_ok = image::text().contains(dtor) && mem::matches(dtor, mem::parse_pattern(kSigCardDtor));
  const uint32_t path_off = ctor_ok ? u32(ctor + 16) : 0, card_vt = ctor_ok ? u32(ctor + 10) : 0;
  if (!ctor_ok || !dtor_ok || field != u32(gl + 14) || cards < 1 || cards > 4 || stride <= kCardBytes ||
      size != first + cards * stride || rel_target(dtor + 44) != fl || u32(dtor + 31) != card_vt ||
      u32(fl + 23) != path_off || u32(fl + 192) != static_cast<uint32_t>(kCardBytes) ||
      mem::read<uint8_t>(cr + 70) != image_off || u32(cr + 7) != image_off + kCardEntryBytes ||
      path_off < image_off + kCardBytes || path_off >= stride || !image::data().contains(u32(gl + 20))) {
    logf("game: the memory card objects' sites do not agree (%d of %#x bytes, image at +%#x, folder at +%#x) - the "
         "save files are not managed",
         cards, stride, image_off, path_off);
    return false;
  }
  uint32_t name_method = 0;
  if (!mem::read_safe(card_vt + mem::read<uint8_t>(fl + 57), &name_method) || !image::text().contains(name_method) ||
      !mem::matches(name_method, mem::parse_pattern("8D 41 ?? C3")) ||
      !read_cstr(u32(fl + 131), c.ext, sizeof(c.ext)) || c.ext[0] != '.') {
    logf("game: the memory card's name or file extension is not where its write has it - the save files are not "
         "managed");
    return false;
  }
  c.name_off = mem::read<uint8_t>(name_method + 2);
  c.image_off = static_cast<int>(image_off);
  c.path_off = static_cast<int>(path_off);
  c.card_vt = card_vt;
  c.container_vt = u32(mk + 82);
  c.container_ptr = u32(gl + 20);
  c.cards = cards;
  c.first_card = first;
  c.card_stride = static_cast<int>(stride);
  c.screen_entry = e;
  c.screen_ptr = ptr;
  c.screen = screen;

  // The locations' names, out of the game's own text, as the row finds them:
  // a 16-bit offset per location into the messages.
  const uint32_t names = u32(row + 181), messages = u32(row + 186);
  for (int i = 0; i < kMaxLocations; ++i) {
    uint16_t o = 0;
    if (!mem::read_safe(names + 2u * i, &o)) break;
    decode_text(messages + o, c.locations[i], sizeof(c.locations[i]));
  }
  c.known = true;
  g_cards = c;
  logf("game: memory cards: %d card objects of %#x bytes from +%#x of the container at [%#x] (image +%#x, folder "
       "+%#x, name +%#x, files *%s); the memory card screen: entry %#x, state %#x through %#x, reads the card in "
       "state %d and waits on its list in state %d; %d slots, saves named %s##, their data +%#x into the block (saves "
       "+%#x, location +%#x - %s, %s... -, clears +%#x, Easy %#x of +%#x)",
       c.cards, c.card_stride, c.first_card, c.container_ptr, c.image_off, c.path_off, c.name_off, c.ext,
       c.screen_entry, c.screen, c.screen_ptr, c.state_rescan, c.state_list, lay.slots, lay.prefix, lay.data_off,
       lay.saves_off, lay.location_off, c.locations[0], c.locations[1], lay.clears_off, lay.easy_mask, lay.flags_off);
  return true;
}

// Discovery step 15, the clear count and the epilogues. The ending's epilogue
// test, the increment it calls after it and the EPILOGUE screen's menu have to
// name one count, and the two tests the same number of epilogues; the count has
// to be where the memory card screen's list reads it in a save (that many bytes
// into the save block, whose play time comes first). The ending's Easy bit has
// to be the one the reloading tool doubles on, the increment's save count the
// one a save raises, and the menu's buttons the input poll's.
bool discover_clears() {
  if (!g.flags) return false;
  const uintptr_t e = find_one("ending's epilogue", kSigEndingEpilogue);
  const uintptr_t u = find_one("clear count +1", kSigClearsUp);
  const uintptr_t m = find_one("epilogue menu", kSigEpilogueMenu);
  if (!e || !u || !m) return false;
  const uint32_t clears = u32(e + 13), easy = static_cast<uint32_t>(mem::read<uint8_t>(e + 7)) << 8;
  const int epilogues = mem::read<uint8_t>(e + 17);
  if (u32(e + 1) != g.flags || rel_target(e + 72) != u || u32(u + 46) != clears || u32(m + 3) != clears ||
      mem::read<uint8_t>(m + 14) != epilogues || mem::read<uint8_t>(m + 17) != epilogues || epilogues < 1 || !easy ||
      !image::data().contains(clears) || (g_mix.easy_mask && easy != g_mix.easy_mask) ||
      (g.save_count && u32(u + 12) != g.save_count) || (g.in_pressed && u32(m + 29) != g.in_pressed) ||
      (g.timer && g_cards.known && clears != g.timer + g_cards.layout.clears_off)) {
    logf("game: the clear count's sites do not agree (%#x, %d epilogues) - it is not used", clears, epilogues);
    return false;
  }
  g.clears = clears;
  g.epilogues = epilogues;
  g.ending_easy = easy;
  logf("game: the clear count %#x: the ending shows its epilogue while it is under %d (not under state flag %#x) and "
       "adds one; the EPILOGUE screen unlocks up to it",
       clears, epilogues, easy);
  return true;
}

// Jill's outfits, by the value the loader reads: the names the title's costume
// grid gives them (its cells' labels in data_au/etc2/titleu03.tim, NORMAL and
// SPECIAL 1..7, each cell's value checked by discover_outfit), with what each
// looks like - pl00 (pl01 with the backpack), pl02..pl06 (the order
// kSigModelLoad's cases are checked in), and pl00 and pl02 with Pl000.tim and
// Pl006.tim loaded over them.
const char* const kOutfitNames[kOutfits] = {"Normal",
                                            "Special 1 (biker)",
                                            "Special 2 (S.T.A.R.S.)",
                                            "Special 3 (white suit)",
                                            "Special 4 (police)",
                                            "Special 5 (Regina)",
                                            "Special 6 (Normal, black top)",
                                            "Special 7 (biker, blue top)"};
constexpr uint8_t kModelMark = '#';  // in the loader's file name, where it writes '0'

// Discovery step 16, Jill's outfit. The room load's model test and the end of
// the load it jumps past have to name one type byte - the player's, at the
// entity offset the other entities' types are read at - and one character, and
// the jump has to land where that load ends; the load has to set the maximum
// health and the player's state, pick the inventory through the inventory
// select (whose last shared character has to be the one below the test's first
// that is not Jill) and call the model loader. The loader has to write one file
// name's digits, send the outfits to its cases in the order kOutfitNames
// assumes, and read the value through a getter that the title's outfit grid's
// setter writes, out of a table that gives its cells the outfits their labels
// name: SPECIAL 1..7, then NORMAL (0).
bool discover_outfit() {
  if (!g.hp || !g.max_hp || !g.condition || !g.inv_select) return false;
  const uintptr_t c = find_one("player model test", kSigModelCheck);
  const uintptr_t r = find_one("player model reload", kSigModelReload);
  const uintptr_t l = find_one("player model loader", kSigModelLoad);
  const uintptr_t gr = find_one("outfit grid", kSigOutfitGrid);
  if (!c || !r || !l || !gr) return false;

  const uint32_t player = g.hp - kEntityHp, type = u32(c + 1), who = u32(c + 8), kept = u32(c + 58);
  const int first_other = mem::read<uint8_t>(c + 29);
  if (type != player + kEntityType || u32(c + 44) != g.hp || u32(c + 51) != g.condition ||
      mem::read<uint8_t>(c + 38) != first_other || mem::read<uint8_t>(g.inv_select + 11) != first_other ||
      rel_target(c + 22) != r + 54 || u32(r + 3) != who || u32(r + 9) != who || u32(r + 18) != type ||
      u32(r + 25) != g.max_hp || rel_target(r + 31) != g.inv_select || u32(r + 36) != player ||
      u32(r + 50) != player + 4 || rel_target(r + 41) != l || !image::data().contains(who) ||
      !image::data().contains(kept)) {
    logf("game: the room load's model test does not line up with its load (type %#x, character %#x) - outfits are "
         "not changed",
         type, who);
    return false;
  }

  // The file name, its two digits before ".pld" ("...pl00.pld"), written by the
  // loader in every case; one function loads the model and either texture.
  const uint32_t digit = u32(l + 37), name = u32(l + 178);
  char file[64] = {}, tex[2][64] = {};
  const size_t len = read_cstr(name, file, sizeof(file)) ? std::strlen(file) : 0;
  bool loader_ok = len >= 8 && !_stricmp(file + len - 4, ".pld") && digit == name + len - 6 &&
                   mem::read<uint8_t>(l + 32) == '0' && rel_target(l + 162) == rel_target(l + 136) &&
                   rel_target(l + 183) == rel_target(l + 136) && mem::read<uint8_t>(l + 106) == kOutfits - 2 &&
                   read_cstr(u32(l + 131), tex[0], sizeof(tex[0])) && read_cstr(u32(l + 157), tex[1], sizeof(tex[1]));
  for (const int off : {43, 56, 76, 119, 145, 171}) loader_ok = loader_ok && u32(l + off) == digit + 1;
  // The switch on outfit - 1: the first five load another model, the last two
  // a model with a texture over it - the cases kOutfitNames is written for.
  const uint32_t cases = u32(l + 112);
  for (int i = 0; loader_ok && i < kOutfits - 1; ++i) {
    uint32_t to = 0;
    const int want = i < 5 ? kOutfitCaseModel : i == 5 ? kOutfitCaseTex1 : kOutfitCaseTex2;
    loader_ok = mem::read_safe(cases + 4u * i, &to) && to == l + want;
  }
  const uintptr_t getter = rel_target(l + 81), setter = rel_target(gr + 12);
  loader_ok = loader_ok && image::text().contains(getter) && mem::matches(getter, mem::parse_pattern("A1 ?? ?? ?? ?? C3"));
  const uint32_t outfit = loader_ok ? u32(getter + 1) : 0;
  // The grid's setter writes what the getter reads, and its cells - labelled
  // SPECIAL 1 to SPECIAL 7, then NORMAL - give outfits 1..7, then 0.
  const uint32_t values = u32(gr + 6);
  bool grid_ok = image::text().contains(setter) && mem::matches(setter, mem::parse_pattern("8B 44 24 04 A3 ?? ?? ?? ?? C3")) &&
                 u32(setter + 5) == outfit;
  for (int i = 0; grid_ok && i < kOutfits; ++i) {
    uint32_t v = 0;
    grid_ok = mem::read_safe(values + 4u * i, &v) && v == static_cast<uint32_t>((i + 1) % kOutfits);
  }
  if (!loader_ok || !grid_ok || !image::data().contains(outfit) || !image::data().contains(digit)) {
    logf("game: the model loader and the title's outfit grid do not agree (outfit %#x, file name %#x \"%s\") - "
         "outfits are not changed",
         outfit, name, file);
    return false;
  }
  g.outfit = outfit;
  g.character = who;
  g.kept_character = kept;
  g.model_type = type;
  g.model_mark = digit;
  g.first_other = first_other;
  prepare(kSiteModelReload, c + 20, "0F 84 ?? ?? ?? ??", mem::nops(6));
  char in_block[40] = "";
  if (g.timer) std::snprintf(in_block, sizeof(in_block), ", +%#x of the save block", outfit - g.timer);
  logf("game: Jill's outfit %#x (%d of them%s): the loader \"%s\" takes it for types 0 and 1, the last two with %s "
       "and %s over a model; the character %#x (Jill below %d, kept for her at %#x), the model's type byte %#x",
       outfit, kOutfits, in_block, file, tex[0], tex[1], who, first_other, kept, type);
  return true;
}

}  // namespace

bool discover() {
  if (g_ready) return true;
  image::init();
  int found = 0, wanted = 0;
  auto count = [&](bool ok) {
    ++wanted;
    found += ok ? 1 : 0;
  };

  // 1. The task scheduler: Task_chain gives both tables and the stride,
  //    Task_state must name the same state table, and Task_sleep the block of
  //    whichever task is running.
  if (const uintptr_t c = find_one("Task_chain", kSigTaskChain)) {
    const uint32_t entry = u32(c + 13), state = u32(c + 20);
    const uint32_t stride = 1u << mem::read<uint8_t>(c + 10);
    if (entry == state + 4 && stride >= 0x20 && stride <= 0x400) {
      if (const uintptr_t s = find_one("Task_state", kSigTaskState))
        if (u32(s + 12) == state) {
          g.task_state_tab = state;
          g.task_entry_tab = entry;
          g.task_stride = stride;
        }
    } else {
      logf("game: Task_chain: its two tables are not four bytes apart - not used");
    }
  }
  if (const uintptr_t s = find_one("Task_sleep", kSigTaskSleep)) g.task_current = u32(s + 1);
  count(g.task_entry_tab != 0);

  // 2. The status screen: the gameplay task chains it onto task 1, and its
  //    entry passes the screen its own state struct.
  if (const uintptr_t a = find_one("status screen chain", kSigStatusChain)) {
    const uint32_t flags = u32(a + 2);
    if (u32(a + 39) == flags) {
      g.flags = flags;
      g.save_flags = u32(a + 7);
      const uintptr_t entry = u32(a + 46);
      if (image::text().contains(entry) && mem::matches(entry, mem::parse_pattern(kSigStatusEntry))) {
        g.status_entry = entry;
        g.status_state = u32(entry + 2);
      } else {
        logf("game: the status screen's entry %#x does not open the way this build's does - not used", entry);
      }
    } else {
      logf("game: status screen chain: its two state-flag operands disagree - not used");
    }
  }
  count(g.status_entry != 0);

  // 3. The player: the damage routine gives HP and the condition word it sets
  //    the dying bit in (its high byte, so the word is one byte in front);
  //    a new game sets HP and its maximum side by side.
  if (const uintptr_t d = find_one("player damage", kSigDamage)) {
    const uint32_t hp = u32(d + 3);
    if (u32(d + 29) == hp && u32(d + 43) == hp) {
      g.hp = hp;
      g.condition = u32(d + 51) - 1;
      // `sub cx,ax` gone, the routine stores HP back unchanged and answers
      // "alive"; every test after it sets its own flags.
      prepare(kSitePlayerDamage, d + 16, "66 2B C8", mem::nops(3));
    } else {
      logf("game: player damage: its HP operands disagree - not used");
    }
  }
  if (g.hp)
    if (const uintptr_t n = find_one("new game HP", kSigNewGameHp)) {
      const uint32_t hp = u32(n + 7), mx = u32(n + 13);
      if (hp == g.hp && mx == hp + 2) g.max_hp = mx;
      else logf("game: new game HP: %#x/%#x are not the player's - the maximum is not used", hp, mx);
    }
  count(g.hp && g.max_hp && g.condition);

  // 4. The room's entities: the walk from index 2 to the list's live end, and
  //    the index arithmetic that gives the array's base.
  if (const uintptr_t w = find_one("entity walk", kSigEntityWalk)) {
    const uint32_t end = u32(w + 2), second = u32(w + 8);
    if (u32(w + 20) == second)
      if (const uintptr_t b = find_one("entity list base", kSigEntityBase)) {
        const uint32_t base = u32(b + 3);
        if (second == base + 4u * kFirstRoomEntity && end > base && end - base <= 0x400) {
          g.entities = base;
          g.entities_end = end;
        } else {
          logf("game: the entity list at %#x does not line up with its walk (%#x, end %#x) - not used", base, second,
               end);
        }
      }
  }
  count(g.entities != 0);

  // 5. The play time, and the state flags again.
  const uintptr_t timer_at = find_one("play time", kSigTimer);
  if (const uintptr_t t = timer_at) {
    const uint32_t flags = u32(t + 2), timer = u32(t + 21), base = u32(t + 40);
    if (u32(t + 10) == flags && u32(t + 50) == timer && u32(t + 61) == timer && u32(t + 66) == base &&
        (!g.flags || flags == g.flags)) {
      g.timer = timer;
      g.tick_base = base;
      if (!g.flags) g.flags = flags;
    } else {
      logf("game: play time: its operands do not line up with the state flags - not used");
    }
  }
  count(g.timer != 0);

  // 5b. What the play time is measured against, for Freeze play time. The
  //     save's update and the ending's both have to call the one VSync(-1),
  //     name the same base and total, and VSync's method has to answer the
  //     field the pacing loop raises. Then the store each makes to the total
  //     is a site the freeze takes out.
  if (g.timer && g.tick_base) {
    const uintptr_t e = find_one("ending's play time", kSigTimerEnd);
    const uintptr_t v = find_one("VSync", kSigVSync);
    const uintptr_t m = find_one("VSync count", kSigVSyncCount);
    const uintptr_t b = find_one("vblank", kSigVBlank);
    if (e && v && m && b) {
      const uintptr_t save_calls = timer_at + 38 + static_cast<int32_t>(u32(timer_at + 34));
      const uintptr_t end_calls = e + 7 + static_cast<int32_t>(u32(e + 3));
      const uint32_t off = u32(m + 24);
      if (save_calls == v && end_calls == v && u32(e + 9) == g.tick_base && u32(e + 15) == g.timer &&
          u32(e + 27) == g.timer && u32(b + 2) == off && u32(b + 17) == off && off >= 4 && off < 0x10000) {
        g.vsync_obj = u32(v + 2);
        g.vsync_slot = mem::read<uint8_t>(v + 15);
        g.vsync_method = m;
        g.vblank_off = off;
        prepare(kSiteTimerSave, timer_at + 59, "89 15 ?? ?? ?? ??", mem::nops(6));
        prepare(kSiteTimerEnd, e + 25, "89 15 ?? ?? ?? ??", mem::nops(6));
      } else {
        logf("game: the play time's clock: the sites do not agree (VSync %#x, called from %#x and %#x; count +%#x) "
             "- Freeze play time is not used",
             static_cast<unsigned>(v), static_cast<unsigned>(save_calls), static_cast<unsigned>(end_calls), off);
      }
    }
  }
  count(timer_freeze_known());

  // 6. The end of a save: the ink ribbon it takes and the save count it raises.
  if (const uintptr_t s = find_one("save", kSigSave)) {
    const uint32_t flags = u32(s + 1), sc = u32(s + 44);
    const int ribbon = mem::read<uint8_t>(s + 21);
    if (u32(s + 64) == flags && u32(s + 75) == flags && u32(s + 49) == sc && u32(s + 84) == sc &&
        (!g.flags || flags == g.flags) && ribbon == items::kInkRibbonId) {
      g.save_count = sc;
      prepare(kSiteSaveCount, s + 82, "FE 05 ?? ?? ?? ??", mem::nops(6));
      prepare(kSiteInkRibbon, s + 8, "75 20", {0xEB, 0x20});
    } else {
      logf("game: save: its operands do not line up (ribbon id %d) - not used", ribbon);
    }
  }
  count(g.save_count != 0);

  // 7. The inventory: which block is in play, and the block's own shape.
  if (const uintptr_t v = find_one("inventory select", kSigInvSelect)) {
    const uint32_t idx = u32(v + 20), base = u32(v + 47), ptr = u32(v + 52);
    const uint32_t stride = 5u << mem::read<uint8_t>(v + 45);
    if (u32(v + 29) == idx && u32(v + 36) == idx && stride == 0x140) {
      g.inv_index = idx;
      g.inv_base = base;
      g.inv_ptr = ptr;
      g.inv_stride = stride;
      g.inv_select = v;
    } else {
      logf("game: inventory select: its operands disagree (stride %#x) - not used", stride);
    }
  }
  if (g.inv_ptr)
    if (const uintptr_t f = find_one("find_item_slot", kSigFindSlot)) {
      const uint32_t ptr = u32(f + 7);
      const int slots_off = mem::read<uint16_t>(f + 18);
      if (ptr == g.inv_ptr && slots_off == kSlotsOff) {
        g.inv_slots_off = slots_off;
        g.find_slot = f;
      } else {
        logf("game: find_item_slot: it walks %#x with its count at +%#x - not used", ptr, slots_off);
      }
    }
  // The item box: the box screen's list names the same pointer, the box's size
  // and its offset twice.
  if (g.inv_ptr)
    if (const uintptr_t b = find_one("item box", kSigItemBox)) {
      const int size = mem::read<uint8_t>(b + 9), off = mem::read<uint8_t>(b + 50);
      if (u32(b + 43) == g.inv_ptr && size == kBoxSlots && off == kBoxOff && mem::read<uint8_t>(b + 54) == off &&
          off + kBoxSlots * kSlotStride <= kEquippedOff)
        g.box_off = off;
      else
        logf("game: item box: %d slots at +%#x of %#x - not used", size, off, u32(b + 43));
    }
  // The status screen's icon-rebuild bit, named by the game's own item removal
  // next to the put-away byte and the saved slot.
  if (g.status_state)
    if (const uintptr_t r = find_one("remove_item_slot", kSigRemoveSlot)) {
      const uint32_t st = g.status_state;
      if (u32(r + 2) == st + kPutAwayOff && u32(r + 13) == st + kSavedSlotOff && u32(r + 28) == st + kSavedSlotOff &&
          u32(r + 33) == st + kScreenFlagsOff && u32(r + 43) == st + kScreenFlagsOff)
        g.icons_known = true;
      else
        logf("game: remove_item_slot: its operands are not the status screen's (%#x) - icons are not rebuilt", st);
    }
  // The typewriter's ribbon test calls the same find_item_slot for the same id
  // the save takes.
  if (g.find_slot)
    if (const uintptr_t t = find_one("typewriter", kSigTypewriter)) {
      const uintptr_t target = t + 11 + static_cast<int32_t>(u32(t + 7));
      const int ribbon = mem::read<uint8_t>(t + 2);
      if (target == g.find_slot && ribbon == items::kInkRibbonId)
        prepare(kSiteInkCheck, t + 16, "7C 1A", {0x90, 0x90});
      else
        logf("game: typewriter: it calls %#x for item %d - not used", static_cast<unsigned>(target), ribbon);
    }
  // The item table: a pickup reads how much of an item one slot holds from its
  // second column, so the table starts one byte in front of that.
  if (g.inv_ptr)
    if (const uintptr_t p = find_one("item table", kSigItemTable))
      if (u32(p + 2) == g.inv_ptr) {
        g.item_table = u32(p + 16) - 1;
        // The records run to the last item ("no item"); past that the data is
        // something else, so the end is where the kind column stops making sense.
        int last = 0;
        for (int id = 1; id <= 0xFF; ++id) {
          uint8_t kind = 0, max = 0;
          if (!mem::read_safe(g.item_table + 4u * id, &kind) ||
              !mem::read_safe(g.item_table + 4u * id + 1, &max))
            break;
          if (kind < 1 || kind > 8 || max < 1) break;
          last = id;
        }
        g.item_table_last = last;
      }
  count(items_known());

  // 8. The input poll's button words, so the panel can take the keyboard.
  if (const uintptr_t i = find_one("button state stores", kSigInputMain)) {
    g.in_held = u32(i + 2);
    g.in_pressed = u32(i + 8);
    g.in_repeat = u32(i + 14);
    prepare(kSiteInputMain, i, "89 2D ?? ?? ?? ?? 89 15 ?? ?? ?? ?? 89 3D ?? ?? ?? ??", mem::nops(18));
    if (const uintptr_t r = find_one("button repeat store", kSigInputRepeat)) {
      if (u32(r + 2) == g.in_repeat) prepare(kSiteInputRepeat, r, "89 3D ?? ?? ?? ??", mem::nops(6));
      else logf("game: the second repeat store names %#x, not %#x - the input guard is not used", u32(r + 2),
                g.in_repeat);
    }
  }
  count(g_site_ok[kSiteInputMain] && g_site_ok[kSiteInputRepeat]);

  // 9. Cutscenes: the letterbox's bit in the save flags, the movie's in the
  //    state flags, the movie player's end byte, and the message box (a skip
  //    waits for the player at a page or a choice).
  if (g.save_flags)
    if (const uintptr_t c = find_one("letterbox", kSigCinema)) {
      if (u32(c + 1) == g.save_flags) g.cinema_known = true;
      else logf("game: the letterbox reads %#x, not the save flags %#x - cutscenes are not seen", u32(c + 1),
                g.save_flags);
    }
  if (g.flags)
    if (const uintptr_t m = find_one("movie frame", kSigMovieFrame)) {
      if (u32(m + 2) == g.flags) g.movie_known = true;
      else logf("game: the movie frame tests %#x, not the state flags %#x - movies are not seen", u32(m + 2), g.flags);
    }
  if (const uintptr_t m = find_one("movie end", kSigMovieEnd)) {
    const uint32_t end = u32(m + 7), chain = u32(m + 13);
    if (u32(m + 0x57) == end && u32(m + 0x61) == chain && u32(m + 0x6E) == end) {
      g.movie_end = end;
      g.movie_chain = chain;
    } else {
      logf("game: movie end: its operands disagree - movies are not ended");
    }
  }
  if (const uintptr_t m = find_one("message box", kSigMessage))
    if (const uintptr_t c = find_one("message box call", kSigMessageCall)) {
      const uint32_t flags = u32(c + 1), box = u32(c + 30), state_off = u32(m + 33);
      const uintptr_t target = c + 39 + static_cast<int32_t>(u32(c + 35));
      if (u32(c + 21) == flags && u32(c + 40) == box && box + kMessageFlagsOff == flags && target == m &&
          state_off > kMessageFlagsOff && state_off < kMessageFlagsOff + 8) {
        g.msg_flags = flags;
        g.msg_state = box + state_off;
      } else {
        logf("game: message box: its operands disagree - a skip does not stop for messages");
      }
    }
  if (const uintptr_t v = find_one("voice player", kSigVoicePlay)) {
    const uint32_t path = u32(v + 1), state = u32(v + 17);
    char dir[16] = {};
    if (image::game_image().contains(path)) std::memcpy(dir, reinterpret_cast<const void*>(path), sizeof(dir) - 1);
    if (const uintptr_t c = find_one("voice stop", kSigVoiceStop)) {
      if (!std::strcmp(dir, "/data_a/voice/") && u32(c + 1) == state && u32(c + 11) == state) g.voice_state = state;
      else logf("game: the voice player and its stop disagree (%#x, %#x, \"%s\") - voices are not ended", state,
                u32(c + 1), dir);
    }
  }
  count(cutscenes_known());

  // 10. The countdown (the Mercenaries' clock, and the main game's timed
  //     scenes). Both sites name the frame count, the hundredths and last
  //     frame's save flags; the second copies those flags out of the [esi+]
  //     block, which puts the block at the save flags less that offset, and the
  //     clock the first takes a second off at its own offset in it - the
  //     address the Mercenaries' bonus for a kill adds to directly. The first
  //     also calls the radar in the clock's place, which counts the missile at
  //     the end in (discover_missile): the same switch freezes both.
  uint32_t bonus_takings = 0;  // the money the same bonus pays, for step 11
  uintptr_t clock_tick = 0;    // the countdown tick's match, for the radar
  if (g.save_flags && g.flags)
    if (const uintptr_t t = find_one("countdown tick", kSigCountdownTick))
      if (const uintptr_t f = find_one("countdown frames", kSigCountdownFrames))
        if (const uintptr_t b = find_one("countdown bonus", kSigCountdownBonus)) {
          const uint32_t frames = u32(t + 69), hundredths = u32(t + 112), last = u32(t + 14);
          const uint32_t block = g.save_flags - u32(f + 53), clock = block + u32(t + 91);
          if (u32(t + 1) == g.save_flags && u32(t + 54) == g.flags && u32(t + 96) == g.flags &&
              u32(t + 78) == last && u32(t + 105) == frames && u32(f + 22) == frames && u32(f + 15) == hundredths &&
              u32(f + 36) == hundredths && u32(f + 45) == hundredths && u32(f + 58) == last && u32(b + 7) == clock) {
            g.countdown = clock;
            clock_tick = t;
            bonus_takings = u32(b + 26);
            prepare(kSiteCountdownTick, t + 88, "66 FF 8E ?? ?? ?? ??", mem::nops(7));
            prepare(kSiteCountdownFrames, f + 11, "75 26", {0xEB, 0x26});
          } else {
            logf("game: the countdown's sites do not agree (clock %#x, the bonus adds to %#x) - it cannot be frozen",
                 clock, u32(b + 7));
          }
        }
  count(countdown_known());
  if (clock_tick) discover_missile(clock_tick);
  count(missile_known());

  // 11. The Mercenaries money. Both results screens read the takings and count
  //     into the bank, each naming the bank three times and its cap once; the
  //     full one sums the takings with the countdown, and the bonus for a kill
  //     pays into the same takings.
  if (g.countdown)
    if (const uintptr_t r = find_one("results tally", kSigMoneyTally))
      if (const uintptr_t h = find_one("half tally", kSigMoneyHalf)) {
        const uint32_t takings = u32(r + 19), bank = u32(r + 43), cap = u32(r + 48);
        if (u32(r + 3) == g.countdown && takings == bonus_takings && u32(h + 2) == takings && u32(r + 56) == bank &&
            u32(r + 69) == bank && u32(h + 29) == bank && u32(h + 47) == bank && u32(h + 55) == bank &&
            u32(h + 34) == cap && cap > 0 && cap <= 0x7FFFFFFF) {
          g.takings = takings;
          g.bank = bank;
          g.bank_max = cap;
        } else {
          logf("game: the Mercenaries money: the results name takings %#x and bank %#x (cap %u), the bonus pays "
               "%#x - not used",
               takings, bank, cap, bonus_takings);
        }
      }
  count(money_known());

  // 12. The reloading tool's proficiency: the batch counts in the inventory
  //     block, the level table and the recipes (discover_mixing).
  if (!g_mix.known) discover_mixing();
  count(mixing_known());

  // 13. The recovery total: the healing adds to it and the ending's rank reads
  //     it, each by address. The healing also names the player's HP, its
  //     maximum and the condition word it cures the poison in; the rank's steps
  //     have to go down in total and up in score.
  if (g.hp && g.max_hp && g.condition)
    if (const uintptr_t h = find_one("healing", kSigHeal))
      if (const uintptr_t r = find_one("rank: recovery", kSigRankRecovery)) {
        const uint32_t total = u32(h + 47);
        RankStep steps[kRankSteps];
        bool ordered = u32(r + 10) <= 0x7FFFFFFF;
        for (int i = 0; i < kRankSteps; ++i) {
          const uintptr_t s = r + 8 + kRankStepBytes * i;
          steps[i].most = static_cast<int>(u32(s + 2));
          steps[i].score = mem::read<uint8_t>(s + 9);
          if (i > 0 && (steps[i].most >= steps[i - 1].most || steps[i].score <= steps[i - 1].score)) ordered = false;
        }
        if (u32(h + 79) == total && u32(r + 2) == total && u32(h + 10) == g.condition && u32(h + 41) == g.hp &&
            u32(h + 58) == g.hp && u32(h + 88) == g.hp && u32(h + 70) == g.max_hp && ordered) {
          g.recovery = total;
          std::memcpy(g.rank_steps, steps, sizeof(steps));
          prepare(kSiteRecovery, h + 77, "89 2D ?? ?? ?? ??", mem::nops(6));
        } else {
          logf("game: the recovery total: the healing adds to %#x, the rank reads %#x (steps %s) - not used", total,
               u32(r + 2), ordered ? "in order" : "out of order");
        }
      }
  count(recovery_known());

  // 14. The memory cards and the memory card screen, for the save file manager
  //     (discover_cards).
  if (!g_cards.known) discover_cards();
  count(cards_known());

  // 15. The clear count and the epilogues (discover_clears), after the memory
  //     cards: the count has to be where their list reads it in a save.
  if (!g.clears) discover_clears();
  count(clears_known());

  // 16. Jill's outfit: the value the model loader reads, and the room load's
  //     test that decides whether to load the model (discover_outfit).
  if (!g.outfit) discover_outfit();
  count(outfit_known());

  // The stage and the room follow the play time in the save block; they are
  // read for the panel's read-out only, from the offsets this build has.
  if (g.timer) {
    g.stage = g.timer + 0x4E;
    g.room = g.timer + 0x50;
  }

  std::snprintf(g_status, sizeof(g_status), "%d of %d anchor groups found", found, wanted);
  logf("game: discovery: %s - tasks %#x/%#x (stride %#x), status screen %#x (state %#x), flags %#x, save flags %#x, "
       "HP %#x, condition %#x, entities %#x..[%#x], play time %#x (base %#x, vblanks [%#x]+%#x), save count %#x, "
       "inventory %#x (base %#x, index %#x), "
       "item table %#x (%d records), buttons %#x/%#x/%#x, letterbox %s, movie %s (end %#x), message box %#x, "
       "voice %#x, countdown %#x, missile %#x, takings %#x, bank %#x (to %u), batch counts +%#x, recovery %#x "
       "(scored in full up to %d, nothing over %d), clears %#x (%d epilogues), outfit %#x",
       g_status, g.task_state_tab, g.task_entry_tab, g.task_stride, g.status_entry, g.status_state, g.flags,
       g.save_flags, g.hp, g.condition, g.entities, g.entities_end, g.timer, g.tick_base, g.vsync_obj, g.vblank_off,
       g.save_count, g.inv_ptr, g.inv_base,
       g.inv_index, g.item_table, g.item_table_last, g.in_held, g.in_pressed, g.in_repeat,
       g.cinema_known ? "yes" : "no", g.movie_known ? "yes" : "no", g.movie_end, g.msg_flags, g.voice_state,
       g.countdown, g.missile, g.takings, g.bank, g.bank_max, g_mix.count_off, g.recovery,
       g.rank_steps[kRankSteps - 1].most, g.rank_steps[0].most, g.clears, g.epilogues, g.outfit);
  // The screens are what the panel needs to come up at all; each cheat checks
  // its own anchors.
  if (!(g.task_entry_tab && g.status_entry && g.inv_ptr)) return false;
  InterlockedExchange(&g_ready, 1);
  return true;
}

bool ready() { return g_ready != 0; }
const char* status_text() { return g_status; }

int task_entry(int id) {
  if (!g.task_entry_tab || id < 0 || id > 7) return 0;
  return static_cast<int>(u32(g.task_entry_tab + static_cast<uintptr_t>(id) * g.task_stride));
}
int task_state(int id) {
  if (!g.task_state_tab || id < 0 || id > 7) return 0;
  return peek<uint16_t>(g.task_state_tab + static_cast<uintptr_t>(id) * g.task_stride);
}

bool in_game() { return g_ready && bag_block() != 0; }
bool status_screen() {
  return g_ready && g.status_entry &&
         static_cast<uint32_t>(task_entry(kStatusTask)) == g.status_entry && task_state(kStatusTask) != 0;
}
bool player_dead() { return g.flags && (u32(g.flags) & kDeadBit) != 0; }

bool player(Player* out) {
  if (!g.hp || !g.max_hp || !g.condition) return false;
  out->hp = peek<int16_t>(g.hp);
  out->max_hp = peek<int16_t>(g.max_hp);
  out->condition = peek<uint16_t>(g.condition);
  return true;
}
bool set_player_hp(int hp) {
  if (!g.hp) return false;
  poke<int16_t>(g.hp, static_cast<int16_t>(hp));
  return true;
}
bool set_player_condition(int condition) {
  if (!g.condition) return false;
  poke<uint16_t>(g.condition, static_cast<uint16_t>(condition));
  return true;
}

bool entities_known() { return g.entities != 0 && g.entities_end != 0; }

int entities(Entity* out, int max) {
  if (!entities_known()) return 0;
  uintptr_t end = 0;
  if (!mem::read_safe(g.entities_end, &end)) return 0;
  const uintptr_t first = g.entities + 4u * kFirstRoomEntity;
  if (end <= first || end > g.entities + 0x400) return 0;
  int n = 0;
  for (uintptr_t slot = first; slot < end && n < max; slot += 4) {
    uintptr_t e = 0;
    if (!mem::read_safe(slot, &e) || !e || !mem::readable(e, kEntityHp + 2)) continue;
    if (!mem::read<uint32_t>(e)) continue;  // the walk's own liveness test
    out[n].index = static_cast<int>((slot - g.entities) / 4);
    out[n].at = e;
    out[n].type = peek<uint8_t>(e + kEntityType);
    out[n].hp = peek<int16_t>(e + kEntityHp);
    ++n;
  }
  return n;
}

bool set_entity_hp(uintptr_t at, int hp) {
  if (!entities_known() || !at || !mem::readable(at, kEntityHp + 2)) return false;
  poke<int16_t>(at + kEntityHp, static_cast<int16_t>(hp));
  return true;
}

bool counters_ready() { return g.timer && g.save_count; }

// --- the play time ---------------------------------------------------------------------
namespace {
// The vblank count VSync(-1) answers, read straight out of the system object -
// once the object's vtable has been seen to send VSync to the method that
// discovery found answering it. The object is checked again whenever the
// pointer to it changes.
uintptr_t g_vsync_seen = 0;
bool g_vsync_mismatch_logged = false;
bool vblanks(uint32_t* out) {
  if (!g.vsync_obj) return false;
  const uintptr_t obj = u32(g.vsync_obj);
  if (!obj) return false;
  if (obj != g_vsync_seen) {
    uintptr_t vtable = 0, method = 0;
    if (!mem::read_safe(obj, &vtable) || !mem::read_safe(vtable + g.vsync_slot, &method) ||
        method != g.vsync_method || !mem::readable(obj + g.vblank_off, 4)) {
      if (!g_vsync_mismatch_logged) {
        g_vsync_mismatch_logged = true;
        logf("game: the system object %#x does not answer VSync with %#x (its vtable has %#x) - the play time's "
             "vblank count is not read",
             static_cast<unsigned>(obj), static_cast<unsigned>(g.vsync_method), static_cast<unsigned>(method));
      }
      return false;
    }
    g_vsync_seen = obj;
  }
  *out = peek<uint32_t>(obj + g.vblank_off);
  return true;
}
}  // namespace

// The total, plus the vblanks since its base: what the next save would write.
// The Mercenaries' 0x80 skips both of the game's adds, so there the total is
// all there is.
uint32_t game_timer() {
  if (!g.timer) return 0;
  const uint32_t total = peek<uint32_t>(g.timer);
  uint32_t now = 0;
  if (!g.tick_base || mercenaries() || !vblanks(&now)) return total;
  return total + (now - peek<uint32_t>(g.tick_base));
}
bool set_game_timer(uint32_t v) {
  if (!g.timer) return false;
  poke<uint32_t>(g.timer, v);
  // Without this the next save would add every vblank since the last one (or
  // since the session started) on top of the new value.
  uint32_t now = 0;
  if (g.tick_base && vblanks(&now)) poke<uint32_t>(g.tick_base, now);
  return true;
}
bool timer_freeze_known() {
  return g.timer && g.tick_base && g.vsync_obj && g_site_ok[kSiteTimerSave] && g_site_ok[kSiteTimerEnd];
}
bool settle_game_timer() {
  uint32_t now = 0;
  if (!g.timer || !g.tick_base || !vblanks(&now)) return false;
  if (!mercenaries()) poke<uint32_t>(g.timer, peek<uint32_t>(g.timer) + (now - peek<uint32_t>(g.tick_base)));
  poke<uint32_t>(g.tick_base, now);
  return true;
}
bool restart_game_timer() {
  uint32_t now = 0;
  if (!g.tick_base || !vblanks(&now)) return false;
  poke<uint32_t>(g.tick_base, now);
  return true;
}
int save_count() { return g.save_count ? peek<uint8_t>(g.save_count) : -1; }
bool set_save_count(int n) {
  if (!g.save_count) return false;
  poke<uint8_t>(g.save_count, static_cast<uint8_t>(n < 0 ? 0 : n > 99 ? 99 : n));
  return true;
}
int stage() { return g.stage ? peek<int16_t>(g.stage) : -1; }
int room() { return g.room ? peek<int16_t>(g.room) : -1; }

// --- the recovery total -------------------------------------------------------------------
bool recovery_known() { return g.recovery != 0; }
int recovery() {
  if (!g.recovery) return -1;
  // A u32 that only healing items add to - a few hundred at a time - so the
  // top bit is never set; an int is enough.
  const uint32_t v = peek<uint32_t>(g.recovery);
  return v > 0x7FFFFFFFu ? 0x7FFFFFFF : static_cast<int>(v);
}
bool set_recovery(int total) {
  if (!g.recovery) return false;
  poke<uint32_t>(g.recovery, static_cast<uint32_t>(total < 0 ? 0 : total > kRecoveryMax ? kRecoveryMax : total));
  return true;
}
bool recovery_scale(RankStep* out) {
  if (!g.recovery) return false;
  for (int i = 0; i < kRankSteps; ++i) out[i] = g.rank_steps[i];
  return true;
}
// The rank's own walk (0x446182): every step the total is not above sets its
// score, so the last of them, the best, is the one that counts.
int recovery_score(int total) {
  if (!g.recovery || total < 0) return -1;
  int score = 0;
  for (const RankStep& s : g.rank_steps)
    if (total <= s.most) score = s.score;
  return score;
}

// --- the clear count and the epilogues ----------------------------------------------------
bool clears_known() { return g.clears && g.epilogues > 0; }
int clears() { return g.clears ? peek<uint16_t>(g.clears) : -1; }
bool set_clears(int n) {
  if (!g.clears) return false;
  poke<uint16_t>(g.clears, static_cast<uint16_t>(n < 0 ? 0 : n > kClearsMax ? kClearsMax : n));
  return true;
}
int epilogues() { return g.epilogues; }

// --- Jill's outfit ------------------------------------------------------------------------
bool outfit_known() { return g.outfit && g.character && g.model_mark && g_site_ok[kSiteModelReload]; }
const char* outfit_name(int o) { return o >= 0 && o < kOutfits ? kOutfitNames[o] : "?"; }
int outfit() {
  if (!outfit_known()) return -1;
  // The loader's switch sends anything past the last outfit to the regular one.
  const uint32_t v = peek<uint32_t>(g.outfit);
  return v < static_cast<uint32_t>(kOutfits) ? static_cast<int>(v) : 0;
}
int character() { return outfit_known() ? peek<int16_t>(g.character) : -1; }
int outfit_next(bool* boutique) {
  if (boutique) *boutique = false;
  const int who = character(), o = outfit();
  if (who < 0 || o < 0) return -1;
  // A boutique type loads model pl0<type>: types 2..6 are the models outfits
  // 1..5 load, and type 7's model is Normal's (pl07.pld is pl00.pld, byte for
  // byte).
  if (who >= 2 && who < g.first_other) {
    if (boutique) *boutique = true;
    return who <= 6 ? who - 1 : 0;
  }
  return o;
}
bool jill_in_play() {
  if (!outfit_known()) return false;
  const int who = peek<int16_t>(g.character), model = peek<uint8_t>(g.model_type);
  return who >= 0 && who < g.first_other && model < g.first_other;
}
bool set_outfit(int o) {
  if (!outfit_known() || o < 0 || o >= kOutfits) return false;
  poke<uint32_t>(g.outfit, static_cast<uint32_t>(o));
  // A boutique outfit is a character type of its own, which the loader takes
  // before the value: Jill goes back to her regular type - the room load then
  // loads her model for the change of type - and so does the type kept for her
  // while another character is played, which the room load would otherwise put
  // back. The boutique's own script sets the two together.
  const int who = peek<int16_t>(g.character), kept = peek<int16_t>(g.kept_character);
  if (who >= 2 && who < g.first_other) poke<int16_t>(g.character, 0);
  if (kept >= 2 && kept < g.first_other) poke<int16_t>(g.kept_character, 0);
  return true;
}
bool mark_model_load() {
  if (!outfit_known()) return false;
  poke<uint8_t>(g.model_mark, kModelMark);
  return true;
}
bool model_loaded() { return outfit_known() && peek<uint8_t>(g.model_mark) != kModelMark; }

// --- the inventory -----------------------------------------------------------------------
bool items_known() { return g.inv_ptr && g.inv_base && g.inv_stride && g.inv_slots_off; }
int inventory_index() { return g.inv_index ? static_cast<int>(u32(g.inv_index)) : -1; }

bool read_bag(Bag* out) {
  const uintptr_t b = bag_block();
  if (!items_known() || !b) return false;
  const int slots = peek<uint8_t>(b + g.inv_slots_off);
  out->slots = slots > kMaxSlots ? kMaxSlots : slots;
  const int eq = peek<uint8_t>(b + kEquippedOff);
  out->equipped = eq == 0xFF ? -1 : eq;
  out->weapon = peek<uint8_t>(b + kWeaponOff);
  for (int i = 0; i < kMaxSlots; ++i) {
    const uintptr_t s = b + static_cast<uintptr_t>(i) * kSlotStride;
    out->slot[i].id = peek<uint8_t>(s + kSlotId);
    out->slot[i].count = peek<uint8_t>(s + kSlotCount);
    out->slot[i].flags = peek<uint8_t>(s + kSlotFlags);
  }
  return true;
}

bool box_known() { return items_known() && g.box_off != 0; }

bool read_box(ItemSlot* out) {
  const uintptr_t b = bag_block();
  if (!box_known() || !b) return false;
  for (int i = 0; i < kBoxSlots; ++i) {
    const uintptr_t s = b + g.box_off + static_cast<uintptr_t>(i) * kSlotStride;
    out[i].id = peek<uint8_t>(s + kSlotId);
    out[i].count = peek<uint8_t>(s + kSlotCount);
    out[i].flags = peek<uint8_t>(s + kSlotFlags);
  }
  return true;
}

namespace {
void put_slot(uintptr_t s, int id, int count, int flags) {
  poke<uint8_t>(s + kSlotId, static_cast<uint8_t>(id));
  poke<uint8_t>(s + kSlotCount, static_cast<uint8_t>(id ? count : 0));
  if (!id) poke<uint8_t>(s + kSlotFlags, 0);
  else if (flags >= 0) poke<uint8_t>(s + kSlotFlags, static_cast<uint8_t>(flags));
}
bool slot_values_ok(int id, int count, int flags) {
  return id >= 0 && id <= 0xFF && count >= 0 && count <= 0xFF && flags <= 0xFF;
}
}  // namespace

bool write_slot(int index, int id, int count, int flags) {
  const uintptr_t b = bag_block();
  if (!items_known() || !b || index < 0 || index >= kMaxSlots || !slot_values_ok(id, count, flags)) return false;
  put_slot(b + static_cast<uintptr_t>(index) * kSlotStride, id, count, flags);
  mark_icons_stale();
  return true;
}

bool write_box_slot(int index, int id, int count, int flags) {
  const uintptr_t b = bag_block();
  if (!box_known() || !b || index < 0 || index >= kBoxSlots || !slot_values_ok(id, count, flags)) return false;
  put_slot(b + g.box_off + static_cast<uintptr_t>(index) * kSlotStride, id, count, flags);
  return true;
}

bool set_equipped(int index) {
  const uintptr_t b = bag_block();
  if (!items_known() || !b || index < -1 || index >= kMaxSlots) return false;
  const int id = index < 0 ? 0 : peek<uint8_t>(b + static_cast<uintptr_t>(index) * kSlotStride + kSlotId);
  poke<uint8_t>(b + kEquippedOff, index < 0 ? 0xFF : static_cast<uint8_t>(index));
  poke<uint8_t>(b + kWeaponOff, static_cast<uint8_t>(id));
  mark_icons_stale();
  return true;
}

bool mark_icons_stale() {
  if (!g.icons_known) return false;
  poke<uint32_t>(g.status_state + kScreenFlagsOff, peek<uint32_t>(g.status_state + kScreenFlagsOff) | kIconsStale);
  return true;
}

bool remove_slot(int index) {
  const uintptr_t b = bag_block();
  if (!items_known() || !b || index < 0 || index >= kMaxSlots) return false;
  int n = peek<uint8_t>(b + g.inv_slots_off);
  if (n > kMaxSlots) n = kMaxSlots;
  if (index >= n) return write_slot(index, 0, 0, 0);
  for (int i = index; i + 1 < n; ++i)
    for (int k = 0; k < kSlotStride; ++k)
      poke<uint8_t>(b + i * kSlotStride + k, peek<uint8_t>(b + (i + 1) * kSlotStride + k));
  for (int k = 0; k < kSlotStride; ++k) poke<uint8_t>(b + (n - 1) * kSlotStride + k, 0);
  // The equipped slot follows its weapon up; while the status screen holds the
  // weapon back it is the screen's saved copy that does.
  const bool away = weapon_put_away();
  const uintptr_t eq = away ? g.status_state + kSavedSlotOff : b + kEquippedOff;
  const int e = peek<uint8_t>(eq);
  if (e != 0xFF && e > index) poke<uint8_t>(eq, static_cast<uint8_t>(e - 1));
  mark_icons_stale();
  return true;
}

bool weapon_put_away() { return g.status_state && peek<uint8_t>(g.status_state + kPutAwayOff) != 0; }

bool item_rules_known() { return g.item_table != 0 && g.item_table_last > 0; }
int item_table_last() { return g.item_table_last; }
int item_max(int id) {
  if (item_rules_known() && id > 0 && id <= g.item_table_last) return peek<uint8_t>(g.item_table + 4u * id + 1);
  return items::maximum(id);
}
int item_kind(int id) {
  if (item_rules_known() && id > 0 && id <= g.item_table_last) return peek<uint8_t>(g.item_table + 4u * id);
  return items::kind(id);
}
bool item_stacks(int id) {
  const int k = item_kind(id);
  return (k == 2 || k == 5 || k == 6) && item_max(id) > 1;
}

// --- cutscenes ----------------------------------------------------------------------------
bool cutscenes_known() { return g.cinema_known && g.movie_known; }
bool cinematic() { return g.cinema_known && (u32(g.save_flags) & kCinemaBit) != 0; }
bool movie_playing() { return g.movie_known && (u32(g.flags) & kMovieBit) != 0; }
bool movie_end_known() { return g.movie_end != 0; }
bool end_movie() {
  if (!g.movie_end || !movie_playing()) return false;
  // What the player's own frame writes when the stream runs out (0x42FB2F):
  // the next movie frame closes it the way it closes any movie that has ended.
  poke<uint8_t>(g.movie_end, 1);
  poke<uint8_t>(g.movie_chain, 0);
  return true;
}
bool message_up() { return g.msg_flags && (peek<uint8_t>(g.msg_flags) & kMessageUp) != 0; }
bool voice_known() { return g.voice_state != 0; }
bool voice_playing() { return g.voice_state && peek<uint8_t>(g.voice_state) != 0; }
bool end_voice() {
  // A line starting (1) or playing (2); one already stopping (3) is left be.
  const uint8_t st = g.voice_state ? peek<uint8_t>(g.voice_state) : 0;
  if (st != 1 && st != 2) return false;
  // What the game itself does to cut a line short (0x42B69C): the voice
  // player's next frame stops the sound and clears the "line playing" flag.
  poke<uint8_t>(g.voice_state, kVoiceStop);
  return true;
}
bool message_waiting() {
  if (!message_up() || !g.msg_state) return false;
  const uint8_t st = peek<uint8_t>(g.msg_state);
  return st == kMessagePage || st == kMessageChoice || st == kMessageEnd;
}

// --- the countdown -----------------------------------------------------------------------
bool countdown_known() { return g.countdown && g_site_ok[kSiteCountdownTick] && g_site_ok[kSiteCountdownFrames]; }
bool countdown_running() {
  // The radar receiver's bit sends the gameplay loop down its own path instead.
  const uint32_t f = g.countdown ? u32(g.save_flags) : 0;
  return (f & kCountdownBit) && !(f & kRadarBit);
}
int countdown_seconds() { return g.countdown ? peek<int16_t>(g.countdown) : -1; }

bool missile_known() { return g.missile && g_site_ok[kSiteMissileStep]; }
bool missile_running() {
  if (!g.missile || (u32(g.save_flags) & (kCountdownBit | kRadarBit)) != (kCountdownBit | kRadarBit)) return false;
  const int sub = peek<uint8_t>(g.radar + g.radar_sub);
  return peek<uint8_t>(g.radar) == g.radar_run && (sub == g.radar_far || sub == g.radar_near);
}
int missile_seconds() {
  if (!g.missile) return -1;
  // The radar takes its step off once a frame of the gameplay loop's.
  const int32_t left = peek<int32_t>(g.missile), per_second = g.missile_step * kLoopFps;
  return left <= 0 ? 0 : static_cast<int>((left + per_second - 1) / per_second);
}

// --- the Mercenaries money ------------------------------------------------------------------
bool money_known() { return g.takings && g.bank && g.bank_max; }
bool mercenaries() { return g.flags && (u32(g.flags) & kMercenariesBit) != 0; }
int takings() { return money_known() ? peek<uint16_t>(g.takings) : -1; }
int bank() {
  if (!money_known()) return -1;
  // The results hold it at the cap and the load refuses a negative value, so
  // the top bit is never set; an int is enough.
  const uint32_t v = peek<uint32_t>(g.bank);
  return v > 0x7FFFFFFFu ? 0x7FFFFFFF : static_cast<int>(v);
}
int bank_max() { return money_known() ? static_cast<int>(g.bank_max) : 0; }
bool set_takings(int dollars) {
  if (!money_known()) return false;
  poke<uint16_t>(g.takings, static_cast<uint16_t>(dollars < 0 ? 0 : dollars > kTakingsMax ? kTakingsMax : dollars));
  return true;
}
bool set_bank(int dollars) {
  if (!money_known()) return false;
  const int cap = bank_max();
  poke<uint32_t>(g.bank, static_cast<uint32_t>(dollars < 0 ? 0 : dollars > cap ? cap : dollars));
  return true;
}

// --- the reloading tool's proficiency ---------------------------------------------------
bool mixing_known() { return g_mix.known; }
namespace {
bool mix_kind_ok(int kind) { return g_mix.known && kind >= 0 && kind < kMixKinds; }
}  // namespace
const char* mix_kind_name(int kind) { return kind >= 0 && kind < kMixKinds ? kMixNames[kind] : "?"; }
int mixes_made(int kind) {
  const uintptr_t b = bag_block();
  if (!mix_kind_ok(kind) || !b) return -1;
  return peek<uint16_t>(b + g_mix.count_off + 2u * kind);
}
bool set_mixes_made(int kind, int n) {
  const uintptr_t b = bag_block();
  if (!mix_kind_ok(kind) || !b) return false;
  poke<uint16_t>(b + g_mix.count_off + 2u * kind, static_cast<uint16_t>(n < 0 ? 0 : n > kMixMax ? kMixMax : n));
  return true;
}
// The game's own walk (0x4DAF49): up the row while the count is past the
// level's threshold, stopping at the last level.
int mix_level(int kind, int made) {
  if (!mix_kind_ok(kind)) return 0;
  int i = 0;
  while (i < kMixLevels - 1 && made > g_mix.threshold[kind][i]) ++i;
  return i;
}
int mix_level_from(int kind, int level) {
  if (!mix_kind_ok(kind) || level <= 0) return 0;
  if (level >= kMixLevels) level = kMixLevels - 1;
  return g_mix.threshold[kind][level - 1] + 1;
}
int mix_bonus(int kind, int level) {
  if (!mix_kind_ok(kind) || level < 0 || level >= kMixLevels) return 0;
  return g_mix.bonus[kind][level];
}
int mix_enhanced_bonus(int bonus) {
  if (!g_mix.known) return -1;
  return bonus >= 0 && bonus < 8 ? g_mix.enhanced_bonus[bonus] : bonus;  // past the switch, kept as it is
}
bool mix_offers_enhanced(int kind) { return mix_kind_ok(kind) && kind < g_mix.enhanced_kinds; }
int mix_enhanced_after() { return g_mix.enhanced_after; }
// 0x4DAFB8: the bonus share of the batch in whole rounds, one more when the
// tenths left over come to half a round or more, all of it in a byte, and the
// byte doubled on Easy.
int mix_batch(int base, int bonus, bool easy) {
  const int extra = base * bonus;
  int n = (base + extra / 10 + (extra % 10 >= 5 ? 1 : 0)) & 0xFF;
  if (easy) n = (n * 2) & 0xFF;
  return n;
}
int mix_recipes(int kind, MixRecipe* out, int max) {
  if (!mix_kind_ok(kind)) return 0;
  int n = 0;
  for (; n < g_mix.recipes[kind] && n < max; ++n) out[n] = g_mix.recipe[kind][n];
  return n;
}
bool easy_mode() {
  // The reloading tool's doubling and the ending's test name the same bit
  // (discover_clears checks it); either will do.
  const uint32_t mask = g_mix.easy_mask ? g_mix.easy_mask : g.ending_easy;
  return mask && g.flags && (u32(g.flags) & mask) != 0;
}

// --- the memory cards and the save screen ---------------------------------------------------
bool cards_known() { return g_cards.known; }
bool card_layout(CardLayout* out) {
  if (!g_cards.known) return false;
  *out = g_cards.layout;
  return true;
}
const char* location_name(int id) {
  if (!g_cards.known || id < 0 || id >= kMaxLocations || !g_cards.locations[id][0]) return nullptr;
  return g_cards.locations[id];
}
bool card_screen() {
  return g_ready && g_cards.known &&
         static_cast<uint32_t>(task_entry(kCardScreenTask)) == g_cards.screen_entry &&
         task_state(kCardScreenTask) != 0 && u32(g_cards.screen_ptr) == g_cards.screen;
}
int card_screen_state() { return card_screen() ? peek<uint8_t>(g_cards.screen + kCardStateOff) : -1; }
bool card_list_waiting() { return g_cards.known && card_screen_state() == g_cards.state_list; }
int card_screen_card() { return card_screen() ? peek<uint8_t>(g_cards.screen + kCardCardOff) : -1; }
int card_screen_cursor() { return card_list_waiting() ? peek<int8_t>(g_cards.screen + kCardCursorOff) : -1; }
bool rescan_card_screen() {
  if (!card_list_waiting()) return false;
  poke<uint8_t>(g_cards.screen + kCardStateOff, static_cast<uint8_t>(g_cards.state_rescan));
  return true;
}

namespace {
// A card object, checked against what discovery found: the container the
// global names (by its vtable), then the card in it (by the card class's).
uintptr_t card_object(int card) {
  if (!g_cards.known || card < 0 || card >= g_cards.cards) return 0;
  uint32_t container = 0, vt = 0;
  if (!mem::read_safe(g_cards.container_ptr, &container) || !container || !mem::read_safe(container, &vt) ||
      vt != g_cards.container_vt)
    return 0;
  const uintptr_t at = container + g_cards.first_card + static_cast<uintptr_t>(card) * g_cards.card_stride;
  if (!mem::readable(at, g_cards.card_stride) || u32(at) != g_cards.card_vt) return 0;
  return at;
}
}  // namespace

uint8_t* card_image(int card) {
  const uintptr_t at = card_object(card);
  return at ? reinterpret_cast<uint8_t*>(at + g_cards.image_off) : nullptr;
}

bool card_file(int card, char* out, size_t n) {
  const uintptr_t at = card_object(card);
  char folder[0x400], name[16];
  if (!at || !read_cstr(at + g_cards.path_off, folder, sizeof(folder), true) ||
      !read_cstr(at + g_cards.name_off, name, sizeof(name)))
    return false;
  // As the card's write builds it: the folder, the name, then the extension
  // over the name's last character (its colon).
  char path[sizeof(folder) + sizeof(name) + sizeof(g_cards.ext)];
  const int len = std::snprintf(path, sizeof(path), "%s%s", folder, name);
  if (len < 2 || len >= static_cast<int>(sizeof(path))) return false;
  std::snprintf(path + len - 1, sizeof(path) - (len - 1), "%s", g_cards.ext);
  if (std::strlen(path) >= n) return false;
  std::snprintf(out, n, "%s", path);
  return true;
}

// --- code sites ---------------------------------------------------------------------------
mem::Patch* site(Site s) { return s >= 0 && s < kSiteCount && g_site_ok[s] ? &g_sites[s] : nullptr; }
const char* site_name(Site s) { return s >= 0 && s < kSiteCount ? kSiteNames[s] : "?"; }

bool clear_input_state() {
  if (!g.in_held || !g.in_pressed || !g.in_repeat) return false;
  poke<uint32_t>(g.in_held, 0);
  poke<uint32_t>(g.in_pressed, 0);
  poke<uint32_t>(g.in_repeat, 0);
  return true;
}

void trace_tick() {
  const config::Settings& c = config::get();
  if (!c.nwatch) return;
  const DWORD now = GetTickCount();
  if (now - g_watch_second >= 1000) {
    if (g_watch_lines > 60) logf("watch: %d change(s) in the last second were not logged", g_watch_lines - 60);
    g_watch_second = now;
    g_watch_lines = 0;
  }
  for (int i = 0; i < c.nwatch; ++i) {
    const uintptr_t a = c.watch[i].addr;
    uint32_t v = 0;
    bool ok = false;
    if (c.watch[i].size == 1) {
      uint8_t b = 0;
      ok = mem::read_safe(a, &b);
      v = b;
    } else if (c.watch[i].size == 2) {
      uint16_t w = 0;
      ok = mem::read_safe(a, &w);
      v = w;
    } else {
      ok = mem::read_safe(a, &v);
    }
    if (!ok || (g_watch_seen[i] && v == g_watch_last[i])) continue;
    if (++g_watch_lines <= 60) {
      if (g_watch_seen[i]) logf("watch: 0x%08X = 0x%X (was 0x%X)", static_cast<unsigned>(a), v, g_watch_last[i]);
      else logf("watch: 0x%08X = 0x%X", static_cast<unsigned>(a), v);
    }
    g_watch_seen[i] = true;
    g_watch_last[i] = v;
  }
}

}  // namespace re3cc::game
