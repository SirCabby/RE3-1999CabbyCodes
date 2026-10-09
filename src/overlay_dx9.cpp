// D3D9 backend. The game draws through GOG's DirectDraw wrapper
// (ddraw_orig.dll), which imports Direct3DCreate9 statically: that import slot
// is patched right after the proxy loads the wrapper, and from the returned
// IDirect3D9 we hook CreateDevice (slot 16). On the device: Present (17),
// CreateAdditionalSwapChain (13), EndScene (42) and Reset (16); on its swap
// chains: Present (3).
//
// The wrapper does not present through IDirect3DDevice9::Present - a whole
// session drew the game without one call to it (2026-09-13) - so every present
// path is hooked, the panel is drawn by whichever one the wrapper uses, once
// per frame, into that swap chain's back buffer, and the log says which path
// it is. The main-thread tick does not come from here: dispatch.cpp hooks the
// game's own PeekMessageA.
//
// Under Wine those hooks go into the D3D9 classes' vtables. On Windows they go
// into vtables of the game's device's and swap chains' own (adopt.cpp: the
// Steam overlay hooks what the classes' vtables point to), and the adopted
// device also answers GetSwapChain(0) itself, with the adopted implicit swap
// chain: D3D9's GetSwapChain is where the overlay hooks every swap chain it
// hands out, by whatever that swap chain's vtable holds then - the mod's
// Present, in the copy. The one call the overlay sees is the mod's own, before
// the swap chain is adopted. IDirect3D9's CreateDevice stays hooked in place:
// with the overlay that object is the overlay's own wrapper.

#include <windows.h>
#include <d3d9.h>

#include "overlay.h"

#include "imgui.h"
#include "backends/imgui_impl_dx9.h"
#include "backends/imgui_impl_win32.h"
#include "adopt.h"
#include "config.h"
#include "crash.h"
#include "cutscene.h"
#include "log.h"
#include "mem.h"

