// Private vtables for the D3D9 device GOG's wrapper makes for the game's window
// and for its swap chains.
//
// The panel's first design, which Wine keeps, put the mod's functions into
// D3D9's class vtables: the device's Present, CreateAdditionalSwapChain,
// EndScene and Reset, the swap chain's Present. On Windows the Steam overlay
// hooks those classes by writing a jump into the function a slot points to at
// that moment, keeping ONE saved original per hook: the device's Release,
// CreateAdditionalSwapChain, GetSwapChain, Reset, Present and more as each
// device is made, and a swap chain's Present in its GetSwapChain and
// CreateAdditionalSwapChain hooks - on EVERY call, whatever that swap chain's
// vtable holds then. GOG's wrapper asks the device for its swap chain right
// after making it, after the mod had put its Present into the swap chain class's
// vtable: the overlay would take the mod's Present for its original while the
// mod's own way on is D3D9's Present, by then the overlay's hook, and the two
// would call each other until the stack ran out at the first frame (found from
// gameoverlayrenderer.dll's code, 2026-09-27, with Fear3CabbyCodes' crash of
// the same kind on its D3D11 swap chain, and fixed this way in
// RE1-1996CabbyCodes, whose GOG wrapper does the same; brought here
// 2026-10-08, not yet seen in this game).
//
// So on Windows the device and its swap chains get copies of their vtables
// with the mod's functions in them, and their own vtable pointers move to the
// copies. The overlay reads a device's vtable only as it is made, before the
// mod sees it; a swap chain's it reads in GetSwapChain every time, so the
// adopted device answers GetSwapChain itself (overlay_dx9.cpp): it only ever
// finds the classes' own functions. The hooks call on through the vtable the object had, as it is at
// the time (original()), so a tool that hooks that vtable later still gets the
// game's frames.

#include "adopt.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "log.h"
#include "mem.h"

namespace re3cc::adopt {
namespace {

constexpr int kMaxHooks = 8;
constexpr int kMaxObjects = 64;

struct Entry {
  void* obj = nullptr;       // stale once the game frees it
  void** orig = nullptr;     // the vtable the object had: what the hooks call on through
  void** copy = nullptr;     // its own (never freed: the object may outlive what the mod knows)
  bool orig_static = false;  // orig is a class's vtable in a module (not another tool's copy, which it may free)
  int least = 0;             // slots the object is known to have
  int copied = 0;
  int nhooks = 0;
  int slot[kMaxHooks] = {};
  void* real[kMaxHooks] = {};  // what orig held in those slots at adoption
};

Entry g_entries[kMaxObjects];
int g_count = 0;
CRITICAL_SECTION g_cs;
bool g_ready = false;
bool g_enabled = false;
DWORD g_game_thread = 0;
mem::Range g_self{};
thread_local int t_calls = 0;

struct Lock {
  Lock() { EnterCriticalSection(&g_cs); }
  ~Lock() { LeaveCriticalSection(&g_cs); }
  Lock(const Lock&) = delete;
  Lock& operator=(const Lock&) = delete;
};

HMODULE module_of(const void* p) {
  HMODULE m = nullptr;
  GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                     static_cast<LPCSTR>(p), &m);
  return m;
}

Entry* find(const void* obj) {  // under g_cs
  for (int i = 0; i < g_count; ++i)
    if (g_entries[i].obj == obj) return &g_entries[i];
  return nullptr;
}

// A free entry: a new one, or one whose object no longer has its copy (freed).
Entry* free_entry() {  // under g_cs
  if (g_count < kMaxObjects) return &g_entries[g_count];
  for (Entry& e : g_entries) {
    void* vt = nullptr;
    if (!mem::read_safe(reinterpret_cast<uintptr_t>(e.obj), &vt) || vt != e.copy) return &e;
  }
  return nullptr;
}

}  // namespace

void init() {
  if (g_ready) return;
  InitializeCriticalSection(&g_cs);
  g_game_thread = GetCurrentThreadId();
  HMODULE self = module_of(reinterpret_cast<const void*>(&init));
  g_self = mem::module_range(self);
  const HMODULE ntdll = GetModuleHandleA("ntdll.dll");
  const bool wine = ntdll && GetProcAddress(ntdll, "wine_get_version");
  const bool forced = GetEnvironmentVariableA("RE3CC_ADOPT", nullptr, 0) != 0;
  g_enabled = !wine || forced;
  g_ready = true;
  if (g_enabled) {
    // The adopted objects point into this image for as long as they live.
    HMODULE pinned = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                       reinterpret_cast<LPCSTR>(&init), &pinned);
    logf("overlay: %s - the game's D3D9 device and its swap chains get vtables of their own as they are made; the "
         "classes' vtables are left alone (the Steam overlay hooks what those point to)",
         wine ? "Wine, RE3CC_ADOPT set" : "Windows");
  } else {
    logf("overlay: Wine - the device and swap chain class vtables are hooked, as before");
  }
}

