// prism_host.exe: the 64-bit side of the frame bridge (bridge_proto.h) for DX9, DX10, DX11 and
// 32-bit games. NVIDIA's runtime only serves calls from a module named like an NGX one, so the work
// runs inside the 64-bit Prism Core (host_run.cpp): this loads it from its own folder
// (prism64_nvngx.dll beside a 32-bit game, prism_nvngx.dll beside a 64-bit one) and calls
// PrismHostRun with the game's process id. It exits with the game.
#include <windows.h>
#include <shellapi.h>

#include <cwchar>
#include <string>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  int argc = 0;
  wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (!argv || argc < 2) return 2;
  wchar_t exe[MAX_PATH];
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  std::wstring dir = exe;
  dir.resize(dir.find_last_of(L"\\/") + 1);
  HMODULE core = LoadLibraryW((dir + L"prism64_nvngx.dll").c_str());
  if (!core) core = LoadLibraryW((dir + L"prism_nvngx.dll").c_str());
  using RunFn = int (*)(unsigned long);
  auto run = core ? (RunFn)(void*)GetProcAddress(core, "PrismHostRun") : nullptr;
  if (!run) return 3;
  return run(std::wcstoul(argv[1], nullptr, 10));
}