namespace re3cc::overlay::dx9 {
namespace {

using Create9Fn = IDirect3D9*(WINAPI*)(UINT);
using CreateDeviceFn = HRESULT(__stdcall*)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*,
                                           IDirect3DDevice9**);
using PresentFn = HRESULT(__stdcall*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
using SwapPresentFn = HRESULT(__stdcall*)(IDirect3DSwapChain9*, const RECT*, const RECT*, HWND, const RGNDATA*, DWORD);
using CreateSwapFn = HRESULT(__stdcall*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*, IDirect3DSwapChain9**);
using GetSwapFn = HRESULT(__stdcall*)(IDirect3DDevice9*, UINT, IDirect3DSwapChain9**);
using EndSceneFn = HRESULT(__stdcall*)(IDirect3DDevice9*);
using ResetFn = HRESULT(__stdcall*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);

Create9Fn g_real_create9 = nullptr;
CreateDeviceFn g_orig_create_device = nullptr;
PresentFn g_orig_present = nullptr;
SwapPresentFn g_orig_swap_present = nullptr;
CreateSwapFn g_orig_create_swap = nullptr;
GetSwapFn g_orig_get_swap = nullptr;  // adopted devices only
EndSceneFn g_orig_end_scene = nullptr;
ResetFn g_orig_reset = nullptr;
uintptr_t g_d3d_vt = 0;
uintptr_t g_device_vt = 0;  // the class hooks' (Wine)
uintptr_t g_swap_vt = 0;

constexpr int kCreateSwap = 13, kGetSwap = 14, kReset = 16, kPresent = 17, kEndScene = 42, kSwapPresent = 3;
constexpr int kCreateDevice = 16;  // IDirect3D9's
// IDirect3DDevice9's slots (IDirect3DDevice9Ex has 134), IDirect3DSwapChain9's
// (IDirect3DSwapChain9Ex has 13) and IDirect3D9's (IDirect3D9Ex has 22); an
// adopted object's copy has as many as can be read, up to the second number.
constexpr int kDeviceSlots = 119, kDeviceCopy = 192, kSwapSlots = 10, kSwapCopy = 32, kD3DSlots = 17, kD3DCopy = 32;
// The adopted device and its implicit swap chain (Windows): what GetSwapChain(0)
// answers with. The swap chain lives as long as the device (it survives Reset);
// no reference is held, which would keep the device alive.
IDirect3DDevice9* g_game_device = nullptr;
IDirect3DSwapChain9* g_game_chain = nullptr;
uintptr_t g_iat_slot = 0;
IDirect3DDevice9* g_device = nullptr;
IDirect3DSwapChain9* g_swap = nullptr;  // the one the wrapper presents through (not ours to release)
HWND g_hwnd = nullptr;
// The last frame presented while a cutscene was on screen (cutscene.cpp's
// pause). A render-target texture, so a multisampled back buffer resolves into
// it and ImGui can draw it.
IDirect3DTexture9* g_still = nullptr;
UINT g_still_w = 0, g_still_h = 0;
D3DFORMAT g_still_fmt = D3DFMT_UNKNOWN;
bool g_still_logged = false;
bool g_renderer_ready = false;
// The device's own Present may hand over to its swap chain's through the
// vtable; the panel must not be drawn twice in one frame.
bool g_in_device_present = false;

// Per-second call counts (dispatch.cpp's tick reads them once a second).
Stats g_stats;
volatile LONG g_presents = 0, g_swap_presents = 0, g_end_scenes = 0;
volatile LONG g_first_present = 0, g_first_swap_present = 0, g_first_end_scene = 0;
int g_rate_logs = 0;

bool ensure_renderer(IDirect3DDevice9* device) {
  if (g_renderer_ready) return true;
  if (!g_hwnd) {
    D3DDEVICE_CREATION_PARAMETERS cp{};
    if (SUCCEEDED(device->GetCreationParameters(&cp))) g_hwnd = cp.hFocusWindow;
  }
  if (!ensure_context(g_hwnd)) return false;
  if (!ImGui_ImplDX9_Init(device)) {
    logf("ERROR: dx9: ImGui DX9 backend init failed");
    return false;
  }
  g_renderer_ready = true;
  return true;
}

// The panel, into `back` (the back buffer of the swap chain being presented),
// in a scene of its own. The state block puts the device state back; it does
// not cover render targets, so the old one is restored first - and the viewport
// with the state block after it.
void draw_into(IDirect3DDevice9* device, IDirect3DSurface9* back) {
  IDirect3DStateBlock9* state = nullptr;
  device->CreateStateBlock(D3DSBT_ALL, &state);
  IDirect3DSurface9* old_rt = nullptr;
  device->GetRenderTarget(0, &old_rt);
  device->SetRenderTarget(0, back);
  if (SUCCEEDED(device->BeginScene())) {
    ImGui_ImplDX9_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    draw_frame();
    ImGui::EndFrame();
    ImGui::Render();
    ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
    device->EndScene();
  }
  if (old_rt) {
    device->SetRenderTarget(0, old_rt);
    old_rt->Release();
  }
  if (state) {
    state->Apply();
    state->Release();
  }
}

void panel_frame(IDirect3DDevice9* device, IDirect3DSurface9* back);

void release_still() {
  if (g_still) g_still->Release();
  g_still = nullptr;
}

// The frame about to be presented, kept (before the panel goes over it).
void capture(IDirect3DDevice9* device, IDirect3DSurface9* back) {
  D3DSURFACE_DESC d{};
  if (FAILED(back->GetDesc(&d))) return;
  if (g_still && (d.Width != g_still_w || d.Height != g_still_h || d.Format != g_still_fmt)) release_still();
  if (!g_still) {
    const HRESULT hr =
        device->CreateTexture(d.Width, d.Height, 1, D3DUSAGE_RENDERTARGET, d.Format, D3DPOOL_DEFAULT, &g_still, nullptr);
    if (FAILED(hr) || !g_still) {
      g_still = nullptr;
      if (!g_still_logged) {
        g_still_logged = true;
        logf("ERROR: dx9: no texture for the cutscene pause (%ux%u fmt %u, hr=0x%08lX)", d.Width, d.Height,
             static_cast<unsigned>(d.Format), static_cast<unsigned long>(hr));
      }
      return;
    }
    g_still_w = d.Width;
    g_still_h = d.Height;
    g_still_fmt = d.Format;
  }
  IDirect3DSurface9* s = nullptr;
  if (SUCCEEDED(g_still->GetSurfaceLevel(0, &s)) && s) {
    const HRESULT hr = device->StretchRect(back, nullptr, s, nullptr, D3DTEXF_NONE);
    if (FAILED(hr) && !g_still_logged) {
      g_still_logged = true;
      logf("ERROR: dx9: the frame could not be copied for the cutscene pause (hr=0x%08lX)",
           static_cast<unsigned long>(hr));
    }
    s->Release();
  }
}

// Every present path: false when this frame is not to be shown at all (a
// skip runs the game faster than the screen); otherwise the frame is kept for
// a pause if one could come, and the overlay drawn over it.
bool before_present(IDirect3DDevice9* device, IDirect3DSurface9* back) {
  if (cutscene::drop_present()) return false;
  if (cutscene::want_capture()) capture(device, back);
  panel_frame(device, back);
  return true;
}

void panel_frame(IDirect3DDevice9* device, IDirect3DSurface9* back) {
  // The window procedure feeds this same context.
  ImGuiLock guard;
  if (ensure_renderer(device) && wants_draw()) draw_into(device, back);
}

void note_first(volatile LONG* flag, const char* what, HWND wnd, const RECT* dst) {
  if (InterlockedExchange(flag, 1)) return;
  if (dst)
    logf("dx9: first %s on thread %lu (hwnd=%p, dst %ld,%ld-%ld,%ld)", what, GetCurrentThreadId(), static_cast<void*>(wnd),
         dst->left, dst->top, dst->right, dst->bottom);
  else
    logf("dx9: first %s on thread %lu (hwnd=%p)", what, GetCurrentThreadId(), static_cast<void*>(wnd));
}

// A present that comes back in while one is on its way on (adopt::calling_on:
// another tool's hook calling the object's vtable again) is the same frame:
// the panel is drawn the first time through.
HRESULT __stdcall hk_present(IDirect3DDevice9* device, const RECT* src, const RECT* dst, HWND wnd,
                             const RGNDATA* dirty) {
  InterlockedIncrement(&g_presents);
  g_stats.present_thread = GetCurrentThreadId();
  note_first(&g_first_present, "IDirect3DDevice9::Present", wnd, dst);
  IDirect3DSurface9* back = nullptr;
  bool show = true;
  if (!adopt::calling_on() && SUCCEEDED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back)) && back) {
    show = before_present(device, back);
    back->Release();
  }
  if (!show) return D3D_OK;
  const auto orig = reinterpret_cast<PresentFn>(adopt::original(device, kPresent, reinterpret_cast<void*>(g_orig_present)));
  g_in_device_present = true;
  HRESULT hr;
  {
    adopt::CallingOn on;
    hr = orig(device, src, dst, wnd, dirty);
  }
  g_in_device_present = false;
  return hr;
}

