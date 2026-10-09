#include "pad.h"

#include <windows.h>

#include <cstring>

#include "log.h"

namespace re3cc::pad {
namespace {

// XInput's own types, declared here so the headers are not needed.
struct Gamepad {
  WORD buttons;
  BYTE left_trigger, right_trigger;
  SHORT lx, ly, rx, ry;
};
struct State {
  DWORD packet;
  Gamepad pad;
};
using GetStateFn = DWORD(WINAPI*)(DWORD, State*);

GetStateFn g_get_state = nullptr;
bool g_loaded = false;
WORD g_last[4] = {};
bool g_here[4] = {};
DWORD g_next_look = 0;  // when to try the pads that were not there again
bool g_connected = false;

void load() {
  g_loaded = true;
  const char* const dlls[] = {"xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll"};
  for (const char* dll : dlls)
    if (HMODULE m = LoadLibraryA(dll))
      if (auto fn = reinterpret_cast<GetStateFn>(GetProcAddress(m, "XInputGetState"))) {
        g_get_state = fn;
        logf("pad: reading game pads through %s", dll);
        return;
      }
  logf("pad: no XInput - the cutscene skip is on the keyboard only");
}

struct Named {
  const char* name;
  unsigned button;
};
const Named kNames[] = {{"Start", kStart},       {"Back", kBack},         {"A", kA},
                        {"B", kB},               {"X", kX},               {"Y", kY},
                        {"LB", kLeftShoulder},   {"RB", kRightShoulder},  {"LS", kLeftThumb},
                        {"RS", kRightThumb}};

}  // namespace

unsigned poll() {
  if (!g_loaded) load();
  if (!g_get_state) return 0;
  const DWORD now = GetTickCount();
  const bool look_all = static_cast<LONG>(now - g_next_look) >= 0;
  if (look_all) g_next_look = now + 2000;
  unsigned pressed = 0;
  bool any = false;
  for (DWORD i = 0; i < 4; ++i) {
    if (!g_here[i] && !look_all) continue;
    State st{};
    if (g_get_state(i, &st) != ERROR_SUCCESS) {
      g_here[i] = false;
      g_last[i] = 0;
      continue;
    }
    if (!g_here[i]) g_last[i] = st.pad.buttons;  // a pad just found: what it already holds is not a press
    g_here[i] = true;
    any = true;
    pressed |= st.pad.buttons & ~g_last[i];
    g_last[i] = st.pad.buttons;
  }
  if (any != g_connected) {
    g_connected = any;
    logf("pad: %s", any ? "a game pad is connected" : "no game pad connected");
  }
  return pressed;
}

bool connected() { return g_connected; }

unsigned parse(const char* name) {
  for (const Named& n : kNames)
    if (!_stricmp(name, n.name)) return n.button;
  return kNone;
}

const char* name(unsigned button) {
  for (const Named& n : kNames)
    if (n.button == button) return n.name;
  return "";
}

}  // namespace re3cc::pad
