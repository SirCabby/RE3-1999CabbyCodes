#include "overlay.h"

#include <cstdio>
#include <cstring>
#include <utility>

#include "adopt.h"
#include "cheats.h"
#include "config.h"
#include "cutscene.h"
#include "dispatch.h"
#include "game.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "backends/imgui_impl_win32.h"
#include "input.h"
#include "inventory.h"
#include "items.h"
#include "savefiles.h"

#include "log.h"
#include "mem.h"
#include "proxy.h"
#include "version.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace re3cc::overlay {
namespace {

using GetProcAddressFn = FARPROC(WINAPI*)(HMODULE, LPCSTR);
GetProcAddressFn g_orig_gpa = nullptr;
uintptr_t g_gpa_slot = 0;  // found once: matched by value, it cannot be found again while it holds our hook

HWND g_hwnd = nullptr;
WNDPROC g_orig_wndproc = nullptr;
bool g_context_ready = false;
bool g_visible = false;
bool g_user_hidden = false;  // the toggle key, while the panel's screen is up

// The panel is drawn half as big again as ImGui's own sizes: GOG's wrapper
// draws the game at the display's resolution (2560x1440 on the machine this was
// made on), where ImGui's 13-pixel font is small. Every size below that is
// written in pixels is at ImGui's own scale and goes through scaled().
constexpr int kScalePercent = 150;
constexpr float kScale = kScalePercent / 100.0f;
float scaled(float px) { return px * kScale; }

// Kept in the ImGui layout file ([RE3-1999CabbyCodes][Panel]) beside the
// panel's position and size: whether the cheats are folded away, giving the
// inventory and the item box the room, and the scale that size was saved at. A
// size saved at another scale - or before the panel had one, at ImGui's own -
// is brought to this one the first time the panel is drawn, so it keeps its
// place and shows as much as it did.
bool g_cheats_open = true;
int g_layout_percent = 100;
bool g_layout_checked = false;

// The context lock (see overlay.h). Created in install(), from DllMain, before
// either path that takes it exists; never destroyed, because a frame can be in
// progress while the process tears down.
CRITICAL_SECTION g_imgui_cs;
bool g_imgui_cs_ready = false;

// What the panel is taking this frame, published for the input guard.
volatile LONG g_capture_mouse = 0;
volatile LONG g_capture_keyboard = 0;

// With Trace=1 the distinct names resolved through the exe's GetProcAddress
// import are logged: that slot is Enigma's loader's, so this shows whether it
// resolves the game's own imports through it - and which of them we can reach.
constexpr int kGpaSeen = 2048;  // Enigma's own runtime alone resolves more than 500 names
char g_gpa_seen[kGpaSeen][48];
volatile LONG g_gpa_n = 0;
volatile LONG g_gpa_calls = 0;

void trace_gpa(HMODULE module, const char* name) {
  if (!config::get().trace) return;
  const LONG n = g_gpa_n;
  for (LONG i = 0; i < n && i < kGpaSeen; ++i)
    if (!std::strncmp(g_gpa_seen[i], name, 47)) return;
  const LONG idx = InterlockedIncrement(&g_gpa_n) - 1;
  if (idx >= kGpaSeen) return;
  std::snprintf(g_gpa_seen[idx], sizeof(g_gpa_seen[idx]), "%s", name);
  char path[MAX_PATH] = "?";
  GetModuleFileNameA(module, path, MAX_PATH);
  const char* leaf = std::strrchr(path, '\\');
  logf("trace: GetProcAddress(%s, %s) thread %lu", leaf ? leaf + 1 : path, name, GetCurrentThreadId());
}

FARPROC WINAPI hk_get_proc_address(HMODULE module, LPCSTR name) {
  FARPROC real = g_orig_gpa(module, name);
  InterlockedIncrement(&g_gpa_calls);
  if (!real || !name || !HIWORD(reinterpret_cast<uintptr_t>(name))) return real;
  trace_gpa(module, name);
  return real;
}

LRESULT CALLBACK hk_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  const dispatch::Snapshot s = dispatch::snapshot();
  const bool showable = s.show_panel || config::get().always_show;
  // The skip key during a cutscene, and the prompt's keys while it asks.
  if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && !(lp & (1 << 30)) && cutscene::on_key(static_cast<int>(wp)))
    return 0;
  // Nothing of the keyboard reaches the game while it is paused for the prompt.
  if (cutscene::phase() == cutscene::Phase::kAsking && msg >= WM_KEYFIRST && msg <= WM_KEYLAST) return 0;
  // F-keys above F9 arrive as WM_SYSKEYDOWN; accept both, ignore auto-repeat.
  if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && !(lp & (1 << 30)) &&
      static_cast<int>(wp) == config::get().toggle_key && showable) {
    ImGuiLock guard;  // wants_draw reads and clears this
    g_user_hidden = !g_user_hidden;
    logf("panel %s by the toggle key", g_user_hidden ? "hidden" : "shown");
    return 0;
  }
  // ImGui sees every message, visible or not: a button release that arrives
  // after the panel is hidden would otherwise never be delivered, leaving it
  // convinced the button is still held (and the window stuck to the pointer).
  if (g_context_ready) {
    bool swallow = false;
    {
      ImGuiLock guard;
      if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return 1;
      // Swallowing is the visible panel's business, and only for what it is
      // using. The game reads its keys with GetAsyncKeyState rather than from
      // here (input.cpp does the real work), and it reads no mouse at all.
      if (g_visible) {
        const ImGuiIO& io = ImGui::GetIO();
        if (io.WantCaptureMouse && msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) swallow = true;
        if (io.WantTextInput && msg >= WM_KEYFIRST && msg <= WM_KEYLAST && wp != VK_ESCAPE) swallow = true;
      }
    }
    if (swallow) return 0;
  }
  // The game's own window procedure runs outside the lock.
  return CallWindowProcA(g_orig_wndproc, hwnd, msg, wp, lp);
}

// --- the pointer -------------------------------------------------------------------
// GOG's wrapper keeps the pointer inside the game picture with ClipCursor: the
// picture's rectangle (the centred 4:3 part of a widescreen window), set when
// the window activates and released when it deactivates - and the game's own
// ClipCursor calls it turns into no-ops. That leaves the panel, in the bars
// either side of the picture, out of the pointer's reach, and every other
// window and monitor too. So the wrapper's own ClipCursor import is hooked:
// while the panel is on screen the pointer is let go altogether - a clip the
// wrapper sets meanwhile is remembered and answered with a release - and when
// the panel goes, the wrapper's last clip is put back. Its releases always go
// through, and it releases when the window deactivates, so a clip is only ever
// put back while the game is the active window, never over whatever the player
// went to.
using ClipCursorFn = BOOL(WINAPI*)(const RECT*);
ClipCursorFn g_real_clip_cursor = nullptr;  // user32's, as the wrapper's import slot held it
uintptr_t g_clip_slot = 0;
CRITICAL_SECTION g_cursor_cs;     // created in install(), before the hook
bool g_wrapper_clipping = false;  // the wrapper's last call set a clip rather than releasing it
RECT g_wrapper_clip{};            // ...and this was the clip
bool g_cursor_free = false;       // the panel is up: nothing of ours or the wrapper's holds the pointer
volatile LONG g_clip_logs = 0;

// The whole desktop, every monitor, in screen coordinates: what GetClipCursor
// reports when nothing holds the pointer.
RECT desktop_rect() {
  const int x = GetSystemMetrics(SM_XVIRTUALSCREEN), y = GetSystemMetrics(SM_YVIRTUALSCREEN);
  return RECT{x, y, x + GetSystemMetrics(SM_CXVIRTUALSCREEN), y + GetSystemMetrics(SM_CYVIRTUALSCREEN)};
}

BOOL WINAPI hk_clip_cursor(const RECT* r) {
  EnterCriticalSection(&g_cursor_cs);
  g_wrapper_clipping = r != nullptr;
  if (r) g_wrapper_clip = *r;
  // A clip held back is still a success to the wrapper. It is answered with a
  // release rather than nothing, so a hold something else took on the same
  // occasion (Wine can confine a fullscreen window as it activates) goes too.
  const bool held_back = r && g_cursor_free;
  const BOOL ok = g_real_clip_cursor(held_back ? nullptr : r);
  LeaveCriticalSection(&g_cursor_cs);
  if (InterlockedIncrement(&g_clip_logs) <= 4) {
    if (r)
      logf("pointer: the wrapper clips it to %ld,%ld-%ld,%ld%s", r->left, r->top, r->right, r->bottom,
           held_back ? " - held back while the panel is up" : "");
    else
      logf("pointer: the wrapper releases its clip");
  }
  return ok;
}

// The cursor itself. The game hides it once, as its renderer starts full
// screen (ShowCursor(FALSE) at 0x497230 - its window class has the arrow), and
// the panel used to draw ImGui's own in its place. But an invisible cursor is
// also what makes gamescope hold the pointer: it switches to relative mouse
// mode, which locks the pointer to its window, whenever the focused window's
// cursor is hidden. So while the panel is on screen the game's own cursor is
// shown - ShowCursor(TRUE) until the display count is back at 0, looked at
// every frame, each call counted - and every call is taken back when the panel
// goes, which leaves the game's count exactly as it was. ImGui sets the
// cursor's shape instead of drawing one (io.MouseDrawCursor, wants_draw).
int g_cursor_shows = 0;  // ShowCursor(TRUE) calls of ours still to take back

void show_game_cursor(bool show) {
  if (show) {
    // The display count, read without changing it: one up, one down.
    int count = ShowCursor(TRUE) - 1;
    ShowCursor(FALSE);
    const bool first = g_cursor_shows == 0;
    while (count < 0 && g_cursor_shows < 64) {
      count = ShowCursor(TRUE);
      ++g_cursor_shows;
    }
    if (first && g_cursor_shows)
      logf("pointer: the game's cursor is shown while the panel is up (%d ShowCursor call(s), display count %d)",
           g_cursor_shows, count);
  } else if (g_cursor_shows) {
    const int n = g_cursor_shows;
    for (; g_cursor_shows > 0; --g_cursor_shows) ShowCursor(FALSE);
    logf("pointer: the game's cursor is hidden again (%d ShowCursor call(s) taken back)", n);
  }
}

// From wants_draw, every frame: the panel is on screen, or not.
void free_cursor(bool free) {
  if (config::get().disable_cursor) return;
  show_game_cursor(free);
  if (!g_real_clip_cursor) return;
  EnterCriticalSection(&g_cursor_cs);
  if (free != g_cursor_free) {
    g_cursor_free = free;
    if (free) g_real_clip_cursor(nullptr);
    else if (g_wrapper_clipping) g_real_clip_cursor(&g_wrapper_clip);
    // Whether it is really free is the system's answer, not ours: say so when
    // something else still holds it.
    const RECT desk = desktop_rect();
    RECT now{};
    const bool held = GetClipCursor(&now) && !EqualRect(&now, &desk);
    if (free && !held)
      logf("pointer: let go while the panel is up - it can leave the game for other windows and monitors");
    else if (free)
      logf("pointer: let go while the panel is up, but something else still holds it to %ld,%ld-%ld,%ld (the "
           "desktop is %ld,%ld-%ld,%ld)",
           now.left, now.top, now.right, now.bottom, desk.left, desk.top, desk.right, desk.bottom);
    else
      logf("pointer: %s", g_wrapper_clipping ? "back in the wrapper's clip" : "the wrapper holds no clip to put back");
  }
  LeaveCriticalSection(&g_cursor_cs);
}