HRESULT __stdcall hk_swap_present(IDirect3DSwapChain9* sc, const RECT* src, const RECT* dst, HWND wnd,
                                  const RGNDATA* dirty, DWORD flags) {
  InterlockedIncrement(&g_swap_presents);
  g_stats.present_thread = GetCurrentThreadId();
  note_first(&g_first_swap_present, "IDirect3DSwapChain9::Present", wnd, dst);
  if (!g_in_device_present && !adopt::calling_on()) {
    g_swap = sc;
    bool show = true;
    IDirect3DDevice9* device = nullptr;
    if (SUCCEEDED(sc->GetDevice(&device)) && device) {
      IDirect3DSurface9* back = nullptr;
      if (SUCCEEDED(sc->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &back)) && back) {
        show = before_present(device, back);
        back->Release();
      }
      device->Release();
    }
    if (!show) return D3D_OK;
  }
  const auto orig =
      reinterpret_cast<SwapPresentFn>(adopt::original(sc, kSwapPresent, reinterpret_cast<void*>(g_orig_swap_present)));
  adopt::CallingOn on;
  return orig(sc, src, dst, wnd, dirty, flags);
}

HRESULT __stdcall hk_end_scene(IDirect3DDevice9* device) {
  InterlockedIncrement(&g_end_scenes);
  note_first(&g_first_end_scene, "IDirect3DDevice9::EndScene", nullptr, nullptr);
  const auto orig = reinterpret_cast<EndSceneFn>(adopt::original(device, kEndScene, reinterpret_cast<void*>(g_orig_end_scene)));
  adopt::CallingOn on;
  return orig(device);
}

