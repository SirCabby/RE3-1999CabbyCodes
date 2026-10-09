// The overlay's D3D9 vtable handling with the Steam overlay's way of hooking, outside the game:
//
//   mkdir -p tests/build
//   i686-w64-mingw32-g++ -std=c++20 -O2 -Wall -DWIN32_LEAN_AND_MEAN -static tests/test_adopt.cpp
//       -luser32 -lgdi32 -o tests/build/test_adopt.exe
//   cp build/ddraw.dll <dir>/      (a scratch folder), and tests/fake_ddraw.cpp built as <dir>/ddraw_orig.dll
//   WINEDLLOVERRIDES=ddraw=n,b RE3CC_ADOPT=1 wine tests/build/test_adopt.exe 'Z:\<dir>\' overlay adopt
//   WINEDLLOVERRIDES=ddraw=n,b RE3CC_ADOPT=1 wine tests/build/test_adopt.exe 'Z:\<dir>\' shim adopt
//   WINEDLLOVERRIDES=ddraw=n,b wine tests/build/test_adopt.exe 'Z:\<dir>\' none class      (Wine's way, as before)
//
// (one process per run: the proxy keeps its state; RE3CC_ADOPT makes it take
// the Windows path under Wine; ddraw=n,b is what Proton sets for the game). The
// exe loads the proxy on its primary thread, which loads the stand-in wrapper
// and hooks its Direct3DCreate9 import; the exe then plays GOG's wrapper: an
// IDirect3D9 made, released and made again, a device for its window,
// GetSwapChain(0) right after it, frames presented through that swap chain
// (never the device's Present), a Reset, an additional swap chain, a second
// device.
//
// "shim" plays Windows' compatibility shim for ResidentEvil3.exe (apphelp.dll,
// 2026-10-08). It is in the wrapper's Direct3DCreate9 import before the proxy
// loads; every call writes its own CreateDevice into the IDirect3D9 class's
// vtable, over whatever is there, and that CreateDevice calls d3d9's own. A
// CreateDevice hook in the class's vtable is gone by the time the wrapper's
// second IDirect3D9 makes its device, so no device is seen (the class path
// fails here). With the fix the wrapper's IDirect3D9 has a vtable of its own,
// the class's keeps the shim's, and every CreateDevice goes on through the shim.
//
// "overlay" plays the Steam overlay on Windows (gameoverlayrenderer.dll,
// 2026-09-27). Installed before the proxy, its CreateDevice hook hooks every
// device as it is made - CreateAdditionalSwapChain, GetSwapChain, Reset,
// Present - and its GetSwapChain and CreateAdditionalSwapChain hooks hook the
// swap chain they hand out, its Present, on EVERY call: each by what the slot
// holds at that moment (here by writing its own function into that slot,
// which a caller of the slot meets the same way as the overlay's jump in the
// function), keeping ONE saved original per hook and skipping a slot it hooked
// already. With the proxy's Present in the swap chain class's vtable (0.1.0)
// the wrapper's GetSwapChain makes the overlay take it for the original and
// the two call each other at the first frame: a stack overflow on Windows.
// Here the overlay's hooks count their depth and give up past 4 (a "LOOP"
// failure). With the fix the device and its swap chains must come back with
// vtables of their own holding the proxy's hooks, the classes' vtables must
// hold only the overlay's, every present must pass through the overlay
// exactly once, and a device for another thread's window must keep its class's
// vtable.
#include <windows.h>
#include <d3d9.h>

#include <cstdio>
#include <cstring>

namespace {

int g_failures = 0;
HMODULE g_proxy = nullptr;
bool g_expect_adopt = false;

void check(bool ok, const char* what) {
  std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) ++g_failures;
}

bool in_proxy(const void* fn) {
  HMODULE owner = nullptr;
  return GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCSTR>(fn), &owner) &&
         owner == g_proxy;
}

void** vtable_of(const void* obj) { return *static_cast<void** const*>(obj); }

void write_slot(void** vt, int slot, void* fn) {
  DWORD old = 0;
  VirtualProtect(&vt[slot], sizeof(void*), PAGE_EXECUTE_READWRITE, &old);
  vt[slot] = fn;
  VirtualProtect(&vt[slot], sizeof(void*), old, &old);
}