void install_cursor_hook(HMODULE wrapper) {
  const uintptr_t slot = mem::iat_slot(wrapper, "USER32.dll", "ClipCursor");
  void* prev = slot ? mem::read<void*>(slot) : nullptr;
  if (!prev) {
    logf("overlay: ClipCursor is not in ddraw_orig.dll's import table - the pointer stays in the game picture");
    return;
  }
  g_real_clip_cursor = reinterpret_cast<ClipCursorFn>(prev);
  if (!mem::write<uintptr_t>(slot, reinterpret_cast<uintptr_t>(&hk_clip_cursor))) {
    g_real_clip_cursor = nullptr;
    logf("overlay: ddraw_orig.dll's ClipCursor import could not be written - the pointer stays in the game picture");
    return;
  }
  g_clip_slot = slot;
  logf("overlay: ddraw_orig.dll's ClipCursor import hooked (original %p)", prev);
}

// The part of the window the pointer can reach: all of it, or what a cursor
// clip leaves of it (the game picture, should letting the pointer go have
// failed, or something else still hold it). A panel outside it could never be
// clicked or dragged back.
ImVec4 reachable_rect(const ImVec2& display) {
  ImVec4 r(0.0f, 0.0f, display.x, display.y);
  RECT clip{};
  if (!g_hwnd || !GetClipCursor(&clip)) return r;
  POINT a{clip.left, clip.top}, b{clip.right, clip.bottom};
  ScreenToClient(g_hwnd, &a);
  ScreenToClient(g_hwnd, &b);
  const float l = static_cast<float>(a.x), t = static_cast<float>(a.y);
  const float rt = static_cast<float>(b.x), bm = static_cast<float>(b.y);
  const ImVec4 c(l > 0.0f ? l : 0.0f, t > 0.0f ? t : 0.0f, rt < display.x ? rt : display.x,
                 bm < display.y ? bm : display.y);
  if (c.z - c.x < 240.0f || c.w - c.y < 240.0f) return r;  // a clip that makes no sense is ignored
  return c;
}

// Keep the current window inside `r`: moved back whenever any part of it has
// ended up outside (a saved position from a wider reach, a panel that grew).
void keep_in_reach(const ImVec4& r) {
  const ImVec2 pos = ImGui::GetWindowPos(), size = ImGui::GetWindowSize();
  ImVec2 want = pos;
  if (want.x + size.x > r.z) want.x = r.z - size.x;
  if (want.y + size.y > r.w) want.y = r.w - size.y;
  if (want.x < r.x) want.x = r.x;
  if (want.y < r.y) want.y = r.y;
  if (want.x != pos.x || want.y != pos.y) ImGui::SetWindowPos(want);
}

void cheat_row(cheats::Kind k, const char* label, const char* help, bool available, const char* unavailable_text) {
  bool v = cheats::enabled(k);
  if (!available) ImGui::BeginDisabled();
  if (ImGui::Checkbox(label, &v)) {
    cheats::set_enabled(k, v);
    logf("%s %s (panel)", cheats::name(k), v ? "ON" : "OFF");
  }
  if (!available) ImGui::EndDisabled();
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && ImGui::BeginTooltip()) {
    ImGui::PushTextWrapPos(scaled(380.0f));
    ImGui::TextUnformatted(help);
    if (!available && unavailable_text) {
      ImGui::Separator();
      ImGui::TextUnformatted(unavailable_text);
    }
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
  }
  if (!available && unavailable_text) {
    ImGui::SameLine();
    ImGui::TextDisabled("(%s)", unavailable_text);
  }
}

void hms(float seconds, char* out, size_t n) {
  if (seconds < 0.0f) {
    std::snprintf(out, n, "?");
    return;
  }
  const int t = static_cast<int>(seconds);
  std::snprintf(out, n, "%d:%02d:%02d", t / 3600, (t / 60) % 60, t % 60);
}

// A dollar amount, with thousands separators.
void dollars(int v, char* out, size_t n) {
  if (v < 0) std::snprintf(out, n, "?");
  else if (v >= 1000000) std::snprintf(out, n, "$%d,%03d,%03d", v / 1000000, (v / 1000) % 1000, v % 1000);
  else if (v >= 1000) std::snprintf(out, n, "$%d,%03d", v / 1000, v % 1000);
  else std::snprintf(out, n, "$%d", v);
}

// The help for the item just drawn, on hover (a disabled one too).
void item_help(const char* help) {
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && ImGui::BeginTooltip()) {
    ImGui::PushTextWrapPos(scaled(380.0f));
    ImGui::TextUnformatted(help);
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
  }
}

// --- the Mercenaries money -------------------------------------------------------------
// An amount's field follows the game's value until something is typed into it,
// and again once the game holds what was set, or from the next time the panel
// comes up.
struct MoneyEdit {
  int value = 0;
  bool typed = false;
  bool sent = false;
};
MoneyEdit g_takings, g_bank;

void forget_money_edits() { g_takings = g_bank = MoneyEdit{}; }

// One amount: what the game holds, a field kept to 0..max (at `column`, so the
// rows line up), and Set.
void money_row(const char* label, const char* id, int have, int max, int step, float column, MoneyEdit* e,
               const char* help, void (*set)(int)) {
  if (e->typed && e->sent && have == e->value) e->typed = e->sent = false;
  if (!e->typed && have >= 0) e->value = have;
  char now[32], top[32];
  dollars(have, now, sizeof(now));
  dollars(max, top, sizeof(top));
  ImGui::PushID(id);
  ImGui::Text("%s: %s", label, now);
  item_help(help);
  ImGui::SameLine(column);
  ImGui::SetNextItemWidth(scaled(130.0f));
  if (ImGui::InputInt("##amount", &e->value, step, step * 10)) {
    e->typed = true;
    e->sent = false;
    e->value = e->value < 0 ? 0 : e->value > max ? max : e->value;
  }
  item_help(help);
  ImGui::SameLine();
  if (ImGui::SmallButton("Set")) {
    set(e->value);
    e->sent = true;
  }
  ImGui::SameLine();
  ImGui::TextDisabled("(0 to %s)", top);
  ImGui::PopID();
}

// In the Mercenaries only (draw_cheats).
void draw_money(const cheats::Status& st) {
  if (!st.money_ok) {
    ImGui::TextDisabled("Money: not found in this game");
    return;
  }
  const float column = ImGui::CalcTextSize("Takings this run: $999,999").x + ImGui::GetStyle().ItemSpacing.x;
  money_row("Takings this run", "takings", st.takings, game::kTakingsMax, 100, column, &g_takings,
            "What this run has earned so far: every kill pays into it. The results rank you on it - $1,500 is an A - "
            "and pay it into your bank, with half the seconds left on the clock and any bonus on top. It goes up to "
            "$9,999 here: the results count that whole sum in a way that goes wrong past $32,767, and this leaves "
            "room for the time bonus and for everything the rest of the run earns.",
            cheats::request_takings);
  money_row("Bank", "bank", st.bank, st.bank_max, 1000, column, &g_bank,
            "Your Mercenaries money: every run's results pay into it, and the shop after them spends it. The game "
            "keeps it in saves\\data.sav and writes it there after a run's results, so a change made here is kept "
            "once you have finished the run. The game itself never holds more than $999,999.",
            cheats::request_bank);
}

// --- the clear count and the epilogues -------------------------------------------------
// The main game's own (draw_cheats): the times it has been finished, which the
// ending shows an epilogue for and the title menu's EPILOGUE screen unlocks
// them by.
void draw_clears(const cheats::Status& st) {
  ImGui::SeparatorText("Clears & epilogues");
  if (!st.clears_ok) {
    ImGui::TextDisabled("Clear count: not found in this game - see the log");
    return;
  }
  const int n = st.epilogues;
  char help[900];
  std::snprintf(help, sizeof(help),
                "How many times the game has been finished. The ending adds one to it and records it in "
                "saves\\data.sav, and the title menu's EPILOGUE screen unlocks one epilogue for every clear recorded "
                "there, up to all %d. Each of the first %d endings also shows its new epilogue, except on Easy.\n\n"
                "To unlock them all, set this to %d before you finish: the ending shows the last epilogue and one "
                "more picture after it, and the EPILOGUE screen has all %d from then on. %d or more unlocks them all "
                "too, but then the ending shows none.\n\n"
                "The count is saved with the game, and the EPILOGUE screen goes by the last ending's, so finishing an "
                "older save that counts fewer clears takes it back down. It also decides Nemesis's seventh drop on "
                "Hard: an assault rifle while it is 0, Inf. Bullets after that.",
                n, n, n - 1, n, n);
  // The field follows the game's count whenever that changes (a load, an Apply),
  // and holds what is typed into it until then.
  static int edit_clears = -1, seen_clears = -1;
  if (st.clears != seen_clears) edit_clears = seen_clears = st.clears;
  ImGui::Text("Times cleared: %d", st.clears);
  item_help(help);
  ImGui::SameLine();
  ImGui::SetNextItemWidth(scaled(100.0f));
  if (ImGui::InputInt("##clears", &edit_clears))
    edit_clears = edit_clears < 0 ? 0 : edit_clears > game::kClearsMax ? game::kClearsMax : edit_clears;
  item_help(help);
  ImGui::SameLine();
  if (ImGui::SmallButton("Apply##clears")) cheats::request_clears(edit_clears);
  // What this game's ending will make of it.
  ImGui::SameLine();
  if (st.easy) ImGui::TextDisabled("(Easy: the ending shows no epilogue, but counts the clear)");
  else if (st.clears < n - 1) ImGui::TextDisabled("(the ending shows epilogue %d of %d)", st.clears + 1, n);
  else if (st.clears == n - 1) ImGui::TextDisabled("(the ending shows the last epilogue and unlocks all %d)", n);
  else ImGui::TextDisabled("(the ending unlocks all %d epilogues, but shows none)", n);
}

