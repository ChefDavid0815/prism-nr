// PrismLaunch.exe: loads Prism Core late, into a game that is already running, for games whose
// anti-tamper refuses a proxy DLL in their folder (they crash at start with version.dll beside the
// EXE). Nothing about the protection is touched: no file of the game is changed, nothing is
// disguised, and Core is loaded the ordinary way (LoadLibraryW in the game's process, listed in its
// modules like any other DLL), after the game has started and built its renderer.
//
// As a Steam launch option:   "<game folder>\PrismLaunch.exe" %command%
//
// It starts the game with exactly the command it was given and waits outside it. On the first
// press of the panel key (KeyOverlay in prism-core.ini, Home by default) while a window of the
// game is in front, and once that process has its D3D12 renderer, it loads prism_nvngx.dll from the
// game's folder into it; the panel then opens by itself. The game always starts, whatever happens
// to Prism; the launcher stays until the game exits so Steam sees it running.
#include <windows.h>
#include <tlhelp32.h>

#include <cstdarg>
#include <cstdio>
#include <cwchar>
#include <set>
#include <string>

namespace {

std::wstring logPath;

void Log(const char* fmt, ...) {
  if (logPath.empty()) return;
  FILE* f = _wfopen(logPath.c_str(), L"a");
  if (!f) return;
  SYSTEMTIME t;
  GetLocalTime(&t);
  std::fprintf(f, "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
  va_list a;
  va_start(a, fmt);
  std::vfprintf(f, fmt, a);
  va_end(a);
  std::fputc('\n', f);
  std::fclose(f);
}

std::string Utf8(const std::wstring& w) {
  const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
  std::string s(n > 0 ? n - 1 : 0, '\0');
  if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
  return s;
}

std::wstring Lower(std::wstring s) {
  for (wchar_t& c : s) c = towlower(c);
  return s;
}

std::wstring DirOf(const std::wstring& path) {
  const size_t i = path.find_last_of(L"\\/");
  return i == std::wstring::npos ? L"" : path.substr(0, i + 1);
}

// What follows our own program name on the command line: the game's command, as Steam gave it.
std::wstring RestOfCommandLine(const wchar_t* line) {
  const wchar_t* p = line;
  while (*p == L' ' || *p == L'\t') ++p;
  if (*p == L'"') { ++p; while (*p && *p != L'"') ++p; if (*p) ++p; }
  else while (*p && *p != L' ' && *p != L'\t') ++p;
  while (*p == L' ' || *p == L'\t') ++p;
  return p;
}

// The program of a command line (its first token, quoted or not).
std::wstring ProgramOf(const std::wstring& command) {
  if (command.empty()) return L"";
  if (command[0] == L'"') {
    const size_t end = command.find(L'"', 1);
    return command.substr(1, end == std::wstring::npos ? std::wstring::npos : end - 1);
  }
  return command.substr(0, command.find_first_of(L" \t"));
}

// KeyOverlay=: Home (default), Insert, End, PageUp, PageDown, F1-F24, or a virtual-key number.
int KeyFromName(const std::wstring& n) {
  const std::wstring s = Lower(n);
  if (s.empty() || s == L"home") return VK_HOME;
  if (s == L"insert") return VK_INSERT;
  if (s == L"end") return VK_END;
  if (s == L"pageup") return VK_PRIOR;
  if (s == L"pagedown") return VK_NEXT;
  if (s == L"none") return 0;
  if (s[0] == L'f' && s.size() <= 3) { const int f = _wtoi(s.c_str() + 1); if (f >= 1 && f <= 24) return VK_F1 + f - 1; }
  const int v = _wtoi(s.c_str());
  return v > 0 && v < 256 ? v : VK_HOME;
}

std::wstring ImageOf(DWORD pid) {
  HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!h) return L"";
  wchar_t b[32768];
  DWORD n = DWORD(std::size(b));
  const BOOL ok = QueryFullProcessImageNameW(h, 0, b, &n);
  CloseHandle(h);
  return ok ? std::wstring(b, n) : L"";
}

bool HasModule(DWORD pid, const wchar_t* name) {
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
  if (snap == INVALID_HANDLE_VALUE) return false;
  MODULEENTRY32W e = {};
  e.dwSize = sizeof(e);
  bool found = false;
  for (BOOL ok = Module32FirstW(snap, &e); ok && !found; ok = Module32NextW(snap, &e)) found = _wcsicmp(e.szModule, name) == 0;
  CloseHandle(snap);
  return found;
}

// Any live process whose program sits in the game's folder (or below it).
bool AnyGameProcess(const std::wstring& dir) {
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE) return false;
  PROCESSENTRY32W e = {};
  e.dwSize = sizeof(e);
  bool found = false;
  const std::wstring low = Lower(dir);
  for (BOOL ok = Process32FirstW(snap, &e); ok && !found; ok = Process32NextW(snap, &e)) {
    if (e.th32ProcessID == GetCurrentProcessId()) continue;
    const std::wstring image = Lower(ImageOf(e.th32ProcessID));
    found = !image.empty() && image.compare(0, low.size(), low) == 0 && image.find(L"prismlaunch.exe") == std::wstring::npos;
  }
  CloseHandle(snap);
  return found;
}

