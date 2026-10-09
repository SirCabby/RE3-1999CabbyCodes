#include "sound.h"

#include <mmsystem.h>
#include <dsound.h>

#include <vector>

#include "config.h"
#include "crash.h"
#include "log.h"
#include "mem.h"

namespace re3cc::sound {
namespace {

// IDirectSoundBuffer's vtable.
constexpr int kRelease = 2, kGetStatus = 9, kLock = 11, kPlay = 12, kSetVolume = 15, kStop = 18;

using ReleaseFn = ULONG(WINAPI*)(IDirectSoundBuffer*);
using GetStatusFn = HRESULT(WINAPI*)(IDirectSoundBuffer*, DWORD*);
using LockFn = HRESULT(WINAPI*)(IDirectSoundBuffer*, DWORD, DWORD, void**, DWORD*, void**, DWORD*, DWORD);
using PlayFn = HRESULT(WINAPI*)(IDirectSoundBuffer*, DWORD, DWORD, DWORD);
using SetVolumeFn = HRESULT(WINAPI*)(IDirectSoundBuffer*, LONG);
using StopFn = HRESULT(WINAPI*)(IDirectSoundBuffer*);
using CreateFn = HRESULT(WINAPI*)(LPCGUID, LPDIRECTSOUND*, LPUNKNOWN);

uintptr_t g_vt = 0;
ReleaseFn g_release = nullptr;
GetStatusFn g_get_status = nullptr;
LockFn g_lock = nullptr;
PlayFn g_play = nullptr;
SetVolumeFn g_set_volume = nullptr;
StopFn g_stop = nullptr;
bool g_ready = false;

// Every buffer the game has used since the hooks went in: the hooked methods
// note it, the last Release forgets it. The game plays sound from more than
// one thread, so the list is taken under a lock.
struct Buffer {
  IDirectSoundBuffer* b;
  DWORD bytes;
  bool primary;
  int plays;
};
CRITICAL_SECTION g_cs;
std::vector<Buffer> g_buffers;
struct Paused {
  IDirectSoundBuffer* b;
  bool loop;
};
std::vector<Paused> g_paused;
volatile LONG g_quiet = 0;
int g_trace_plays = 0;

// A movie's soundtrack is not DirectSound's: GOG's dshow.dll, which plays the
// game's movies, writes it to a waveOut device. Its waveOutOpen and
// waveOutClose imports are watched, so the pause can hold that device too.
using WaveOpenFn = MMRESULT(WINAPI*)(LPHWAVEOUT, UINT, LPCWAVEFORMATEX, DWORD_PTR, DWORD_PTR, DWORD);
using WaveCloseFn = MMRESULT(WINAPI*)(HWAVEOUT);
WaveOpenFn g_wave_open = nullptr;
WaveCloseFn g_wave_close = nullptr;
HMODULE g_dshow = nullptr;
std::vector<HWAVEOUT> g_waves, g_waves_paused;

struct Lock {
  Lock() { EnterCriticalSection(&g_cs); }
  ~Lock() { LeaveCriticalSection(&g_cs); }
  Lock(const Lock&) = delete;
  Lock& operator=(const Lock&) = delete;
};

// The buffer's entry, made on first sight (under the lock). A primary buffer,
// should it ever share the vtable, is noted so as to be left alone.
Buffer* note(IDirectSoundBuffer* b) {
  for (Buffer& e : g_buffers)
    if (e.b == b) return &e;
  DSBCAPS caps{};
  caps.dwSize = sizeof(caps);
  const bool got = SUCCEEDED(b->GetCaps(&caps));
  g_buffers.push_back(Buffer{b, got ? caps.dwBufferBytes : 0, got && (caps.dwFlags & DSBCAPS_PRIMARYBUFFER) != 0, 0});
  return &g_buffers.back();
}

ULONG WINAPI hk_release(IDirectSoundBuffer* b) {
  const ULONG n = g_release(b);
  if (n == 0) {
    // Gone: from the list, and from a pause too (a movie closing while a skip
    // runs releases its soundtrack).
    Lock l;
    for (size_t i = 0; i < g_buffers.size(); ++i)
      if (g_buffers[i].b == b) {
        g_buffers[i] = g_buffers.back();
        g_buffers.pop_back();
        break;
      }
    for (size_t i = 0; i < g_paused.size(); ++i)
      if (g_paused[i].b == b) {
        g_paused[i] = g_paused.back();
        g_paused.pop_back();
        break;
      }
  }
  return n;
}

HRESULT WINAPI hk_get_status(IDirectSoundBuffer* b, DWORD* status) {
  {
    Lock l;
    note(b);
  }
  return g_get_status(b, status);
}

HRESULT WINAPI hk_lock(IDirectSoundBuffer* b, DWORD off, DWORD bytes, void** p1, DWORD* n1, void** p2, DWORD* n2,
                       DWORD flags) {
  {
    Lock l;
    note(b);
  }
  return g_lock(b, off, bytes, p1, n1, p2, n2, flags);
}

HRESULT WINAPI hk_play(IDirectSoundBuffer* b, DWORD reserved, DWORD priority, DWORD flags) {
  bool silent = false;
  {
    Lock l;
    Buffer* e = note(b);
    ++e->plays;
    silent = g_quiet && !(flags & DSBPLAY_LOOPING) && !e->primary;
    if (config::get().trace && g_trace_plays < 200) {
      ++g_trace_plays;
      logf("sound: Play %p (%lu bytes, play %d)%s%s", static_cast<void*>(b), e->bytes, e->plays,
           (flags & DSBPLAY_LOOPING) ? " looping" : "", silent ? " - held back (skipping)" : "");
    }
  }
  return silent ? DS_OK : g_play(b, reserved, priority, flags);
}

HRESULT WINAPI hk_set_volume(IDirectSoundBuffer* b, LONG volume) {
  {
    Lock l;
    note(b);
  }
  return g_set_volume(b, volume);
}

HRESULT WINAPI hk_stop(IDirectSoundBuffer* b) {
  {
    Lock l;
    note(b);
  }
  return g_stop(b);
}

MMRESULT WINAPI hk_wave_open(LPHWAVEOUT out, UINT device, LPCWAVEFORMATEX format, DWORD_PTR callback,
                             DWORD_PTR instance, DWORD flags) {
  const MMRESULT r = g_wave_open(out, device, format, callback, instance, flags);
  if (r == MMSYSERR_NOERROR && out && *out && !(flags & WAVE_FORMAT_QUERY)) {
    Lock l;
    g_waves.push_back(*out);
  }
  return r;
}

MMRESULT WINAPI hk_wave_close(HWAVEOUT h) {
  {
    Lock l;
    for (auto* list : {&g_waves, &g_waves_paused})
      for (size_t i = 0; i < list->size(); ++i)
        if ((*list)[i] == h) {
          (*list)[i] = list->back();
          list->pop_back();
          break;
        }
  }
  return g_wave_close(h);
}

void hook_movie_sound() {
  g_dshow = GetModuleHandleA("dshow.dll");
  if (!g_dshow) return;  // not GOG's movie player: nothing to hold
  const uintptr_t open = mem::iat_slot(g_dshow, "WINMM.dll", "waveOutOpen");
  const uintptr_t close = mem::iat_slot(g_dshow, "WINMM.dll", "waveOutClose");
  if (!open || !close) return;
  // Originals first, as for the buffers; close before open, so that no device
  // is opened through our hook that its close could miss.
  g_wave_open = mem::read<WaveOpenFn>(open);
  g_wave_close = mem::read<WaveCloseFn>(close);
  if (!g_wave_open || !g_wave_close || !mem::write<void*>(close, reinterpret_cast<void*>(&hk_wave_close))) return;
  if (!mem::write<void*>(open, reinterpret_cast<void*>(&hk_wave_open))) return;
  logf("sound: the movie player's waveOut is watched too");
}

// The original is stored before the slot changes: the game's threads go on
// calling these while the hooks go in, and must never find one half made.
template <typename Fn>
bool hook(int slot, void* replacement, Fn* original) {
  const uintptr_t entry = g_vt + static_cast<uintptr_t>(slot) * sizeof(void*);
  void* prev = mem::read<void*>(entry);
  if (!prev) return false;
  *original = reinterpret_cast<Fn>(prev);
  return mem::write<void*>(entry, replacement);
}

}  // namespace

bool install(HWND hwnd) {
  if (g_ready) return true;
  HMODULE m = GetModuleHandleA("dsound.dll");
  auto create = m ? reinterpret_cast<CreateFn>(GetProcAddress(m, "DirectSoundCreate")) : nullptr;
  if (!create) {
    logf("sound: dsound.dll is not loaded - the cutscene skip leaves the sound alone");
    return false;
  }
  IDirectSound* ds = nullptr;
  HRESULT hr = create(nullptr, &ds, nullptr);
  if (FAILED(hr) || !ds) {
    logf("sound: no DirectSound of our own (hr=0x%08lX) - the cutscene skip leaves the sound alone",
         static_cast<unsigned long>(hr));
    return false;
  }
  ds->SetCooperativeLevel(hwnd, DSSCL_NORMAL);
  WAVEFORMATEX wf{};
  wf.wFormatTag = WAVE_FORMAT_PCM;
  wf.nChannels = 1;
  wf.nSamplesPerSec = 22050;
  wf.wBitsPerSample = 16;
  wf.nBlockAlign = 2;
  wf.nAvgBytesPerSec = 44100;
  DSBUFFERDESC desc{};
  desc.dwSize = sizeof(desc);
  desc.dwFlags = DSBCAPS_CTRLVOLUME;
  desc.dwBufferBytes = 4410;
  desc.lpwfxFormat = &wf;
  IDirectSoundBuffer* b = nullptr;
  hr = ds->CreateSoundBuffer(&desc, &b, nullptr);
  if (FAILED(hr) || !b) {
    logf("sound: no sound buffer of our own (hr=0x%08lX) - the cutscene skip leaves the sound alone",
         static_cast<unsigned long>(hr));
    ds->Release();
    return false;
  }
  InitializeCriticalSection(&g_cs);
  g_vt = mem::read<uintptr_t>(reinterpret_cast<uintptr_t>(b));
  const bool ok = hook(kRelease, reinterpret_cast<void*>(&hk_release), &g_release) &&
                  hook(kGetStatus, reinterpret_cast<void*>(&hk_get_status), &g_get_status) &&
                  hook(kLock, reinterpret_cast<void*>(&hk_lock), &g_lock) &&
                  hook(kPlay, reinterpret_cast<void*>(&hk_play), &g_play) &&
                  hook(kSetVolume, reinterpret_cast<void*>(&hk_set_volume), &g_set_volume) &&
                  hook(kStop, reinterpret_cast<void*>(&hk_stop), &g_stop);
  b->Release();
  ds->Release();
  if (!ok) {
    logf("ERROR: sound: the buffer vtable %p could not be hooked", reinterpret_cast<void*>(g_vt));
    uninstall();
    return false;
  }
  g_ready = true;
  hook_movie_sound();
  char where[MAX_PATH + 32] = "?";
  re3cc::describe_address(reinterpret_cast<void*>(g_play), where, sizeof(where));
  logf("sound: DirectSound buffers hooked (vtable %p, Play was %s)", reinterpret_cast<void*>(g_vt), where);
  return true;
}

void uninstall() {
  if (!g_vt) return;
  // Only the hooks go: this runs as the process ends, when starting sound
  // again would be no kindness to anyone.
  g_ready = false;
  // Only slots still holding our hooks go back.
  auto restore = [](int slot, void* ours, void* original) {
    const uintptr_t entry = g_vt + static_cast<uintptr_t>(slot) * sizeof(void*);
    if (original && mem::read<void*>(entry) == ours) mem::write<void*>(entry, original);
  };
  restore(kRelease, reinterpret_cast<void*>(&hk_release), reinterpret_cast<void*>(g_release));
  restore(kGetStatus, reinterpret_cast<void*>(&hk_get_status), reinterpret_cast<void*>(g_get_status));
  restore(kLock, reinterpret_cast<void*>(&hk_lock), reinterpret_cast<void*>(g_lock));
  restore(kPlay, reinterpret_cast<void*>(&hk_play), reinterpret_cast<void*>(g_play));
  restore(kSetVolume, reinterpret_cast<void*>(&hk_set_volume), reinterpret_cast<void*>(g_set_volume));
  restore(kStop, reinterpret_cast<void*>(&hk_stop), reinterpret_cast<void*>(g_stop));
  g_vt = 0;
  if (g_dshow && g_wave_open && g_wave_close) {
    const uintptr_t o = mem::iat_slot(g_dshow, "WINMM.dll", "waveOutOpen");
    const uintptr_t c = mem::iat_slot(g_dshow, "WINMM.dll", "waveOutClose");
    if (o && mem::read<void*>(o) == reinterpret_cast<void*>(&hk_wave_open))
      mem::write<void*>(o, reinterpret_cast<void*>(g_wave_open));
    if (c && mem::read<void*>(c) == reinterpret_cast<void*>(&hk_wave_close))
      mem::write<void*>(c, reinterpret_cast<void*>(g_wave_close));
  }
}

bool ready() { return g_ready; }

void pause() {
  if (!g_ready) return;
  Lock l;
  g_paused.clear();
  for (const Buffer& e : g_buffers) {
    if (e.primary) continue;
    DWORD st = 0;
    if (FAILED(g_get_status(e.b, &st)) || !(st & DSBSTATUS_PLAYING)) continue;
    g_stop(e.b);
    g_paused.push_back(Paused{e.b, (st & DSBSTATUS_LOOPING) != 0});
  }
  g_waves_paused.clear();
  for (HWAVEOUT h : g_waves)
    if (waveOutPause(h) == MMSYSERR_NOERROR) g_waves_paused.push_back(h);
  if (config::get().trace)
    logf("sound: paused %u buffer(s) of %u and %u movie soundtrack(s)", static_cast<unsigned>(g_paused.size()),
         static_cast<unsigned>(g_buffers.size()), static_cast<unsigned>(g_waves_paused.size()));
}

void resume(bool one_shots) {
  if (!g_ready) return;
  Lock l;
  int played = 0, wound = 0;
  for (const Paused& p : g_paused) {
    if (p.loop || one_shots) {
      g_play(p.b, 0, 0, p.loop ? DSBPLAY_LOOPING : 0);
      ++played;
    } else {
      p.b->SetCurrentPosition(0);  // over: a later Play starts it from the top
      ++wound;
    }
  }
  g_paused.clear();
  // A movie's soundtrack always goes on: a movie being skipped closes within a
  // frame or two, and a held waveOut could keep it from closing at all.
  for (HWAVEOUT h : g_waves_paused) waveOutRestart(h);
  const unsigned waves = static_cast<unsigned>(g_waves_paused.size());
  g_waves_paused.clear();
  if (config::get().trace)
    logf("sound: resumed %d buffer(s) and %u movie soundtrack(s), %d one-shot(s) left stopped", played, waves, wound);
}

void set_quiet(bool on) { InterlockedExchange(&g_quiet, on ? 1 : 0); }

}  // namespace re3cc::sound