// --- Jill's outfit ------------------------------------------------------------------------
// The main game's own (draw_cheats): the outfit the title offers before a new
// game, picked here at any time. The game only loads her model at a room load
// or a session start, so the pick shows from the next door.
void draw_outfit(const cheats::Status& st) {
  if (!st.outfit_ok) {
    ImGui::TextDisabled("Outfit: not found in this game - see the log");
    return;
  }
  const char* help =
      "Jill's outfit: the costume the game lets you pick before a new game, with the names its COSTUME SELECT "
      "screen gives them, changed here at any time. The game only loads Jill's model when she goes through a door "
      "(or a game starts), so she changes into the outfit at the next door you go through. The outfit is saved "
      "with the game, so a save made after the change keeps it.\n\n"
      "Special 5 is Regina, from Dino Crisis. Normal shows Jill's backpack once she has it. While Carlos is in "
      "play, the outfit is the one Jill wears when she is back.";
  const ImVec4 orange(1.0f, 0.6f, 0.3f, 1.0f);
  char label[64];
  if (st.outfit < 0) std::snprintf(label, sizeof(label), "?");
  else if (st.outfit_boutique) std::snprintf(label, sizeof(label), "%s (boutique)", game::outfit_name(st.outfit));
  else std::snprintf(label, sizeof(label), "%s", game::outfit_name(st.outfit));
  // As wide as the longest name, or what the box shows now.
  float widest = ImGui::CalcTextSize(label).x;
  for (int o = 0; o < game::kOutfits; ++o) {
    const float w = ImGui::CalcTextSize(game::outfit_name(o)).x;
    if (w > widest) widest = w;
  }
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted("Outfit");
  item_help(help);
  ImGui::SameLine();
  ImGui::SetNextItemWidth(widest + ImGui::GetStyle().FramePadding.x * 2.0f + ImGui::GetFrameHeight());
  if (ImGui::BeginCombo("##outfit", label)) {
    for (int o = 0; o < game::kOutfits; ++o) {
      const bool worn = o == st.outfit && !st.outfit_boutique;
      if (ImGui::Selectable(game::outfit_name(o), worn) && !worn) cheats::request_outfit(o);
    }
    ImGui::EndCombo();
  }
  item_help(help);
  if (st.outfit_pending) {
    ImGui::SameLine();
    ImGui::TextColored(orange, "(Jill changes at the next door)");
  } else if (!st.jill) {
    ImGui::SameLine();
    ImGui::TextDisabled("(Jill wears it when she is back)");
  }
}

// --- the reloading tool's proficiency ---------------------------------------------------
// One row per kind of ammo: the level its next batch is made at, picked from the
// game's own table, and the batches of it made so far. A level is set as the
// fewest batches that give it - except that for the kinds with enhanced rounds,
// a level whose span takes in the batch count the game starts offering them at
// is set there, so a level picked here gives everything it can.
int mix_target(int kind, int level) {
  int v = game::mix_level_from(kind, level);
  const int offer = game::mix_enhanced_after() + 1;
  if (game::mix_offers_enhanced(kind) && v < offer && game::mix_level(kind, offer) == level) v = offer;
  return v;
}

// "Level 3: +30%", and with `span` the batch counts it covers.
void mix_level_label(int kind, int level, bool span, char* out, size_t n) {
  const int bonus = game::mix_bonus(kind, level);
  char what[16];
  if (bonus) std::snprintf(what, sizeof(what), "+%d%%", bonus * 10);
  else std::snprintf(what, sizeof(what), "no bonus");
  const int from = game::mix_level_from(kind, level);
  if (!span) std::snprintf(out, n, "Level %d: %s", level + 1, what);
  else if (level + 1 >= game::kMixLevels) std::snprintf(out, n, "Level %d: %s   (%d or more made)", level + 1, what, from);
  else std::snprintf(out, n, "Level %d: %s   (%d-%d made)", level + 1, what, from, game::mix_level_from(kind, level + 1) - 1);
}

// What a batch of each powder comes to at a level, `made` batches in: the
// game's own recipes and arithmetic.
void mix_yields(int kind, int level, int made, bool easy) {
  game::MixRecipe r[8];
  const int n = game::mix_recipes(kind, r, 8);
  const int bonus = game::mix_bonus(kind, level), enhanced = game::mix_enhanced_bonus(bonus);
  const bool offered = game::mix_offers_enhanced(kind) && made > game::mix_enhanced_after();
  char label[32];
  mix_level_label(kind, level, false, label, sizeof(label));
  ImGui::Text("%s - a batch with the reloading tool makes", label);
  for (int i = 0; i < n; ++i) {
    ImGui::BulletText("%s: %d %s", items::name(r[i].powder), game::mix_batch(r[i].base, bonus, easy),
                      items::name(r[i].result));
    if (offered && r[i].enhanced && enhanced >= 0) {
      ImGui::SameLine();
      ImGui::TextDisabled("or %d %s", game::mix_batch(r[i].enhanced_base, enhanced, easy), items::name(r[i].enhanced));
    }
  }
  if (!n) ImGui::TextDisabled("(the game's recipes were not found)");
  if (easy) ImGui::TextDisabled("Doubled: you are playing on Easy.");
  if (kind == game::kMixGrenade)
    ImGui::TextDisabled("Powder mixed straight into grenade rounds counts as a batch too, and gets the same bonus.");
}

void draw_mixing(const cheats::Status& st, int who) {
  ImGui::SeparatorText("Reloading tool");
  if (!st.mix_ok) {
    ImGui::TextDisabled("Proficiency: not found in this game - see the log");
    return;
  }
  ImGui::SameLine();
  ImGui::TextDisabled("(?)");
  {
    char help[900];
    std::snprintf(help, sizeof(help),
                  "Every batch of ammo you make with the reloading tool makes you better at that kind of ammo: the "
                  "game counts the batches of each kind, and the more you have made, the bigger each new batch - up "
                  "to %d%% more at level %d. Pick a level to set it: the count of batches made goes to the fewest "
                  "that give that level.\n\n"
                  "Handgun bullets and shotgun shells also offer enhanced rounds once more than %d batches of them "
                  "have been made: the game asks each time whether to make them instead, at the bonus of the level "
                  "below. On Easy every batch comes out doubled.\n\n"
                  "Hover over a level to see what each powder makes at it. The counts belong to the inventory of the "
                  "character you are playing (Carlos has his own) and are saved with the game.",
                  10 * game::mix_bonus(game::kMixHandgun, game::kMixLevels - 1), game::kMixLevels,
                  game::mix_enhanced_after());
    item_help(help);
  }
  if (who == 1) ImGui::TextDisabled("Carlos's own counts");
  char label[64];
  if (ImGui::BeginTable("mixing", 3, ImGuiTableFlags_SizingFixedFit)) {
    for (int k = 0; k < game::kMixKinds; ++k) {
      const int made = st.mixes[k] < 0 ? 0 : st.mixes[k];
      const int level = game::mix_level(k, made);
      const bool offered = game::mix_offers_enhanced(k) && made > game::mix_enhanced_after();
      ImGui::PushID(k);
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted(game::mix_kind_name(k));
      ImGui::TableNextColumn();
      mix_level_label(k, level, false, label, sizeof(label));
      ImGui::SetNextItemWidth(scaled(160.0f));
      if (ImGui::BeginCombo("##level", label)) {
        for (int l = 0; l < game::kMixLevels; ++l) {
          char item[64];
          mix_level_label(k, l, true, item, sizeof(item));
          if (ImGui::Selectable(item, l == level) && l != level) cheats::request_mixes(k, mix_target(k, l));
          if (ImGui::IsItemHovered() && ImGui::BeginTooltip()) {
            mix_yields(k, l, mix_target(k, l), st.easy);
            ImGui::EndTooltip();
          }
        }
        ImGui::EndCombo();
      } else if (ImGui::IsItemHovered() && ImGui::BeginTooltip()) {
        mix_yields(k, level, made, st.easy);
        ImGui::EndTooltip();
      }
      ImGui::TableNextColumn();
      ImGui::TextDisabled("%d made%s", made, offered ? ", enhanced rounds offered" : "");
      ImGui::PopID();
    }
    ImGui::EndTable();
  }
  // Only the kinds below the top level move: a count already past it is left as it is.
  const int top = game::kMixLevels - 1;
  bool below = false;
  for (int k = 0; k < game::kMixKinds; ++k) below |= game::mix_level(k, st.mixes[k] < 0 ? 0 : st.mixes[k]) < top;
  ImGui::BeginDisabled(!below);
  std::snprintf(label, sizeof(label), "All to level %d", top + 1);
  if (ImGui::SmallButton(label))
    for (int k = 0; k < game::kMixKinds; ++k)
      if (game::mix_level(k, st.mixes[k] < 0 ? 0 : st.mixes[k]) < top) cheats::request_mixes(k, mix_target(k, top));
  ImGui::EndDisabled();
  char tip[128];
  std::snprintf(tip, sizeof(tip), "Every kind of ammo at level %d: +%d%% on every batch, and the enhanced rounds on offer",
                top + 1, 10 * game::mix_bonus(game::kMixHandgun, top));
  item_help(tip);
}


// --- the inventory and the item box ---------------------------------------------------
// An inventory row being edited (RE1's buffer). A dirty row stops following the
// live inventory - that is the point of it - so it must not outlive the panel
// it was typed into: reopening the status screen shows the inventory as it is
// now, not an edit abandoned before a save, a load or a pickup moved it on.
int g_edit_id[inventory::kBagMax], g_edit_cnt[inventory::kBagMax];
bool g_edit_dirty[inventory::kBagMax];
int g_ask_slot = -1;           // the row a confirmation is open for
int g_drop_slot = -1;          // the box slot the x button asked about...
inventory::Slot g_drop_seen;   // ...and what it held

void forget_item_dialogs() {
  std::memset(g_edit_dirty, 0, sizeof(g_edit_dirty));
  g_ask_slot = -1;
  g_drop_slot = -1;
}

// An item as the panel names it: its name, or its number for an id the table
// does not know.
const char* item_label(int id, char* buf, size_t n) {
  if (id == 0) return "(empty)";
  if (items::find(id)) return items::name(id);
  std::snprintf(buf, n, "item 0x%02X", id);
  return buf;
}

// Case-insensitive substring match against "<id> 0x<id> <name>", so a list can
// be narrowed by typing a name, the bracketed name an item shows before it is
// examined, or an item number. The picker and the item box share it.
bool item_matches(int id, const char* name, const char* needle) {
  if (!needle || !needle[0]) return true;
  char hay[112];
  std::snprintf(hay, sizeof(hay), "%d 0x%02X %s", id, id, name);
  auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; };
  for (const char* p = hay; *p; ++p) {
    size_t i = 0;
    while (needle[i] && lower(p[i]) == lower(needle[i])) ++i;
    if (!needle[i]) return true;
  }
  return false;
}

// Every item the picker offers, bank by bank and by name within a bank.
const int* picker_order(int* n_out) {
  static int order[items::kCount];
  static int n = -1;
  if (n < 0) {
    n = 0;
    for (const items::Def& d : items::kTable)
      if (items::listable(d.id)) order[n++] = d.id;
    for (int i = 1; i < n; ++i)
      for (int j = i; j > 0 && items::listed_before(order[j], order[j - 1]); --j) std::swap(order[j], order[j - 1]);
  }
  *n_out = n;
  return order;
}

