// Compiles every compute entry point of Prism Core's runtime shaders with the system's
// d3dcompiler_47.dll, so shader errors show up at build time rather than in a running game.
//   shader_check.exe <shader source file> Entry1 Entry2 ...
#include <windows.h>
#include <d3dcompiler.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

int main(int argc, char** argv) {
  if (argc < 3) { std::fprintf(stderr, "usage: shader_check <file> <entry>...\n"); return 2; }
  std::ifstream in(argv[1], std::ios::binary);
  std::stringstream ss;
  ss << in.rdbuf();
  const std::string src = ss.str();
  using CompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
  auto compile = (CompileFn)(void*)GetProcAddress(LoadLibraryW(L"d3dcompiler_47.dll"), "D3DCompile");
  if (!compile) { std::fprintf(stderr, "no d3dcompiler_47\n"); return 2; }
  int bad = 0;
  for (int i = 2; i < argc; ++i) {
    ID3DBlob *cs = nullptr, *err = nullptr;
    const HRESULT hr = compile(src.data(), src.size(), "prism", nullptr, nullptr, argv[i], "cs_5_1", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &cs, &err);
    std::printf("%-12s %s\n", argv[i], SUCCEEDED(hr) ? "ok" : "FAILED");
    if (err) std::printf("%s\n", (const char*)err->GetBufferPointer());
    if (FAILED(hr)) ++bad;
  }
  return bad;
}
