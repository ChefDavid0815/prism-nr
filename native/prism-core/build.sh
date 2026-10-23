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
g++ -std=c++20 -O2 -Wall -Wextra -Wno-cast-function-type -shared -o build/prism_nvngx.dll src/core.cpp src/pipeline.cpp src/overlay.cpp build/obj/*.o \
  -Isrc -Iexternal/minhook/include -ld3d12 -ldxgi -ldwrite -static -static-libgcc -static-libstdc++ 2>&1
g++ -std=c++20 -O2 -Wall -Wno-cast-function-type -shared -o build/version.dll src/proxy/version.cpp src/proxy/version.def \
  -static -static-libgcc -static-libstdc++ 2>&1
g++ -std=c++20 -O2 -Wall -Wextra -municode -mwindows -o build/PrismLaunch.exe src/launch/prism_launch.cpp -static -static-libgcc -static-libstdc++ 2>&1
# The other loaders (dxgi, d3d12, winmm, dinput8): exports read from the system's own DLLs.
node tools/proxy/gen.cjs >/dev/null && mkdir -p build/loaders
for n in dxgi d3d12 winmm dinput8; do
  g++ -std=c++20 -O2 -Wall -shared -o build/loaders/$n.dll src/proxy/proxy.cpp build/proxy/${n}_thunks.S build/proxy/$n.def \
    -DPRISM_PROXY_NAME="L\"$n.dll\"" -DPRISM_PROXY_TABLE="\"../../build/proxy/${n}_table.inc\"" -static -static-libgcc -static-libstdc++ 2>&1
done
echo "built build/prism_nvngx.dll build/version.dll build/PrismLaunch.exe build/loaders/{dxgi,d3d12,winmm,dinput8}.dll"
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
