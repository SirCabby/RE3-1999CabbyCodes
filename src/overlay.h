#pragma once

#include <windows.h>

struct ImDrawList;

// The Dear ImGui panel and the machinery that gets it on screen. The game
// presents through GOG's DirectDraw -> Direct3D 9 wrapper (ddraw_orig.dll), so
// the renderer is captured through the wrapper's Direct3DCreate9 import; the
// exe's GetProcAddress import (Enigma's loader) is hooked too, to trace what
// Enigma resolves for the game and to hand it our input and crash hooks.
namespace re3cc::overlay {

bool install();  // from DllMain (import-table patches only)
void uninstall();

bool ensure_context(HWND hwnd);   // ImGui context + Win32 backend + WndProc, once
HWND window();                    // the game's window, once the overlay has seen it
bool wants_draw();                // the visibility rule for this frame
void draw_frame();                // between ImGui::NewFrame() and ImGui::Render(): the panel and the cutscene prompt
void shutdown_imgui();            // Win32 backend + context (renderer already gone)

// One ImGui context; the window procedure feeds it messages and the present
// hook draws from it. Both run on the game's thread here, but ImGui is not
// thread-safe and nothing guarantees that for every wrapper, so every touch of
// the context is taken under this lock (RE0 learned it the hard way: a frame
// freed the event queue under a WM_MOUSEMOVE that was reading it).
void lock_imgui();
void unlock_imgui();
struct ImGuiLock {
  ImGuiLock() { lock_imgui(); }
  ~ImGuiLock() { unlock_imgui(); }
  ImGuiLock(const ImGuiLock&) = delete;
  ImGuiLock& operator=(const ImGuiLock&) = delete;
};

// What the panel is using this frame, for the input guard (input.cpp).
bool capturing_mouse();     // the pointer is over the panel
bool capturing_keyboard();  // a panel field has keyboard focus

namespace dx9 {
bool install_import(HMODULE wrapper);  // hook d3d9.dll!Direct3DCreate9 in ddraw_orig.dll's import table
void uninstall();

// What the recon log and the stub panel show about the device.
struct Stats {
  unsigned long present_thread = 0;
  unsigned present_per_s = 0;       // IDirect3DDevice9::Present
  unsigned swap_present_per_s = 0;  // IDirect3DSwapChain9::Present
  unsigned end_scene_per_s = 0;
  unsigned backbuffer_w = 0, backbuffer_h = 0;
  bool windowed = false;
};
Stats stats();
void log_rates();  // main thread, once a second: the counts of the last second

// The pause (cutscene.cpp): while a cutscene is on screen each frame is copied
// as it is presented, so with the game held the last one can be shown again,
// the prompt over it.
bool can_present_still();             // a copy and a swap chain to show it on
bool present_still();                 // one frame: the copy, the overlay, Present
void draw_still(ImDrawList* dl, float w, float h);  // the copy, behind everything
}  // namespace dx9

}  // namespace re3cc::overlay
