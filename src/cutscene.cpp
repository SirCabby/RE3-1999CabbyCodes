#include "cutscene.h"

#include <cstdio>

#include "cheats.h"
#include "config.h"
#include "game.h"
#include "input.h"
#include "log.h"
#include "overlay.h"
#include "pad.h"
#include "sound.h"

namespace re3cc::cutscene {
namespace {

// A skip that has run this long at full speed (minutes of the game's time) is
// not one it understands: it hands back at normal speed rather than run away.
// Time held at a page, a choice or a closing movie does not count.
constexpr DWORD kMaxFastMs = 30000;
// ...nor one that has made this many frames: five minutes of the game's time
// at its usual 30 a second, past any scene it has. A skip stuck behind a
// screen that waits for the player gives up in a second or two, and the
// play-time clock, which counts those frames, is not run on for an hour.
constexpr unsigned kMaxFrames = 9000;
// The game's buttons stay held up this long after the question closes, so the
// key that closed it is not also a press in the game.
constexpr DWORD kReleaseMs = 400;
// While skipping, the paused frame is shown again this often, with the note.
constexpr DWORD kStillEveryMs = 100;

Phase g_phase = Phase::kIdle;
bool g_request = false;      // the key or the pad button: ask on the next PeekMessageA
int g_decision = 0;          // while asking: 1 skip, 2 keep watching
bool g_in_question = false;  // inside the question's own message loop
DWORD g_scene_since = 0;     // when the cutscene on screen started (0: none)
DWORD g_release_until = 0;
// The skip under way.
DWORD g_skip_since = 0, g_last_step = 0, g_fast_ms = 0, g_still_at = 0;
bool g_movie_ended = false;  // the movie on screen has been told to end
bool g_sound_back = false;   // the looping sound held by the pause is playing again
unsigned g_frames = 0;       // frames the game made while skipping
int g_voices_cut = 0;
char g_key_name[32] = "F8";

// --- the clock ----------------------------------------------------------------------------
// The game's logic task paces each frame by spinning on timeGetTime until the
// next emulated vertical blank is due (~130,000 reads a second). While a skip
// runs, every read moves the clock on by a millisecond instead: the spin is
// over after a few reads, and as many vertical blanks go by as the frames the
// scene takes - so the play-time clock, which counts them, moves on exactly as
// far as it would have - and nothing but the machine limits the speed. At
// normal speed the game reads real time plus whatever the skips moved the
// clock on by, so it never runs backwards. The switch is thrown from the main
// thread while the logic thread reads, hence the lock.
using TimeGetTimeFn = DWORD(WINAPI*)();
TimeGetTimeFn g_real_time = nullptr;
CRITICAL_SECTION g_clock_cs;
bool g_clock_ready = false;
bool g_fast = false;
DWORD g_offset = 0;   // the game's clock less real time, at normal speed
DWORD g_virtual = 0;  // the game's clock, while fast
bool g_held = false;  // stopped while the question holds the game...
DWORD g_held_at = 0;  // ...since this real time

DWORD WINAPI hk_time_get_time() {
  EnterCriticalSection(&g_clock_cs);
  const DWORD v = g_fast ? ++g_virtual : (g_held ? g_held_at : g_real_time()) + g_offset;
  LeaveCriticalSection(&g_clock_cs);
  return v;
}

void set_fast(bool on) {
  if (!g_clock_ready || on == g_fast) return;
  EnterCriticalSection(&g_clock_cs);
  const DWORD real = g_real_time();
  if (on) g_virtual = real + g_offset;
  else g_offset = g_virtual - real;
  g_fast = on;
  LeaveCriticalSection(&g_clock_cs);
}

// The question holds the game; its clock stands still with it, so the time
// spent answering is neither caught up afterwards nor counted as play time.
void hold_clock(bool on) {
  if (!g_clock_ready || on == g_held) return;
  EnterCriticalSection(&g_clock_cs);
  const DWORD real = g_real_time();
  if (on) g_held_at = real;
  else g_offset -= real - g_held_at;
  g_held = on;
  LeaveCriticalSection(&g_clock_cs);
}

// --- the skip -------------------------------------------------------------------------------
bool enabled() { return cheats::enabled(cheats::kSkipCutscenes) && game::ready() && game::cutscenes_known(); }
bool scene_running() { return game::cinematic() || game::movie_playing(); }
// What there is to skip. A scene can open the status screen with its letterbox
// bit still set (the Mercenaries' intro ends that way), and that screen is the
// player's, not the scene's.
bool on_screen() { return scene_running() && !game::status_screen(); }

void request() {
  if (g_phase == Phase::kIdle && enabled() && on_screen()) g_request = true;
}

void finish(const char* why) {
  set_fast(false);
  sound::set_quiet(false);
  if (!g_sound_back) sound::resume(false);
  g_sound_back = true;
  logf("cutscene: skip finished after %lu ms, %u frames, %d line(s) of speech cut (%s)",
       GetTickCount() - g_skip_since, g_frames, g_voices_cut, why);
  g_phase = Phase::kIdle;
  g_release_until = GetTickCount() + kReleaseMs;
}

void begin_skip() {
  g_phase = Phase::kSkipping;
  g_skip_since = g_last_step = GetTickCount();
  g_fast_ms = 0;
  g_still_at = 0;
  g_frames = 0;
  g_voices_cut = 0;
  g_movie_ended = false;
  g_sound_back = false;  // music and ambience come back once a movie being skipped has closed
  sound::set_quiet(true);
  logf("cutscene: skipping");
}

// The question: the game's frame is held here, inside its own PeekMessageA,
// and its sound with it, while this loop keeps the window's messages moving
// and shows the last frame with the prompt over it until it is answered.
void ask(PeekFn real_peek) {
  if (!overlay::dx9::can_present_still()) {
    logf("cutscene: no copy of the last frame to show - the skip is not offered");
    return;
  }
  g_phase = Phase::kAsking;
  g_decision = 0;
  g_in_question = true;
  sound::pause();
  hold_clock(true);
  input::tick(true);
  logf("cutscene: paused (%s) - asking whether to skip", game::movie_playing() ? "a movie" : "a scripted scene");
  const unsigned confirm = pad::kA | static_cast<unsigned>(config::get().skip_pad);
  while (!g_decision) {
    MSG m;
    while (real_peek(&m, nullptr, 0, 0, PM_REMOVE)) {
      if (m.message == WM_QUIT) {
        PostQuitMessage(static_cast<int>(m.wParam));  // the game's own loop sees it next
        g_decision = 2;
        break;
      }
      TranslateMessage(&m);
      DispatchMessageA(&m);
    }
    if (g_decision) break;
    const unsigned pressed = pad::poll();
    if (pressed & confirm) g_decision = 1;
    else if (pressed & pad::kB) g_decision = 2;
    if (g_decision) break;
    // Present waits for the vertical blank itself; a device that cannot
    // present (a minimised window) is waited on instead.
    if (!overlay::dx9::present_still()) Sleep(16);
  }
  g_in_question = false;
  hold_clock(false);
  if (g_decision == 1) {
    begin_skip();
  } else {
    sound::resume(true);
    g_phase = Phase::kIdle;
    g_release_until = GetTickCount() + kReleaseMs;
    logf("cutscene: resumed");
  }
}

// One step of the skip, on every PeekMessageA the game makes while it runs:
// once a frame, at whatever speed the frames now come.
void step() {
  if (!enabled()) return finish("the skip was switched off");
  if (!scene_running()) return finish("the game has ended the cutscene");
  if (game::player_dead()) return finish("the player died");
  // The status screen waits for the player as a page does, but a scene that
  // opens it has handed the game over: the skip ends there, and the screen
  // runs at its own speed with the keys the player's.
  if (game::status_screen()) return finish("the scene has opened the status screen");
  const DWORD now = GetTickCount();
  if (g_fast) g_fast_ms += now - g_last_step;
  g_last_step = now;
  if (g_fast_ms > kMaxFastMs) return finish("still running after 30 s at full speed - normal speed");
  if (g_frames > kMaxFrames) return finish("still running after five minutes of the game's time - normal speed");
  if (game::movie_playing()) {
    // A movie keeps its own pace while it closes; it is ended once.
    set_fast(false);
    if (!g_movie_ended && (g_movie_ended = game::end_movie())) logf("cutscene: movie ended");
    return;
  }
  g_movie_ended = false;
  if (!g_sound_back) {
    g_sound_back = true;
    sound::resume(false);  // what loops (music, ambience) carries on; what played once stays over
  }
  // A page to turn or a choice to make is the player's: the game runs at its
  // own speed, sound and all, until it is dealt with.
  const bool waiting = game::message_waiting();
  set_fast(!waiting);
  sound::set_quiet(!waiting);
  if (waiting) return;
  if (game::end_voice()) ++g_voices_cut;
  if (now - g_still_at >= kStillEveryMs) {
    g_still_at = now;
    overlay::dx9::present_still();
  }
}

}  // namespace

bool on_key(int vk) {
  if (g_phase == Phase::kAsking) {
    if (vk == config::get().skip_key || vk == VK_RETURN || vk == VK_SPACE) g_decision = 1;
    else if (vk == VK_ESCAPE) g_decision = 2;
    return true;  // nothing reaches the game while it is held
  }
  if (vk != config::get().skip_key || !enabled() || !on_screen()) return false;
  request();
  return true;
}

void pump(PeekFn real_peek) {
  if (g_in_question) return;  // the question's own loop
  if (g_request) {
    g_request = false;
    if (g_phase == Phase::kIdle && enabled() && on_screen()) ask(real_peek);
  }
  if (g_phase == Phase::kSkipping) step();
}

void tick(bool game_ready) {
  static bool named = false;
  if (!named) {
    named = true;
    const UINT sc = MapVirtualKeyA(static_cast<UINT>(config::get().skip_key), MAPVK_VK_TO_VSC);
    char n[32] = {};
    if (sc && GetKeyNameTextA(static_cast<LONG>(sc << 16), n, sizeof(n)) > 0)
      std::snprintf(g_key_name, sizeof(g_key_name), "%s", n);
    else
      std::snprintf(g_key_name, sizeof(g_key_name), "key 0x%02X", config::get().skip_key);
  }
  // A status screen the scene opens does not start the hint over when it closes.
  const bool showing = game_ready && game::cutscenes_known() && scene_running();
  if (!showing) g_scene_since = 0;
  else if (!g_scene_since) g_scene_since = GetTickCount();
  // The pad is read all along, so that a button already held when a scene
  // starts is not taken for a press.
  if (config::get().skip_pad && cheats::enabled(cheats::kSkipCutscenes) &&
      (pad::poll() & static_cast<unsigned>(config::get().skip_pad)))
    request();
  if (!game_ready && g_phase == Phase::kSkipping) finish("the game is not ready");
}

Phase phase() { return g_phase; }
bool skippable() { return enabled() && on_screen(); }
bool waiting_for_player() { return g_phase == Phase::kSkipping && game::message_waiting(); }
float hint_seconds() {
  return g_scene_since ? static_cast<float>(GetTickCount() - g_scene_since) / 1000.0f : -1.0f;
}
const char* key_name() { return g_key_name; }
const char* pad_name() {
  return config::get().skip_pad && pad::connected() ? pad::name(static_cast<unsigned>(config::get().skip_pad)) : "";
}
void decide(bool skip) {
  if (g_phase == Phase::kAsking) g_decision = skip ? 1 : 2;
}
bool hold_input() {
  if (g_phase == Phase::kAsking) return true;
  if (g_phase == Phase::kSkipping) return !game::message_waiting();
  return static_cast<LONG>(g_release_until - GetTickCount()) > 0;
}
bool want_capture() {
  // The frame shown while the game is held: the last one before the question,
  // and - while a skip waits at a page or a choice - the latest one, so the
  // run goes on from what was last on screen.
  return (g_phase == Phase::kIdle && skippable()) || waiting_for_player();
}
bool drop_present() {
  if (g_phase != Phase::kSkipping || game::message_waiting()) return false;
  ++g_frames;
  return true;
}

void install_time_hooks() {
  if (g_clock_ready) return;
  HMODULE winmm = GetModuleHandleA("winmm.dll");
  FARPROC real = winmm ? GetProcAddress(winmm, "timeGetTime") : nullptr;
  if (!real) {
    logf("cutscene: winmm's timeGetTime was not found - a skip cannot run faster than the scene");
    return;
  }
  InitializeCriticalSection(&g_clock_cs);
  g_real_time = reinterpret_cast<TimeGetTimeFn>(real);
  g_clock_ready = true;
  if (!game::hook_import("timeGetTime", reinterpret_cast<void*>(real), reinterpret_cast<void*>(&hk_time_get_time)))
    logf("cutscene: the game's clock is not hooked - a skip cannot run faster than the scene");
}

void remove_time_hooks() {
  if (!g_clock_ready) return;
  // Put back at its own rate. From here the game reads real time, which is
  // behind its clock by whatever the skips moved it on, so its next vertical
  // blank waits that long - which only ever happens as the mod unloads with
  // the game on its way out.
  set_fast(false);
  game::hook_import("timeGetTime (restore)", reinterpret_cast<void*>(&hk_time_get_time),
                    reinterpret_cast<void*>(g_real_time));
}

}  // namespace re3cc::cutscene