// --- the overlay -------------------------------------------------------------
struct OvHook {
  void* saved = nullptr;  // ONE original, whatever the slot held at the last pass
  int calls = 0;
  int depth = 0;
  bool looped = false;
};
OvHook g_reset, g_present9, g_get_swap, g_create_add, g_swap_present;
void* g_real_create_device = nullptr;
bool g_overlay = false;

template <typename Fn, typename... A>
HRESULT through(OvHook& h, A... a) {
  ++h.calls;
  if (h.depth >= 4) {
    h.looped = true;
    return E_FAIL;
  }
  ++h.depth;
  const HRESULT hr = reinterpret_cast<Fn>(h.saved)(a...);
  --h.depth;
  return hr;
}

using ResetFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
using Present9Fn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
using GetSwapFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, IDirect3DSwapChain9**);
using CreateAddFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*, IDirect3DSwapChain9**);
using SwapPresentFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DSwapChain9*, const RECT*, const RECT*, HWND, const RGNDATA*,
                                                  DWORD);
using CreateDeviceFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*,
                                                   IDirect3DDevice9**);

// Each slot, unless it is the overlay's already.
void ov_hook(void** vt, int slot, void* hook, OvHook& h) {
  if (vt[slot] == hook) return;
  h.saved = vt[slot];
  write_slot(vt, slot, hook);
}

HRESULT STDMETHODCALLTYPE ov_reset(IDirect3DDevice9* d, D3DPRESENT_PARAMETERS* pp) { return through<ResetFn>(g_reset, d, pp); }
HRESULT STDMETHODCALLTYPE ov_present9(IDirect3DDevice9* d, const RECT* a, const RECT* b, HWND w, const RGNDATA* r) {
  return through<Present9Fn>(g_present9, d, a, b, w, r);
}
HRESULT STDMETHODCALLTYPE ov_swap_present(IDirect3DSwapChain9* sc, const RECT* a, const RECT* b, HWND w, const RGNDATA* r,
                                          DWORD f) {
  return through<SwapPresentFn>(g_swap_present, sc, a, b, w, r, f);
}
// D3D9Device_GetSwapChain / _CreateAdditionalSwapChain: the swap chain handed out gets its Present hooked, every time.
HRESULT STDMETHODCALLTYPE ov_get_swap(IDirect3DDevice9* d, UINT i, IDirect3DSwapChain9** out) {
  ++g_get_swap.calls;
  const HRESULT hr = reinterpret_cast<GetSwapFn>(g_get_swap.saved)(d, i, out);
  if (SUCCEEDED(hr) && out && *out) ov_hook(vtable_of(*out), 3, reinterpret_cast<void*>(&ov_swap_present), g_swap_present);
  return hr;
}
HRESULT STDMETHODCALLTYPE ov_create_add(IDirect3DDevice9* d, D3DPRESENT_PARAMETERS* pp, IDirect3DSwapChain9** out) {
  ++g_create_add.calls;
  const HRESULT hr = reinterpret_cast<CreateAddFn>(g_create_add.saved)(d, pp, out);
  if (SUCCEEDED(hr) && out && *out) ov_hook(vtable_of(*out), 3, reinterpret_cast<void*>(&ov_swap_present), g_swap_present);
  return hr;
}

HRESULT STDMETHODCALLTYPE ov_create_device(IDirect3D9* d3d, UINT a, D3DDEVTYPE t, HWND w, DWORD fl, D3DPRESENT_PARAMETERS* pp,
                                           IDirect3DDevice9** out) {
  const HRESULT hr = reinterpret_cast<CreateDeviceFn>(g_real_create_device)(d3d, a, t, w, fl, pp, out);
  if (SUCCEEDED(hr) && out && *out) {
    void** vt = vtable_of(*out);
    ov_hook(vt, 13, reinterpret_cast<void*>(&ov_create_add), g_create_add);
    ov_hook(vt, 14, reinterpret_cast<void*>(&ov_get_swap), g_get_swap);
    ov_hook(vt, 16, reinterpret_cast<void*>(&ov_reset), g_reset);
    ov_hook(vt, 17, reinterpret_cast<void*>(&ov_present9), g_present9);
  }
  return hr;
}

