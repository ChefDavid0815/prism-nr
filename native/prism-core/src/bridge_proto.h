// The frame bridge between a game that cannot run the network itself (DX9, DX10, DX11, or any
// 32-bit game: the NVIDIA runtime is 64-bit and D3D12 only) and prism_host.exe, a 64-bit process
// that runs Prism's pipeline. The game's side (bridge.cpp) copies each presented frame into shared
// memory, signals, waits, and copies the result back before the real Present. The layout uses
// fixed-size fields only: a 32-bit game and the 64-bit host see the same bytes.
#pragma once

#include <cstdint>

namespace prism {

constexpr uint32_t kBridgeMagic = 0x4d535250;  // "PRSM"
constexpr uint32_t kBridgeVersion = 1;
constexpr uint32_t kBridgeMaxBytes = 3840u * 2160u * 8u;  // up to 4K at 8 bytes a pixel (scRGB)

struct BridgeHeader {
  uint32_t magic, version;
  uint32_t hostReady;     // the host has its device and the network; the game posts only then
  uint32_t hostPid;
  uint32_t width, height;
  uint32_t format;        // DXGI_FORMAT of the pixels (8-bit formats as UNORM, BGRA for D3D9)
  uint32_t pitch;         // bytes per row in the pixel area
  uint32_t mode;          // the encoding: 0 SDR, 1 scRGB, 2 HDR10
  uint32_t api;           // 9, 10, 11 or 12 (the log's information)
  volatile int32_t posted;  // the game's frame number
  volatile int32_t done;    // the host sets it to the frame it finished
  uint32_t status;        // 0 the result is in the pixel area, 1 passed through untouched
  uint32_t gameLuidLow;   // the adapter the game renders on (0 and 0 when unknown)
  int32_t gameLuidHigh;
  uint32_t offload;       // 1: a D3D12 game hands its frames to a second card (Offload= in the ini)
  uint32_t reserved[16];
};
static_assert(sizeof(BridgeHeader) == 128, "a fixed header for 32 and 64-bit processes");

// Kernel object names, by the game's process id: the mapping (header, then pixels) and the two
// events, posted (game -> host) and done (host -> game).
inline void BridgeNames(unsigned long pid, wchar_t* mapping, wchar_t* posted, wchar_t* done, int n) {
  _snwprintf(mapping, n, L"Local\\PrismBridge-%lu", pid);
  _snwprintf(posted, n, L"Local\\PrismBridge-%lu-posted", pid);
  _snwprintf(done, n, L"Local\\PrismBridge-%lu-done", pid);
}

}  // namespace prism