// Every swap chain of the device shares one class, so the first one's vtable
// covers the rest; a different vtable is logged rather than hooked. On Windows
// each gets a copy of its vtable instead (adopt.cpp).
void hook_swap_chain(IDirect3DSwapChain9* sc, const char* which) {
  const auto vt = mem::read<uintptr_t>(reinterpret_cast<uintptr_t>(sc));
  if (adopt::enabled()) {
    static const adopt::Hook hooks[] = {{kSwapPresent, reinterpret_cast<void*>(&hk_swap_present)}};
    // What the hook falls back on: the class's Present (its vtable is left alone).
    if (!g_orig_swap_present) g_orig_swap_present = mem::read<SwapPresentFn>(vt + kSwapPresent * sizeof(void*));
    const int copied = adopt::take(sc, kSwapSlots, kSwapCopy, hooks, 1);
    char where[MAX_PATH + 32];
    re3cc::describe_address(reinterpret_cast<void*>(vt), where, sizeof(where));
    if (copied)
      logf("dx9: %s swap chain %p adopted - a vtable of its own with Present (%d slots copied; the class's at %s)", which,
           static_cast<void*>(sc), copied, where);
    else
      logf("ERROR: dx9: %s swap chain %p could not be adopted (its vtable at %s) - no panel on it", which,
           static_cast<void*>(sc), where);
    return;
  }
  if (!g_orig_swap_present) {
    g_orig_swap_present =
        reinterpret_cast<SwapPresentFn>(mem::hook_vtable(vt, 3, reinterpret_cast<void*>(&hk_swap_present)));
    g_swap_vt = vt;
    logf("dx9: %s swap chain %p: Present hooked", which, static_cast<void*>(sc));
  } else if (vt != g_swap_vt) {
    logf("dx9: %s swap chain %p has another vtable %p - not hooked", which, static_cast<void*>(sc),
         reinterpret_cast<void*>(vt));
  }
}

HRESULT __stdcall hk_create_swap(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* pp, IDirect3DSwapChain9** out) {
  const auto orig =
      reinterpret_cast<CreateSwapFn>(adopt::original(device, kCreateSwap, reinterpret_cast<void*>(g_orig_create_swap)));
  const HRESULT hr = orig(device, pp, out);
  if (SUCCEEDED(hr) && out && *out) {
    logf("dx9: the wrapper created an additional swap chain (%ux%u, hwnd=%p)", pp ? pp->BackBufferWidth : 0u,
         pp ? pp->BackBufferHeight : 0u, pp ? static_cast<void*>(pp->hDeviceWindow) : nullptr);
    hook_swap_chain(*out, "additional");
  }
  return hr;
}

// Adopted devices only (Windows): GetSwapChain(0) of the game's device is
// answered here with the adopted implicit swap chain - one more reference, all
// D3D9's does - so that the Steam overlay's GetSwapChain hook never reads the
// copy (see the top of this file).
HRESULT __stdcall hk_get_swap(IDirect3DDevice9* device, UINT i, IDirect3DSwapChain9** out) {
  if (out && i == 0 && device == g_game_device && g_game_chain) {
    g_game_chain->AddRef();
    *out = g_game_chain;
    return D3D_OK;
  }
  const auto orig = reinterpret_cast<GetSwapFn>(adopt::original(device, kGetSwap, reinterpret_cast<void*>(g_orig_get_swap)));
  return orig(device, i, out);
}

HRESULT __stdcall hk_reset(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* pp) {
  // The real Reset stays outside the lock: it can block on the GPU, and the
  // window procedure must not wait that out.
  if (g_renderer_ready) {
    ImGuiLock guard;
    ImGui_ImplDX9_InvalidateDeviceObjects();
  }
  release_still();  // D3DPOOL_DEFAULT: it must go before a Reset
  const auto orig = reinterpret_cast<ResetFn>(adopt::original(device, kReset, reinterpret_cast<void*>(g_orig_reset)));
  HRESULT hr;
  {
    adopt::CallingOn on;
    hr = orig(device, pp);
  }
  if (pp) {
    g_stats.backbuffer_w = pp->BackBufferWidth;
    g_stats.backbuffer_h = pp->BackBufferHeight;
    g_stats.windowed = pp->Windowed != FALSE;
    logf("dx9: Reset to %ux%u %s (hr=0x%08lX)", pp->BackBufferWidth, pp->BackBufferHeight,
         pp->Windowed ? "windowed" : "fullscreen", static_cast<unsigned long>(hr));
  }
  if (g_renderer_ready) {
    ImGuiLock guard;
    ImGui_ImplDX9_CreateDeviceObjects();
  }
  return hr;
}