bool looped() { return g_reset.looped || g_present9.looped || g_swap_present.looped; }

// --- Windows' compatibility shim for ResidentEvil3.exe --------------------------
using Create9Fn = IDirect3D9*(WINAPI*)(UINT);
bool g_shim = false;
Create9Fn g_shim_real_create9 = nullptr;
void* g_shim_real_cd = nullptr;  // d3d9's own, saved the first time
int g_shim_cd_calls = 0;

HRESULT STDMETHODCALLTYPE shim_create_device(IDirect3D9* d3d, UINT a, D3DDEVTYPE t, HWND w, DWORD fl,
                                             D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** out) {
  ++g_shim_cd_calls;
  return reinterpret_cast<CreateDeviceFn>(g_shim_real_cd)(d3d, a, t, w, fl, pp, out);
}

IDirect3D9* WINAPI shim_create9(UINT sdk) {
  IDirect3D9* d3d = g_shim_real_create9(sdk);
  if (d3d) {
    void** vt = vtable_of(d3d);
    if (!g_shim_real_cd) g_shim_real_cd = vt[16];
    write_slot(vt, 16, reinterpret_cast<void*>(&shim_create_device));  // whatever was there
  }
  return d3d;
}

// The import slot of `fn` from `dll` in module `m`.
void** import_slot(HMODULE m, const char* dll, const char* fn) {
  auto* base = reinterpret_cast<BYTE*>(m);
  auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
  const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
  for (auto* d = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); d->Name; ++d) {
    if (_stricmp(reinterpret_cast<char*>(base + d->Name), dll) != 0) continue;
    auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
    auto* slots = reinterpret_cast<void**>(base + d->FirstThunk);
    for (int i = 0; names[i].u1.AddressOfData; ++i) {
      if (IMAGE_SNAP_BY_ORDINAL(names[i].u1.Ordinal)) continue;
      auto* by_name = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names[i].u1.AddressOfData);
      if (std::strcmp(reinterpret_cast<char*>(by_name->Name), fn) == 0) return &slots[i];
    }
  }
  return nullptr;
}

// --- windows -------------------------------------------------------------------
HWND make_window(const char* title) {
  WNDCLASSA wc{};
  wc.lpfnWndProc = DefWindowProcA;
  wc.hInstance = GetModuleHandleA(nullptr);
  wc.lpszClassName = "RE3CCTest";
  RegisterClassA(&wc);  // a second registration fails harmlessly
  return CreateWindowA("RE3CCTest", title, WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, 640, 480, nullptr, nullptr, wc.hInstance,
                       nullptr);
}

void pump() {
  MSG m;
  while (PeekMessageA(&m, nullptr, 0, 0, PM_REMOVE)) {
    TranslateMessage(&m);
    DispatchMessageA(&m);
  }
}

// Another thread's window: a tool's throwaway, not the game's.
HWND g_other = nullptr;
HANDLE g_other_ready = nullptr;
DWORD g_other_tid = 0;
DWORD WINAPI other_thread(LPVOID) {
  g_other = make_window("other thread");
  SetEvent(g_other_ready);
  MSG m;
  while (GetMessageA(&m, nullptr, 0, 0) > 0) {
    TranslateMessage(&m);
    DispatchMessageA(&m);
  }
  return 0;
}

// --- D3D9, the wrapper's way ----------------------------------------------------
IDirect3DDevice9* make_device(IDirect3D9* d3d, HWND w, D3DPRESENT_PARAMETERS* pp) {
  *pp = D3DPRESENT_PARAMETERS{};
  pp->Windowed = TRUE;
  pp->SwapEffect = D3DSWAPEFFECT_DISCARD;
  pp->BackBufferFormat = D3DFMT_UNKNOWN;
  pp->hDeviceWindow = w;
  IDirect3DDevice9* dev = nullptr;
  const HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, w, D3DCREATE_SOFTWARE_VERTEXPROCESSING, pp, &dev);
  if (FAILED(hr)) std::printf("  CreateDevice -> 0x%08lX\n", hr);
  return SUCCEEDED(hr) ? dev : nullptr;
}

