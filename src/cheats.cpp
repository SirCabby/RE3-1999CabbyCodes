#include "cheats.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "config.h"
#include "items.h"
#include "log.h"
#include "mem.h"

namespace re3cc::cheats {
namespace {

volatile LONG g_enabled[kCount] = {};
// g_status is the main thread's working copy; g_published is the only thing
// another thread may look at, swapped over under the lock (an unlocked copy of
// a struct this size can pair one tick's values with another's).
Status g_status;
Status g_published;
CRITICAL_SECTION g_status_lock;
bool g_status_lock_ready = false;
const char* kNames[kCount] = {"God mode",           "One hit kills",         "Infinite ammo",
                              "Infinite ink ribbons", "Save without counting", "Freeze play time",
                              "Heal without counting", "Freeze countdown timer", "Skip cutscenes"};

bool on(Kind k) { return g_enabled[k] != 0; }

// --- requests (panel -> main thread) ----------------------------------------------------
struct Request {
  enum Type { kSaves, kPlaytime, kRecovery, kClears, kTakings, kBank, kMixes, kOutfit } type;
  int a;
  int b = 0;
};
Request g_req[16];
int g_req_n = 0;
CRITICAL_SECTION g_req_lock;
bool g_lock_ready = false;

void push(Request r) {
  if (!g_lock_ready) return;
  EnterCriticalSection(&g_req_lock);
  if (g_req_n < 16) g_req[g_req_n++] = r;
  LeaveCriticalSection(&g_req_lock);
}
int drain(Request* out, int max) {
  if (!g_lock_ready) return 0;
  EnterCriticalSection(&g_req_lock);
  const int n = g_req_n < max ? g_req_n : max;
  std::memcpy(out, g_req, sizeof(Request) * static_cast<size_t>(n));
  g_req_n = 0;
  LeaveCriticalSection(&g_req_lock);
  return n;
}

void publish(const Status& st) {
  g_status = st;
  if (!g_status_lock_ready) return;
  EnterCriticalSection(&g_status_lock);
  g_published = st;
  LeaveCriticalSection(&g_status_lock);
}

void note(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  vsnprintf(g_status.last_action, sizeof(g_status.last_action), fmt, args);
  va_end(args);
  logf("%s", g_status.last_action);
}

void toggle_patch(game::Site s, bool want, bool* applied_out) {
  mem::Patch* p = game::site(s);
  if (!p) {
    *applied_out = false;
    return;
  }
  if (want && !p->applied) {
    if (p->apply()) logf("patch applied: %s", p->label);
    else logf("ERROR: patch %s could not be applied (site changed?)", p->label);
  } else if (!want && p->applied) {
    if (p->revert()) logf("patch restored: %s", p->label);
    else logf("ERROR: patch %s could not be restored (site changed?)", p->label);
  }
  *applied_out = p->applied;
}

// --- holds ---------------------------------------------------------------------------------
// God mode: HP back to its maximum and the poison cleared. With the damage
// patch in, no hit lowers HP at all and this only fills a bar that was low when
// the switch went on (or that something other than a hit took down). The dying
// bit is left alone - it is the game's own way of finishing a death it has started.
bool g_god_logged = false;
void hold_god(const game::Player& p) {
  if (p.max_hp > 0 && p.hp < p.max_hp) {
    game::set_player_hp(p.max_hp);
    if (!g_god_logged) {
      g_god_logged = true;
      logf("god mode: holding the player at %d HP", p.max_hp);
    }
  }
  if (p.condition & game::kPoison) game::set_player_condition(p.condition & ~game::kPoison);
}

// One hit kills: every room entity with HP left is held at 1 - the last value
// that still counts as alive, so the next hit takes it under and the game's own
// death, drops and scripts run. The player and its partner are never touched
// (the walk starts at the first room entity).
int g_ohk_logged = 0;
int hold_entities(bool hold) {
  game::Entity list[64];
  const int n = game::entities(list, 64);
  int live = 0;
  for (int i = 0; i < n; ++i) {
    const game::Entity& e = list[i];
    if (e.hp <= 0) continue;
    ++live;
    if (hold && e.hp > 1) {
      game::set_entity_hp(e.at, 1);
      if (g_ohk_logged < 8) {
        ++g_ohk_logged;
        logf("one hit kills: entity %d (type 0x%02X) held at 1 HP (was %d)", e.index, e.type, e.hp);
      }
    }
  }
  return live;
}

// Infinite ammo: the equipped slot is topped back up to what the game's item
// table says one slot of that weapon holds. The knife has a maximum of 1 and
// takes no ammo, so it is left alone.
int g_ammo_logged = 0;
bool hold_ammo(bool hold, int* item_out) {
  game::Bag b;
  if (!game::read_bag(&b) || b.equipped < 0 || b.equipped >= game::kMaxSlots) return false;
  const game::ItemSlot& s = b.slot[b.equipped];
  if (!s.id || game::item_kind(s.id) != items::kWeapon) return false;
  const int max = game::item_max(s.id);
  if (max <= 1) return false;
  *item_out = s.id;
  if (hold && s.count < max) {
    game::write_slot(b.equipped, s.id, max);
    if (g_ammo_logged < 4) {
      ++g_ammo_logged;
      logf("infinite ammo: %s held at %d (was %d)", items::name(s.id), max, s.count);
    }
  }
  return true;
}

// Freeze play time. The game keeps the play time as a total and the vblank
// count it last brought the total up to date at (its base), and brings it up to
// date only as it saves and as the ending starts (game.h) - so holding the total
// alone holds nothing: the save adds every vblank since the base on top of it
// and writes that to the card in the same frame, before any tick can put it
// back. The freeze takes both adds out instead (two patches, so the total does
// not move at all), and holds the base at the present, so nothing waits to be
// added when it goes off. Switching it on mid-game first adds what the game had
// counted up to then, as a save would.
bool g_timer_frozen = false;   // the freeze as the last tick had it
bool g_timer_session = false;  // a game has been running since the last tick without one

void log_play_time(const char* what, const char* tail = "") {
  const uint32_t s = game::game_timer() / 60u;
  logf("play time %s at %u:%02u:%02u%s", what, s / 3600u, (s / 60u) % 60u, s % 60u, tail);
}

// Jill's outfit. The game loads her model only as a session starts and when a
// room load finds its type is not the character to be in play, so an outfit
// set here waits for the next room load, with that test taken out until then:
// the load then takes the path it takes for a change of type. The loader
// writes its file name afresh each time it runs, which says it has.
bool g_outfit_pending = false;

void update_outfit(bool* patched) {
  if (g_outfit_pending && game::model_loaded()) {
    g_outfit_pending = false;
    logf("outfit: the game has loaded Jill's model again, in %s", game::outfit_name(game::outfit_next(nullptr)));
  }
  toggle_patch(game::kSiteModelReload, g_outfit_pending, patched);
}

// The switches as the ini last had them: a change from the panel is written
// back on the next tick.
config::Cheats g_saved{};

config::Cheats switches() {
  return config::Cheats{on(kGodMode),         on(kOneHitKills),      on(kInfiniteAmmo),
                        on(kInfiniteInk),     on(kNoSaveCount),      on(kFreezePlaytime),
                        on(kNoRecoveryCount), on(kFreezeCountdown), on(kSkipCutscenes)};
}

void save_if_changed() {
  const config::Cheats now = switches();
  if (!std::memcmp(&now, &g_saved, sizeof(now))) return;
  g_saved = now;
  if (config::save_cheats(now)) logf("cheats: switches saved to the ini");
}

}  // namespace

void init() {
  if (g_lock_ready) return;
  InitializeCriticalSection(&g_req_lock);
  InitializeCriticalSection(&g_status_lock);
  g_status_lock_ready = true;
  g_lock_ready = true;
}

const char* name(Kind k) { return k >= 0 && k < kCount ? kNames[k] : "?"; }
bool enabled(Kind k) { return k >= 0 && k < kCount && g_enabled[k] != 0; }
void set_enabled(Kind k, bool v) {
  if (k >= 0 && k < kCount) InterlockedExchange(&g_enabled[k], v ? 1 : 0);
}
Status status() {
  if (!g_status_lock_ready) return Status{};
  EnterCriticalSection(&g_status_lock);
  const Status s = g_published;
  LeaveCriticalSection(&g_status_lock);
  return s;
}

void request_saves(int saves) { push({Request::kSaves, saves}); }
void request_playtime_seconds(int seconds) { push({Request::kPlaytime, seconds}); }
void request_recovery(int total) { push({Request::kRecovery, total}); }
void request_clears(int n) { push({Request::kClears, n}); }
void request_takings(int dollars) { push({Request::kTakings, dollars}); }
void request_bank(int dollars) { push({Request::kBank, dollars}); }
void request_mixes(int kind, int made) { push({Request::kMixes, kind, made}); }
void request_outfit(int outfit) { push({Request::kOutfit, outfit}); }

void tick(bool in_game, bool status_screen) {
  (void)status_screen;
  static bool init_done = false;
  if (!init_done) {
    init_done = true;
    const config::Settings& c = config::get();
    set_enabled(kGodMode, c.god_mode);
    set_enabled(kOneHitKills, c.one_hit_kills);
    set_enabled(kInfiniteAmmo, c.infinite_ammo);
    set_enabled(kInfiniteInk, c.infinite_ink);
    set_enabled(kNoSaveCount, c.no_save_count);
    set_enabled(kFreezePlaytime, c.freeze_playtime);
    set_enabled(kNoRecoveryCount, c.no_recovery_count);
    set_enabled(kFreezeCountdown, c.freeze_mercs_timer);
    set_enabled(kSkipCutscenes, c.skip_cutscenes);
    g_saved = switches();
  }
  save_if_changed();

  Status st;
  std::memcpy(st.last_action, g_status.last_action, sizeof(st.last_action));
  st.in_game = in_game;
  st.site_ink = game::site(game::kSiteInkRibbon) != nullptr;
  st.site_ink_check = game::site(game::kSiteInkCheck) != nullptr;
  st.site_savecount = game::site(game::kSiteSaveCount) != nullptr;
  st.site_damage = game::site(game::kSitePlayerDamage) != nullptr;
  st.site_recovery = game::site(game::kSiteRecovery) != nullptr;
  st.counters_ok = game::counters_ready();
  st.recovery_ok = game::recovery_known();
  st.entities_ok = game::entities_known();
  st.site_countdown = game::countdown_known();
  st.site_missile = game::missile_known();
  st.mix_ok = game::mixing_known();

  // Patches are independent of a session: toggle them whenever asked.
  // God mode takes the hit out of the player's damage routine, so a blow bigger
  // than the player's whole health - which kills in the frame it lands, before
  // any hold could put HP back - does nothing; the hold below is the backstop.
  toggle_patch(game::kSitePlayerDamage, on(kGodMode), &st.damage_patched);
  // Infinite ink ribbons is two patches: the typewriter offers a save without
  // a ribbon in the inventory, and the save takes none.
  toggle_patch(game::kSiteInkRibbon, on(kInfiniteInk), &st.ink_patched);
  toggle_patch(game::kSiteInkCheck, on(kInfiniteInk), &st.ink_check_patched);
  toggle_patch(game::kSiteSaveCount, on(kNoSaveCount), &st.savecount_patched);
  // Healing items still heal: only the total the ending ranks on is not written.
  toggle_patch(game::kSiteRecovery, on(kNoRecoveryCount), &st.recovery_patched);
  // Freezing the countdown is two as well: the second it takes off is skipped,
  // and the frame count that says when stands still, so the hundredths on the
  // clock stop too. Bonuses still add to it. It is the same clock in the
  // Mercenaries and in the main game's timed scenes.
  bool tick_patched = false, frames_patched = false;
  const bool freeze = on(kFreezeCountdown) && st.site_countdown;
  toggle_patch(game::kSiteCountdownTick, freeze, &tick_patched);
  toggle_patch(game::kSiteCountdownFrames, freeze, &frames_patched);
  st.countdown_patched = tick_patched && frames_patched;
  st.countdown_running = game::countdown_running();
  st.countdown = st.countdown_running ? game::countdown_seconds() : 0;
  // The missile at the very end is counted in by the radar, in the clock's
  // place, with a step off its distance every frame: that step is taken out.
  toggle_patch(game::kSiteMissileStep, freeze && st.site_missile, &st.missile_patched);
  st.missile_running = game::missile_running();
  st.missile = st.missile_running ? game::missile_seconds() : 0;
  // Freeze play time: the game's two adds to the total - a save's and the
  // ending's - taken out.
  st.site_timer = game::timer_freeze_known();
  const bool freeze_time = on(kFreezePlaytime) && st.site_timer;
  bool time_save_patched = false, time_end_patched = false;
  toggle_patch(game::kSiteTimerSave, freeze_time, &time_save_patched);
  toggle_patch(game::kSiteTimerEnd, freeze_time, &time_end_patched);
  st.playtime_hold = freeze_time && time_save_patched && time_end_patched;
  // Jill's outfit: the room load's model test stays out until the next model
  // load, whatever the screen - a session start loads it too.
  bool model_patched = false;
  update_outfit(&model_patched);

  if (!in_game) {
    g_god_logged = false;
    g_ohk_logged = 0;
    g_ammo_logged = 0;
    // No total or base to settle outside a game: a session's start sets the
    // base to its own first moment.
    g_timer_frozen = freeze_time;
    g_timer_session = false;
    publish(st);
    return;
  }

  // Requests first, so the readings below reflect them.
  Request reqs[16];
  const int n = drain(reqs, 16);
  for (int i = 0; i < n; ++i) {
    const Request& r = reqs[i];
    switch (r.type) {
      case Request::kSaves:
        if (game::set_save_count(r.a)) note("saves set to %d", r.a < 0 ? 0 : r.a > 99 ? 99 : r.a);
        else note("saves not set: the save counter was not found");
        break;
      case Request::kPlaytime:
        if (game::set_game_timer(static_cast<uint32_t>(r.a) * 60u)) {
          note("play time set to %d:%02d:%02d", r.a / 3600, (r.a / 60) % 60, r.a % 60);
        } else {
          note("play time not set: the clock was not found");
        }
        break;
      case Request::kRecovery:
        if (game::set_recovery(r.a)) note("recovery total set to %d", game::recovery());
        else note("recovery total not set: it was not found");
        break;
      // The Mercenaries carries the count through to saves\data.sav untouched;
      // it is the main game's.
      case Request::kClears:
        if (!game::clears_known()) note("clear count not set: it was not found");
        else if (game::mercenaries()) note("clear count not set: the Mercenaries has no ending to count");
        else if (game::set_clears(r.a)) note("clear count set to %d", game::clears());
        break;
      // The money is only the Mercenaries': the main game never pays into it.
      case Request::kTakings:
        if (!game::money_known()) note("takings not set: the Mercenaries money was not found");
        else if (!game::mercenaries()) note("takings not set: this is not the Mercenaries");
        else if (game::set_takings(r.a)) note("Mercenaries takings set to $%d", game::takings());
        break;
      case Request::kBank:
        if (!game::money_known()) note("bank not set: the Mercenaries money was not found");
        else if (!game::mercenaries()) note("bank not set: this is not the Mercenaries");
        else if (game::set_bank(r.a)) note("Mercenaries bank set to $%d", game::bank());
        break;
      case Request::kMixes:
        if (!game::set_mixes_made(r.a, r.b)) {
          note("reloading tool proficiency not set: the batch counts were not found");
        } else {
          const int made = game::mixes_made(r.a), level = game::mix_level(r.a, made);
          note("reloading tool: %s at level %d (+%d%%), %d batch%s made", game::mix_kind_name(r.a), level + 1,
               10 * game::mix_bonus(r.a, level), made, made == 1 ? "" : "es");
        }
        break;
      // The main game's own: the Mercenaries' characters have no outfits.
      case Request::kOutfit:
        if (!game::outfit_known()) {
          note("outfit not changed: Jill's outfit was not found");
        } else if (game::mercenaries()) {
          note("outfit not changed: the Mercenaries has no outfits");
        } else if (!game::set_outfit(r.a)) {
          note("outfit not changed: there is no outfit %d", r.a);
        } else if (!game::jill_in_play()) {
          // Going back to Jill is a change of type, which loads her model anyway.
          note("outfit set to %s: Jill wears it when she is back in play", game::outfit_name(r.a));
        } else {
          g_outfit_pending = game::mark_model_load();
          update_outfit(&model_patched);
          if (model_patched) {
            note("outfit set to %s: Jill changes into it at the next door", game::outfit_name(r.a));
          } else {
            g_outfit_pending = false;
            note("outfit set to %s: the room load could not be made to load her model, so Jill changes into it "
                 "the next time a save is loaded with it", game::outfit_name(r.a));
          }
        }
        break;
    }
  }

  st.player_ok = game::player(&st.player);
  // A dead player is the game's to finish.
  if (on(kGodMode) && st.player_ok && !game::player_dead()) hold_god(st.player);
  else if (!on(kGodMode)) g_god_logged = false;

  st.live_entities = hold_entities(on(kOneHitKills));
  if (!on(kOneHitKills)) g_ohk_logged = 0;

  // The hold runs on every tick of a running game, screens included. The
  // typewriter's save screen copies the ten slots away as it opens and puts
  // them back as it closes, so a write that lands while it is up is undone by
  // its own restore - and a save written in between simply records the full
  // magazine, which is what the cheat is for.
  st.ammo_ok = hold_ammo(on(kInfiniteAmmo), &st.ammo_item);
  if (!on(kInfiniteAmmo)) g_ammo_logged = 0;

  for (int k = 0; st.mix_ok && k < game::kMixKinds; ++k) st.mixes[k] = game::mixes_made(k);
  st.easy = game::easy_mode();

  st.money_ok = game::money_known();
  st.mercenaries = game::mercenaries();
  if (st.money_ok && st.mercenaries) {
    st.takings = game::takings();
    st.bank = game::bank();
    st.bank_max = game::bank_max();
  }

  if (st.site_timer) {
    const bool started = !g_timer_session;
    g_timer_session = true;
    if (freeze_time != g_timer_frozen) {
      g_timer_frozen = freeze_time;
      // On: what the game had counted up to now goes into the total, as a save
      // would put it there. Off: the clock runs on from now, the time it was
      // frozen for never counted.
      const bool ok = freeze_time ? game::settle_game_timer() : game::restart_game_timer();
      log_play_time(freeze_time ? "frozen" : "running again", ok ? "" : " (the vblank count could not be read)");
    } else if (freeze_time) {
      game::restart_game_timer();
      if (started) log_play_time("frozen");
    }
  }
  if (st.counters_ok) {
    st.saves = game::save_count();
    st.timer = game::game_timer();
    st.stage = game::stage();
    st.room = game::room();
  }
  if (st.recovery_ok) st.recovery = game::recovery();
  st.clears_ok = game::clears_known();
  if (st.clears_ok) {
    st.clears = game::clears();
    st.epilogues = game::epilogues();
  }
  st.outfit_ok = game::outfit_known();
  if (st.outfit_ok) {
    st.outfit = game::outfit_next(&st.outfit_boutique);
    st.jill = game::jill_in_play();
  }
  st.outfit_pending = g_outfit_pending;
  publish(st);
}

void remove_hooks() {
  for (int s = 0; s < game::kSiteCount; ++s)
    if (mem::Patch* p = game::site(static_cast<game::Site>(s)))
      if (p->applied) p->revert();
}

}  // namespace re3cc::cheats