// Windows: the game's device gets a vtable of its own, then its implicit swap
// chain - taken through the class's GetSwapChain, the one call of it the
// overlay sees for this device - and that swap chain is what GetSwapChain(0)
// answers with from then on (hk_get_swap).
HRESULT adopt_device(IDirect3DDevice9* device, HWND hwnd, const D3DPRESENT_PARAMETERS* pp, DWORD flags, HRESULT hr) {
  if (!adopt::game_window(hwnd)) {
    adopt::leave_alone("dx9: a device", "CreateDevice", hwnd);
    return hr;
  }
  const auto vt = mem::read<uintptr_t>(reinterpret_cast<uintptr_t>(device));
  // What the hooks fall back on: the class's functions (its vtable is left alone).
  if (!g_orig_present) {
    g_orig_present = mem::read<PresentFn>(vt + kPresent * sizeof(void*));
    g_orig_create_swap = mem::read<CreateSwapFn>(vt + kCreateSwap * sizeof(void*));
    g_orig_get_swap = mem::read<GetSwapFn>(vt + kGetSwap * sizeof(void*));
    g_orig_end_scene = mem::read<EndSceneFn>(vt + kEndScene * sizeof(void*));
    g_orig_reset = mem::read<ResetFn>(vt + kReset * sizeof(void*));
  }
  static const adopt::Hook hooks[] = {{kPresent, reinterpret_cast<void*>(&hk_present)},
                                      {kCreateSwap, reinterpret_cast<void*>(&hk_create_swap)},
                                      {kGetSwap, reinterpret_cast<void*>(&hk_get_swap)},
                                      {kEndScene, reinterpret_cast<void*>(&hk_end_scene)},
                                      {kReset, reinterpret_cast<void*>(&hk_reset)}};
  const int copied = adopt::take(device, kDeviceSlots, kDeviceCopy, hooks, 5);
  char where[MAX_PATH + 32], win[128];
  re3cc::describe_address(reinterpret_cast<void*>(vt), where, sizeof(where));
  adopt::describe_window(hwnd, win, sizeof(win));
  if (!copied) {
    logf("ERROR: dx9: the game's device %p (%s) could not be adopted (its vtable at %s) - no panel",
         static_cast<void*>(device), win, where);
    return hr;
  }
  g_device = device;
  g_hwnd = hwnd;
  if (pp) {
    g_stats.backbuffer_w = pp->BackBufferWidth;
    g_stats.backbuffer_h = pp->BackBufferHeight;
    g_stats.windowed = pp->Windowed != FALSE;
  }
  RECT rc{};
  if (hwnd) GetClientRect(hwnd, &rc);
  logf("dx9: device %p created on thread %lu (%s, client %ldx%ld, backbuffer %ux%u %s, fmt %u, flags 0x%lX) adopted - "
       "a vtable of its own with Present/CreateAdditionalSwapChain/GetSwapChain/EndScene/Reset (%d slots copied; the "
       "class's at %s)",
       static_cast<void*>(device), GetCurrentThreadId(), win, rc.right - rc.left, rc.bottom - rc.top,
       pp ? pp->BackBufferWidth : 0u, pp ? pp->BackBufferHeight : 0u, pp && pp->Windowed ? "windowed" : "fullscreen",
       pp ? static_cast<unsigned>(pp->BackBufferFormat) : 0u, static_cast<unsigned long>(flags), copied, where);
  g_game_device = device;
  g_game_chain = nullptr;
  IDirect3DSwapChain9* sc = nullptr;
  const auto get = reinterpret_cast<GetSwapFn>(adopt::original(device, kGetSwap, reinterpret_cast<void*>(g_orig_get_swap)));
  if (SUCCEEDED(get(device, 0, &sc)) && sc) {
    hook_swap_chain(sc, "implicit");
    g_game_chain = sc;
    sc->Release();
  } else {
    logf("ERROR: dx9: the game's device has no implicit swap chain - no panel on its presents");
  }
  return hr;
}

