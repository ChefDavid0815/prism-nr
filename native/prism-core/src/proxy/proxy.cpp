// A loader proxy for games that do not import version.dll: built as dxgi.dll, d3d12.dll, winmm.dll
// or dinput8.dll (the export list, the .def and the jump thunks come from tools/proxy/gen.cjs, read
// from the system's own DLL). At load it brings in the real DLL from System32, points every thunk
// at the real export, and loads Prism Core (prism_nvngx.dll) from its own folder, as version.dll
// does. The game sees the system DLL's exact functions.
#include <windows.h>

#include <cstdint>

#ifndef PRISM_PROXY_NAME
#error PRISM_PROXY_NAME (L"dxgi.dll" and so on) is set by the build
#endif

extern "C" void* PrismReal[];

namespace {
struct Export { const char* name; uintptr_t ordinal; };
const Export kExports[] = {
#include PRISM_PROXY_TABLE
};

void Bind() {
  wchar_t path[MAX_PATH];
  GetSystemDirectoryW(path, MAX_PATH);
  lstrcatW(path, L"\\");
  lstrcatW(path, PRISM_PROXY_NAME);
  HMODULE real = LoadLibraryW(path);
  if (!real) return;
  for (size_t i = 0; i < sizeof(kExports) / sizeof(kExports[0]); ++i)
    PrismReal[i] = reinterpret_cast<void*>(GetProcAddress(real, kExports[i].name ? kExports[i].name : MAKEINTRESOURCEA(kExports[i].ordinal)));
}

// Target= in prism-core.ini names the game's executable; any other process from the folder that
// loads this DLL (a launcher, a crash reporter) gets the system DLL only, not Prism Core.
bool IsTarget(const wchar_t* dir) {
  wchar_t ini[MAX_PATH], want[MAX_PATH] = {}, exe[MAX_PATH];
  lstrcpynW(ini, dir, MAX_PATH);
  lstrcatW(ini, L"prism-core.ini");
  GetPrivateProfileStringW(L"Prism Core", L"Target", L"", want, MAX_PATH, ini);
  if (!want[0]) return true;
  const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
  const wchar_t* name = exe;
  for (DWORD i = 0; i < n; ++i) if (exe[i] == L'\\' || exe[i] == L'/') name = exe + i + 1;
  return lstrcmpiW(name, want) == 0;
}

void LoadCore(HMODULE self) {
  wchar_t path[MAX_PATH];
  const DWORD n = GetModuleFileNameW(self, path, MAX_PATH);
  for (DWORD i = n; i > 0; --i) {
    if (path[i - 1] == L'\\' || path[i - 1] == L'/') { path[i] = 0; break; }
  }
  if (!IsTarget(path)) return;
  lstrcatW(path, L"prism_nvngx.dll");
  LoadLibraryW(path);
}
}  // namespace

BOOL WINAPI DllMain(HINSTANCE self, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(self);
    Bind();
    LoadCore(self);
  }
  return TRUE;
}
