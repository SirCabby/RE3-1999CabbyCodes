#include "savefiles.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <vector>

#include "log.h"

namespace re3cc::savefiles {

bool same_save(const Save& a, const Save& b) {
  return a.known == b.known && a.used == b.used && a.other == b.other && a.odd == b.odd && a.entry == b.entry &&
         a.hash == b.hash;
}

const char* describe(const Save& s, char* out, size_t n) {
  if (!s.known) {
    std::snprintf(out, n, "?");
    return out;
  }
  if (!s.used) {
    std::snprintf(out, n, "%s", s.other ? "other data" : "no data");
    return out;
  }
  char where[40];
  if (const char* name = game::location_name(s.location)) std::snprintf(where, sizeof(where), "%s", name);
  else std::snprintf(where, sizeof(where), "location %d", s.location);
  const unsigned t = s.frames / 60;  // the play time counts sixtieths of a second
  char saves[24] = "";
  if (s.saves >= 0 && s.saves != 0xFF)
    std::snprintf(saves, sizeof(saves), ", %d save%s", s.saves, s.saves == 1 ? "" : "s");
  char cleared[32] = "";
  if (s.clears == 1) std::snprintf(cleared, sizeof(cleared), ", cleared once");
  else if (s.clears > 1) std::snprintf(cleared, sizeof(cleared), ", cleared %d times", s.clears);
  std::snprintf(out, n, "%s, %u:%02u:%02u%s%s, %s", where, t / 3600, (t / 60) % 60, t % 60, saves, cleared,
                s.easy ? "Easy" : "Hard");
  return out;
}

namespace {

// The game only ever uses the first card: the card question on its memory
// card screen skips the second (the cursor in its state 2, 0x44CFD0).
constexpr int kCard = 0;
// A directory entry, as the game's card creates one (kSigCardCreate): its
// state (0x51 the first block of a file, 0 free), the file's size, the next
// block of a longer file (0xFFFF for none) and its name.
constexpr int kEntryState = 0, kEntrySize = 4, kEntryNext = 8, kEntryName = 0x0A, kEntryNameMax = 0x15;
constexpr uint32_t kFileState = 0x51, kFreeState = 0;
constexpr uint16_t kNoNext = 0xFFFF;
constexpr int kEntryBytes = game::kCardEntryBytes, kBlockBytes = game::kCardBlockBytes;
constexpr int kListPlaces = game::kCardBlocks - 1;  // the screen's list of the card's files: one place per block

CRITICAL_SECTION g_lock;  // the view, the note and the request queue
bool g_ready = false;
View g_view;              // what the panel was last handed
char g_note[240] = {};
bool g_note_error = false;
bool g_showing = false;   // main thread only, as is everything below
DWORD g_last_read = 0;

struct Request {
  enum Type { kDelete, kCopy } type;
  int from, to;
  Save from_seen, to_seen;
};
Request g_req[8];
int g_req_n = 0;

void note(bool error, const char* fmt, ...) {
  char text[240];
  va_list args;
  va_start(args, fmt);
  vsnprintf(text, sizeof(text), fmt, args);
  va_end(args);
  logf("%ssave files: %s", error ? "ERROR: " : "", text);
  if (!g_ready) return;
  EnterCriticalSection(&g_lock);
  std::snprintf(g_note, sizeof(g_note), "%s", text);
  g_note_error = error;
  LeaveCriticalSection(&g_lock);
}

void push(const Request& r) {
  if (!g_ready) return;
  EnterCriticalSection(&g_lock);
  if (g_req_n < 8) g_req[g_req_n++] = r;
  LeaveCriticalSection(&g_lock);
}

int drain(Request* out, int max) {
  if (!g_ready) return 0;
  EnterCriticalSection(&g_lock);
  const int n = g_req_n < max ? g_req_n : max;
  for (int i = 0; i < n; ++i) out[i] = g_req[i];
  g_req_n = 0;
  LeaveCriticalSection(&g_lock);
  return n;
}

template <typename T>
T get(const uint8_t* p) {
  T v;
  std::memcpy(&v, p, sizeof(T));
  return v;
}
template <typename T>
void put(uint8_t* p, T v) {
  std::memcpy(p, &v, sizeof(T));
}

uint32_t fnv1a(const uint8_t* p, size_t n, uint32_t h = 2166136261u) {
  for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 16777619u;
  return h;
}

// The card, slot by slot, mapped the way the screen's own read maps it
// (kSigCardRead, kSigCardSlots): the files are listed in directory order, each
// at the place the blocks of the files before it put it; a file named the
// prefix and two digits is the save in that slot; every other place counts
// against the empty slots from the last one up, which the game shows as
// "Other data". A card the game's own read would misread - more places than
// its list has, or a save named for a slot it does not have - is not touched.
struct Card {
  bool ok = false;
  int slots = 0;
  Save save[kSlots];
  int free_entries = 0;
  int first_free = -1;  // where a new file goes: the game's own pick, the first free entry from 1
  char why[96] = {};    // why not ok
};

Card read_card(const uint8_t* img, const game::CardLayout& lay) {
  Card c;
  c.slots = lay.slots;
  for (Save& s : c.save) s.known = true;
  struct Place {
    char name[kEntryNameMax + 1];
    int entry;
  };
  Place places[kListPlaces] = {};
  unsigned count = 0;  // a byte in the game
  for (int e = 1; e < game::kCardBlocks; ++e) {
    const uint8_t* d = img + e * kEntryBytes;
    const uint32_t state = get<uint32_t>(d + kEntryState);
    if (state == kFreeState) {
      ++c.free_entries;
      if (c.first_free < 0) c.first_free = e;
      continue;
    }
    if (state != kFileState) continue;  // a later block of a longer file
    if (count >= static_cast<unsigned>(kListPlaces)) {
      std::snprintf(c.why, sizeof(c.why), "its files take more than its %d blocks", kListPlaces);
      return c;
    }
    std::memcpy(places[count].name, d + kEntryName, kEntryNameMax);
    places[count].entry = e;
    count = (count + (get<uint32_t>(d + kEntrySize) >> 13)) & 0xFF;
  }
  if (count > static_cast<unsigned>(kListPlaces)) {
    std::snprintf(c.why, sizeof(c.why), "its files take more than its %d blocks", kListPlaces);
    return c;
  }
  int map[kSlots], claims[kSlots] = {};
  for (int& m : map) m = -1;
  int others = 0;
  const size_t plen = std::strlen(lay.prefix);
  for (unsigned k = 0; k < count; ++k) {
    const char* name = places[k].name;
    if (std::strncmp(name, lay.prefix, plen) != 0) {
      ++others;
      continue;
    }
    const int tens = name[plen] - '0', ones = name[plen + 1] - '0';
    if (tens < 0 || tens > 9 || ones < 0 || ones > 9 || tens * 10 + ones >= c.slots) {
      std::snprintf(c.why, sizeof(c.why), "a save on it is named %.21s, for a slot the game does not have", name);
      return c;
    }
    map[tens * 10 + ones] = static_cast<int>(k);
    ++claims[tens * 10 + ones];
  }
  for (int s = c.slots - 1; s >= 0 && others > 0; --s)
    if (map[s] < 0) {
      c.save[s].other = true;
      --others;
    }
  for (int s = 0; s < c.slots; ++s) {
    if (map[s] < 0) continue;
    Save& v = c.save[s];
    const int e = places[map[s]].entry;
    const uint8_t* d = img + e * kEntryBytes;
    const uint8_t* block = img + e * kBlockBytes;
    const uint8_t* data = block + lay.data_off;
    v.used = true;
    v.entry = e;
    // The game writes a save as one block of its own, and never two of a slot.
    v.odd = claims[s] > 1 || get<uint32_t>(d + kEntrySize) != static_cast<uint32_t>(kBlockBytes) ||
            get<uint16_t>(d + kEntryNext) != kNoNext;
    v.frames = get<uint32_t>(data);  // the save block's first field
    v.saves = data[lay.saves_off];
    v.location = data[lay.location_off];
    v.clears = get<uint16_t>(data + lay.clears_off);
    v.easy = (get<uint32_t>(data + lay.flags_off) & lay.easy_mask) != 0;
    v.hash = fnv1a(block, kBlockBytes, fnv1a(d, kEntryBytes));
  }
  c.ok = true;
  return c;
}

// The title the game writes into a save's header (0x44F2F0, block +4, in
// Shift-JIS): "ＲＥ３　", the save's slot in two full-width digits - a
// full-width space for the tens under 10 - then "．". A copy gets the number of
// the slot it goes to, as a save made there would; a title of any other shape
// is left as it is.
bool retitle(uint8_t* block, int slot) {
  static const uint8_t kHead[8] = {0x82, 0x71, 0x82, 0x64, 0x82, 0x52, 0x81, 0x40};
  uint8_t* t = block + 4;
  auto digit = [](const uint8_t* p) { return p[0] == 0x82 && p[1] >= 0x4F && p[1] <= 0x58; };
  const bool space = t[8] == 0x81 && t[9] == 0x40;
  if (std::memcmp(t, kHead, sizeof(kHead)) != 0 || !(space || digit(t + 8)) || !digit(t + 10) || t[12] != 0x81 ||
      t[13] != 0x44)
    return false;
  const int n = slot + 1;
  if (n < 10) {
    t[8] = 0x81;
    t[9] = 0x40;
  } else {
    t[8] = 0x82;
    t[9] = static_cast<uint8_t>(0x4F + n / 10);
  }
  t[10] = 0x82;
  t[11] = static_cast<uint8_t>(0x4F + n % 10);
  return true;
}

// false with ERROR_FILE_NOT_FOUND / ERROR_PATH_NOT_FOUND in GetLastError for a
// missing file; any other failure is not a missing file.
bool read_file(const char* path, std::vector<uint8_t>* out) {
  HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  const DWORD size = GetFileSize(h, nullptr);
  bool ok = size != INVALID_FILE_SIZE && size <= 4u * game::kCardBytes;
  if (ok) {
    out->resize(size);
    DWORD got = 0;
    ok = !size || (ReadFile(h, out->data(), size, &got, nullptr) && got == size);
  }
  CloseHandle(h);
  if (!ok) SetLastError(ERROR_READ_FAULT);
  return ok;
}
bool missing(DWORD e) { return e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND; }

// The bytes into `path` through a temp file in the same folder and one rename,
// so the file is either what it was or all of the new one.
bool write_atomic(const char* path, const uint8_t* b, size_t n) {
  char tmp[MAX_PATH + 16];
  std::snprintf(tmp, sizeof(tmp), "%s.re3cc-new", path);
  HANDLE h = CreateFileA(tmp, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  DWORD put_n = 0;
  bool ok = WriteFile(h, b, static_cast<DWORD>(n), &put_n, nullptr) && put_n == n;
  ok = FlushFileBuffers(h) && ok;
  CloseHandle(h);
  if (ok) ok = MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
  if (!ok) {
    const DWORD e = GetLastError();
    DeleteFileA(tmp);
    SetLastError(e);
  }
  return ok;
}

void apply(const Request& r, const game::CardLayout& lay) {
  const bool copy = r.type == Request::kCopy;
  if (!game::card_list_waiting()) {
    note(true, "The game's list of saves is not waiting for you any more - nothing was changed.");
    return;
  }
  uint8_t* img = game::card_image(kCard);
  char path[MAX_PATH];
  if (!img || !game::card_file(kCard, path, sizeof(path))) {
    note(true, "The game's memory card could not be found - nothing was changed.");
    return;
  }
  if (r.to < 0 || r.to >= lay.slots || (copy && (r.from < 0 || r.from >= lay.slots || r.from == r.to))) {
    note(true, "That is not a slot the game's list has - nothing was changed.");
    return;
  }
  const Card now = read_card(img, lay);
  if (!now.ok) {
    note(true, "The memory card is not laid out the way the game writes one (%s) - nothing was changed.", now.why);
    return;
  }
  const Save& to = now.save[r.to];
  if (!same_save(to, r.to_seen) || (copy && !same_save(now.save[r.from], r.from_seen))) {
    note(true, "The memory card changed since that was clicked - nothing was changed.");
    return;
  }

  // The card as it is to be, built aside.
  std::vector<uint8_t> next(img, img + game::kCardBytes);
  int entry = -1;  // the directory entry and block that change
  char a[96], b[96];
  if (!copy) {
    if (!to.used) {
      note(true, "Slot %d has no save to delete.", r.to + 1);
      return;
    }
    if (to.odd) {
      note(true, "The file in slot %d is not one the game would write - it was left alone.", r.to + 1);
      return;
    }
    entry = to.entry;
    std::memset(next.data() + entry * kEntryBytes, 0, kEntryBytes);
    std::memset(next.data() + entry * kBlockBytes, 0, kBlockBytes);
  } else {
    const Save& from = now.save[r.from];
    if (!from.used) {
      note(true, "Slot %d has no save to copy.", r.from + 1);
      return;
    }
    if (from.odd || to.odd) {
      note(true, "The file in slot %d is not one the game would write - it was left alone.",
           (from.odd ? r.from : r.to) + 1);
      return;
    }
    if (to.other) {
      note(true, "The game keeps slot %d for another game's data - nothing was changed.", r.to + 1);
      return;
    }
    entry = to.used ? to.entry : now.first_free;
    if (entry < 0) {
      note(true, "The memory card has no room for another save - nothing was changed.");
      return;
    }
    uint8_t* block = next.data() + entry * kBlockBytes;
    std::memcpy(block, img + from.entry * kBlockBytes, kBlockBytes);
    if (!retitle(block, r.to)) logf("save files: the save in slot %d has a title of its own - copied as it is", r.from + 1);
    if (!to.used) {
      // A new file, as the game's card creates one.
      uint8_t* d = next.data() + entry * kEntryBytes;
      char name[kEntryNameMax + 1];
      std::snprintf(name, sizeof(name), "%s%02d", lay.prefix, r.to);
      std::memset(d, 0, kEntryBytes);
      put<uint32_t>(d + kEntryState, kFileState);
      put<uint32_t>(d + kEntrySize, kBlockBytes);
      put<uint16_t>(d + kEntryNext, kNoNext);
      std::memcpy(d + kEntryName, name, std::strlen(name));
    }
  }
  // The card it makes has to read back as asked: the slot holds the save (or
  // nothing), and every other save is untouched.
  const Card after = read_card(next.data(), lay);
  bool as_asked = after.ok && after.save[r.to].used == copy && (!copy || after.save[r.to].entry == entry);
  for (int s = 0; as_asked && s < lay.slots; ++s)
    if (s != r.to && (after.save[s].used != now.save[s].used || after.save[s].hash != now.save[s].hash)) as_asked = false;
  if (!as_asked) {
    note(true, "The change did not come out on the card the way it should - nothing was changed.");
    return;
  }

  // The file first: if the game were to close before its own card is changed,
  // it would write that card over the file.
  std::vector<uint8_t> was;
  const bool had_file = read_file(path, &was);
  if (!had_file && !missing(GetLastError())) {
    note(true, "The memory card's file %s could not be read (error %lu) - nothing was changed.", path, GetLastError());
    return;
  }
  if (had_file && (was.size() != static_cast<size_t>(game::kCardBytes) || std::memcmp(was.data(), img, was.size()) != 0))
    logf("save files: %s did not hold the card the game has - it is written from the game's card, as the game's own "
         "next save would",
         path);
  if (!write_atomic(path, next.data(), next.size())) {
    note(true, "The memory card's file %s could not be written (error %lu) - nothing was changed.", path,
         GetLastError());
    return;
  }
  std::vector<uint8_t> back;
  if (!read_file(path, &back) || back != next) {
    const bool restored = had_file ? write_atomic(path, was.data(), was.size()) : DeleteFileA(path) != 0;
    note(true, "The memory card's file did not read back as written - %s, and the game's card was left alone.",
         restored ? "it was put back as it was" : "it could not be put back; check the saves folder");
    return;
  }
  // Then the game's own card - the entry and the block that changed - and its
  // list, which reads the card again in its next frame.
  std::memcpy(img + entry * kEntryBytes, next.data() + entry * kEntryBytes, kEntryBytes);
  std::memcpy(img + entry * kBlockBytes, next.data() + entry * kBlockBytes, kBlockBytes);
  const bool relisted = game::card_screen_card() == kCard && game::rescan_card_screen();
  char full[MAX_PATH] = {};
  if (!GetFullPathNameA(path, sizeof(full), full, nullptr)) std::snprintf(full, sizeof(full), "%s", path);
  if (!copy) {
    logf("save files: deleted slot %d (%s), directory entry %d - %s written%s", r.to + 1, describe(to, a, sizeof(a)),
         entry, full, relisted ? ", the game's list read the card again" : "");
    note(false, "The save in slot %d was deleted.", r.to + 1);
  } else {
    logf("save files: copied slot %d (%s) %s slot %d (%s), directory entry %d - %s written%s", r.from + 1,
         describe(now.save[r.from], a, sizeof(a)), to.used ? "over" : "to", r.to + 1, describe(to, b, sizeof(b)),
         entry, full, relisted ? ", the game's list read the card again" : "");
    note(false, "The save in slot %d was copied to slot %d.", r.from + 1, r.to + 1);
  }
}

}  // namespace

void init() {
  if (g_ready) return;
  InitializeCriticalSection(&g_lock);
  g_ready = true;
}

View view() {
  View v;
  if (!g_ready) return v;
  EnterCriticalSection(&g_lock);
  v = g_view;
  std::snprintf(v.note, sizeof(v.note), "%s", g_note);
  v.note_error = g_note_error;
  LeaveCriticalSection(&g_lock);
  return v;
}

bool showing() { return g_showing; }

void request_delete(int slot, const Save& seen) { push({Request::kDelete, -1, slot, Save{}, seen}); }
void request_copy(int from, int to, const Save& from_seen, const Save& to_seen) {
  push({Request::kCopy, from, to, from_seen, to_seen});
}

void tick(bool screen_up) {
  if (!g_ready) return;
  static Card card;                 // the last reading, while the screen is up
  static char file[MAX_PATH] = {};  // ...and the card's file, in full
  game::CardLayout lay;
  const bool known = game::card_layout(&lay);
  if (screen_up && !g_showing) {
    g_last_read = 0;
    logf("save files: the memory card screen is up%s", known ? "" : " - but its cards were not found, see discovery");
  } else if (!screen_up && g_showing) {
    logf("save files: the memory card screen is gone");
  }
  g_showing = screen_up;

  // What the list is doing is read before a change: one sends it through the
  // read of the card, and it is back on the list in its next frame.
  View v;
  v.ready = known;
  v.up = g_showing;
  if (g_showing && known) {
    v.waiting = game::card_list_waiting();
    v.cursor = v.waiting ? game::card_screen_cursor() : -1;
  }
  Request reqs[8];
  const int n = drain(reqs, 8);
  for (int i = 0; i < n; ++i) {
    if (!g_showing || !known) note(true, "The memory card screen is not up any more - nothing was changed.");
    else apply(reqs[i], lay);
  }

  if (g_showing && known) {
    // The card is read four times a second while the screen is up, and right
    // after a change - not every frame.
    const DWORD now = GetTickCount();
    if (n || !g_last_read || now - g_last_read > 250) {
      g_last_read = now;
      const uint8_t* img = game::card_image(kCard);
      card = img ? read_card(img, lay) : Card{};
      if (!img) std::snprintf(card.why, sizeof(card.why), "the game's card is not where discovery left it");
      static char said[96] = {};
      if (std::strcmp(said, card.why) != 0) {
        if (card.why[0]) logf("save files: the card cannot be changed: %s", card.why);
        std::snprintf(said, sizeof(said), "%s", card.why);
      }
      char path[MAX_PATH];
      file[0] = '\0';
      if (game::card_file(kCard, path, sizeof(path)) && !GetFullPathNameA(path, sizeof(file), file, nullptr))
        std::snprintf(file, sizeof(file), "%s", path);
    }
    v.card_ok = card.ok;
    std::snprintf(v.why, sizeof(v.why), "%s", card.why);
    v.slots = card.ok ? card.slots : 0;
    for (int s = 0; s < v.slots; ++s) v.save[s] = card.save[s];
    v.free_entries = card.free_entries;
    std::snprintf(v.file, sizeof(v.file), "%s", file);
  }
  EnterCriticalSection(&g_lock);
  g_view = v;
  LeaveCriticalSection(&g_lock);
}

}  // namespace re3cc::savefiles