// A frame drawn with 2 scenes and presented through the swap chain, as the wrapper does.
bool frame(IDirect3DDevice9* dev, IDirect3DSwapChain9* sc) {
  pump();
  dev->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(20, 20, 40), 1.0f, 0);
  bool ok = SUCCEEDED(dev->BeginScene()) && SUCCEEDED(dev->EndScene());
  ok = SUCCEEDED(dev->BeginScene()) && SUCCEEDED(dev->EndScene()) && ok;
  return SUCCEEDED(sc->Present(nullptr, nullptr, nullptr, nullptr, 0)) && ok;
}

void game_device(const char* name, IDirect3D9* d3d, HWND w, void** dev_cls, void** sc_cls) {
  char what[200];
  D3DPRESENT_PARAMETERS pp;
  IDirect3DDevice9* dev = make_device(d3d, w, &pp);
  std::snprintf(what, sizeof(what), "%s made", name);
  check(dev != nullptr, what);
  if (!dev) return;
  void** vt = vtable_of(dev);
  // The wrapper asks for the swap chain right after making the device.
  IDirect3DSwapChain9* sc = nullptr;
  const int gets_before = g_get_swap.calls;
  check(SUCCEEDED(dev->GetSwapChain(0, &sc)) && sc, "the wrapper's GetSwapChain(0) succeeds");
  if (!sc) {
    dev->Release();
    return;
  }
  void** svt = vtable_of(sc);
  const int swap_slots[] = {13, 14, 16, 17, 42};
  bool dev_ours = true, cls_clean = true;
  if (g_expect_adopt) {
    for (int s : swap_slots) dev_ours = dev_ours && in_proxy(vt[s]);
    for (int s : swap_slots) cls_clean = cls_clean && !in_proxy(dev_cls[s]);
    std::snprintf(what, sizeof(what), "%s has a vtable of its own, its 5 hooks the proxy's", name);
    check(vt != dev_cls && dev_ours, what);
    std::snprintf(what, sizeof(what), "%s: its implicit swap chain has a vtable of its own, Present the proxy's", name);
    check(svt != sc_cls && in_proxy(svt[3]), what);
    std::snprintf(what, sizeof(what), "%s: the device and swap chain classes' vtables hold no function of the proxy's", name);
    check(cls_clean && !in_proxy(sc_cls[3]), what);
    if (g_overlay) {
      std::snprintf(what, sizeof(what), "%s: the wrapper's GetSwapChain did not reach the overlay's GetSwapChain", name);
      check(g_get_swap.calls == gets_before, what);
    }
  } else {
    const int class_slots[] = {13, 16, 17, 42};
    for (int s : class_slots) dev_ours = dev_ours && in_proxy(dev_cls[s]);
    std::snprintf(what, sizeof(what), "%s and its swap chain share the classes' vtables, the proxy's hooks in them", name);
    check(vt == dev_cls && svt == sc_cls && dev_ours && in_proxy(sc_cls[3]), what);
  }
  if (g_overlay) {
    std::snprintf(what, sizeof(what), "%s: none of the overlay's saved originals is the proxy's", name);
    check(!in_proxy(g_reset.saved) && !in_proxy(g_present9.saved) && !in_proxy(g_get_swap.saved) &&
              !in_proxy(g_create_add.saved) && !in_proxy(g_swap_present.saved),
          what);
  }
  int before = g_swap_present.calls;
  bool ok = true;
  for (int i = 0; i < 3; ++i) ok = frame(dev, sc) && ok;
  std::snprintf(what, sizeof(what), "%s: 3 frames presented through the swap chain%s", name,
                g_overlay ? ", each through the overlay once" : "");
  check(ok && !looped() && (!g_overlay || g_swap_present.calls - before == 3), what);
  // A Reset: the swap chain's back buffer reference must go first.
  sc->Release();
  sc = nullptr;
  const int resets_before = g_reset.calls;
  const HRESULT hr = dev->Reset(&pp);
  std::snprintf(what, sizeof(what), "%s: Reset succeeds%s (0x%08lX)", name, g_overlay ? ", through the overlay once" : "", hr);
  check(SUCCEEDED(hr) && !looped() && (!g_overlay || g_reset.calls - resets_before == 1), what);
  check(SUCCEEDED(dev->GetSwapChain(0, &sc)) && sc, "GetSwapChain(0) after the Reset succeeds");
  if (sc) {
    before = g_swap_present.calls;
    std::snprintf(what, sizeof(what), "%s: a frame after the Reset%s", name, g_overlay ? ", through the overlay once" : "");
    check(frame(dev, sc) && !looped() && (!g_overlay || g_swap_present.calls - before == 1), what);
    sc->Release();
  }
  // An additional swap chain for the same window.
  D3DPRESENT_PARAMETERS pp2 = pp;
  IDirect3DSwapChain9* add = nullptr;
  const HRESULT ha = dev->CreateAdditionalSwapChain(&pp2, &add);
  std::snprintf(what, sizeof(what), "%s: an additional swap chain is made (0x%08lX)", name, ha);
  check(SUCCEEDED(ha) && add, what);
  if (add) {
    if (g_expect_adopt) {
      std::snprintf(what, sizeof(what), "%s: the additional swap chain has a vtable of its own, Present the proxy's", name);
      check(vtable_of(add) != sc_cls && in_proxy(vtable_of(add)[3]), what);
    }
    before = g_swap_present.calls;
    std::snprintf(what, sizeof(what), "%s: a frame through the additional swap chain%s", name,
                  g_overlay ? ", through the overlay once" : "");
    check(frame(dev, add) && !looped() && (!g_overlay || g_swap_present.calls - before == 1), what);
    add->Release();
  }
  dev->Release();
}

