#include "dispatch.h"

#include "cheats.h"
#include "config.h"
#include "crash.h"
#include "cutscene.h"
#include "game.h"
#include "input.h"
#include "inventory.h"
#include "log.h"
#include "overlay.h"
#include "savefiles.h"
#include "sound.h"

namespace re3cc::dispatch {
namespace {

DWORD g_main_thread = 0;  // the process's primary thread: DllMain(PROCESS_ATTACH) runs on it
using PeekMessageAFn = BOOL(WINAPI*)(LPMSG, HWND, UINT, UINT, UINT);
PeekMessageAFn g_real_peek = nullptr;
DWORD g_last_rates = 0;
Snapshot g_snap;
DWORD g_last_tick = 0;
DWORD g_last_filter = 0;
volatile LONG g_first_tick = 0;
volatile LONG g_unpacked = 0;
volatile LONG g_foreign_logged = 0;
HANDLE g_first_event = nullptr;
bool g_imports_done = false;
bool g_in_game_prev = false;
bool g_status_prev = false;

// Once the image is unpacked, the game's rebuilt import table can be reached:
// its GetAsyncKeyState and SetUnhandledExceptionFilter slots are re-pointed to
// our hooks when Enigma left the real addresses there. (The GetProcAddress
// hook may already have handed the game ours; then nothing matches.)
void hooks_after_unpack() {
  HMODULE k32 = GetModuleHandleA("kernel32.dll");
  if (void* real = k32 ? reinterpret_cast<void*>(GetProcAddress(k32, "SetUnhandledExceptionFilter")) : nullptr)
    game::hook_import("SetUnhandledExceptionFilter", real, crash_filter_hook());
  // The game's clock and its sleep, for the cutscene skip's fast-forward.
  cutscene::install_time_hooks();
}

BOOL WINAPI hk_peek_message(LPMSG msg, HWND hwnd, UINT min, UINT max, UINT remove) {
  // Other threads pump messages too; the game runs on the primary thread, and
  // so must everything that touches it.
  if (GetCurrentThreadId() == g_main_thread) {
    tick();
    // Unthrottled: a skip lets every frame go from inside the game's wait for
    // it, and the skip's question holds the game right here.
    cutscene::pump(g_real_peek);
  }
  return g_real_peek(msg, hwnd, min, max, remove);
}

}  // namespace

bool hook_message_pump() {
  HMODULE u32 = GetModuleHandleA("user32.dll");
  FARPROC real = u32 ? GetProcAddress(u32, "PeekMessageA") : nullptr;
  if (!real) return false;
  g_real_peek = reinterpret_cast<PeekMessageAFn>(real);
  return game::hook_import("PeekMessageA", reinterpret_cast<void*>(real), reinterpret_cast<void*>(&hk_peek_message)) > 0;
}

void unhook_message_pump() {
  cutscene::remove_time_hooks();
  if (!g_real_peek) return;
  game::hook_import("PeekMessageA (restore)", reinterpret_cast<void*>(&hk_peek_message), reinterpret_cast<void*>(g_real_peek));
}

void init() {
  g_main_thread = GetCurrentThreadId();
  g_first_event = CreateEventA(nullptr, TRUE, FALSE, nullptr);
}

bool on_main_thread() { return GetCurrentThreadId() == g_main_thread; }

void tick() {
  if (GetCurrentThreadId() != g_main_thread) {
    if (!InterlockedExchange(&g_foreign_logged, 1))
      logf("dispatch: a tick came in on thread %lu, not the game's %lu - ignored", GetCurrentThreadId(), g_main_thread);
    return;
  }
  // The pump spins many times per frame.
  const DWORD now = GetTickCount();
  if (now - g_last_tick < 16) return;
  g_last_tick = now;

  if (!g_first_tick) {
    InterlockedExchange(&g_first_tick, 1);
    if (g_first_event) SetEvent(g_first_event);
    logf("main-thread tick running on thread %lu (the game's PeekMessageA)", GetCurrentThreadId());
  }
  if (now - g_last_filter > 1000) {
    g_last_filter = now;
    reassert_crash_filter();
  }
  if (now - g_last_rates >= 1000) {
    g_last_rates = now;
    overlay::dx9::log_rates();
  }

  Snapshot s;
  s.main_thread = g_main_thread;
  s.ticks = g_snap.ticks + 1;
  s.unpacked = g_unpacked != 0;
  if (s.unpacked && !g_imports_done) {
    g_imports_done = true;
    hooks_after_unpack();
  }
  if (s.unpacked) game::trace_tick();
  s.game_ready = game::ready();
  if (s.game_ready) {
    s.in_game = game::in_game();
    s.status_screen = s.in_game && game::status_screen();
    if (s.in_game != g_in_game_prev) {
      logf("game session %s", s.in_game ? "started" : "ended");
      g_in_game_prev = s.in_game;
    }
    if (s.status_screen != g_status_prev) {
      if (config::get().trace) logf("status screen %s", s.status_screen ? "opened" : "closed");
      g_status_prev = s.status_screen;
    }
    // The cheats run every tick, screens or not: holds are re-asserted every
    // frame, and a switch flipped on the status screen takes effect at once.
    cheats::tick(s.in_game, s.status_screen);
    // The inventory is only written while the status screen is up.
    inventory::tick(s.in_game, s.status_screen);
    // The save files, beside the memory card screen: the title's Load the game
    // or a typewriter's Save the game, whether a game is running or not.
    s.card_screen = game::card_screen();
    savefiles::tick(s.card_screen);
  }
  // The sound hooks need a window to make their throwaway buffer for; one try.
  static bool sound_tried = false;
  if (!sound_tried && s.game_ready && overlay::window()) {
    sound_tried = true;
    sound::install(overlay::window());
  }
  cutscene::tick(s.game_ready);
  s.show_panel = s.status_screen || s.card_screen;
  // The guard takes the game's buttons away only while a panel field has the
  // keyboard, and it is a code patch, so it belongs on the game's thread.
  // The cutscene skip holds them too, while it asks and while it runs.
  input::tick((overlay::capturing_keyboard() && s.show_panel) || cutscene::hold_input());
  g_snap = s;
}

Snapshot snapshot() { return g_snap; }
HANDLE first_tick_event() { return g_first_event; }
void set_unpacked(bool on) { InterlockedExchange(&g_unpacked, on ? 1 : 0); }

}  // namespace re3cc::dispatch