HRESULT __stdcall hk_create_device(IDirect3D9* self, UINT adapter, D3DDEVTYPE type, HWND focus, DWORD flags,
                                   D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** out) {
  const auto orig = reinterpret_cast<CreateDeviceFn>(
      adopt::original(self, kCreateDevice, reinterpret_cast<void*>(g_orig_create_device)));
  HRESULT hr;
  {
    adopt::CallingOn on;
    hr = orig(self, adapter, type, focus, flags, pp, out);
  }
  if (FAILED(hr) || !out || !*out) {
    static volatile LONG failures = 0;
    if (InterlockedIncrement(&failures) <= 8)
      logf("dx9: CreateDevice failed (hr=0x%08lX; adapter %u, type %d, hwnd=%p, flags 0x%lX, backbuffer %ux%u %s, fmt %u)",
           static_cast<unsigned long>(hr), adapter, static_cast<int>(type), static_cast<void*>(focus),
           static_cast<unsigned long>(flags), pp ? pp->BackBufferWidth : 0u, pp ? pp->BackBufferHeight : 0u,
           pp && pp->Windowed ? "windowed" : "fullscreen", pp ? static_cast<unsigned>(pp->BackBufferFormat) : 0u);
    return hr;
  }
  if (adopt::enabled()) return adopt_device(*out, focus ? focus : pp ? pp->hDeviceWindow : nullptr, pp, flags, hr);
  g_device = *out;
  if (focus) g_hwnd = focus;
  else if (pp && pp->hDeviceWindow) g_hwnd = pp->hDeviceWindow;
  if (pp) {
    g_stats.backbuffer_w = pp->BackBufferWidth;
    g_stats.backbuffer_h = pp->BackBufferHeight;
    g_stats.windowed = pp->Windowed != FALSE;
  }
  char cls[64] = "?";
  if (g_hwnd) GetClassNameA(g_hwnd, cls, sizeof(cls));
  RECT rc{};
  if (g_hwnd) GetClientRect(g_hwnd, &rc);
  const auto vt = mem::read<uintptr_t>(reinterpret_cast<uintptr_t>(g_device));
  if (!g_orig_present) {
    g_orig_present = reinterpret_cast<PresentFn>(mem::hook_vtable(vt, 17, reinterpret_cast<void*>(&hk_present)));
    g_orig_create_swap =
        reinterpret_cast<CreateSwapFn>(mem::hook_vtable(vt, 13, reinterpret_cast<void*>(&hk_create_swap)));
    g_orig_end_scene = reinterpret_cast<EndSceneFn>(mem::hook_vtable(vt, 42, reinterpret_cast<void*>(&hk_end_scene)));
    g_orig_reset = reinterpret_cast<ResetFn>(mem::hook_vtable(vt, 16, reinterpret_cast<void*>(&hk_reset)));
    g_device_vt = vt;
    char where[MAX_PATH + 32];
    re3cc::describe_address(reinterpret_cast<void*>(g_orig_present), where, sizeof(where));
    logf("dx9: device %p created on thread %lu (hwnd=%p class '%s' client %ldx%ld, backbuffer %ux%u %s, fmt %u, "
         "flags 0x%lX); hooked Present/CreateAdditionalSwapChain/EndScene/Reset (Present was %s)",
         static_cast<void*>(g_device), GetCurrentThreadId(), static_cast<void*>(g_hwnd), cls, rc.right - rc.left,
         rc.bottom - rc.top, pp ? pp->BackBufferWidth : 0u, pp ? pp->BackBufferHeight : 0u,
         pp && pp->Windowed ? "windowed" : "fullscreen", pp ? static_cast<unsigned>(pp->BackBufferFormat) : 0u,
         static_cast<unsigned long>(flags), where);
  } else if (vt != g_device_vt) {
    logf("dx9: another device %p with a different vtable - not hooked", static_cast<void*>(g_device));
  }
  IDirect3DSwapChain9* sc = nullptr;
  if (SUCCEEDED(g_device->GetSwapChain(0, &sc)) && sc) {
    hook_swap_chain(sc, "implicit");
    sc->Release();
  }
  return hr;
}

