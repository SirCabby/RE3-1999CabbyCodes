// A stand-in for GOG's DirectDraw wrapper (ddraw_orig.dll) for tests/test_adopt.cpp:
//
//   i686-w64-mingw32-g++ -std=c++20 -O2 -shared -static -Wl,--kill-at tests/fake_ddraw.cpp -ld3d9
//       -o <dir>/ddraw_orig.dll
//
// It has the 14 exports the proxy forwards (stubs: the test calls none of
// them) and, like the real wrapper, imports Direct3DCreate9 from d3d9.dll - the
// import slot the proxy hooks. FakeDirect3DCreate9 calls through that slot.
#include <windows.h>
#include <d3d9.h>

#define STUB(name) \
  extern "C" __declspec(dllexport) long name() { return E_NOTIMPL; }
STUB(AcquireDDThreadLock)
STUB(CompleteCreateSysmemSurface)
STUB(D3DParseUnknownCommand)
STUB(DDInternalLock)
STUB(DDInternalUnlock)
STUB(DirectDrawCreate)
STUB(DirectDrawCreateClipper)
STUB(DirectDrawCreateEx)
STUB(DirectDrawEnumerateA)
STUB(DirectDrawEnumerateExA)
STUB(DirectDrawEnumerateExW)
STUB(DirectDrawEnumerateW)
STUB(ReleaseDDThreadLock)

// Declared by the COM headers already, with this signature (the export's name
// is kept undecorated by --kill-at).
extern "C" __declspec(dllexport) HRESULT __stdcall DllGetClassObject(REFCLSID, REFIID, LPVOID*) {
  return CLASS_E_CLASSNOTAVAILABLE;
}

extern "C" __declspec(dllexport) IDirect3D9* FakeDirect3DCreate9(UINT sdk) { return Direct3DCreate9(sdk); }
