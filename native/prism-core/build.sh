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
  -ld3d12 -ldxgi -static -static-libgcc -static-libstdc++ 2>&1
echo "built build/prism_nvngx.dll.exe"
# Prism Core and its version.dll loader.
MH="external/minhook/src/hook.c external/minhook/src/buffer.c external/minhook/src/trampoline.c external/minhook/src/hde/hde64.c"
gcc -O2 -c $MH -Iexternal/minhook/include && mkdir -p build/obj && mv hook.o buffer.o trampoline.o hde64.o build/obj/
g++ -std=c++20 -O2 -Wall -Wextra -Wno-cast-function-type -shared -o build/prism_nvngx.dll src/core.cpp src/pipeline.cpp build/obj/*.o \
  -Isrc -Iexternal/minhook/include -ld3d12 -static -static-libgcc -static-libstdc++ 2>&1
g++ -std=c++20 -O2 -Wall -Wno-cast-function-type -shared -o build/version.dll src/proxy/version.cpp src/proxy/version.def \
  -static -static-libgcc -static-libstdc++ 2>&1
echo "built build/prism_nvngx.dll build/version.dll"
# Runtime shaders: compile every entry point now, so errors show at build time.
g++ -std=c++20 -O1 -o build/shader_check.exe tests/shader_check.cpp -static -static-libgcc -static-libstdc++ 2>&1
node -e "const s=require('fs').readFileSync('src/pipeline.cpp','utf8');const m='const char* kShaders = R\"(';const a=s.indexOf(m);require('fs').writeFileSync('build/shaders.hlsl',s.slice(a+m.length,s.indexOf(')\";',a)))"
./build/shader_check.exe build/shaders.hlsl Reduce1 Reduce2 Encode Compose Carry Fill Bake CopyGuides Fit
