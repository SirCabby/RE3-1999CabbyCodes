// The save file manager's card handling, outside the game:
//
//   mkdir -p tests/build
//   i686-w64-mingw32-g++ -std=c++20 -O2 -Wall -Wextra -DWIN32_LEAN_AND_MEAN -static -Isrc
//       tests/test_savefiles.cpp -o tests/build/test_savefiles.exe
//   wine tests/build/test_savefiles.exe            (from the repository's root)
//
// savefiles.cpp is compiled in whole against a stand-in for the game: a card
// image in memory, its file in a scratch folder (tests\build\saves\bu00.sav,
// relative like the game's own .\saves\), the layout discovery reads out of
// build 21774525 and a memory card screen whose list waits for the player. Each
// case asks for a change through the module's own entry points, runs its tick
// and checks the card in memory, the file on disk and the screen.
#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <vector>

#include "savefiles.h"

namespace re3cc {
void logf(const char* fmt, ...) {
  va_list a;
  va_start(a, fmt);
  std::printf("    log: ");
  std::vprintf(fmt, a);
  std::printf("\n");
  va_end(a);
}
}  // namespace re3cc

// --- the stand-in game ---------------------------------------------------------------
namespace {
std::vector<uint8_t> g_img(re3cc::game::kCardBytes);
bool g_waiting = true;
bool g_card_there = true;
int g_rescans = 0;
const char* g_path = ".\\tests\\build\\saves\\bu00.sav";
const char* const kLocations[] = {"Warehouse", "Alley",   "Hall",         "Dark Room",    "Shopping Dist.",
                                  "Chapel",    "Living Room", "Park",     "Hospital",     "Graveyard",
                                  "Resting Room", "Monitor Room", "Parking Lot", "Machinery Room"};
}  // namespace

namespace re3cc::game {
bool card_layout(CardLayout* out) {
  CardLayout l;
  l.slots = 15;
  l.data_off = 0x200;
  l.saves_off = 0x18;
  l.location_off = 0x1B;
  l.clears_off = 0x16;
  l.flags_off = 0x08;
  l.easy_mask = 1;
  std::snprintf(l.prefix, sizeof(l.prefix), "BASLUS-00923");
  *out = l;
  return true;
}
bool cards_known() { return true; }
const char* location_name(int id) {
  if (id == 0x1C) return "Next Game";
  return id >= 0 && id < 14 ? kLocations[id] : nullptr;
}
bool card_list_waiting() { return g_waiting; }
int card_screen_card() { return 0; }
int card_screen_cursor() { return g_waiting ? 0 : -1; }
bool rescan_card_screen() {
  if (!g_waiting) return false;
  ++g_rescans;
  return true;
}
uint8_t* card_image(int card) { return card == 0 && g_card_there ? g_img.data() : nullptr; }
bool card_file(int card, char* out, size_t n) {
  if (card != 0) return false;
  std::snprintf(out, n, "%s", g_path);
  return true;
}
}  // namespace re3cc::game

#include "../src/savefiles.cpp"