// Where a move would put an item, for a button's tooltip: the same answer the
// move itself acts on (inventory::fit_into_*).
void describe_fit(const inventory::Fit& f, int id, int count, const char* dest, const char* free_name) {
  const int topped = f.topped();
  const char* name = items::name(id);
  if (topped > 0 && f.slot >= 0)
    ImGui::Text("Put %d onto the %s %s already holds and %d into %s %d", topped, name, dest, f.into, free_name, f.slot + 1);
  else if (topped > 0)
    ImGui::Text("Put %d onto the %s %s already holds", topped, name, dest);
  else
    ImGui::Text("Put %s%s in %s %d", name, items::quantity(id, count).text, free_name, f.slot + 1);
  if (f.left > 0) ImGui::TextDisabled("The other %d do not fit and stay where they are", f.left);
}

// The inventory of the character in play, as an editable table. Edits live in
// a buffer until Apply, and both Apply and the discard ask first: they write to
// the game's inventory or throw away what was typed.
void draw_inventory(const inventory::View& v) {
  const ImVec4 orange(1.0f, 0.6f, 0.3f, 1.0f);
  const char* who = inventory::who_name(v.who);
  bool ask_apply = false, ask_discard = false;
  char lb[24];

  ImGui::SeparatorText("Inventory");
  ImGui::SameLine();
  ImGui::TextDisabled("(?)");
  if (ImGui::IsItemHovered() && ImGui::BeginTooltip()) {
    ImGui::PushTextWrapPos(scaled(400.0f));
    ImGui::TextUnformatted(
        "The inventory of the character you are playing, straight from the game. Pick an item and a count for a slot "
        "and press Apply; Store moves the item into the item box below.\n\n"
        "The game keeps the inventory packed: something new goes into the first empty slot, and emptying a slot moves "
        "the items after it up. The game's own screen draws its icons as it opens, so it shows a change the next time "
        "you open it.\n\n"
        "E marks the equipped weapon. Putting another weapon in its slot equips that one, and emptying or storing it "
        "unequips it: the game hands you the new weapon when the status screen closes.");
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
  }
  if (!v.known) {
    ImGui::TextColored(orange, "The inventory was not found in this build - see the log.");
    return;
  }
  if (v.size == 0) {
    ImGui::TextDisabled("(the inventory cannot be read right now)");
    return;
  }
  ImGui::Text("%s - %d of %d slots used", who, v.held, v.size);
  if (!v.open) {
    ImGui::SameLine();
    ImGui::TextDisabled("(open the status screen to change it)");
  } else if (v.redraw_due) {
    ImGui::TextColored(orange, "Close and reopen the status screen to see the change on the game's own screen.");
  }

  ImGui::BeginDisabled(!v.open);
  if (ImGui::BeginTable("bag", 5, ImGuiTableFlags_SizingFixedFit)) {
    ImGui::TableSetupColumn("slot");
    ImGui::TableSetupColumn("item");
    ImGui::TableSetupColumn("count");
    ImGui::TableSetupColumn("");
    ImGui::TableSetupColumn("");
    for (int s = 0; s < v.size && s < inventory::kBagMax; ++s) {
      const inventory::Slot live = v.bag[s];
      if (!g_edit_dirty[s]) {
        g_edit_id[s] = live.id;
        g_edit_cnt[s] = live.count;
      }
      const int id = g_edit_id[s];
      ImGui::PushID(s);
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::Text("%d%s", s + 1, s == v.equipped ? " E" : "");
      if (s == v.equipped && ImGui::IsItemHovered() && ImGui::BeginTooltip()) {
        ImGui::TextUnformatted("The equipped weapon");
        ImGui::EndTooltip();
      }

      ImGui::TableNextColumn();
      ImGui::SetNextItemWidth(scaled(250.0f));
      if (ImGui::BeginCombo("##item", item_label(id, lb, sizeof(lb)), ImGuiComboFlags_HeightLarge)) {
        auto pick = [&](int nid) {
          if (nid == g_edit_id[s]) return;
          g_edit_id[s] = nid;
          g_edit_cnt[s] = inventory::default_count(nid);
          g_edit_dirty[s] = true;
        };
        // The filter belongs to whichever list is open, so it starts empty and
        // with the caret in it every time one does.
        static char filter[48] = {};
        if (ImGui::IsWindowAppearing()) {
          filter[0] = '\0';
          ImGui::SetKeyboardFocusHere();
        }
        ImGui::SetNextItemWidth(-FLT_MIN);
        const bool enter = ImGui::InputTextWithHint("##filter", "type to narrow the list", filter, sizeof(filter),
                                                    ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::Separator();
        int first = -1, shown = 0;
        if (item_matches(0, "(empty)", filter)) {
          first = 0;
          ++shown;
          if (ImGui::Selectable("(empty)", g_edit_id[s] == 0)) pick(0);
        }
        int n = 0;
        const int* order = picker_order(&n);
        int bank = -1;
        for (int k = 0; k < n; ++k) {
          const int oid = order[k];
          const char* label = items::name(oid);
          if (!item_matches(oid, label, filter)) continue;
          if (items::bank(oid) != bank) {
            bank = items::bank(oid);
            ImGui::SeparatorText(items::bank_name(bank));
          }
          if (first < 0) first = oid;
          ++shown;
          const bool sel = oid == g_edit_id[s];
          ImGui::PushID(oid);
          if (ImGui::Selectable(label, sel)) pick(oid);
          ImGui::PopID();
          if (sel && !filter[0]) ImGui::SetItemDefaultFocus();
        }
        if (!shown) ImGui::TextDisabled("nothing matches \"%s\"", filter);
        if (enter && first >= 0) {  // Enter takes the first match
          pick(first);
          ImGui::CloseCurrentPopup();
        }
        ImGui::EndCombo();
      }

      // The count, held to the game's own limits as it is typed, so the dialog
      // asks about the number that will actually be written.
      ImGui::TableNextColumn();
      const int limit = inventory::count_limit(id);
      if (!id || !limit) {
        ImGui::TextDisabled("-");
      } else {
        ImGui::SetNextItemWidth(scaled(80.0f));
        if (ImGui::InputInt("##cnt", &g_edit_cnt[s])) {
          const int lo = inventory::count_floor(id);
          if (g_edit_cnt[s] < lo) g_edit_cnt[s] = lo;
          if (g_edit_cnt[s] > limit) g_edit_cnt[s] = limit;
          g_edit_dirty[s] = true;
        }
        if (ImGui::IsItemHovered() && ImGui::BeginTooltip()) {
          if (items::is_weapon(id)) ImGui::Text("Rounds loaded - the %s takes up to %d", items::name(id), limit);
          else ImGui::Text("One slot holds up to %d %s", limit, items::name(id));
          ImGui::EndTooltip();
        }
      }

      ImGui::TableNextColumn();
      if (g_edit_dirty[s]) {
        if (ImGui::SmallButton("Apply")) {
          g_ask_slot = s;
          ask_apply = true;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) {
          g_ask_slot = s;
          ask_discard = true;
        }
      }

      // Into the item box, as the move itself would lay it out.
      ImGui::TableNextColumn();
      if (live.id && v.box_size) {
        const inventory::Fit f = inventory::fit_into_box(v, live.id, live.count);
        ImGui::BeginDisabled(!f.ok());
        if (ImGui::SmallButton("Store")) inventory::request_store(s, live);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && ImGui::BeginTooltip()) {
          if (!f.ok())
            ImGui::Text("The item box has no free slot%s", game::item_stacks(live.id) ? ", and no stack of it with room" : "");
          else describe_fit(f, live.id, live.count, "the item box", "box slot");
          if (f.ok() && s == v.equipped) ImGui::TextDisabled("It is equipped: storing it unequips it");
          ImGui::EndTooltip();
        }
      }
      ImGui::PopID();
    }
    ImGui::EndTable();
  }
  ImGui::EndDisabled();

  // Asked outside the table, so the dialog does not belong to a row that may
  // have been redrawn differently by the time it is answered - and outside the
  // disabled block, so an open dialog can always be answered or dismissed.
  if (ask_apply) ImGui::OpenPopup("apply this change?");
  if (ask_discard) ImGui::OpenPopup("discard this edit?");
  const int as = g_ask_slot;
  if (ImGui::BeginPopupModal("apply this change?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    if (as < 0 || as >= v.size || !g_edit_dirty[as] || !v.open) {
      g_ask_slot = -1;
      ImGui::CloseCurrentPopup();
    } else {
      const inventory::Slot live = v.bag[as];
      const int id = g_edit_id[as], cnt = g_edit_cnt[as];
      if (!id) {
        ImGui::Text("Empty %s's slot %d (%s%s)?", who, as + 1, item_label(live.id, lb, sizeof(lb)),
                    items::quantity(live.id, live.count).text);
        if (as + 1 < v.held) ImGui::TextDisabled("The items after it move up one slot, as the game keeps them.");
      }
      else if (!live.id)
        ImGui::Text("Put %s%s in %s's first empty slot (slot %d)?", items::name(id), items::quantity(id, cnt).text, who,
                    v.held + 1);
      else
        ImGui::Text("Set %s's slot %d to %s%s?", who, as + 1, items::name(id), items::quantity(id, cnt).text);
      ImGui::TextDisabled("This writes straight into the game's inventory.");
      if (as == v.equipped && live.id && id != live.id)
        ImGui::TextDisabled(items::is_weapon(id) ? "The slot is equipped: the game equips this weapon when the status "
                                                   "screen closes."
                                                 : "The slot is equipped: this unequips it.");
      if (ImGui::Button("Apply")) {
        inventory::request_set(as, id, cnt, live);
        g_edit_dirty[as] = false;
        g_ask_slot = -1;
        ImGui::CloseCurrentPopup();
      }
      ImGui::SameLine();
      if (ImGui::Button("Cancel")) {
        g_ask_slot = -1;
        ImGui::CloseCurrentPopup();
      }
    }
    ImGui::EndPopup();
  }
  if (ImGui::BeginPopupModal("discard this edit?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    if (as < 0 || as >= v.size || !g_edit_dirty[as] || !v.open) {
      g_ask_slot = -1;
      ImGui::CloseCurrentPopup();
    } else {
      ImGui::Text("Throw away the edit to %s's slot %d?", who, as + 1);
      ImGui::TextDisabled("The row goes back to showing what the inventory holds.");
      if (ImGui::Button("Discard")) {
        g_edit_dirty[as] = false;
        g_ask_slot = -1;
        ImGui::CloseCurrentPopup();
      }
      ImGui::SameLine();
      if (ImGui::Button("Keep editing")) {
        g_ask_slot = -1;
        ImGui::CloseCurrentPopup();
      }
    }
    ImGui::EndPopup();
  }
}

