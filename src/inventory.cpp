#include "inventory.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "items.h"
#include "log.h"

namespace re3cc::inventory {

int Fit::topped() const {
  int n = 0;
  for (int v : onto) n += v;
  return n;
}

const char* who_name(int who) {
  switch (who) {
    case 0: return "Jill";
    case 1: return "Carlos";
    default: break;
  }
  return "the player";
}

int count_limit(int id) {
  if (id <= 0) return 0;
  const int max = game::item_max(id);
  return max > 1 ? max : 0;  // the knife, keys, herbs: one is all a slot holds
}

int count_floor(int id) { return game::item_kind(id) == items::kWeapon ? 0 : 1; }

// A gun comes loaded, everything else as one.
int default_count(int id) {
  const int limit = count_limit(id);
  if (!limit) return 0;
  return game::item_kind(id) == items::kWeapon ? limit : 1;
}

namespace {

// What is written for a count: within the editor's limits, and 1 for an item
// that keeps no count of its own.
int clamp_count(int id, int count) {
  if (!id) return 0;
  const int limit = count_limit(id);
  if (!limit) return 1;
  const int lo = count_floor(id);
  return count < lo ? lo : count > limit ? limit : count;
}

// The stacks of `id` in `slots` are topped up first, slot by slot, each to what
// one slot holds (only for the items the game combines), and the rest goes into
// the first free slot, at most one slot's worth. Anything else goes whole into
// the first free slot - a gun's count is its magazine, not a number of guns -
// and a count of 0 (an empty gun) still moves.
Fit fit(const Slot* slots, int n, int id, int count) {
  Fit f;
  const int want = count < 0 ? 0 : count;
  int rest = want;
  const int cap = game::item_stacks(id) ? game::item_max(id) : 0;
  for (int s = 0; cap && s < n && s < kBoxMax && rest > 0; ++s) {
    if (slots[s].id != id || slots[s].count >= cap) continue;
    f.onto[s] = cap - slots[s].count < rest ? cap - slots[s].count : rest;
    rest -= f.onto[s];
  }
  if (rest > 0 || rest == want)
    for (int s = 0; s < n && s < kBoxMax; ++s)
      if (!slots[s].id) {
        f.slot = s;
        f.into = cap && rest > cap ? cap : rest;
        rest -= f.into;
        break;
      }
  f.left = rest;
  return f;
}

}  // namespace

Fit fit_into_bag(const View& v, int id, int count) { return fit(v.bag, v.size, id, count); }
Fit fit_into_box(const View& v, int id, int count) { return fit(v.box, v.box_size, id, count); }

namespace {

CRITICAL_SECTION g_lock;  // the view, the note and the request queue
bool g_ready = false;
View g_view;              // what the panel was last handed
char g_note[200] = {};
bool g_note_error = false;

struct Request {
  enum Type { kSet, kStore, kTake, kDrop } type;
  int slot, id, count;
  Slot seen;
};
Request g_req[16];
int g_req_n = 0;
bool g_redraw_due = false;  // main thread only

void note(bool error, const char* fmt, ...) {
  char text[200];
  va_list args;
  va_start(args, fmt);
  vsnprintf(text, sizeof(text), fmt, args);
  va_end(args);
  logf("%sinventory: %s", error ? "ERROR: " : "", text);
  if (!g_ready) return;
  EnterCriticalSection(&g_lock);
  std::snprintf(g_note, sizeof(g_note), "%s", text);
  g_note_error = error;
  LeaveCriticalSection(&g_lock);
}

void push(const Request& r) {
  if (!g_ready) return;
  EnterCriticalSection(&g_lock);
  if (g_req_n < 16) g_req[g_req_n++] = r;
  LeaveCriticalSection(&g_lock);
}

int drain(Request* out, int max) {
  if (!g_ready) return 0;
  EnterCriticalSection(&g_lock);
  const int n = g_req_n < max ? g_req_n : max;
  std::memcpy(out, g_req, sizeof(Request) * static_cast<size_t>(n));
  g_req_n = 0;
  LeaveCriticalSection(&g_lock);
  return n;
}

// The first empty slot of those the character may use, or -1.
int first_free(const View& v) {
  for (int i = 0; i < v.size; ++i)
    if (!v.bag[i].id) return i;
  return -1;
}

bool same(const Slot& a, const Slot& b) { return a.id == b.id && a.count == b.count; }

// "Hand Gun x15" for a log line or a note.
const char* describe(int id, int count, char* buf, size_t n) {
  std::snprintf(buf, n, "%s%s", items::name(id), items::quantity(id, count).text);
  return buf;
}

// Everything the panel and a move need, read fresh.
bool read(View* v) {
  game::Bag b;
  if (!game::read_bag(&b)) return false;
  v->size = b.slots;
  v->equipped = b.equipped >= 0 && b.equipped < b.slots ? b.equipped : -1;
  v->held = 0;
  for (int i = 0; i < kBagMax; ++i) {
    v->bag[i] = b.slot[i];
    if (i < b.slots && b.slot[i].id) ++v->held;
  }
  v->box_size = v->box_known && game::read_box(v->box) ? kBoxMax : 0;
  v->who = game::inventory_index();
  v->put_away = game::weapon_put_away();
  return true;
}

// An equip change the status screen would undo: refused while it is holding the
// weapon back for an item.
bool equip_blocked(const View& v, int slot) {
  if (slot != v.equipped || !v.put_away) return false;
  note(true, "slot %d is equipped and the status screen is holding the weapon back for an item - close it and try "
             "again", slot + 1);
  return true;
}

void apply_set(const View& v, const Request& r) {
  char a[64], b[64];
  if (r.slot < 0 || r.slot >= v.size) {
    note(true, "%s has no slot %d", who_name(v.who), r.slot + 1);
    return;
  }
  const Slot& live = v.bag[r.slot];
  if (!same(live, r.seen)) {
    note(true, "slot %d changed before the edit could be applied - nothing written", r.slot + 1);
    return;
  }
  const bool weapon = r.id && game::item_kind(r.id) == items::kWeapon;
  const bool touches_equip = r.slot == v.equipped && r.id != live.id;
  if (touches_equip && equip_blocked(v, r.slot)) return;
  const int count = clamp_count(r.id, r.count);
  if (!r.id) {
    // Emptied, the slot leaves the inventory and the ones after it move up.
    if (touches_equip) game::set_equipped(-1);
    if (!game::remove_slot(r.slot)) {
      note(true, "slot %d could not be emptied", r.slot + 1);
      return;
    }
    note(false, "%s slot %d emptied (was %s)", who_name(v.who), r.slot + 1, describe(live.id, live.count, a, sizeof(a)));
    return;
  }
  // Something new goes where the game would put it: the first empty slot.
  const int at = live.id ? r.slot : first_free(v);
  if (at < 0) {
    note(true, "%s has no empty slot", who_name(v.who));
    return;
  }
  // A new item starts with clear flags; a count change keeps the slot's own.
  if (!game::write_slot(at, r.id, count, r.id == live.id ? -1 : 0)) {
    note(true, "slot %d could not be written", at + 1);
    return;
  }
  if (touches_equip) game::set_equipped(weapon ? at : -1);
  if (!live.id) note(false, "%s slot %d: %s", who_name(v.who), at + 1, describe(r.id, count, a, sizeof(a)));
  else
    note(false, "%s slot %d: %s (was %s)%s", who_name(v.who), r.slot + 1, describe(r.id, count, a, sizeof(a)),
         describe(live.id, live.count, b, sizeof(b)),
         touches_equip ? (weapon ? " - equipped" : " - unequipped") : "");
}

void apply_store(const View& v, const Request& r) {
  char a[64];
  if (!v.box_size) {
    note(true, "the item box is not readable - nothing stored");
    return;
  }
  if (r.slot < 0 || r.slot >= v.size || !v.bag[r.slot].id) {
    note(true, "slot %d holds nothing to store", r.slot + 1);
    return;
  }
  const Slot live = v.bag[r.slot];
  if (!same(live, r.seen)) {
    note(true, "slot %d changed before it could be stored - nothing moved", r.slot + 1);
    return;
  }
  if (equip_blocked(v, r.slot)) return;
  const Fit f = fit_into_box(v, live.id, live.count);
  if (!f.ok()) {
    note(true, "the item box has no room for %s", describe(live.id, live.count, a, sizeof(a)));
    return;
  }
  for (int s = 0; s < v.box_size; ++s)
    if (f.onto[s]) game::write_box_slot(s, v.box[s].id, v.box[s].count + f.onto[s], v.box[s].flags);
  if (f.slot >= 0) game::write_box_slot(f.slot, live.id, f.into, live.flags);
  if (f.left > 0) {
    game::write_slot(r.slot, live.id, f.left, -1);
  } else {
    if (r.slot == v.equipped) game::set_equipped(-1);
    game::remove_slot(r.slot);  // the slots after it move up, as the game keeps them
  }
  note(false, "stored %s from slot %d in the item box%s", describe(live.id, live.count - f.left, a, sizeof(a)),
       r.slot + 1, f.left > 0 ? " (the rest did not fit)" : "");
}

void apply_take(const View& v, const Request& r) {
  char a[64];
  if (r.slot < 0 || r.slot >= v.box_size || !v.box[r.slot].id) {
    note(true, "box slot %d holds nothing to take", r.slot + 1);
    return;
  }
  const Slot live = v.box[r.slot];
  if (!same(live, r.seen)) {
    note(true, "box slot %d changed before it could be taken - nothing moved", r.slot + 1);
    return;
  }
  const Fit f = fit_into_bag(v, live.id, live.count);
  if (!f.ok()) {
    note(true, "%s has no room for %s", who_name(v.who), describe(live.id, live.count, a, sizeof(a)));
    return;
  }
  for (int s = 0; s < v.size; ++s)
    if (f.onto[s]) game::write_slot(s, v.bag[s].id, v.bag[s].count + f.onto[s], -1);
  if (f.slot >= 0) game::write_slot(f.slot, live.id, f.into, live.flags);
  if (f.left > 0) game::write_box_slot(r.slot, live.id, f.left, live.flags);
  else game::write_box_slot(r.slot, 0, 0, 0);
  note(false, "took %s from box slot %d%s", describe(live.id, live.count - f.left, a, sizeof(a)), r.slot + 1,
       f.left > 0 ? " (the rest stays in the box)" : "");
}

void apply_drop(const View& v, const Request& r) {
  char a[64];
  if (r.slot < 0 || r.slot >= v.box_size || !v.box[r.slot].id) return;
  const Slot live = v.box[r.slot];
  if (!same(live, r.seen)) {
    note(true, "box slot %d changed before it could be thrown away - nothing changed", r.slot + 1);
    return;
  }
  game::write_box_slot(r.slot, 0, 0, 0);
  note(false, "threw away %s from box slot %d", describe(live.id, live.count, a, sizeof(a)), r.slot + 1);
}

void publish(const View& v) {
  if (!g_ready) return;
  EnterCriticalSection(&g_lock);
  g_view = v;
  std::memcpy(g_view.note, g_note, sizeof(g_view.note));
  g_view.note_error = g_note_error;
  LeaveCriticalSection(&g_lock);
}

}  // namespace

void init() {
  if (g_ready) return;
  InitializeCriticalSection(&g_lock);
  g_ready = true;
}

View view() {
  if (!g_ready) return View{};
  EnterCriticalSection(&g_lock);
  const View v = g_view;
  LeaveCriticalSection(&g_lock);
  return v;
}

void request_set(int slot, int id, int count, const Slot& seen) { push({Request::kSet, slot, id, count, seen}); }
void request_store(int slot, const Slot& seen) { push({Request::kStore, slot, 0, 0, seen}); }
void request_take(int box_slot, const Slot& seen) { push({Request::kTake, box_slot, 0, 0, seen}); }
void request_drop(int box_slot, const Slot& seen) { push({Request::kDrop, box_slot, 0, 0, seen}); }

void tick(bool in_game, bool status_screen) {
  View v;
  v.known = game::items_known();
  v.box_known = game::box_known();
  v.in_game = in_game;
  const bool readable = in_game && v.known && read(&v);
  v.open = readable && status_screen;

  Request reqs[16];
  const int n = drain(reqs, 16);
  if (n && !v.open) {
    note(true, "the status screen is not up - nothing changed");
  } else {
    const View before = v;
    for (int i = 0; i < n; ++i) {
      if (i && !read(&v)) break;  // each request sees what the last one left
      switch (reqs[i].type) {
        case Request::kSet: apply_set(v, reqs[i]); break;
        case Request::kStore: apply_store(v, reqs[i]); break;
        case Request::kTake: apply_take(v, reqs[i]); break;
        case Request::kDrop: apply_drop(v, reqs[i]); break;
      }
    }
    if (n) read(&v);
    if (n && (v.equipped != before.equipped || std::memcmp(v.bag, before.bag, sizeof(v.bag)) != 0))
      g_redraw_due = true;
  }
  // The game's screen shows the inventory as it was when it opened until it is
  // opened again; the panel says so until then.
  if (!v.open) g_redraw_due = false;
  v.redraw_due = g_redraw_due;
  if (!readable) {
    v.size = v.box_size = 0;
    v.held = 0;
  }
  publish(v);
}

}  // namespace re3cc::inventory
