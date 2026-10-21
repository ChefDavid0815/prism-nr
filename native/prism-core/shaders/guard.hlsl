// Character guard, applied to the network's edit before it is composed onto the frame.
// CPU reference: guard() in tools/nets-sim/sim.js.
//
// The edit e = log2(neural / original) splits into a luminance part (light, shading, texture),
// which passes untouched, and a chroma part, which is limited where the original is skin or
// strongly saturated (hair, eyes, costume). The network may relight a face; it may not recolour
// it. Measured on CONTROL (RenoDX, strength 1.0): skin colour shift 10.4 -> 1.9 (CbCr, 8-bit),
// red hair 18.2 -> 10.2, luminance edit changed by under 0.01 stop.

cbuffer GuardConstants : register(b0) {
  uint2 Size;
  float SkinStrength;  // 0.8
  float SatStrength;   // 0.7
  float ChromaLimit;   // stops a protected pixel's chroma may still move; 0.08
};

Texture2D<float4> Original : register(t0);  // linear colour
RWTexture2D<float4> Edit : register(u0);    // rgb = log2 ratio, in place

float Smooth(float a, float b, float x) {
  const float t = saturate((x - a) / (b - a));
  return t * t * (3.0 - 2.0 * t);
}
float3 SrgbEncode(float3 c) {
  return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(max(c, 0.0), 1.0 / 2.4) - 0.055;
}

[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (any(id.xy >= Size)) return;
  const float3 o = Original[id.xy].rgb;
  const float4 e = Edit[id.xy];

  // Skin: a soft ellipse in Cb/Cr of the display-encoded original (Chai & Ngan ranges).
  const float3 d = SrgbEncode(o) * 255.0;
  const float cb = 128.0 - 0.168736 * d.r - 0.331264 * d.g + 0.5 * d.b;
  const float cr = 128.0 + 0.5 * d.r - 0.418688 * d.g - 0.081312 * d.b;
  const float2 q = float2((cb - 107.0) / 22.0, (cr - 150.0) / 18.0);
  const float luma = dot(o, float3(0.2126, 0.7152, 0.0722));
  const float skin = exp(-dot(q, q)) * Smooth(0.02, 0.06, luma);
  const float mx = max(o.r, max(o.g, o.b)), mn = min(o.r, min(o.g, o.b));
  const float sat = Smooth(0.35, 0.7, mx > 1e-4 ? (mx - mn) / mx : 0.0);
  const float w = max(skin * SkinStrength, sat * SatStrength);

  const float y = dot(e.rgb, float3(0.2126, 0.7152, 0.0722));
  const float3 kept = (e.rgb - y) * (1.0 - w);
  const float3 capped = clamp(kept, -ChromaLimit, ChromaLimit);
  Edit[id.xy] = float4(y + kept + (capped - kept) * w, e.a);
}
