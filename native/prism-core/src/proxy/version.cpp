// version.dll proxy: forwards every export to the system's version.dll and loads Prism Core
// (prism_nvngx.dll) from its own folder. Games that import VERSION.dll load it at start-up.
// All version.dll functions take integer or pointer arguments only, so on x64 one forwarder
// shape that passes eight register/stack slots through serves all of them.
#include <windows.h>

#include <cstdint>

namespace {
HMODULE real = nullptr;

FARPROC Real(const char* name) {
  if (!real) {
    wchar_t path[MAX_PATH];
    GetSystemDirectoryW(path, MAX_PATH);
    lstrcatW(path, L"\\version.dll");
    real = LoadLibraryW(path);
  }
  return real ? GetProcAddress(real, name) : nullptr;
}

using Fn = uintptr_t (*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);

void LoadCore(HMODULE self) {
  wchar_t path[MAX_PATH];
  const DWORD n = GetModuleFileNameW(self, path, MAX_PATH);
  for (DWORD i = n; i > 0; --i) {
    if (path[i - 1] == L'\\' || path[i - 1] == L'/') { path[i] = 0; break; }
  }
  lstrcatW(path, L"prism_nvngx.dll");
  LoadLibraryW(path);
}
}  // namespace

#define FORWARD(name)                                                                              \
  extern "C" uintptr_t Proxy_##name(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e, \
                                    uintptr_t f, uintptr_t g, uintptr_t h) {                       \
    static Fn fn = reinterpret_cast<Fn>(reinterpret_cast<void*>(Real(#name)));                     \
    return fn ? fn(a, b, c, d, e, f, g, h) : 0;                                                    \
  }

FORWARD(GetFileVersionInfoA)
FORWARD(GetFileVersionInfoByHandle)
FORWARD(GetFileVersionInfoExA)
FORWARD(GetFileVersionInfoExW)
FORWARD(GetFileVersionInfoSizeA)
FORWARD(GetFileVersionInfoSizeExA)
FORWARD(GetFileVersionInfoSizeExW)
FORWARD(GetFileVersionInfoSizeW)
FORWARD(GetFileVersionInfoW)
FORWARD(VerFindFileA)
FORWARD(VerFindFileW)
FORWARD(VerInstallFileA)
FORWARD(VerInstallFileW)
FORWARD(VerLanguageNameA)
FORWARD(VerLanguageNameW)
FORWARD(VerQueryValueA)
FORWARD(VerQueryValueW)

BOOL WINAPI DllMain(HINSTANCE self, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(self);
    LoadCore(self);
  }
  return TRUE;
}