// Windows: every IDirect3D9 the wrapper makes gets a vtable of its own with
// CreateDevice in it. Windows' compatibility shim for ResidentEvil3.exe
// (apphelp.dll) writes its own CreateDevice into d3d9.dll's class vtable on
// every Direct3DCreate9, over whatever was put there before, and the wrapper
// makes a second IDirect3D9 before its device: a hook in the class's vtable
// was gone by then (2026-10-08, the first Windows run). The copy's CreateDevice
// calls on through the class's slot as it is at the time (the shim's, then
// d3d9.dll's).
IDirect3D9* adopt_d3d(IDirect3D9* d3d) {
  const auto vt = mem::read<uintptr_t>(reinterpret_cast<uintptr_t>(d3d));
  void* now = mem::read<void*>(vt + kCreateDevice * sizeof(void*));
  // What the hook falls back on: the class's CreateDevice (its vtable is left alone).
  if (!g_orig_create_device) g_orig_create_device = reinterpret_cast<CreateDeviceFn>(now);
  static const adopt::Hook hooks[] = {{kCreateDevice, reinterpret_cast<void*>(&hk_create_device)}};
  const int copied = adopt::take(d3d, kD3DSlots, kD3DCopy, hooks, 1);
  static int logged = 0;
  if (logged < 4) {
    ++logged;
    char where[MAX_PATH + 32], cd[MAX_PATH + 32];
    re3cc::describe_address(reinterpret_cast<void*>(vt), where, sizeof(where));
    re3cc::describe_address(now, cd, sizeof(cd));
    if (copied)
      logf("dx9: the wrapper called Direct3DCreate9 (thread %lu): IDirect3D9 %p adopted - a vtable of its own with "
           "CreateDevice (%d slots copied; the class's at %s, its CreateDevice %s)",
           GetCurrentThreadId(), static_cast<void*>(d3d), copied, where, cd);
    else
      logf("ERROR: dx9: the wrapper's IDirect3D9 %p could not be adopted (its vtable at %s) - no device will be seen",
           static_cast<void*>(d3d), where);
  }
  return d3d;
}

IDirect3D9* WINAPI hk_create9(UINT sdk) {
  IDirect3D9* d3d = g_real_create9(sdk);
  if (d3d && adopt::enabled()) return adopt_d3d(d3d);
  if (d3d && !g_orig_create_device) {
    const auto vt = mem::read<uintptr_t>(reinterpret_cast<uintptr_t>(d3d));
    g_orig_create_device =
        reinterpret_cast<CreateDeviceFn>(mem::hook_vtable(vt, 16, reinterpret_cast<void*>(&hk_create_device)));
    g_d3d_vt = vt;
    logf("dx9: the wrapper called Direct3DCreate9 (thread %lu); hooked IDirect3D9::CreateDevice", GetCurrentThreadId());
  }
  return d3d;
}

}  // namespace

bool install_import(HMODULE wrapper) {
  void* prev = mem::iat_hook(wrapper, "d3d9.dll", "Direct3DCreate9", reinterpret_cast<void*>(&hk_create9));
  if (!prev) {
    logf("ERROR: could not hook Direct3DCreate9 in ddraw_orig.dll's import table - no overlay");
    return false;
  }
  g_real_create9 = reinterpret_cast<Create9Fn>(prev);
  g_iat_slot = mem::iat_slot(wrapper, "d3d9.dll", "Direct3DCreate9");
  // Where it led: d3d9.dll, or a compatibility shim in front of it (apphelp.dll
  // for ResidentEvil3.exe on Windows).
  char where[MAX_PATH + 32];
  re3cc::describe_address(prev, where, sizeof(where));
  logf("overlay: ddraw_orig.dll's Direct3DCreate9 import hooked (it led to %s)", where);
  return true;
}

Stats stats() { return g_stats; }

bool can_present_still() { return g_device && g_swap && g_still && g_orig_swap_present && g_renderer_ready; }