// The game's item box, listed one bank at a time and sorted by name - the game
// keeps its 64 slots in whatever order things were put in, and # says which
// slot an entry is in on the game's own item box screen.
void draw_item_box(const inventory::View& v) {
  const ImVec4 orange(1.0f, 0.6f, 0.3f, 1.0f);
  bool ask_drop = false;
  char lb[24];

  ImGui::SeparatorText("Item box");
  ImGui::SameLine();
  ImGui::TextDisabled("(?)");
  if (ImGui::IsItemHovered() && ImGui::BeginTooltip()) {
    ImGui::PushTextWrapPos(scaled(400.0f));
    ImGui::TextUnformatted(
        "The game's own item box: the same 64 slots every item box in the city opens, saved with your game. Take puts "
        "an item into the inventory of the character you are playing, x throws it away, and Store beside an inventory "
        "slot puts one in.\n\n"
        "Ammo, ink ribbons and the reloading tool top up a stack of the same item first, as the game's own box screen "
        "combines them; everything else takes a slot of its own. What fits nowhere stays where it was.\n\n"
        "The list is filed under banks - key items (the files and maps among them), weapons, ammo, heals, and other: "
        "gun powder, ink ribbons and Inf. Bullets - and each bank is sorted by name. # is the slot an item is in on "
        "the game's item box screen.");
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
  }
  if (!v.box_known) {
    ImGui::TextColored(orange, "The item box was not found in this build - see the log.");
    return;
  }
  if (v.box_size == 0) {
    ImGui::TextDisabled("(the item box cannot be read right now)");
    return;
  }
  // The used slots in listing order: bank, name, the fullest stack first, slot.
  int order[inventory::kBoxMax];
  int n = 0;
  for (int i = 0; i < v.box_size; ++i)
    if (v.box[i].id) order[n++] = i;
  const auto before = [&](int a, int b) {
    const inventory::Slot &x = v.box[a], &y = v.box[b];
    if (x.id != y.id) return items::listed_before(x.id, y.id);
    if (x.count != y.count) return x.count > y.count;
    return a < b;
  };
  for (int i = 1; i < n; ++i)
    for (int j = i; j > 0 && before(order[j], order[j - 1]); --j) std::swap(order[j], order[j - 1]);

  ImGui::Text("%d of %d slots used", n, v.box_size);

  // One bank on show at a time, with every tab carrying its count, and the
  // filter, kept across frames and banks and cleared by hand.
  static int shown_bank = items::kBankKey;
  static char filter[48] = {};
  int held[items::kBankCount] = {}, matched[items::kBankCount] = {};
  for (int k = 0; k < n; ++k) {
    const int id = v.box[order[k]].id;
    const int bk = items::bank(id);
    ++held[bk];
    if (item_matches(id, item_label(id, lb, sizeof(lb)), filter)) ++matched[bk];
  }
  if (ImGui::BeginTabBar("banks")) {
    for (int k = 0; k < items::kBankCount; ++k) {
      char tab[48];
      std::snprintf(tab, sizeof(tab), "%s %d###bank%d", items::bank_name(k), held[k], k);
      if (ImGui::BeginTabItem(tab)) {
        shown_bank = k;
        ImGui::EndTabItem();
      }
    }
    ImGui::EndTabBar();
  }
  const int bank = shown_bank;
  const int shown = matched[bank];
  if (n > 0) {
    ImGui::SetNextItemWidth(scaled(230.0f));
    ImGui::InputTextWithHint("##box filter", "type to narrow the list", filter, sizeof(filter));
    if (filter[0]) {
      ImGui::SameLine();
      if (ImGui::SmallButton("Clear")) filter[0] = '\0';
      ImGui::SameLine();
      ImGui::TextDisabled("%d of %d shown", shown, held[bank]);
    }
  }

  char table_id[16];  // each bank keeps its own scroll position
  std::snprintf(table_id, sizeof(table_id), "box%d", bank);
  if (n == 0) {
    ImGui::TextDisabled("The item box is empty - use Store beside an inventory slot to put something in it.");
  } else if (held[bank] == 0) {
    ImGui::TextDisabled("Nothing in the box is filed under %s.", items::bank_name(bank));
  } else if (shown == 0) {
    char elsewhere[160] = "";
    int len = 0;
    for (int k = 0; k < items::kBankCount; ++k)
      if (k != bank && matched[k] && len < static_cast<int>(sizeof(elsewhere)))
        len += std::snprintf(elsewhere + len, sizeof(elsewhere) - len, "%s%d under %s", len ? ", " : " - ", matched[k],
                             items::bank_name(k));
    ImGui::TextDisabled("nothing under %s matches \"%s\"%s", items::bank_name(bank), filter, elsewhere);
  } else if (ImGui::BeginTable(table_id, 4, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg,
                               ImVec2(0.0f, ImGui::GetTextLineHeightWithSpacing() * (shown > 12 ? 12.5f : shown + 0.5f)))) {
    ImGui::TableSetupColumn("item");
    ImGui::TableSetupColumn("count");
    ImGui::TableSetupColumn("#");
    ImGui::TableSetupColumn("");
    const char* who = inventory::who_name(v.who);
    for (int k = 0; k < n; ++k) {
      const int i = order[k];
      const inventory::Slot e = v.box[i];
      const char* label = item_label(e.id, lb, sizeof(lb));
      if (items::bank(e.id) != bank || !item_matches(e.id, label, filter)) continue;
      ImGui::PushID(i);
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(label);
      ImGui::TableNextColumn();
      if (inventory::count_limit(e.id)) ImGui::Text("%d", e.count);
      else ImGui::TextDisabled("-");
      ImGui::TableNextColumn();
      ImGui::TextDisabled("%d", i + 1);
      ImGui::TableNextColumn();
      const inventory::Fit f = v.size ? inventory::fit_into_bag(v, e.id, e.count) : inventory::Fit{};
      ImGui::BeginDisabled(!v.open || !f.ok());
      if (ImGui::SmallButton("Take")) inventory::request_take(i, e);
      ImGui::EndDisabled();
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && ImGui::BeginTooltip()) {
        if (!v.open) ImGui::TextUnformatted("Open the status screen to take it");
        else if (!f.ok())
          ImGui::Text("%s has no free slot%s", who, game::item_stacks(e.id) ? ", and no stack of it with room" : "");
        else describe_fit(f, e.id, e.count, who, "slot");
        ImGui::EndTooltip();
      }
      ImGui::SameLine();
      ImGui::BeginDisabled(!v.open);
      if (ImGui::SmallButton("x")) {
        g_drop_slot = i;
        g_drop_seen = e;
        ask_drop = true;
      }
      ImGui::EndDisabled();
      if (ImGui::IsItemHovered() && ImGui::BeginTooltip()) {
        ImGui::TextUnformatted("Throw this item away");
        ImGui::EndTooltip();
      }
      ImGui::PopID();
    }
    ImGui::EndTable();
  }

  // Asked outside the table so the dialog is not scoped to a row that may be
  // gone by the time it is answered.
  if (ask_drop) ImGui::OpenPopup("throw item away?");
  if (ImGui::BeginPopupModal("throw item away?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    if (g_drop_slot < 0 || !v.open) {
      g_drop_slot = -1;
      ImGui::CloseCurrentPopup();
    } else {
      ImGui::Text("Throw away %s%s from box slot %d?", item_label(g_drop_seen.id, lb, sizeof(lb)),
                  items::quantity(g_drop_seen.id, g_drop_seen.count).text, g_drop_slot + 1);
      ImGui::TextDisabled("It is gone for good once the game is saved.");
      if (ImGui::Button("Throw away")) {
        inventory::request_drop(g_drop_slot, g_drop_seen);
        g_drop_slot = -1;
        ImGui::CloseCurrentPopup();
      }
      ImGui::SameLine();
      if (ImGui::Button("Cancel")) {
        g_drop_slot = -1;
        ImGui::CloseCurrentPopup();
      }
    }
    ImGui::EndPopup();
  }
}

// --- the save files ----------------------------------------------------------------------
// The slot a Delete or Copy asked about, and what the panel saw of the slots
// when it was clicked. Like RE1's, it belongs to the panel it was clicked in: a
// screen that closed and came back asks again.
enum { kAskNone = 0, kAskDelete, kAskCopy };
int g_file_ask = kAskNone;
int g_file_from = -1, g_file_to = -1;
savefiles::Save g_file_from_seen, g_file_to_seen;

void forget_file_dialogs() {
  g_file_ask = kAskNone;
  g_file_from = g_file_to = -1;
}

