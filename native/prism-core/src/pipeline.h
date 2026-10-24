// Prism Core's neural rendering pipeline, recorded right after the game's DLSS upscale
// (super resolution or ray reconstruction) into the same command list:
//   copy output -> exposure -> encode (display-referred proxy) -> NR -> compose with guard
#pragma once

#include <d3d12.h>

#include <string>

#include "grade.h"

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

// One-time setup: where nvngx_dlssnr.dll lives, the game's DLSS create flags, the log, and the
// game's device (for the Present hook the in-game panel draws from).
void PipelineInit(const std::wstring& gameDir, int createFlags, LogFn log, ID3D12Device* device);
// Present mode (Present=1 in prism-core.ini): Prism on the frame as it is presented, for games
// without DLSS. PresentWanted: on, and no upscale has come for a moment. PipelinePresent records and
// runs on the presenting queue q; bb is the back buffer (PRESENT state before and after); mode is
// its encoding (0 SDR, 1 scRGB, 2 HDR10).
bool PresentWanted();
bool PipelinePresent(ID3D12CommandQueue* q, ID3D12Resource* bb, int mode);
// Prism on one frame, as present mode does it but whether or not it is on: prism_host.exe runs the
// frames the bridge (DX9 / DX10 / DX11, 32-bit games) hands it through this. bb is in the COMMON
// (PRESENT) state before and after.
bool PipelineFrame(ID3D12CommandQueue* q, ID3D12Resource* bb, int mode);
// DX10 / DX11 games (no d3d12.dll): Present hooked through a D3D11 probe, for the frame bridge.
void HookPresentWithoutD3D12(const std::wstring& gameDir, LogFn log);
// prism_host.exe's loop (host/host_run.cpp): serves the game pid's frame bridge until it exits.
int HostRun(unsigned long pid, const std::wstring& gameDir, LogFn log);
// Records Prism's passes for this frame. Returns false (and stays off) after any failure.
bool PipelineRecord(const UpscaleCall& call);

// ------------------------------------------------------------------ the in-game panel's view
// The settings the panel shows and changes. A change applies on the next rendered frame and is
// saved to prism-core.ini (SaveLiveSettings), so the next launch and the app keep it.
struct LiveSettings {
  bool enabled = true;
  int cadence = 2, passes = 2;
  float intensity = 1.0f, netScale = 0.625f, guard = 1.0f;  // netScale: a side; 0.625 is the panel's 100%
  bool async = true, split = false;
  float splitPos = 0.5f;  // the split's column, as a fraction of the width
  int look = 0;           // 0 默认, 1 高还原, 2 高饱和, 3 低饱和, 4 浓烈, 5 自定义 (look_hlsl.h)
  GradeValues grade[kGradeSlots];  // the custom look's controls (grade.h), three slots
  int gradeSlot = 0;               // the slot in use
  bool netAuto = false;   // the network's size follows FpsTarget, up to netScale
  int fpsTarget = 60;     // rendered frames per second (frame generation multiplies what is shown)
};
LiveSettings GetLiveSettings();
void SetLiveSettings(const LiveSettings& s);
void SaveLiveSettings();

constexpr int kFrameHistory = 240;
struct LiveStats {
  bool running = false;        // the pipeline is built and has recorded a frame
  const char* failure = nullptr;  // why Prism stopped, if it did
  unsigned outW = 0, outH = 0, renderW = 0, renderH = 0, netW = 0, netH = 0;  // netW/H: this frame's
  float prismMs = 0.0f;        // Prism's GPU time on the game's queue per rendered frame
  float netMs = 0.0f;          // the asynchronous network's GPU time per launch
  float presentFps = 0.0f, renderFps = 0.0f;  // over the last second
  bool present = false;        // present mode is processing the presented frame (no DLSS)
  int frames = 0;              // valid entries in renderMs, oldest first
  float renderMs[kFrameHistory] = {};  // rendered-frame intervals
};
void GetLiveStats(LiveStats* out);
// A short note for the panel's toast when a hotkey or the app changed something ("" when none).
std::wstring TakeLiveNotice();

std::wstring GameDir();
LogFn PipelineLogger();

}  // namespace prism
