// Access to NGX parameter blocks by MSVC vtable slot. The blocks the game, Streamline and the NGX
// runtimes pass around are MSVC C++ objects; MinGW lays out overloaded virtuals differently, so
// every call goes through the slot index. Order (each overload group reversed by MSVC):
//   Set: void*=0, ID3D12Resource*=1, ID3D11Resource*=2, int=3, uint=4, double=5, float=6, ull=7
//   Get: void**=8, ID3D12Resource**=9, ID3D11Resource**=10, int*=11, uint*=12, double*=13,
//        float*=14, ull*=15;  Reset=16
// Verified against the signed NR runtime by tests/nr_bench.cpp.
#pragma once

#include <d3d12.h>

#include <cstdint>

namespace prism::ngx {

using Result = uint32_t;
constexpr Result kSuccess = 0x1;
constexpr Result kFail = 0xBAD00000u;

inline void** Vt(const void* p) { return *reinterpret_cast<void** const*>(p); }

inline void SetPtr(void* p, const char* k, void* v) { reinterpret_cast<void (*)(void*, const char*, void*)>(Vt(p)[0])(p, k, v); }
inline void SetRes(void* p, const char* k, ID3D12Resource* v) { reinterpret_cast<void (*)(void*, const char*, ID3D12Resource*)>(Vt(p)[1])(p, k, v); }
inline void SetI(void* p, const char* k, int v) { reinterpret_cast<void (*)(void*, const char*, int)>(Vt(p)[3])(p, k, v); }
inline void SetUI(void* p, const char* k, unsigned v) { reinterpret_cast<void (*)(void*, const char*, unsigned)>(Vt(p)[4])(p, k, v); }
inline void SetF(void* p, const char* k, float v) { reinterpret_cast<void (*)(void*, const char*, float)>(Vt(p)[6])(p, k, v); }

inline bool GetRes(const void* p, const char* k, ID3D12Resource** v) {
  *v = nullptr;
  return reinterpret_cast<Result (*)(const void*, const char*, ID3D12Resource**)>(Vt(p)[9])(p, k, v) == kSuccess && *v;
}
inline bool GetI(const void* p, const char* k, int* v) { return reinterpret_cast<Result (*)(const void*, const char*, int*)>(Vt(p)[11])(p, k, v) == kSuccess; }
inline bool GetUI(const void* p, const char* k, unsigned* v) { return reinterpret_cast<Result (*)(const void*, const char*, unsigned*)>(Vt(p)[12])(p, k, v) == kSuccess; }
inline bool GetF(const void* p, const char* k, float* v) { return reinterpret_cast<Result (*)(const void*, const char*, float*)>(Vt(p)[14])(p, k, v) == kSuccess; }

// NVSDK_NGX_Handle: the feature id comes first.
struct Handle { unsigned id; };

constexpr unsigned kFeatureSuperSampling = 1;
constexpr unsigned kFeatureRayReconstruction = 13;
constexpr unsigned kFeatureNeuralRendering = 18;

}  // namespace prism::ngx
