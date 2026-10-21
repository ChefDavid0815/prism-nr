// Prism Core's neural rendering pipeline, recorded right after the game's DLSS upscale
// (super resolution or ray reconstruction) into the same command list:
//   copy output -> exposure -> encode (display-referred proxy) -> NR -> compose with guard
#pragma once

#include <d3d12.h>

#include <string>

namespace prism {

struct UpscaleCall {
  ID3D12GraphicsCommandList* list;
  ID3D12Resource* output;   // the game's upscaled frame, UAV state on entry and exit
  ID3D12Resource* motion;   // render resolution
  ID3D12Resource* depth;    // render resolution
  unsigned renderWidth, renderHeight;
  float motionScaleX, motionScaleY;
  int reset;
};

using LogFn = void (*)(const char* fmt, ...);

// One-time setup: where nvngx_dlssnr.dll lives, the game's DLSS create flags, the log.
void PipelineInit(const std::wstring& gameDir, int createFlags, LogFn log);
// Records Prism's passes for this frame. Returns false (and stays off) after any failure.
bool PipelineRecord(const UpscaleCall& call);

}  // namespace prism