int run() {
  // d3d9's own Direct3DCreate9: an IDirect3D9 that no wrapper, shim or proxy made,
  // which shows the class's vtable.
  auto real_create = reinterpret_cast<Create9Fn>(GetProcAddress(LoadLibraryA("d3d9.dll"), "Direct3DCreate9"));
  IDirect3D9* plain = real_create ? real_create(D3D_SDK_VERSION) : nullptr;
  if (!plain) {
    std::printf("SKIP: no Direct3D 9\n");
    return 0;
  }
  void** d3d_cls = vtable_of(plain);
  if (g_overlay) {
    // The overlay is in before the proxy: its CreateDevice hook on the IDirect3D9 class.
    g_real_create_device = d3d_cls[16];
    write_slot(d3d_cls, 16, reinterpret_cast<void*>(&ov_create_device));
  }
  if (g_shim) {
    // The shim is in the wrapper's import before the proxy loads: the loader binds it there.
    HMODULE fake = LoadLibraryA("ddraw_orig.dll");
    void** slot = fake ? import_slot(fake, "d3d9.dll", "Direct3DCreate9") : nullptr;
    check(slot != nullptr, "the stand-in wrapper's Direct3DCreate9 import found");
    if (!slot) return 1;
    g_shim_real_create9 = reinterpret_cast<Create9Fn>(*slot);
    write_slot(slot, 0, reinterpret_cast<void*>(&shim_create9));
  }
  g_proxy = LoadLibraryA("ddraw.dll");
  check(g_proxy != nullptr, "proxy loaded");
  if (!g_proxy) return 1;
  HMODULE wrapper = GetModuleHandleA("ddraw_orig.dll");
  using FakeCreate9Fn = IDirect3D9*(__cdecl*)(UINT);
  auto create = wrapper ? reinterpret_cast<FakeCreate9Fn>(GetProcAddress(wrapper, "FakeDirect3DCreate9")) : nullptr;
  check(create != nullptr, "the proxy loaded the stand-in wrapper");
  // GOG's wrapper makes an IDirect3D9, releases it and makes another before its
  // device (ddraw_orig.dll 0x1801EC03..0x1801EC43).
  IDirect3D9* first = create ? create(D3D_SDK_VERSION) : nullptr;
  if (first) first->Release();
  IDirect3D9* d3d = create ? create(D3D_SDK_VERSION) : nullptr;
  if (!d3d) return 1;
  if (g_expect_adopt) {
    check(vtable_of(d3d) != d3d_cls && in_proxy(vtable_of(d3d)[16]),
          "the wrapper's IDirect3D9 has a vtable of its own, CreateDevice the proxy's");
    check(!in_proxy(d3d_cls[16]), "the IDirect3D9 class's vtable holds no function of the proxy's");
  } else {
    check(in_proxy(vtable_of(d3d)[16]), "IDirect3D9's CreateDevice is the proxy's (in place)");
  }
  if (g_shim) check(d3d_cls[16] == reinterpret_cast<void*>(&shim_create_device), "the class's CreateDevice is the shim's");
  HWND w = make_window("game window");
  // The classes' vtables, from a device for another thread's window and its swap
  // chain, which are never adopted - and which must present through the classes as they are.
  D3DPRESENT_PARAMETERS pp;
  IDirect3DDevice9* other = make_device(d3d, g_other, &pp);
  check(other != nullptr, "device for another thread's window made");
  if (!other) return 1;
  IDirect3DSwapChain9* other_sc = nullptr;
  other->GetSwapChain(0, &other_sc);
  if (!other_sc) return 1;
  void** dev_cls = vtable_of(other);
  void** sc_cls = vtable_of(other_sc);
  if (g_expect_adopt)
    check(!in_proxy(dev_cls[42]) && !in_proxy(dev_cls[17]) && !in_proxy(sc_cls[3]),
          "another thread's device and swap chain keep their classes' vtables");
  check(frame(other, other_sc) && !looped(), "another thread's device presents");
  game_device("device 1", d3d, w, dev_cls, sc_cls);
  game_device("device 2", d3d, w, dev_cls, sc_cls);  // a second device: the overlay's device pass again
  check(frame(other, other_sc) && !looped(), "another thread's device still presents");
  if (g_shim) check(g_shim_cd_calls == 3, "all 3 CreateDevice calls went on through the shim's");
  other_sc->Release();
  other->Release();
  d3d->Release();
  plain->Release();
  DestroyWindow(w);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 4) {
    std::printf("usage: test_adopt.exe <dir with ddraw.dll + a stand-in ddraw_orig.dll> overlay|shim|none adopt|class\n");
    return 2;
  }
  const char* dir = argv[1];
  g_overlay = std::strstr(argv[2], "overlay") != nullptr;
  g_shim = std::strstr(argv[2], "shim") != nullptr;
  g_expect_adopt = std::strcmp(argv[3], "adopt") == 0;
  SetDllDirectoryA(dir);
  SetCurrentDirectoryA(dir);
  // The proxy's ini: the panel drawn on every frame, no game to look for.
  char ini[MAX_PATH];
  std::snprintf(ini, sizeof(ini), "%sRE3-1999CabbyCodes.ini", dir);
  if (FILE* f = std::fopen(ini, "w")) {
    std::fputs("AlwaysShow = 1\nDisable = game,dispatch,gpa,input,cursor\n", f);
    std::fclose(f);
  }
  g_other_ready = CreateEventA(nullptr, TRUE, FALSE, nullptr);
  HANDLE t = CreateThread(nullptr, 0, other_thread, nullptr, 0, &g_other_tid);
  WaitForSingleObject(g_other_ready, 5000);
  std::printf("D3D9 through GOG's wrapper's way, %s%s, expecting %s:\n",
              g_overlay ? "the Steam overlay's way of hooking" : "no overlay",
              g_shim ? ", Windows' shim for ResidentEvil3.exe" : "",
              g_expect_adopt ? "vtables of their own" : "the class hooks");
  const int rc = run();
  PostThreadMessageA(g_other_tid, WM_QUIT, 0, 0);
  WaitForSingleObject(t, 5000);
  if (looped()) std::printf("  LOOP: the overlay's hook and the proxy's called each other (a stack overflow in the game)\n");
  std::printf("%s (%d failure(s))\n", g_failures || rc ? "FAILED" : "PASSED", g_failures);
  std::fflush(stdout);
  return g_failures || rc ? 1 : 0;  // ExitProcess: the proxy's DllMain sees a process exit
}