bool present_still() {
  if (!can_present_still()) return false;
  IDirect3DSurface9* back = nullptr;
  if (FAILED(g_swap->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &back)) || !back) return false;
  panel_frame(g_device, back);
  back->Release();
  // On to the swap chain's own Present, as the hook does (the panel is in the
  // frame already).
  const auto orig =
      reinterpret_cast<SwapPresentFn>(adopt::original(g_swap, kSwapPresent, reinterpret_cast<void*>(g_orig_swap_present)));
  adopt::CallingOn on;
  return SUCCEEDED(orig(g_swap, nullptr, nullptr, nullptr, nullptr, 0));
}

namespace {
// The copy is opaque whatever its alpha says: the back buffer's alpha channel
// is whatever the wrapper left in it.
void no_blend(const ImDrawList*, const ImDrawCmd*) {
  if (g_device) g_device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
}
}  // namespace

void draw_still(ImDrawList* dl, float w, float h) {
  dl->AddRectFilled(ImVec2(0, 0), ImVec2(w, h), IM_COL32(0, 0, 0, 255));
  if (!g_still) return;
  dl->AddCallback(no_blend, nullptr);
  dl->AddImage(static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(g_still)), ImVec2(0, 0), ImVec2(w, h));
  dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
}

// Main thread, once a second: the counts of the last second, logged for the
// first ten seconds with a device (and whenever they change, with Trace = 1).
void log_rates() {
  const auto p = static_cast<unsigned>(InterlockedExchange(&g_presents, 0));
  const auto sp = static_cast<unsigned>(InterlockedExchange(&g_swap_presents, 0));
  const auto es = static_cast<unsigned>(InterlockedExchange(&g_end_scenes, 0));
  const bool changed = p != g_stats.present_per_s || sp != g_stats.swap_present_per_s || es != g_stats.end_scene_per_s;
  g_stats.present_per_s = p;
  g_stats.swap_present_per_s = sp;
  g_stats.end_scene_per_s = es;
  if (!g_device) return;
  if (g_rate_logs < 10 || (config::get().trace && changed)) {
    ++g_rate_logs;
    logf("dx9: last second: %u device Present, %u swap-chain Present, %u EndScene", p, sp, es);
  }
}

void uninstall() {
  if (g_device_vt && mem::readable(reinterpret_cast<void*>(g_device_vt), 64 * sizeof(void*))) {
    if (g_orig_present) mem::write<void*>(g_device_vt + 17 * sizeof(void*), reinterpret_cast<void*>(g_orig_present));
    if (g_orig_create_swap)
      mem::write<void*>(g_device_vt + 13 * sizeof(void*), reinterpret_cast<void*>(g_orig_create_swap));
    if (g_orig_end_scene)
      mem::write<void*>(g_device_vt + 42 * sizeof(void*), reinterpret_cast<void*>(g_orig_end_scene));
    if (g_orig_reset) mem::write<void*>(g_device_vt + 16 * sizeof(void*), reinterpret_cast<void*>(g_orig_reset));
  }
  if (g_swap_vt && g_orig_swap_present && mem::readable(reinterpret_cast<void*>(g_swap_vt), 4 * sizeof(void*)))
    mem::write<void*>(g_swap_vt + 3 * sizeof(void*), reinterpret_cast<void*>(g_orig_swap_present));
  if (g_d3d_vt && mem::readable(reinterpret_cast<void*>(g_d3d_vt), 17 * sizeof(void*)) && g_orig_create_device)
    mem::write<void*>(g_d3d_vt + 16 * sizeof(void*), reinterpret_cast<void*>(g_orig_create_device));
  if (g_iat_slot && g_real_create9 && mem::read<uintptr_t>(g_iat_slot) == reinterpret_cast<uintptr_t>(&hk_create9))
    mem::write<uintptr_t>(g_iat_slot, reinterpret_cast<uintptr_t>(g_real_create9));
  g_orig_present = nullptr;
  g_orig_swap_present = nullptr;
  g_orig_create_swap = nullptr;
  g_orig_end_scene = nullptr;
  g_orig_reset = nullptr;
  g_orig_create_device = nullptr;
  release_still();
  g_swap = nullptr;
  if (g_renderer_ready) {
    ImGui_ImplDX9_Shutdown();
    g_renderer_ready = false;
  }
}

}  // namespace re3cc::overlay::dx9
