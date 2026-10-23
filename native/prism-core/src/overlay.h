// Prism Core's in-game panel: drawn by Prism itself onto the swap chain's back buffer right
// before each present (generated frames included, so it never goes through frame generation).
// Home (KeyOverlay) or View + Y on a gamepad opens it; while open it takes the mouse, keyboard
// and gamepad from the game.
#pragma once

#include <d3d12.h>
#include <dxgi1_4.h>

namespace prism {

// Hooks the swap chain methods the panel needs (the colour space), from a probe swap chain's vtable.
void OverlaySetup(void** swapChainVtable);
// Every queue the game executes on (from the ExecuteCommandLists hook): the panel draws on the
// one its swap chain presents with.
void OverlayNoteQueue(ID3D12CommandQueue* queue);
// Draws the panel (or its toast and frame-rate corner) onto the back buffer about to be presented.
void OverlayPresent(IDXGISwapChain* swapChain);

}  // namespace prism