// The memory card screen's panel: the game's fifteen slots, with a Delete and a
// Copy for each one holding a save. Both change the card the moment they are
// confirmed - the game's own and its file - so both ask first, and the request
// carries what was seen, so a slot that changed in between is left alone
// (savefiles.cpp).
void draw_save_files() {
  const savefiles::View v = savefiles::view();
  const ImVec4 orange(1.0f, 0.6f, 0.3f, 1.0f);
  bool open_delete = false, open_copy = false;

  ImGui::SeparatorText("Save files");
  ImGui::SameLine();
  ImGui::TextDisabled("(?)");
  if (ImGui::IsItemHovered() && ImGui::BeginTooltip()) {
    ImGui::PushTextWrapPos(scaled(420.0f));
    ImGui::Text("The game's fifteen save slots, on its memory card: %s.", v.file[0] ? v.file : "saves\\bu00.sav");
    ImGui::Spacing();
    ImGui::TextUnformatted(
        "Delete removes a save: the game's list shows No data for its slot from then on, exactly like a slot that "
        "was never used, and your next save there fills it again. Copy to... puts a copy of a save into another "
        "slot, empty or not.");
    ImGui::Spacing();
    ImGui::TextUnformatted(
        "Both work while the game's list of saves is up, and change the card straight away - the game's own and the "
        "file - so neither can be undone: back up the saves folder first if in doubt. The game's list beside this "
        "one shows each change at once; the row its cursor is on is highlighted here.");
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
  }
  if (!v.ready) {
    ImGui::TextColored(orange, "The game's memory cards were not found - see the log.");
    return;
  }
  if (!v.card_ok) ImGui::TextColored(orange, "The memory card cannot be changed: %s.", v.why[0] ? v.why : "see the log");
  else if (!v.waiting) ImGui::TextDisabled("Copy and Delete wait for the game's list of saves.");
  const bool can = v.card_ok && v.waiting;

  if (v.slots > 0 && ImGui::BeginTable("files", 3, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
    ImGui::TableSetupColumn("slot");
    ImGui::TableSetupColumn("save");
    ImGui::TableSetupColumn("");
    ImGui::TableHeadersRow();
    for (int i = 0; i < v.slots; ++i) {
      const savefiles::Save& f = v.save[i];
      ImGui::PushID(i);
      ImGui::TableNextRow();
      if (i == v.cursor) ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, ImGui::GetColorU32(ImGuiCol_Header));
      ImGui::TableNextColumn();
      ImGui::Text("%d", i + 1);
      ImGui::TableNextColumn();
      char d[128];
      savefiles::describe(f, d, sizeof(d));
      if (f.used) ImGui::TextUnformatted(d);
      else ImGui::TextDisabled("%s", d);
      if (f.odd) {
        ImGui::SameLine();
        ImGui::TextColored(orange, "(left alone)");
        item_help("This slot's file is not one the game writes - more than one block, or another file names the "
                  "same slot - so the panel does not touch it.");
      }
      ImGui::TableNextColumn();
      if (f.used && !f.odd) {
        ImGui::BeginDisabled(!can);
        if (ImGui::SmallButton("Copy to...")) ImGui::OpenPopup("copy to");
        ImGui::SameLine();
        if (ImGui::SmallButton("Delete")) {
          g_file_ask = kAskDelete;
          g_file_to = i;
          g_file_to_seen = f;
          open_delete = true;
        }
        ImGui::EndDisabled();
        if (ImGui::BeginPopup("copy to")) {
          ImGui::TextDisabled("Copy the save in slot %d to:", i + 1);
          ImGui::Separator();
          for (int j = 0; j < v.slots; ++j) {
            if (j == i) continue;
            const savefiles::Save& t = v.save[j];
            char dj[128], label[160];
            savefiles::describe(t, dj, sizeof(dj));
            std::snprintf(label, sizeof(label), "Slot %d - %s", j + 1, dj);
            // A slot the game keeps for other data cannot take it, nor an
            // empty one when every block of the card is in use.
            ImGui::BeginDisabled(t.other || t.odd || (!t.used && v.free_entries <= 0));
            if (ImGui::Selectable(label)) {
              g_file_ask = kAskCopy;
              g_file_from = i;
              g_file_to = j;
              g_file_from_seen = f;
              g_file_to_seen = t;
              open_copy = true;
            }
            ImGui::EndDisabled();
          }
          ImGui::EndPopup();
        }
      }
      ImGui::PopID();
    }
    ImGui::EndTable();
  }
  if (v.note[0]) {
    if (v.note_error) ImGui::PushStyleColor(ImGuiCol_Text, orange);
    ImGui::TextWrapped("%s", v.note);
    if (v.note_error) ImGui::PopStyleColor();
  }

  // Asked outside the table, so neither dialog belongs to a row.
  if (open_delete) ImGui::OpenPopup("delete this save?");
  if (open_copy) ImGui::OpenPopup("copy this save?");
  char d[128];
  if (ImGui::BeginPopupModal("delete this save?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    if (g_file_ask != kAskDelete || g_file_to < 0 || g_file_to >= v.slots || !can) {
      forget_file_dialogs();
      ImGui::CloseCurrentPopup();
    } else {
      savefiles::describe(g_file_to_seen, d, sizeof(d));
      ImGui::Text("Delete the save in slot %d (%s)?", g_file_to + 1, d);
      ImGui::TextDisabled("It is removed from the memory card straight away - the game's and its file - so this");
      ImGui::TextDisabled("cannot be undone; the game's list shows No data for the slot at once.");
      if (ImGui::Button("Delete")) {
        savefiles::request_delete(g_file_to, g_file_to_seen);
        forget_file_dialogs();
        ImGui::CloseCurrentPopup();
      }
      ImGui::SameLine();
      if (ImGui::Button("Cancel")) {
        forget_file_dialogs();
        ImGui::CloseCurrentPopup();
      }
    }
    ImGui::EndPopup();
  }
  if (ImGui::BeginPopupModal("copy this save?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    if (g_file_ask != kAskCopy || g_file_from < 0 || g_file_from >= v.slots || g_file_to < 0 || g_file_to >= v.slots ||
        !can) {
      forget_file_dialogs();
      ImGui::CloseCurrentPopup();
    } else {
      savefiles::describe(g_file_from_seen, d, sizeof(d));
      if (g_file_to_seen.used)
        ImGui::Text("Copy the save in slot %d (%s) over the save in slot %d?", g_file_from + 1, d, g_file_to + 1);
      else
        ImGui::Text("Copy the save in slot %d (%s) to the empty slot %d?", g_file_from + 1, d, g_file_to + 1);
      if (g_file_to_seen.used) {
        savefiles::describe(g_file_to_seen, d, sizeof(d));
        ImGui::TextDisabled("The save in slot %d (%s) is replaced.", g_file_to + 1, d);
      }
      ImGui::TextDisabled("The memory card changes straight away, so this cannot be undone.");
      if (ImGui::Button("Copy")) {
        savefiles::request_copy(g_file_from, g_file_to, g_file_from_seen, g_file_to_seen);
        forget_file_dialogs();
        ImGui::CloseCurrentPopup();
      }
      ImGui::SameLine();
      if (ImGui::Button("Cancel")) {
        forget_file_dialogs();
        ImGui::CloseCurrentPopup();
      }
    }
    ImGui::EndPopup();
  }
}

// --- the cutscene skip ------------------------------------------------------------------
constexpr float kHintSeconds = 6.0f;  // the hint shows this long as a cutscene starts

bool cutscene_ui() {
  switch (cutscene::phase()) {
    case cutscene::Phase::kAsking:
    case cutscene::Phase::kSkipping:
      return true;
    case cutscene::Phase::kIdle:
      break;
  }
  const float t = cutscene::hint_seconds();
  return cutscene::skippable() && t >= 0.0f && t < kHintSeconds;
}

constexpr ImGuiWindowFlags kNoteFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                        ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs;

// The keys (and the pad's buttons, when a pad is there) that answer the
// question, or start it.
void skip_keys(char* out, size_t n, bool asking) {
  const char* pad = cutscene::pad_name();
  if (asking) {
    if (pad[0])
      std::snprintf(out, n, "Enter, %s, (A) or (%s): skip     Esc or (B): keep watching", cutscene::key_name(), pad);
    else
      std::snprintf(out, n, "Enter or %s: skip     Esc: keep watching", cutscene::key_name());
  } else if (pad[0]) {
    std::snprintf(out, n, "%s / (%s)", cutscene::key_name(), pad);
  } else {
    std::snprintf(out, n, "%s", cutscene::key_name());
  }
}

// The game is held (or running unseen): the frame it last showed fills the
// screen, dimmed.
void draw_held_frame(const ImVec2& size, int dim) {
  ImDrawList* bg = ImGui::GetBackgroundDrawList();
  dx9::draw_still(bg, size.x, size.y);
  bg->AddRectFilled(ImVec2(0, 0), size, IM_COL32(0, 0, 0, dim));
}