// LoadLibraryW(core) in the game's process. kernel32 sits at the same address in every process of
// the session, so the address resolved here is the one the game calls.
bool Load(DWORD pid, const std::wstring& core, std::string& why) {
  HANDLE p = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ, FALSE, pid);
  if (!p) { why = "cannot open the game's process (Win32 " + std::to_string(GetLastError()) + "; does it run as administrator?)"; return false; }
  const auto load = reinterpret_cast<LPTHREAD_START_ROUTINE>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW")));
  const SIZE_T bytes = (core.size() + 1) * sizeof(wchar_t);
  void* remote = VirtualAllocEx(p, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  bool ok = false;
  if (!remote) why = "cannot reserve memory in the game's process";
  else {
    SIZE_T written = 0;
    if (!WriteProcessMemory(p, remote, core.c_str(), bytes, &written) || written != bytes) why = "cannot write the path into the game's process";
    else if (HANDLE t = CreateRemoteThread(p, nullptr, 0, load, remote, 0, nullptr)) {
      const DWORD waited = WaitForSingleObject(t, 20000);
      DWORD result = 0;
      GetExitCodeThread(t, &result);
      CloseHandle(t);
      if (waited != WAIT_OBJECT_0) why = "the loading thread did not finish in 20 s";
      else if (!result) why = "LoadLibraryW refused prism_nvngx.dll in the game";
      else ok = true;
    } else why = "cannot start the loading thread (Win32 " + std::to_string(GetLastError()) + ")";
    if (ok || why.find("did not finish") == std::string::npos) VirtualFreeEx(p, remote, 0, MEM_RELEASE);
  }
  CloseHandle(p);
  return ok;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  wchar_t self[MAX_PATH];
  GetModuleFileNameW(nullptr, self, MAX_PATH);
  const std::wstring here = DirOf(self);
  const std::wstring command = RestOfCommandLine(GetCommandLineW());
  if (command.empty()) {
    MessageBoxW(nullptr,
                L"PrismLaunch 需要作为 Steam 启动项使用：\n\n在 Steam 库里右键游戏 → 属性 → 通用 → 启动选项，填入：\n\"<游戏目录>\\PrismLaunch.exe\" %command%\n\n"
                L"然后正常启动游戏，进入游戏画面后按 Home 加载 Prism 并打开面板。",
                L"Prism Core", MB_OK | MB_ICONINFORMATION);
    return 2;
  }
  const std::wstring program = ProgramOf(command);
  std::wstring gameDir = DirOf(program);
  if (gameDir.empty()) gameDir = here;
  logPath = here + L"prism-launch.log";
  Log("command: %s", Utf8(command).c_str());

  // The game starts whatever Prism thinks of it.
  STARTUPINFOW si = {};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi = {};
  std::wstring line = command;
  if (!CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
    const DWORD e = GetLastError();
    Log("CreateProcess failed, Win32 %lu", e);
    MessageBoxW(nullptr, (L"无法启动游戏（Win32 错误 " + std::to_wstring(e) + L"）：\n" + command).c_str(), L"Prism Core", MB_OK | MB_ICONERROR);
    return 1;
  }
  CloseHandle(pi.hThread);
  // Core sits beside the launcher (the installer puts both in the game's EXE folder).
  const std::wstring core = here + L"prism_nvngx.dll";
  const bool haveCore = GetFileAttributesW(core.c_str()) != INVALID_FILE_ATTRIBUTES;
  const int key = KeyFromName([&] {
    wchar_t b[64] = {};
    GetPrivateProfileStringW(L"Prism Core", L"KeyOverlay", L"Home", b, 64, (here + L"prism-core.ini").c_str());
    return std::wstring(b);
  }());
  Log("started pid %lu; core %s; panel key %d; game folder %s", pi.dwProcessId, haveCore ? "found" : "missing (the game just runs)", key, Utf8(gameDir).c_str());

  std::set<DWORD> done;
  HANDLE openPanel = nullptr;
  bool wasDown = true;  // a key held while the game starts is not a press
  int gone = 0;
  for (;;) {
    DWORD code = 0;
    const bool alive = GetExitCodeProcess(pi.hProcess, &code) && code == STILL_ACTIVE;
    if (!alive) {
      // A game that hands over to another process of its own folder keeps the launcher here.
      if (!AnyGameProcess(gameDir)) { if (++gone > 40) break; } else gone = 0;
      Sleep(250);
      continue;
    }
    Sleep(50);
    if (!haveCore || !key) continue;
    const bool down = (GetAsyncKeyState(key) & 0x8000) != 0;
    const bool pressed = down && !wasDown;
    wasDown = down;
    if (!pressed) continue;
    DWORD owner = 0;
    const HWND fg = GetForegroundWindow();
    if (!fg || !GetWindowThreadProcessId(fg, &owner) || done.count(owner)) continue;
    const std::wstring image = ImageOf(owner);
    if (image.empty() || Lower(image).compare(0, gameDir.size(), Lower(gameDir)) != 0) { Log("panel key pressed, the game is not in front"); continue; }
    if (HasModule(owner, L"prism_nvngx.dll")) { done.insert(owner); Log("pid %lu already has Prism Core (loaded at start)", owner); continue; }
    if (!HasModule(owner, L"d3d12.dll") || !HasModule(owner, L"dxgi.dll")) { Log("pid %lu has no D3D12 renderer yet; press again in the game", owner); continue; }
    // The panel opens by itself: this press was for it.
    wchar_t name[64];
    std::swprintf(name, 64, L"Local\\PrismCoreOpenPanel-%lu", owner);
    if (openPanel) CloseHandle(openPanel);
    openPanel = CreateEventW(nullptr, TRUE, TRUE, name);
    std::string why;
    if (Load(owner, core, why)) { done.insert(owner); Log("loaded Prism Core into pid %lu (%s)", owner, Utf8(image).c_str()); }
    else Log("could not load Prism Core into pid %lu: %s", owner, why.c_str());
  }
  DWORD code = 0;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hProcess);
  if (openPanel) CloseHandle(openPanel);
  Log("game exited");
  return int(code);
}
