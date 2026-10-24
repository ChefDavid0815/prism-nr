// Prism Core for 32-bit games (prism_nvngx.dll, x86): NVIDIA's network runtime is 64-bit only, so a
// 32-bit game carries just the frame bridge (bridge.cpp). Loaded by a 32-bit loader; once the
// game's graphics API is up it hooks Present (Direct3D 9, or DXGI for DX10 / DX11), and every
// frame goes to prism_host.exe (64-bit) and back.
#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>

#include "MinHook.h"
#include "bridge.h"

namespace {

FILE* logFile = nullptr;
std::mutex logMutex;
void Log(const char* fmt, ...) {
  std::lock_guard<std::mutex> lock(logMutex);
  if (!logFile) return;
  SYSTEMTIME t;
  GetLocalTime(&t);
  std::fprintf(logFile, "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
  va_list a;
  va_start(a, fmt);
  std::vfprintf(logFile, fmt, a);
  va_end(a);
  std::fputc('\n', logFile);
  std::fflush(logFile);
}

std::wstring ModuleDir(HMODULE m) {
  wchar_t p[MAX_PATH];
  const DWORD n = GetModuleFileNameW(m, p, MAX_PATH);
  std::wstring s(p, n);
  return s.substr(0, s.find_last_of(L"\\/") + 1);
}

DWORD WINAPI Watch(void*) {
  auto loaded = [](const wchar_t* m) { return GetModuleHandleW(m) != nullptr; };
  int seen = 0;
  for (int tick = 0; tick < 20 * 600 && seen < 40; ++tick) {  // two seconds after the API loads
    if (loaded(L"d3d9.dll") || loaded(L"d3d11.dll") || loaded(L"d3d10.dll") || loaded(L"d3d10_1.dll")) ++seen;
    Sleep(50);
  }
  if (seen < 40) { Log("no Direct3D 9, 10 or 11 in this process"); return 0; }
  if (loaded(L"d3d9.dll")) prism::BridgeHookD3D9();
  if (loaded(L"dxgi.dll") && (loaded(L"d3d11.dll") || loaded(L"d3d10.dll") || loaded(L"d3d10_1.dll"))) prism::BridgeHookDxgi();
  return 0;
}

}  // namespace

BOOL WINAPI DllMain(HINSTANCE self, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(self);
    const std::wstring dir = ModuleDir(self);
    logFile = _wfopen((dir + L"prism-core.log").c_str(), L"w");
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    Log("Prism Core 1.5.0 (32-bit: the frame bridge to prism_host.exe) loaded into %ls (pid %lu)", exe, GetCurrentProcessId());
    if (MH_Initialize() != MH_OK) { Log("MinHook init failed"); return TRUE; }
    prism::BridgeInit(dir, &Log);
    CloseHandle(CreateThread(nullptr, 0, Watch, nullptr, 0, nullptr));
  }
  return TRUE;
}