void draw_cutscene() {
  const ImGuiIO& io = ImGui::GetIO();
  const ImVec2 size = io.DisplaySize;
  const bool movie = game::movie_playing();
  char keys[160];
  switch (cutscene::phase()) {
    case cutscene::Phase::kAsking: {
      draw_held_frame(size, 110);
      ImGui::SetNextWindowPos(ImVec2(size.x * 0.5f, size.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
      ImGui::SetNextWindowBgAlpha(0.92f);
      ImGui::Begin("##skip cutscene", nullptr,
                   ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove);
      ImGui::SetWindowFontScale(1.5f);
      ImGui::TextUnformatted(movie ? "Skip this movie?" : "Skip this cutscene?");
      ImGui::SetWindowFontScale(1.0f);
      ImGui::Spacing();
      ImGui::TextDisabled("The game is paused.");
      if (!movie) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 26.0f);
        ImGui::TextDisabled("Skipping runs the scene through to its end without showing it, so everything it "
                            "does in the game still happens. It stops for any message or choice that needs you.");
        ImGui::PopTextWrapPos();
      }
      ImGui::Spacing();
      const ImVec2 button(ImGui::GetFontSize() * 9.0f, 0.0f);
      if (ImGui::Button("Skip", button)) cutscene::decide(true);
      ImGui::SameLine();
      if (ImGui::Button("Keep watching", button)) cutscene::decide(false);
      skip_keys(keys, sizeof(keys), true);
      ImGui::TextDisabled("%s", keys);
      ImGui::End();
      break;
    }
    case cutscene::Phase::kSkipping: {
      if (cutscene::waiting_for_player()) {
        // The game is on screen again, at its own speed, until the player has
        // turned the page or made the choice.
        ImGui::SetNextWindowPos(ImVec2(size.x * 0.5f, scaled(24.0f)), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
        ImGui::SetNextWindowBgAlpha(0.75f);
        ImGui::Begin("##skip waits", nullptr, kNoteFlags);
        ImGui::TextUnformatted("Skipping - waiting for you");
        ImGui::End();
        break;
      }
      draw_held_frame(size, 150);
      ImGui::SetNextWindowPos(ImVec2(size.x * 0.5f, size.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
      ImGui::SetNextWindowBgAlpha(0.8f);
      ImGui::Begin("##skipping", nullptr, kNoteFlags);
      ImGui::SetWindowFontScale(1.5f);
      ImGui::TextUnformatted(movie ? "Skipping the movie..." : "Skipping the cutscene...");
      ImGui::SetWindowFontScale(1.0f);
      ImGui::End();
      break;
    }
    case cutscene::Phase::kIdle: {
      const float t = cutscene::hint_seconds();
      const float alpha = t > kHintSeconds - 1.0f ? kHintSeconds - t : 1.0f;
      ImGui::SetNextWindowPos(ImVec2(size.x - scaled(24.0f), size.y - scaled(24.0f)), ImGuiCond_Always, ImVec2(1.0f, 1.0f));
      ImGui::SetNextWindowBgAlpha(0.6f * alpha);
      ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha < 0.0f ? 0.0f : alpha);
      ImGui::Begin("##skip hint", nullptr, kNoteFlags);
      skip_keys(keys, sizeof(keys), false);
      ImGui::Text("%s: skip %s", keys, movie ? "movie" : "cutscene");
      ImGui::End();
      ImGui::PopStyleVar();
      break;
    }
  }
}

// --- the cheats -----------------------------------------------------------------------
// Everything above the inventory: the switches and the editors that go with them,
// folded away under their heading (g_cheats_open).
void draw_cheats(const cheats::Status& st) {
  const ImVec4 orange(1.0f, 0.6f, 0.3f, 1.0f);
  ImGui::SeparatorText("Player");
  cheat_row(cheats::kGodMode, "God mode",
            "No hit takes any health off you - a barrel going off at point blank included - and your health is held "
            "at its maximum and the poison that drains it is cured, every frame. You still flinch and get knocked "
            "about; deaths the story scripts itself are left to the game.",
            st.player_ok, "player not found");
  if (st.player_ok && st.player.max_hp > 0) {
    ImGui::SameLine();
    ImGui::TextDisabled("(%d / %d HP%s)", st.player.hp, st.player.max_hp,
                        (st.player.condition & game::kPoison) ? ", poisoned" : "");
  }
  if (st.player_ok && !st.site_damage) {
    // Without the damage routine's site only the hold is left, and a hit bigger
    // than the whole bar kills before it can put the health back.
    ImGui::SameLine();
    ImGui::TextDisabled("(hits not blocked: patch site not found)");
  }
  cheat_row(cheats::kOneHitKills, "One hit kills",
            "Every enemy in the room is held at 1 HP - the last value the game still counts as alive - so your next "
            "hit kills it; deaths and drops play out through the game's own code. Bosses whose fights are scripted "
            "around their health may end sooner. Nemesis included.",
            st.entities_ok, "entity list not found");
  if (st.entities_ok) {
    ImGui::SameLine();
    ImGui::TextDisabled("(%d in the room)", st.live_entities);
  }
  cheat_row(cheats::kInfiniteAmmo, "Infinite ammo",
            "The weapon you have equipped is topped back up to a full load every frame, so firing never runs it dry. "
            "Reloads and pickups still work as usual.",
            st.ammo_ok, "no weapon equipped");
  if (st.ammo_ok && st.ammo_item) {
    ImGui::SameLine();
    ImGui::TextDisabled("(%s)", items::name(st.ammo_item));
  }
  cheat_row(cheats::kFreezeCountdown, "Freeze countdown timer",
            "Any countdown the game is running stops where it is, so the time never runs out: the Mercenaries' "
            "clock, and the main game's own - the hospital's time bomb, the four-minute one late in the Dead "
            "Factory, and the incoming missile on the radar receiver at the very end. The Mercenaries' time bonuses "
            "still add to it. It runs on again the moment you switch this off.",
            st.site_countdown, "patch sites not found");
  if (st.countdown_running) {
    // The game's clock counts whole seconds.
    const int left = st.countdown < 0 ? 0 : st.countdown;
    ImGui::SameLine();
    if (st.countdown_patched) ImGui::TextColored(orange, "(%d:%02d left, frozen)", left / 60, left % 60);
    else ImGui::TextDisabled("(%d:%02d left)", left / 60, left % 60);
  } else if (st.missile_running) {
    // The radar shows a distance; this is how long it lasts at the radar's pace.
    const int left = st.missile < 0 ? 0 : st.missile;
    ImGui::SameLine();
    if (st.missile_patched) ImGui::TextColored(orange, "(missile in %d:%02d, frozen)", left / 60, left % 60);
    else ImGui::TextDisabled("(missile in %d:%02d)", left / 60, left % 60);
  }
  // The main game's own: the Mercenaries' characters have no outfits.
  if (!st.mercenaries) draw_outfit(st);

  ImGui::SeparatorText("Saves, time & healing");
  cheat_row(cheats::kInfiniteInk, "Infinite ink ribbons",
            "Saving at a typewriter never uses up a ribbon, and you can save without carrying one at all - the "
            "typewriter offers the save as if you had one.",
            st.site_ink && st.site_ink_check, "patch sites not found");
  cheat_row(cheats::kNoSaveCount, "Save without counting",
            "Saving does not add to the save count the ending ranks you on.", st.site_savecount, "patch site not found");
  {
    static int edit_saves = -1;
    if (edit_saves < 0 && st.saves >= 0) edit_saves = st.saves;
    ImGui::BeginDisabled(!st.counters_ok);
    ImGui::Text("Saves: %d", st.saves);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(scaled(100.0f));
    ImGui::InputInt("##saves", &edit_saves);
    ImGui::SameLine();
    if (ImGui::SmallButton("Apply##saves")) cheats::request_saves(edit_saves < 0 ? 0 : edit_saves);
    ImGui::EndDisabled();
  }
  {
    char t[32];
    // The clock counts in sixtieths of a second (the results screen divides it
    // by 216000 for its hours).
    hms(st.counters_ok ? static_cast<float>(st.timer) / 60.0f : -1.0f, t, sizeof(t));
    cheat_row(cheats::kFreezePlaytime, "Freeze play time",
              "The play-time clock the ending ranks you on stops where it is, and runs on again the moment you switch "
              "this off - the time it was frozen for never counts. A save made while it is on keeps the time shown "
              "here; loading a save brings back that save's own time, frozen there.",
              st.counters_ok && st.site_timer, "patch sites not found");
    if (st.playtime_hold) {
      ImGui::SameLine();
      ImGui::TextColored(orange, "(held)");
    }
    static int eh = 0, em = 0, es = 0;
    ImGui::BeginDisabled(!st.counters_ok);
    ImGui::Text("Play time: %s", t);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(scaled(60.0f));
    ImGui::InputInt("h", &eh, 0, 0);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(scaled(60.0f));
    ImGui::InputInt("m", &em, 0, 0);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(scaled(60.0f));
    ImGui::InputInt("s", &es, 0, 0);
    ImGui::SameLine();
    if (ImGui::SmallButton("Set##pt")) {
      eh = eh < 0 ? 0 : eh;
      em = em < 0 ? 0 : (em > 59 ? 59 : em);
      es = es < 0 ? 0 : (es > 59 ? 59 : es);
      cheats::request_playtime_seconds(eh * 3600 + em * 60 + es);
    }
    ImGui::EndDisabled();
    if (st.counters_ok && (st.stage >= 0 || st.room >= 0)) {
      ImGui::SameLine();
      ImGui::TextDisabled("stage %d, room %d", st.stage, st.room);
    }
  }
  cheat_row(cheats::kNoRecoveryCount, "Heal without counting",
            "Healing items heal you as usual but add nothing to the recovery total the ending ranks you on.",
            st.site_recovery, "patch site not found");
  {
    // The ending's score for the total, out of the game's own steps: full marks
    // up to the last of them, nothing past the first.
    game::RankStep steps[game::kRankSteps];
    const bool scale = game::recovery_scale(steps);
    const game::RankStep& best = steps[game::kRankSteps - 1];
    char scoring[160] = "";
    if (scale)
      std::snprintf(scoring, sizeof(scoring),
                    " Up to %d scores full marks (%d), and less the further over it you go: nothing over %d.",
                    best.most, best.score, steps[0].most);
    char help[768];
    std::snprintf(help, sizeof(help),
                  "The ending ranks you on three scores: your saves, your play time and this - the healing your items "
                  "have given. Each item adds its full amount, however much of it you needed: your whole health for "
                  "a first aid spray or box, three green herbs or any mix with a red one; half of it for two green "
                  "herbs and a quarter for one, blue mixed in or not; nothing for a blue herb on its own.%s It is "
                  "saved with the game, and a new game starts it at 0.",
                  scoring);
    static int edit_recovery = -1;
    if (edit_recovery < 0 && st.recovery >= 0) edit_recovery = st.recovery;
    ImGui::BeginDisabled(!st.recovery_ok);
    ImGui::Text("Recovery: %d", st.recovery);
    item_help(help);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(scaled(130.0f));
    ImGui::InputInt("##recovery", &edit_recovery, 50, 200);
    item_help(help);
    ImGui::SameLine();
    if (ImGui::SmallButton("Apply##recovery")) cheats::request_recovery(edit_recovery < 0 ? 0 : edit_recovery);
    ImGui::EndDisabled();
    if (st.recovery_ok && scale) {
      // The Mercenaries ranks a run on its takings, and Easy shows no rank at all.
      const int score = game::recovery_score(st.recovery);
      ImGui::SameLine();
      if (st.mercenaries) ImGui::TextDisabled("(the Mercenaries does not rank on it)");
      else if (st.easy) ImGui::TextDisabled("(no rank on Easy)");
      else if (score < best.score) ImGui::TextColored(orange, "(%d of %d rank points)", score, best.score);
      else ImGui::TextDisabled("(%d of %d rank points)", score, best.score);
    }
  }
  // The main game's own: the Mercenaries has no ending to count.
  if (!st.mercenaries) draw_clears(st);
  draw_mixing(st, inventory::view().who);
  // The Mercenaries' own section is there only while it is being played.
  if (st.mercenaries) {
    ImGui::SeparatorText("The Mercenaries");
    draw_money(st);
  }
  ImGui::SeparatorText("Cutscenes");
  {
    char help[400];
    char keys[64];
    skip_keys(keys, sizeof(keys), false);
    std::snprintf(help, sizeof(help),
                  "While a cutscene or a movie is playing, %s pauses the game - picture and sound - and asks whether "
                  "to skip it. A movie ends at once; a cutscene is run through to its end unseen and unheard, so "
                  "everything it does in the game still happens. It stops for any message or choice that needs you.",
                  keys);
    char label[64];
    std::snprintf(label, sizeof(label), "Skip cutscenes (%s)", cutscene::key_name());
    cheat_row(cheats::kSkipCutscenes, label, help, game::cutscenes_known(), "cutscene state not found");
  }

  if (st.last_action[0]) ImGui::TextWrapped("%s", st.last_action);
}

}  // namespace

bool ensure_context(HWND hwnd) {
  if (g_context_ready) return true;
  if (!hwnd) return false;
  g_hwnd = hwnd;

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  // Window position persists beside the DLL, under the mod's own name rather
  // than a stray imgui.ini in the game folder.
  static char ini_path[MAX_PATH] = {};
  std::snprintf(ini_path, sizeof(ini_path), "%sRE3-1999CabbyCodes.imgui.ini", config::dir());
  io.IniFilename = ini_path;
  // The panel's own lines in that file (g_cheats_open, g_layout_percent),
  // registered before the first frame, which is when ImGui reads it.
  ImGuiSettingsHandler panel{};
  panel.TypeName = "RE3-1999CabbyCodes";
  panel.TypeHash = ImHashStr("RE3-1999CabbyCodes");
  panel.ReadOpenFn = [](ImGuiContext*, ImGuiSettingsHandler*, const char* name) -> void* {
    return std::strcmp(name, "Panel") == 0 ? &g_cheats_open : nullptr;
  };
  panel.ReadLineFn = [](ImGuiContext*, ImGuiSettingsHandler*, void*, const char* line) {
    int v = 0;
    if (std::sscanf(line, "CheatsOpen=%d", &v) == 1) g_cheats_open = v != 0;
    else if (std::sscanf(line, "ScalePercent=%d", &v) == 1 && v > 0) g_layout_percent = v;
  };
  panel.WriteAllFn = [](ImGuiContext*, ImGuiSettingsHandler* h, ImGuiTextBuffer* out) {
    out->appendf("[%s][Panel]\nCheatsOpen=%d\nScalePercent=%d\n\n", h->TypeName, g_cheats_open ? 1 : 0,
                 g_layout_percent);
  };
  ImGui::AddSettingsHandler(&panel);
  // No keyboard navigation: the game's menus keep their keys. Dragging is
  // limited to the title bar so a click on the panel's background cannot pick
  // the window up by accident.
  io.ConfigWindowsMoveFromTitleBarOnly = true;
  ImGui::StyleColorsDark();
  ImGuiStyle& style = ImGui::GetStyle();
  style.WindowRounding = 4.0f;
  style.ScaleAllSizes(kScale);  // the padding, spacing and the rest; the font has its own factor
  style.FontScaleMain = kScale;

  if (!ImGui_ImplWin32_Init(hwnd)) {
    logf("ERROR: ImGui Win32 backend init failed");
    ImGui::DestroyContext();
    return false;
  }
  g_orig_wndproc = reinterpret_cast<WNDPROC>(SetWindowLongPtrA(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(hk_wndproc)));
  g_context_ready = true;
  char cls[64] = "?";
  GetClassNameA(hwnd, cls, sizeof(cls));
  logf("overlay ready (hwnd=%p class '%s', scale %d%%, present thread %lu, window thread %lu)",
       static_cast<void*>(hwnd), cls, kScalePercent, GetCurrentThreadId(), GetWindowThreadProcessId(hwnd, nullptr));
  return true;
}

HWND window() { return g_context_ready ? g_hwnd : nullptr; }

// Published by the frame that drew (draw_panel), cleared by the frame that did
// not (wants_draw): a plain read, from any thread, of what the panel took.
bool capturing_mouse() { return g_capture_mouse != 0; }
bool capturing_keyboard() { return g_capture_keyboard != 0; }

void lock_imgui() {
  if (g_imgui_cs_ready) EnterCriticalSection(&g_imgui_cs);
}

void unlock_imgui() {
  if (g_imgui_cs_ready) LeaveCriticalSection(&g_imgui_cs);
}

// Called from the present hook with the context lock held.
bool wants_draw() {
  const dispatch::Snapshot s = dispatch::snapshot();
  const bool showable = s.show_panel || config::get().always_show;
  if (!showable) {
    g_user_hidden = false;  // the next time the screen comes up the panel is back
    forget_item_dialogs();
    forget_money_edits();
    forget_file_dialogs();
  }
  g_visible = g_context_ready && showable && !g_user_hidden;
  // The cutscene prompt takes the pointer as the panel does; its hint and the
  // skip's banner take nothing.
  const bool asking = g_context_ready && cutscene::phase() == cutscene::Phase::kAsking;
  const bool interactive = g_visible || asking;
  free_cursor(interactive);
  if (!g_visible) {  // nothing is being taken from the game until it draws again
    InterlockedExchange(&g_capture_mouse, 0);
    InterlockedExchange(&g_capture_keyboard, 0);
  }
  ImGuiIO& io = ImGui::GetIO();
  // While the panel is up the game's own cursor is shown (show_game_cursor) and
  // ImGui only sets its shape; it draws one of its own only when that is turned
  // off (Disable = cursor), the game's cursor then staying hidden.
  io.MouseDrawCursor = interactive && config::get().disable_cursor;
  if (interactive) {
    // If a release was ever missed the panel would follow the pointer for ever;
    // the physical button is the truth.
    static const int kVk[3] = {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON};
    for (int b = 0; b < 3; ++b)
      if (io.MouseDown[b] && !(GetAsyncKeyState(kVk[b]) & 0x8000)) io.AddMouseButtonEvent(b, false);
  } else if (g_context_ready) {
    // Input made while the panel is away is not the panel's: ImGui only queues
    // events and drains them in NewFrame, which does not run while the panel is
    // hidden, so they would be replayed onto it the moment it came back (RE0).
    io.ClearEventsQueue();
    io.ClearInputMouse();
    io.ClearInputKeys();
  }
  return g_visible || (g_context_ready && cutscene_ui());
}

// Called from the present hook, between NewFrame and Render, with the context
// lock held.
void draw_panel() {
  const dispatch::Snapshot s = dispatch::snapshot();
  const cheats::Status st = cheats::status();
  const ImGuiIO& io = ImGui::GetIO();
  // NewFrame has run, so what ImGui wants this frame is settled; the input
  // guard reads these from the game's thread.
  InterlockedExchange(&g_capture_mouse, io.WantCaptureMouse ? 1 : 0);
  InterlockedExchange(&g_capture_keyboard, io.WantTextInput ? 1 : 0);
  const ImVec4 orange(1.0f, 0.6f, 0.3f, 1.0f);
  const ImVec4 reach = reachable_rect(io.DisplaySize);
  // Resizable, not auto-fitted: an auto-fitting window whose wrapped text
  // measures itself against the window's own width shrinks a little every frame
  // once a scrollbar takes some of that width. It is never let grow past what
  // the pointer can reach, or its corner could not be dragged back.
  ImGui::SetNextWindowPos(ImVec2(reach.z - scaled(600), reach.y + scaled(60)), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(ImVec2(scaled(560), scaled(700)), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSizeConstraints(ImVec2(scaled(360), scaled(200)), ImVec2(reach.z - reach.x, reach.w - reach.y));
  if (!ImGui::Begin("Resident Evil 3 - Cabby Codes  v" RE3CC_VERSION, nullptr)) {
    ImGui::End();
    return;
  }
  if (!g_layout_checked) {
    // The panel's first frame. A size the layout file kept is brought to this
    // scale (the first-use size above is at it already); the size constraint
    // and keep_in_reach then hold it to what the pointer can reach. The size is
    // the expanded one: a collapsed panel's own is its title bar.
    g_layout_checked = true;
    const ImGuiWindow* w = ImGui::GetCurrentWindow();
    if (w->SettingsOffset != -1 && g_layout_percent != kScalePercent) {
      const float r = static_cast<float>(kScalePercent) / static_cast<float>(g_layout_percent);
      const ImVec2 was = w->SizeFull;
      ImGui::SetWindowSize(ImVec2(was.x * r, was.y * r));
      logf("panel: its saved size, %.0fx%.0f at %d%%, brought to %d%%", was.x, was.y, g_layout_percent, kScalePercent);
    }
    if (g_layout_percent != kScalePercent) {
      g_layout_percent = kScalePercent;
      ImGui::MarkIniSettingsDirty();
    }
  }
  keep_in_reach(reach);
  if (!s.unpacked) {
    ImGui::TextColored(orange, "Waiting for the game to unpack...");
    ImGui::End();
    return;
  }
  if (!s.game_ready) {
    ImGui::TextColored(orange, "Game hooks: %s", game::status_text());
    ImGui::End();
    return;
  }
  if (s.card_screen) {
    // The memory card screen: the panel is the save file manager, and nothing
    // else on it applies here.
    draw_save_files();
    ImGui::Separator();
    ImGui::TextDisabled("F%d hides this panel", config::get().toggle_key - 0x6F);
    ImGui::End();
    return;
  }
  if (!s.in_game) {
    ImGui::TextDisabled("Waiting for a game (start a new game or load a save)...");
    ImGui::Separator();
  }

  {
    // The switches that are on, counted in the heading, so a folded section
    // still says what it holds.
    int on = 0;
    for (int k = 0; k < cheats::kCount; ++k) on += cheats::enabled(static_cast<cheats::Kind>(k)) ? 1 : 0;
    char head[48];
    std::snprintf(head, sizeof(head), "Cheats  (%d on)###cheats", on);
    ImGui::SetNextItemOpen(g_cheats_open, ImGuiCond_Always);
    const bool open = ImGui::CollapsingHeader(head);
    if (open != g_cheats_open) {
      g_cheats_open = open;
      ImGui::MarkIniSettingsDirty();
    }
  }

  ImGui::BeginDisabled(!s.in_game);
  if (g_cheats_open) draw_cheats(st);
  {
    // The inventory and the item box: the game's own, changed only while the
    // status screen is up (inventory.cpp).
    const inventory::View iv = inventory::view();
    draw_inventory(iv);
    draw_item_box(iv);
    // Only a move that was refused says so here: what a move did shows in the
    // lists themselves, and the log keeps every one.
    if (iv.note_error && iv.note[0]) {
      ImGui::PushStyleColor(ImGuiCol_Text, orange);
      ImGui::TextWrapped("%s", iv.note);
      ImGui::PopStyleColor();
    }
  }
  ImGui::EndDisabled();

  ImGui::Separator();
  ImGui::TextDisabled("F%d hides this panel", config::get().toggle_key - 0x6F);
  ImGui::End();
}


void draw_frame() {
  if (g_visible) draw_panel();
  if (cutscene_ui()) draw_cutscene();
}

void shutdown_imgui() {
  if (!g_context_ready) return;
  ImGui_ImplWin32_Shutdown();
  if (ImGui::GetCurrentContext()) ImGui::DestroyContext();
  g_context_ready = false;
  g_visible = false;
  InterlockedExchange(&g_capture_mouse, 0);
  InterlockedExchange(&g_capture_keyboard, 0);
}

bool install() {
  adopt::init();  // from DllMain, on the game's primary thread: the one that makes its window
  if (!g_imgui_cs_ready) {  // before the window procedure, the present hook or the ClipCursor hook exists
    InitializeCriticalSection(&g_imgui_cs);
    InitializeCriticalSection(&g_cursor_cs);
    g_imgui_cs_ready = true;
  }
  bool d3d = false;
  if (HMODULE wrapper = proxy::original()) {
    d3d = dx9::install_import(wrapper);
    if (!config::get().disable_cursor) install_cursor_hook(wrapper);
  }
  if (!config::get().disable_gpa) {
    HMODULE exe = GetModuleHandleA(nullptr);
    const uintptr_t slot = mem::iat_slot(exe, "kernel32.dll", "GetProcAddress");
    void* prev = slot ? mem::read<void*>(slot) : nullptr;
    if (prev && mem::write<uintptr_t>(slot, reinterpret_cast<uintptr_t>(&hk_get_proc_address))) {
      g_orig_gpa = reinterpret_cast<GetProcAddressFn>(prev);
      g_gpa_slot = slot;
      game::note_iat_slot(slot, reinterpret_cast<uintptr_t>(prev));
      logf("overlay: the exe's GetProcAddress import (Enigma's loader) hooked at exe+0x%X (original %p)%s",
           static_cast<unsigned>(slot - reinterpret_cast<uintptr_t>(exe)), prev,
           config::get().trace ? " - tracing names" : "");
    } else {
      logf("overlay: GetProcAddress is not in the exe's import table - no view of what Enigma resolves");
    }
  }
  return d3d;
}

void uninstall() {
  // Window procedure first: a message arriving after our image is gone would
  // jump into freed memory.
  if (g_orig_wndproc && g_hwnd && IsWindow(g_hwnd)) {
    SetWindowLongPtrA(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_orig_wndproc));
    g_orig_wndproc = nullptr;
  }
  dx9::uninstall();
  shutdown_imgui();
  if (g_clip_slot && mem::read<uintptr_t>(g_clip_slot) == reinterpret_cast<uintptr_t>(&hk_clip_cursor))
    mem::write<uintptr_t>(g_clip_slot, reinterpret_cast<uintptr_t>(g_real_clip_cursor));
  g_clip_slot = 0;
  if (g_gpa_slot && g_orig_gpa &&
      mem::read<uintptr_t>(g_gpa_slot) == reinterpret_cast<uintptr_t>(&hk_get_proc_address))
    mem::write<uintptr_t>(g_gpa_slot, reinterpret_cast<uintptr_t>(g_orig_gpa));
  g_orig_gpa = nullptr;
  g_gpa_slot = 0;
  logf("overlay hooks removed");
}

}  // namespace re3cc::overlay