// --- the cases -------------------------------------------------------------------------
namespace {
using namespace re3cc;
namespace sf = re3cc::savefiles;

int g_failed = 0, g_checks = 0;
void check(bool ok, const char* what) {
  ++g_checks;
  if (!ok) ++g_failed;
  std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
}

uint8_t* entry(int e) { return g_img.data() + e * game::kCardEntryBytes; }
uint8_t* block(int e) { return g_img.data() + e * game::kCardBlockBytes; }

// A save the way the game writes one into directory entry `e` for slot `slot`.
void make_save(int e, int slot, uint32_t frames, int saves, int location, bool easy, int clears = 0) {
  uint8_t* d = entry(e);
  std::memset(d, 0, game::kCardEntryBytes);
  const uint32_t state = 0x51, size = 0x2000;
  const uint16_t next = 0xFFFF;
  std::memcpy(d, &state, 4);
  std::memcpy(d + 4, &size, 4);
  std::memcpy(d + 8, &next, 2);
  char name[24];
  std::snprintf(name, sizeof(name), "BASLUS-00923%02d", slot);
  std::memcpy(d + 0x0A, name, std::strlen(name));
  uint8_t* b = block(e);
  std::memset(b, 0, game::kCardBlockBytes);
  b[0] = 'S';
  b[1] = 'C';
  b[2] = 0x11;
  b[3] = 1;
  // "ＲＥ３　" + the slot + "．「" ...
  static const uint8_t head[] = {0x82, 0x71, 0x82, 0x64, 0x82, 0x52, 0x81, 0x40};
  std::memcpy(b + 4, head, sizeof(head));
  const int n = slot + 1;
  if (n < 10) {
    b[12] = 0x81;
    b[13] = 0x40;
  } else {
    b[12] = 0x82;
    b[13] = static_cast<uint8_t>(0x4F + n / 10);
  }
  b[14] = 0x82;
  b[15] = static_cast<uint8_t>(0x4F + n % 10);
  b[16] = 0x81;
  b[17] = 0x44;
  b[18] = 0x81;
  b[19] = 0x75;
  uint8_t* data = b + 0x200;
  std::memcpy(data, &frames, 4);
  const uint32_t flags = easy ? 1u : 0u;
  std::memcpy(data + 8, &flags, 4);
  const uint16_t cl = static_cast<uint16_t>(clears);
  std::memcpy(data + 0x16, &cl, 2);
  data[0x18] = static_cast<uint8_t>(saves);
  data[0x1B] = static_cast<uint8_t>(location);
  for (int i = 0x220; i < 0x980; ++i) b[i] = static_cast<uint8_t>(i * 7 + slot);  // the rest of the save
}

std::vector<uint8_t> disk() {
  std::vector<uint8_t> b;
  HANDLE h = CreateFileA(g_path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
  if (h == INVALID_HANDLE_VALUE) return b;
  b.resize(GetFileSize(h, nullptr));
  DWORD got = 0;
  ReadFile(h, b.data(), static_cast<DWORD>(b.size()), &got, nullptr);
  CloseHandle(h);
  return b;
}
void put_disk(const std::vector<uint8_t>& b) {
  HANDLE h = CreateFileA(g_path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
  DWORD put_n = 0;
  WriteFile(h, b.data(), static_cast<DWORD>(b.size()), &put_n, nullptr);
  CloseHandle(h);
}

// The panel's view after a tick (the screen up).
sf::View tick_view() {
  sf::tick(true);
  return sf::view();
}

// A card with one save, and the screen closed: the next tick opens it, and the
// module reads the card afresh then (it reads it four times a second at most).
void fresh_card() {
  std::fill(g_img.begin(), g_img.end(), 0);
  make_save(1, 0, 3723 * 60, 7, 11, false);  // slot 1: Monitor Room, 1:02:03, 7 saves, Hard
  put_disk(g_img);
  g_waiting = true;
  g_card_there = true;
  g_rescans = 0;
  sf::tick(false);
}

void case_list() {
  std::printf("the list\n");
  fresh_card();
  const sf::View v = tick_view();
  char d[128];
  check(v.ready && v.up && v.card_ok && v.waiting && v.slots == 15, "the card is read while the list waits");
  check(v.save[0].used && v.save[0].entry == 1 && !v.save[0].odd, "slot 1 holds the save in entry 1");
  check(!std::strcmp(sf::describe(v.save[0], d, sizeof(d)), "Monitor Room, 1:02:03, 7 saves, Hard"), d);
  bool rest_empty = true;
  for (int s = 1; s < 15; ++s) rest_empty = rest_empty && !v.save[s].used && !v.save[s].other;
  check(rest_empty && v.free_entries == 14, "the other fourteen are empty, fourteen entries free");
}

void case_copy_to_empty() {
  std::printf("copy to an empty slot\n");
  fresh_card();
  sf::View v = tick_view();
  const std::vector<uint8_t> before = g_img;
  sf::request_copy(0, 4, v.save[0], v.save[4]);
  v = tick_view();
  check(!v.note_error, v.note);
  check(v.save[4].used && v.save[4].entry == 2, "slot 5 now holds a save, in the first free entry (2)");
  char a[128], b[128];
  check(!std::strcmp(sf::describe(v.save[4], a, sizeof(a)), sf::describe(v.save[0], b, sizeof(b))),
        "...that reads as the save it was copied from");
  char name[24] = {};
  std::memcpy(name, entry(2) + 0x0A, 0x15);
  uint32_t state = 0, size = 0;
  uint16_t next = 0;
  std::memcpy(&state, entry(2), 4);
  std::memcpy(&size, entry(2) + 4, 4);
  std::memcpy(&next, entry(2) + 8, 2);
  check(state == 0x51 && size == 0x2000 && next == 0xFFFF && !std::strcmp(name, "BASLUS-0092304"),
        "its directory entry is the one the game would create");
  check(block(2)[14] == 0x82 && block(2)[15] == 0x54 && block(2)[12] == 0x81 && block(2)[13] == 0x40,
        "its title names slot 5");
  bool same = true;
  for (int i = 0; i < game::kCardBlockBytes; ++i)
    if (i < 12 || i > 15) same = same && block(2)[i] == before[1 * game::kCardBlockBytes + i];
  check(same, "...and everything else in the block is the source's");
  check(std::memcmp(g_img.data(), before.data(), 2 * game::kCardEntryBytes) == 0 &&
            std::memcmp(block(1), before.data() + game::kCardBlockBytes, game::kCardBlockBytes) == 0,
        "the source is untouched");
  check(disk() == g_img, "the file on disk is the game's card");
  check(g_rescans == 1, "the game's list was sent to read the card again");
}

void case_two_digits_and_overwrite() {
  std::printf("copy to slot 12, then back over slot 1\n");
  fresh_card();
  sf::View v = tick_view();
  const std::vector<uint8_t> before = g_img;
  sf::request_copy(0, 11, v.save[0], v.save[11]);
  v = tick_view();
  check(!v.note_error && v.save[11].used, v.note);
  const int e = v.save[11].entry;
  check(block(e)[12] == 0x82 && block(e)[13] == 0x50 && block(e)[14] == 0x82 && block(e)[15] == 0x51,
        "the title names slot 12 in two digits");
  sf::request_copy(11, 0, v.save[11], v.save[0]);
  v = tick_view();
  check(!v.note_error && v.save[0].entry == 1, v.note);
  check(std::memcmp(block(1), before.data() + game::kCardBlockBytes, game::kCardBlockBytes) == 0 &&
            std::memcmp(entry(1), before.data() + game::kCardEntryBytes, game::kCardEntryBytes) == 0,
        "copied back over slot 1, the save is the one it started as, entry and block");
  check(disk() == g_img, "the file on disk is the game's card");
}

void case_delete() {
  std::printf("delete\n");
  fresh_card();
  sf::View v = tick_view();
  sf::request_copy(0, 4, v.save[0], v.save[4]);
  v = tick_view();
  sf::request_delete(4, v.save[4]);
  v = tick_view();
  check(!v.note_error && !v.save[4].used, v.note);
  bool zero = true;
  for (int i = 0; i < game::kCardEntryBytes; ++i) zero = zero && entry(2)[i] == 0;
  for (int i = 0; i < game::kCardBlockBytes; ++i) zero = zero && block(2)[i] == 0;
  check(zero, "its directory entry and block are what a never-used slot holds");
  check(v.save[0].used && v.free_entries == 14, "slot 1 is untouched, fourteen entries free again");
  check(disk() == g_img, "the file on disk is the game's card");
  sf::request_delete(0, v.save[0]);
  v = tick_view();
  check(!v.note_error && v.free_entries == 15, "the last save goes too");
  check(g_rescans == 3, "the list read the card again after each change");
}

void case_refusals() {
  std::printf("refusals\n");
  fresh_card();
  sf::View v = tick_view();
  const sf::Save seen = v.save[0];
  sf::request_copy(0, 4, v.save[0], v.save[4]);
  v = tick_view();
  std::vector<uint8_t> was = g_img;
  sf::Save empty_seen;  // what was seen before the copy: slot 5 was empty then
  empty_seen.known = true;
  sf::request_copy(0, 4, seen, empty_seen);
  v = tick_view();
  check(v.note_error && g_img == was && disk() == was, "a slot that changed since the click is left alone");

  g_waiting = false;
  sf::request_delete(0, v.save[0]);
  v = tick_view();
  check(v.note_error && g_img == was && !v.waiting, "nothing changes while the game's list is not waiting");
  g_waiting = true;

  v = tick_view();
  sf::request_delete(2, v.save[2]);
  v = tick_view();
  check(v.note_error && g_img == was, "an empty slot has nothing to delete");

  // A card the way a real PlayStation one marks its free entries (0xA0): the
  // game's card takes only 0 for free, so it has no room for another file.
  fresh_card();
  for (int e = 2; e < 16; ++e) entry(e)[0] = 0xA0;
  put_disk(g_img);
  v = tick_view();
  was = g_img;
  check(v.card_ok && v.free_entries == 0, "a card with no free entry");
  sf::request_copy(0, 1, v.save[0], v.save[1]);
  v = tick_view();
  check(v.note_error && g_img == was, "...takes no copy into an empty slot");

  // Another game's file: the last empty slot is the game's "Other data".
  fresh_card();
  make_save(2, 3, 60, 1, 0, true);
  std::memcpy(entry(2) + 0x0A, "BASLUS-00000XYZ", 15);
  put_disk(g_img);
  v = tick_view();
  was = g_img;
  check(v.card_ok && v.save[14].other && !v.save[3].used, "another game's file shows as Other data in slot 15");
  sf::request_copy(0, 14, v.save[0], v.save[14]);
  v = tick_view();
  check(v.note_error && g_img == was, "...which takes no copy");

  // Two files naming one slot: the game would load one and list the other.
  fresh_card();
  make_save(2, 0, 120, 2, 1, false);
  put_disk(g_img);
  v = tick_view();
  was = g_img;
  check(v.card_ok && v.save[0].odd, "two files claiming slot 1 are left alone");
  sf::request_delete(0, v.save[0]);
  v = tick_view();
  check(v.note_error && g_img == was, "...and not deleted");

  // A save named for a slot the game does not have: the game's own read would
  // write past its map, so the card is not touched at all.
  fresh_card();
  make_save(2, 3, 120, 2, 1, false);
  std::memcpy(entry(2) + 0x0A + 12, "17", 2);
  put_disk(g_img);
  v = tick_view();
  check(!v.card_ok && v.slots == 0 && v.why[0], v.why);

  // The file cannot be written: nothing changes in the game either.
  fresh_card();
  v = tick_view();
  was = g_img;
  g_path = ".\\tests\\build\\no such folder\\bu00.sav";
  sf::request_copy(0, 4, v.save[0], v.save[4]);
  v = tick_view();
  check(v.note_error && g_img == was && g_rescans == 0, "a file that cannot be written leaves the game's card alone");
  g_path = ".\\tests\\build\\saves\\bu00.sav";

  // The card object gone: nothing.
  fresh_card();
  v = tick_view();
  g_card_there = false;
  sf::request_delete(0, v.save[0]);
  v = tick_view();
  check(v.note_error && !v.card_ok, "no card object, no change");
  g_card_there = true;
}

void case_disk_differs() {
  std::printf("a file that is not the game's card\n");
  fresh_card();
  sf::View v = tick_view();
  std::vector<uint8_t> other = g_img;
  other[0x7000] = 0x42;  // something else in the file
  put_disk(other);
  sf::request_copy(0, 2, v.save[0], v.save[2]);
  v = tick_view();
  check(!v.note_error && disk() == g_img, "the change is written from the game's card, as its own next save would");
}

void case_real_card(const char* path) {
  std::printf("a card the game wrote (%s)\n", path);
  HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    std::printf("  (not there - skipped)\n");
    return;
  }
  DWORD got = 0;
  ReadFile(h, g_img.data(), static_cast<DWORD>(g_img.size()), &got, nullptr);
  CloseHandle(h);
  put_disk(g_img);
  g_waiting = true;
  g_rescans = 0;
  sf::tick(false);
  sf::View v = tick_view();
  char d[128];
  int used = 0, first = -1;
  for (int s = 0; s < v.slots; ++s) {
    if (!v.save[s].used) continue;
    ++used;
    if (first < 0) first = s;
    std::printf("    slot %d: %s\n", s + 1, sf::describe(v.save[s], d, sizeof(d)));
  }
  check(got == static_cast<DWORD>(g_img.size()) && v.card_ok && used > 0, "it reads, with saves on it");
  int empty = -1;
  for (int s = 0; s < v.slots && empty < 0; ++s)
    if (!v.save[s].used && !v.save[s].other) empty = s;
  if (first < 0 || empty < 0) return;
  sf::request_copy(first, empty, v.save[first], v.save[empty]);
  v = tick_view();
  check(!v.note_error && v.save[empty].used && disk() == g_img, v.note);
  std::printf("    slot %d: %s\n", empty + 1, sf::describe(v.save[empty], d, sizeof(d)));
  sf::request_delete(empty, v.save[empty]);
  v = tick_view();
  check(!v.note_error && !v.save[empty].used, v.note);
}
}  // namespace

int main(int argc, char** argv) {
  CreateDirectoryA(".\\tests\\build\\saves", nullptr);
  re3cc::savefiles::init();
  case_list();
  case_copy_to_empty();
  case_two_digits_and_overwrite();
  case_delete();
  case_refusals();
  case_disk_differs();
  if (argc > 1) case_real_card(argv[1]);
  std::printf("\n%d checks, %d failed\n", g_checks, g_failed);
  return g_failed ? 1 : 0;
}
