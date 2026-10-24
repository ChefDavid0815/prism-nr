#!/usr/bin/env bash
# Builds Prism Core's native tools with MinGW-w64 g++ (C++20). Shaders are compiled at run time
# by the system's d3dcompiler_47.dll, so no Visual Studio or offline shader compiler is needed.
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p build
g++ -std=c++20 -O2 -Wall -Wextra -o build/nets_harness.exe tests/nets_harness.cpp \
  -ld3d12 -ldxgi -ld3dcompiler -static -static-libgcc -static-libstdc++ 2>&1
echo "built build/nets_harness.exe"
# The bench's file name must contain "nvngx.dll" (NGX gates feature creation on the module path).
g++ -std=c++20 -O2 -Wall -Wextra -Wno-cast-function-type -o "build/prism_nvngx.dll.exe" tests/nr_bench.cpp \
  -ld3d12 -ldxgi -lwindowscodecs -lole32 -luuid -static -static-libgcc -static-libstdc++ 2>&1
echo "built build/prism_nvngx.dll.exe"
# Prism Core and its version.dll loader.
MH="external/minhook/src/hook.c external/minhook/src/buffer.c external/minhook/src/trampoline.c external/minhook/src/hde/hde64.c"
gcc -O2 -c $MH -Iexternal/minhook/include && mkdir -p build/obj && mv hook.o buffer.o trampoline.o hde64.o build/obj/
g++ -std=c++20 -O2 -Wall -Wextra -Wno-cast-function-type -shared -o build/prism_nvngx.dll src/core.cpp src/pipeline.cpp src/overlay.cpp src/bridge.cpp src/host/host_run.cpp build/obj/*.o \
  -Isrc -Iexternal/minhook/include -ld3d12 -ldxgi -ldwrite -luuid -static -static-libgcc -static-libstdc++ 2>&1
# prism_host.exe: the 64-bit side of the frame bridge (DX9 / DX10 / DX11 and 32-bit games).
g++ -std=c++20 -O2 -Wall -Wextra -municode -mwindows -o build/prism_host.exe src/host/host.cpp -static -static-libgcc -static-libstdc++ 2>&1
g++ -std=c++20 -O2 -Wall -Wno-cast-function-type -shared -o build/version.dll src/proxy/version.cpp src/proxy/version.def \
  -static -static-libgcc -static-libstdc++ 2>&1
g++ -std=c++20 -O2 -Wall -Wextra -municode -mwindows -o build/PrismLaunch.exe src/launch/prism_launch.cpp -static -static-libgcc -static-libstdc++ 2>&1
# The other loaders (dxgi, d3d12, winmm, dinput8): exports read from the system's own DLLs.
node tools/proxy/gen.cjs >/dev/null && mkdir -p build/loaders
for n in dxgi d3d12 winmm dinput8 d3d9 d3d11; do
  g++ -std=c++20 -O2 -Wall -shared -o build/loaders/$n.dll src/proxy/proxy.cpp build/proxy/${n}_thunks.S build/proxy/$n.def \
    -DPRISM_PROXY_NAME="L\"$n.dll\"" -DPRISM_PROXY_TABLE="\"../../build/proxy/${n}_table.inc\"" -static -static-libgcc -static-libstdc++ 2>&1
done
echo "built build/prism_nvngx.dll build/prism_host.exe build/version.dll build/PrismLaunch.exe build/loaders/*.dll"
# 32-bit games: Core with the frame bridge only, and 32-bit loaders (MinGW-w64 i686: the WinLibs
# toolchain in PRISM_GCC32, default E:/PrismNR-research/toolchains/mingw32/bin).
GCC32="${PRISM_GCC32:-/e/PrismNR-research/toolchains/mingw32/bin}"
if [ -x "$GCC32/g++.exe" ]; then
  mkdir -p build/obj32 build/x86/loaders
  MH32="external/minhook/src/hook.c external/minhook/src/buffer.c external/minhook/src/trampoline.c external/minhook/src/hde/hde32.c"
  (cd build/obj32 && "$GCC32/gcc.exe" -O2 -c $(for f in $MH32; do echo ../../$f; done) -I../../external/minhook/include)
  "$GCC32/g++.exe" -std=c++20 -O2 -Wall -Wextra -Wno-cast-function-type -shared -o build/x86/prism_nvngx.dll src/core32.cpp src/bridge.cpp build/obj32/*.o \
    -Isrc -Iexternal/minhook/include -luuid -static -static-libgcc -static-libstdc++ 2>&1
  node tools/proxy/gen.cjs 32 >/dev/null
  for n in version winmm dinput8 dxgi d3d9 d3d11; do
    "$GCC32/g++.exe" -std=c++20 -O2 -Wall -shared -o build/x86/loaders/$n.dll src/proxy/proxy.cpp build/proxy32/${n}_thunks.S build/proxy32/$n.def \
      -DPRISM_PROXY_NAME="L\"$n.dll\"" -DPRISM_PROXY_TABLE="\"../../build/proxy32/${n}_table.inc\"" -static -static-libgcc -static-libstdc++ 2>&1
  done
  echo "built build/x86/prism_nvngx.dll build/x86/loaders/{version,winmm,dinput8,dxgi,d3d9,d3d11}.dll"
else
  echo "no 32-bit g++ at $GCC32: the 32-bit Core is not built"
fi
# Runtime shaders: compile every entry point now, so errors show at build time.
g++ -std=c++20 -O1 -o build/shader_check.exe tests/shader_check.cpp -static -static-libgcc -static-libstdc++ 2>&1
node tools/extract_shaders.cjs
./build/shader_check.exe build/shaders.hlsl Reduce1 Reduce2 Encode Compose Carry Fill Bake CopyGuides Fit Down GuidedFit
# The in-game panel without a game: draws it over a still and saves a PNG (or "live": a window).
g++ -std=c++20 -O2 -Wall -Wextra -Wno-cast-function-type -municode -o build/overlay_demo.exe tests/overlay_demo.cpp src/overlay.cpp build/obj/*.o \
  -Isrc -Iexternal/minhook/include -ld3d12 -ldxgi -ldwrite -lwindowscodecs -lole32 -luuid -static -static-libgcc -static-libstdc++ 2>&1
echo "built build/overlay_demo.exe"
# The colour looks on a still or a chart, as a contact sheet.
g++ -std=c++20 -O2 -Wall -Wno-cast-function-type -municode -o build/look_preview.exe tests/look_preview.cpp -Isrc \
  -ld3d12 -ldxgi -lwindowscodecs -lole32 -luuid -static -static-libgcc -static-libstdc++ 2>&1
echo "built build/look_preview.exe"