bool enabled() { return g_enabled; }

bool game_window(HWND w) {
  DWORD pid = 0;
  const DWORD tid = w ? GetWindowThreadProcessId(w, &pid) : 0;
  return tid && pid == GetCurrentProcessId() && tid == g_game_thread;
}

void describe_window(HWND w, char* out, size_t n) {
  char cls[64] = "?";
  DWORD pid = 0, tid = 0;
  if (w) {
    GetClassNameA(w, cls, sizeof(cls));
    tid = GetWindowThreadProcessId(w, &pid);
  }
  std::snprintf(out, n, "window %p (class '%s', thread %lu%s)", static_cast<void*>(w), cls, tid,
                w && pid != GetCurrentProcessId() ? ", another program's" : "");
}

void leave_alone(const char* what, const char* how, HWND w) {
  static volatile LONG logged = 0;
  const LONG n = InterlockedIncrement(&logged);
  if (n > 8) return;
  char win[128];
  describe_window(w, win, sizeof(win));
  logf("overlay: %s made by %s for %s is not the game's (the game's thread is %lu) - it keeps its class's vtable%s", what,
       how, win, g_game_thread, n == 8 ? " (no more of these logged)" : "");
}

int take(void* obj, int least, int want, const Hook* hooks, int nhooks) {
  if (!g_ready || !obj || nhooks > kMaxHooks) return 0;
  void** vt = nullptr;
  if (!mem::read_safe(reinterpret_cast<uintptr_t>(obj), &vt) || !vt) return 0;
  for (int i = 0; i < nhooks; ++i)
    if (hooks[i].slot >= least) return 0;
  Lock lock;
  Entry* e = find(obj);
  if (e && e->copy == vt) return e->copied;  // ours already
  if (!e) e = free_entry();
  if (!e) return 0;

  // As many slots as the read allows, the interface's at least: a class may
  // have virtual functions of its own after the interface's (a wrapper's
  // destructor - OptiScaler's, in RE2CabbyCodes), called through the object's
  // vtable pointer, so the copy must have them too. And the two entries before
  // the first, where MSVC (-1) and the Itanium ABI of a mingw-built DXVK (-2,
  // -1) keep the class's type information, which dynamic_cast reads.
  int n = want > least ? want : least;
  while (!mem::readable(vt, static_cast<size_t>(n) * sizeof(void*))) {
    if (n <= least) return 0;
    n = n - 16 > least ? n - 16 : least;
  }
  auto** block = static_cast<void**>(std::calloc(static_cast<size_t>(n) + 2, sizeof(void*)));
  if (!block) return 0;
  void** copy = block + 2;
  std::memcpy(copy, vt, static_cast<size_t>(n) * sizeof(void*));
  if (mem::readable(vt - 2, 2 * sizeof(void*))) std::memcpy(block, vt - 2, 2 * sizeof(void*));

  Entry fresh;
  fresh.obj = obj;
  fresh.orig = vt;
  fresh.copy = copy;
  fresh.orig_static = module_of(vt) != nullptr;
  fresh.least = least;
  fresh.copied = n;
  fresh.nhooks = nhooks;
  for (int i = 0; i < nhooks; ++i) {
    void* was = vt[hooks[i].slot];
    fresh.slot[i] = hooks[i].slot;
    fresh.real[i] = g_self.contains(reinterpret_cast<uintptr_t>(was)) ? nullptr : was;  // never the mod's own
    copy[hooks[i].slot] = hooks[i].fn;
  }
  const bool added = e == &g_entries[g_count];
  *e = fresh;
  if (added) ++g_count;
  // Registered before the object moves: a call through the copy on another
  // thread finds its entry at once.
  InterlockedExchangePointer(static_cast<void* volatile*>(obj), copy);
  return n;
}

void* original(const void* obj, int slot, void* fallback) {
  if (!g_ready) return fallback;
  Lock lock;
  const Entry* e = find(obj);
  if (!e) return fallback;
  int i = 0;
  while (i < e->nhooks && e->slot[i] != slot) ++i;
  if (i == e->nhooks) return fallback;
  // The class's function as its vtable has it now - unless that is a function
  // of the mod's own, or a call on is under way on this thread (a hook in
  // that vtable that calls the object's again must not come back into itself).
  if (t_calls == 0 && e->orig_static) {
    void* volatile* live = e->orig;
    void* now = live[slot];
    if (now && !g_self.contains(reinterpret_cast<uintptr_t>(now))) return now;
  }
  return e->real[i] ? e->real[i] : fallback;
}

bool calling_on() { return t_calls > 0; }

CallingOn::CallingOn() { ++t_calls; }
CallingOn::~CallingOn() { --t_calls; }

}  // namespace re3cc::adopt
