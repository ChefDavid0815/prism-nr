// The game's side of the frame bridge (bridge_proto.h): DX9, DX10 and DX11 games, 32 or 64-bit,
// hand each presented frame to prism_host.exe and get Prism's result back before the real Present.
#pragma once

#include <dxgi.h>

#include <string>

struct ID3D12CommandQueue;
struct ID3D12Resource;

namespace prism {

using BridgeLogFn = void (*)(const char* fmt, ...);

// Once, with the game's folder (prism_host.exe and prism-core.ini live there) and the log.
void BridgeInit(const std::wstring& gameDir, BridgeLogFn log);
// A DX10 or DX11 swap chain's frame, from a Present hook; false (untouched) for any other kind.
bool BridgeDxgiPresent(IDXGISwapChain* sc);
// A D3D12 frame was just presented: the bridge stands aside while they come (DX12 first).
void BridgeNoteD3D12();
// Offload= in prism-core.ini: a D3D12 game's network runs on a second card, through the bridge.
bool BridgeOffloadWanted();
// A D3D12 back buffer (state PRESENT) through the bridge on the game's queue: read back, run by
// prism_host.exe on its card, written back. True when Prism's result is in the back buffer.
bool BridgeD3D12Frame(ID3D12CommandQueue* q, ID3D12Resource* bb, int mode);
// Hooks Direct3D 9's Present (and Reset, which must see Prism's own video memory released first).
void BridgeHookD3D9();
// Hooks DXGI's Present for DX10 / DX11 games where nothing else does (the 32-bit Core).
void BridgeHookDxgi();

}  // namespace prism
